#include "protocol/chat_protocol.h"
#include "protocol/frame_decoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <asio.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using asio::ip::tcp;
using namespace std::chrono_literals;

constexpr auto kProcessStartupTimeout = 3s;
constexpr auto kAuthenticationReadTimeout = 3s;

class ScopedTestDirectory
{
  public:
    ScopedTestDirectory()
        : m_path(
              std::filesystem::temp_directory_path() /
              ("chathub-process-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path &path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

enum class MySqlPasswordEnvironment
{
    Inherit,
    Remove,
    Set,
};

struct MySqlStartupTestConfig
{
    std::wstring username;
    std::wstring password;
    std::wstring database;
};

// 功能：保存真实 MySQL TCP 聊天测试所需的数据库配置与两名已认证用户的测试输入。
// 边界：JWT 只在测试父进程内用于发送 auth 帧，绝不传给 ChatServer 子进程。
struct MySqlChatTestConfig
{
    MySqlStartupTestConfig mysql;
    std::string alice_username;
    std::string alice_token;
    std::string bob_username;
    std::string bob_token;
};

class ServerProcess
{
  public:
    ~ServerProcess()
    {
        stop();
    }

    bool start(const std::filesystem::path &executable, const std::vector<std::wstring> &arguments,
               const std::filesystem::path &working_directory,
               const MySqlPasswordEnvironment mysql_password_environment = MySqlPasswordEnvironment::Inherit,
               const std::wstring_view mysql_password = {})
    {
        std::wstring command_line = quote(executable.wstring());
        for (const std::wstring &argument : arguments)
        {
            command_line += L" ";
            command_line += quote(argument);
        }

        SECURITY_ATTRIBUTES pipe_attributes{};
        pipe_attributes.nLength = sizeof(pipe_attributes);
        pipe_attributes.bInheritHandle = TRUE;

        HANDLE standard_output_read = nullptr;
        HANDLE standard_output_write = nullptr;
        if (!::CreatePipe(&standard_output_read, &standard_output_write, &pipe_attributes, 0) ||
            !::SetHandleInformation(standard_output_read, HANDLE_FLAG_INHERIT, 0))
        {
            if (standard_output_read != nullptr)
            {
                ::CloseHandle(standard_output_read);
            }
            if (standard_output_write != nullptr)
            {
                ::CloseHandle(standard_output_write);
            }
            return false;
        }

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESTDHANDLES;
        startup_info.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        startup_info.hStdOutput = standard_output_write;
        startup_info.hStdError = standard_output_write;
        PROCESS_INFORMATION process_info{};

        std::optional<std::vector<wchar_t>> child_environment;
        if (mysql_password_environment != MySqlPasswordEnvironment::Inherit)
        {
            child_environment = copyEnvironmentWithMySqlPassword(mysql_password_environment, mysql_password);
            if (!child_environment)
            {
                ::CloseHandle(standard_output_write);
                ::CloseHandle(standard_output_read);
                return false;
            }
        }

        const bool process_started =
            CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             child_environment ? child_environment->data() : nullptr, working_directory.c_str(),
                             &startup_info, &process_info) != FALSE;
        ::CloseHandle(standard_output_write);
        if (!process_started)
        {
            ::CloseHandle(standard_output_read);
            return false;
        }

        m_process = process_info.hProcess;
        m_standard_output_read = standard_output_read;
        ::CloseHandle(process_info.hThread);
        return true;
    }

    bool waitsForStandardOutput(const std::vector<std::string_view> &required_fragments,
                                const std::chrono::milliseconds timeout) const
    {
        std::string output;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            output += readAvailableStandardOutput();

            bool contains_all_fragments = true;
            for (const std::string_view fragment : required_fragments)
            {
                if (output.find(fragment) == std::string::npos)
                {
                    contains_all_fragments = false;
                    break;
                }
            }
            if (contains_all_fragments)
            {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    bool waitForExit(const std::chrono::milliseconds timeout, DWORD &exit_code) const
    {
        if (m_process == nullptr ||
            ::WaitForSingleObject(m_process, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
        {
            return false;
        }
        return ::GetExitCodeProcess(m_process, &exit_code) != FALSE;
    }

    void stop()
    {
        if (m_process == nullptr)
        {
            return;
        }

        if (::WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
        {
            ::TerminateProcess(m_process, EXIT_SUCCESS);
            ::WaitForSingleObject(m_process, static_cast<DWORD>(kProcessStartupTimeout.count()));
        }
        ::CloseHandle(m_process);
        m_process = nullptr;
        if (m_standard_output_read != nullptr)
        {
            ::CloseHandle(m_standard_output_read);
            m_standard_output_read = nullptr;
        }
    }

  private:
    // 复制父进程环境并仅覆盖密码变量；仅影响本次子进程，测试自身环境保持不变。
    static std::optional<std::vector<wchar_t>>
    copyEnvironmentWithMySqlPassword(const MySqlPasswordEnvironment mysql_password_environment,
                                     const std::wstring_view mysql_password)
    {
        if (mysql_password_environment == MySqlPasswordEnvironment::Set && mysql_password.empty())
        {
            return std::nullopt;
        }

        const wchar_t *const inherited_environment = ::GetEnvironmentStringsW();
        if (inherited_environment == nullptr)
        {
            return std::nullopt;
        }

        std::vector<wchar_t> child_environment;
        const wchar_t *entry_begin = inherited_environment;
        while (*entry_begin != L'\0')
        {
            const wchar_t *entry_end = entry_begin;
            while (*entry_end != L'\0')
            {
                ++entry_end;
            }

            const std::wstring_view entry(entry_begin, static_cast<std::size_t>(entry_end - entry_begin));
            const auto equals_position = entry.find(L'=');
            const auto has_name = [entry, equals_position](const wchar_t *const expected_name) {
                return equals_position != std::wstring_view::npos &&
                       ::CompareStringOrdinal(entry.data(), static_cast<int>(equals_position), expected_name, -1, TRUE) ==
                           CSTR_EQUAL;
            };
            const bool is_secret_for_this_child = has_name(L"CHATHUB_MYSQL_PASSWORD") ||
                                                  has_name(L"CHATHUB_MYSQL_REPOSITORY_TEST_PASSWORD") ||
                                                  has_name(L"CHATHUB_MYSQL_E2E_ALICE_TOKEN") ||
                                                  has_name(L"CHATHUB_MYSQL_E2E_BOB_TOKEN");
            if (!is_secret_for_this_child)
            {
                child_environment.insert(child_environment.end(), entry_begin, entry_end + 1);
            }

            entry_begin = entry_end + 1;
        }
        ::FreeEnvironmentStringsW(const_cast<wchar_t *>(inherited_environment));

        if (mysql_password_environment == MySqlPasswordEnvironment::Set)
        {
            constexpr std::wstring_view password_variable = L"CHATHUB_MYSQL_PASSWORD=";
            child_environment.insert(child_environment.end(), password_variable.begin(), password_variable.end());
            child_environment.insert(child_environment.end(), mysql_password.begin(), mysql_password.end());
            child_environment.push_back(L'\0');
        }

        // Windows 环境块必须以额外的空字符结束；空环境则需要两个空字符。
        if (child_environment.empty())
        {
            child_environment.push_back(L'\0');
        }
        child_environment.push_back(L'\0');
        return child_environment;
    }

    std::string readAvailableStandardOutput() const
    {
        if (m_standard_output_read == nullptr)
        {
            return {};
        }

        std::string output;
        DWORD available = 0;
        while (::PeekNamedPipe(m_standard_output_read, nullptr, 0, nullptr, &available, nullptr) && available > 0)
        {
            std::array<char, 512> buffer{};
            DWORD bytes_read = 0;
            const DWORD bytes_to_read = std::min<DWORD>(available, buffer.size());
            if (!::ReadFile(m_standard_output_read, buffer.data(), bytes_to_read, &bytes_read, nullptr) ||
                bytes_read == 0)
            {
                break;
            }
            output.append(buffer.data(), bytes_read);
        }
        return output;
    }

    static std::wstring quote(const std::wstring &value)
    {
        return L"\"" + value + L"\"";
    }

    HANDLE m_process{nullptr};
    HANDLE m_standard_output_read{nullptr};
};

std::uint16_t findAvailablePort()
{
    asio::io_context io_context;
    tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 0));
    return acceptor.local_endpoint().port();
}

std::optional<std::wstring> readRequiredEnvironmentVariable(const wchar_t *const variable_name)
{
    const DWORD length = ::GetEnvironmentVariableW(variable_name, nullptr, 0);
    if (length == 0)
    {
        return std::nullopt;
    }

    std::wstring value(length, L'\0');
    const DWORD characters_written = ::GetEnvironmentVariableW(variable_name, value.data(), length);
    if (characters_written != length - 1 || characters_written == 0)
    {
        return std::nullopt;
    }
    value.resize(characters_written);
    return value;
}

std::optional<MySqlStartupTestConfig> loadMySqlStartupTestConfig()
{
    auto username = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_REPOSITORY_TEST_USERNAME");
    auto password = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_REPOSITORY_TEST_PASSWORD");
    auto database = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_REPOSITORY_TEST_DATABASE");
    if (!username || !password || !database)
    {
        return std::nullopt;
    }

    return MySqlStartupTestConfig{std::move(*username), std::move(*password), std::move(*database)};
}

// 功能：读取专用于 Schema 失败场景的低权限 MySQL 账号。
// 边界：该账号必须能连接目标数据库，但不能 CREATE TABLE，才能证明失败发生在 initializeSchema() 而非连接阶段。
std::optional<MySqlStartupTestConfig> loadMySqlSchemaFailureTestConfig()
{
    auto username = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_SCHEMA_FAILURE_TEST_USERNAME");
    auto password = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_SCHEMA_FAILURE_TEST_PASSWORD");
    auto database = readRequiredEnvironmentVariable(L"CHATHUB_MYSQL_SCHEMA_FAILURE_TEST_DATABASE");
    if (!username || !password || !database)
    {
        return std::nullopt;
    }

    return MySqlStartupTestConfig{std::move(*username), std::move(*password), std::move(*database)};
}

// 功能：读取只允许 ASCII 的环境变量，供用户名和 Base64URL JWT 安全转换到协议所用 std::string。
std::optional<std::string> readRequiredAsciiEnvironmentVariable(const wchar_t *const variable_name)
{
    const auto wide_value = readRequiredEnvironmentVariable(variable_name);
    if (!wide_value)
    {
        return std::nullopt;
    }

    std::string ascii_value;
    ascii_value.reserve(wide_value->size());
    for (const wchar_t character : *wide_value)
    {
        if (character > 0x7F)
        {
            return std::nullopt;
        }
        ascii_value.push_back(static_cast<char>(character));
    }
    return ascii_value;
}

std::optional<MySqlChatTestConfig> loadMySqlChatTestConfig()
{
    auto mysql = loadMySqlStartupTestConfig();
    auto alice_username = readRequiredAsciiEnvironmentVariable(L"CHATHUB_MYSQL_E2E_ALICE_USERNAME");
    auto alice_token = readRequiredAsciiEnvironmentVariable(L"CHATHUB_MYSQL_E2E_ALICE_TOKEN");
    auto bob_username = readRequiredAsciiEnvironmentVariable(L"CHATHUB_MYSQL_E2E_BOB_USERNAME");
    auto bob_token = readRequiredAsciiEnvironmentVariable(L"CHATHUB_MYSQL_E2E_BOB_TOKEN");
    if (!mysql || !alice_username || !alice_token || !bob_username || !bob_token)
    {
        return std::nullopt;
    }

    return MySqlChatTestConfig{std::move(*mysql), std::move(*alice_username), std::move(*alice_token),
                                std::move(*bob_username), std::move(*bob_token)};
}

// 功能：保留一个本机已绑定但未监听的端口，使 MySQL TCP 连接立即被拒绝。
class ReservedClosedPort
{
  public:
    ReservedClosedPort() : m_socket(m_io_context)
    {
        m_socket.open(tcp::v4());
        m_socket.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    }

    std::uint16_t port() const
    {
        return m_socket.local_endpoint().port();
    }

  private:
    asio::io_context m_io_context;
    tcp::socket m_socket;
};

bool connectBeforeDeadline(tcp::socket &socket, const std::uint16_t port, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const tcp::endpoint endpoint(asio::ip::address_v4::loopback(), port);

    while (std::chrono::steady_clock::now() < deadline)
    {
        std::error_code error;
        socket.connect(endpoint, error);
        if (!error)
        {
            return true;
        }
        socket.close(error);
        socket.open(tcp::v4(), error);
        if (error)
        {
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

bool readExactlyBeforeDeadline(tcp::socket &socket, char *data, const std::size_t size,
                               const std::chrono::steady_clock::time_point deadline)
{
    std::size_t received = 0;
    while (received < size)
    {
        std::error_code error;
        const auto bytes_read = socket.read_some(asio::buffer(data + received, size - received), error);
        received += bytes_read;

        if (!error)
        {
            continue;
        }
        if (error != asio::error::would_block && error != asio::error::try_again)
        {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// 功能：保存从 TCP 字节流完整读取并通过帧头校验的一条 ChatHub 协议消息。
struct ReceivedFrame
{
    protocol::MessageType type;
    std::string body;
};

std::uint16_t readUint16BigEndian(const char *const data)
{
    return (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8U) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(data[1]));
}

std::uint32_t readUint32BigEndian(const char *const data)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

// 功能：在期限内读取并校验一条完整协议帧；超时、断开或非法帧头均返回空值。
std::optional<ReceivedFrame> readFrameWithDeadline(tcp::socket &socket, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, protocol::kFrameHeaderLength> header{};
    if (!readExactlyBeforeDeadline(socket, header.data(), header.size(), deadline))
    {
        return std::nullopt;
    }

    const auto raw_type = static_cast<std::uint8_t>(header[3]);
    const auto body_length = readUint32BigEndian(header.data() + 4);
    if (readUint16BigEndian(header.data()) != protocol::kFrameMagic ||
        static_cast<std::uint8_t>(header[2]) != protocol::kProtocolVersion || !protocol::isKnownMessageType(raw_type) ||
        body_length > protocol::kMaxFrameBodyLength)
    {
        return std::nullopt;
    }

    ReceivedFrame frame{static_cast<protocol::MessageType>(raw_type), std::string(body_length, '\0')};
    if (body_length != 0 && !readExactlyBeforeDeadline(socket, frame.body.data(), frame.body.size(), deadline))
    {
        return std::nullopt;
    }
    return frame;
}

bool sendFrameAndEnableNonBlocking(tcp::socket &socket, const protocol::MessageType type, const std::string_view body)
{
    std::error_code error;
    socket.non_blocking(false, error);
    if (error)
    {
        return false;
    }

    const auto frame = protocol::makeFrame(type, body);
    asio::write(socket, asio::buffer(frame), error);
    if (error)
    {
        return false;
    }

    socket.non_blocking(true, error);
    return !error;
}

std::optional<std::string> findStringField(const boost::json::object &object, const std::string_view name)
{
    const auto *const value = object.if_contains(name);
    if (value == nullptr || !value->is_string())
    {
        return std::nullopt;
    }
    const auto &text = value->as_string();
    return std::string(text.data(), text.size());
}

std::optional<std::int64_t> findNonNegativeInt64Field(const boost::json::object &object, const std::string_view name)
{
    const auto *const value = object.if_contains(name);
    if (value == nullptr)
    {
        return std::nullopt;
    }
    if (value->is_int64())
    {
        const auto number = value->as_int64();
        return number >= 0 ? std::optional<std::int64_t>(number) : std::nullopt;
    }
    if (value->is_uint64() && value->as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return static_cast<std::int64_t>(value->as_uint64());
    }
    return std::nullopt;
}

bool isAuthOk(const std::optional<ReceivedFrame> &frame)
{
    if (!frame || frame->type != protocol::MessageType::auth)
    {
        return false;
    }
    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *const ok = object.if_contains("ok");
        return ok != nullptr && ok->is_bool() && ok->as_bool();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool hasExpectedOnlineUsers(const std::optional<ReceivedFrame> &frame, const std::vector<std::string> &expected_users)
{
    if (!frame || frame->type != protocol::MessageType::online_users)
    {
        return false;
    }
    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *const users_value = object.if_contains("users");
        if (users_value == nullptr || !users_value->is_array())
        {
            return false;
        }

        const auto &users_array = users_value->as_array();
        if (users_array.size() != expected_users.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < expected_users.size(); ++index)
        {
            if (!users_array[index].is_string())
            {
                return false;
            }
            const auto &actual = users_array[index].as_string();
            if (std::string_view(actual.data(), actual.size()) != expected_users[index])
            {
                return false;
            }
        }
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool authenticateAndReceiveSnapshot(tcp::socket &socket, const std::string_view token,
                                    const std::vector<std::string> &expected_users)
{
    return sendFrameAndEnableNonBlocking(socket, protocol::MessageType::auth, token) &&
           isAuthOk(readFrameWithDeadline(socket, 2s)) &&
           hasExpectedOnlineUsers(readFrameWithDeadline(socket, 2s), expected_users);
}

// 功能：保存一次已确认 MySQL 写入的最小身份，用于比较重启后读取到的历史记录。
struct PersistedChatExpectation
{
    std::string message_id;
    std::string local_id;
    std::string content;
};

// 功能：从发送方成功 ack 中提取已提交消息的稳定身份；不重复验证收件人转发。
std::optional<PersistedChatExpectation> readAcceptedChatAcknowledgement(const std::optional<ReceivedFrame> &ack_frame,
                                                                         const std::string_view local_id,
                                                                         const std::string_view content)
{
    if (!ack_frame || ack_frame->type != protocol::MessageType::chat_ack)
    {
        return std::nullopt;
    }

    try
    {
        const auto ack = boost::json::parse(ack_frame->body).as_object();
        const auto message_id = findStringField(ack, "message_id");
        const auto ack_local_id = findStringField(ack, "local_id");
        const auto status = findStringField(ack, "status");
        if (!message_id || message_id->empty() || !ack_local_id || *ack_local_id != local_id || !status ||
            *status != "accepted")
        {
            return std::nullopt;
        }
        return PersistedChatExpectation{std::move(*message_id), std::string(local_id), std::string(content)};
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

// 功能：验证 history_query(limit=1) 的单个最终结果正是重启前已经确认写入的消息。
bool hasExpectedSingleHistoryMessage(const std::optional<ReceivedFrame> &frame, const std::string_view request_id,
                                     const PersistedChatExpectation &expected)
{
    if (!frame || frame->type != protocol::MessageType::history_result)
    {
        return false;
    }

    try
    {
        const auto result = boost::json::parse(frame->body).as_object();
        const auto result_request_id = findStringField(result, "request_id");
        const auto *const is_last_chunk = result.if_contains("is_last_chunk");
        const auto *const messages = result.if_contains("messages");
        if (!result_request_id || *result_request_id != request_id || is_last_chunk == nullptr ||
            !is_last_chunk->is_bool() || !is_last_chunk->as_bool() || messages == nullptr || !messages->is_array() ||
            messages->as_array().size() != 1 || !messages->as_array().front().is_object())
        {
            return false;
        }

        const auto &message = messages->as_array().front().as_object();
        const auto message_id = findStringField(message, "message_id");
        const auto local_id = findStringField(message, "local_id");
        const auto content = findStringField(message, "content");
        return message_id && *message_id == expected.message_id && local_id && *local_id == expected.local_id && content &&
               *content == expected.content;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：验证发送方 ack 与接收方转发使用同一条已持久化消息身份，且所有业务字段均来自可信上下文或原请求。
bool hasMatchingAckAndForward(const std::optional<ReceivedFrame> &ack_frame,
                              const std::optional<ReceivedFrame> &forwarded_frame,
                              const MySqlChatTestConfig &config, const std::string_view local_id,
                              const std::string_view content, const std::string_view send_at)
{
    if (!ack_frame || !forwarded_frame || ack_frame->type != protocol::MessageType::chat_ack ||
        forwarded_frame->type != protocol::MessageType::chat)
    {
        return false;
    }

    try
    {
        const auto ack = boost::json::parse(ack_frame->body).as_object();
        const auto forwarded = boost::json::parse(forwarded_frame->body).as_object();
        const auto ack_message_id = findStringField(ack, "message_id");
        const auto ack_local_id = findStringField(ack, "local_id");
        const auto ack_status = findStringField(ack, "status");
        const auto ack_timestamp = findNonNegativeInt64Field(ack, "server_received_at_ms");
        const auto forwarded_message_id = findStringField(forwarded, "message_id");
        const auto forwarded_local_id = findStringField(forwarded, "local_id");
        const auto forwarded_sender = findStringField(forwarded, "from");
        const auto forwarded_recipient = findStringField(forwarded, "to");
        const auto forwarded_content = findStringField(forwarded, "content");
        const auto forwarded_send_at = findStringField(forwarded, "send_at");
        const auto forwarded_timestamp = findNonNegativeInt64Field(forwarded, "server_received_at_ms");

        return ack_message_id && !ack_message_id->empty() && ack_local_id && *ack_local_id == local_id && ack_status &&
               *ack_status == "accepted" && ack_timestamp && forwarded_message_id && *forwarded_message_id == *ack_message_id &&
               forwarded_local_id && *forwarded_local_id == local_id && forwarded_sender &&
               *forwarded_sender == config.alice_username && forwarded_recipient &&
               *forwarded_recipient == config.bob_username && forwarded_content && *forwarded_content == content &&
               forwarded_send_at && *forwarded_send_at == send_at && forwarded_timestamp &&
               *forwarded_timestamp == *ack_timestamp;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool receivesAuthenticationTimeout(tcp::socket &socket, std::chrono::milliseconds &elapsed)
{
    std::error_code error;
    socket.non_blocking(true, error);
    if (error)
    {
        return false;
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + kAuthenticationReadTimeout;
    std::array<char, protocol::kFrameHeaderLength> header{};
    if (!readExactlyBeforeDeadline(socket, header.data(), header.size(), deadline))
    {
        return false;
    }

    const std::uint32_t body_size = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[4])) << 24U) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(header[5])) << 16U) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(header[6])) << 8U) |
                                    static_cast<std::uint32_t>(static_cast<unsigned char>(header[7]));
    if (header[3] != static_cast<char>(protocol::MessageType::error) || body_size > protocol::kMaxFrameBodyLength)
    {
        return false;
    }

    std::string body(body_size, '\0');
    if (!readExactlyBeforeDeadline(socket, body.data(), body.size(), deadline))
    {
        return false;
    }

    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at);
    return body.find("\"scope\":\"auth\"") != std::string::npos &&
           body.find("\"code\":\"authentication_timeout\"") != std::string::npos;
}

bool canBindPort(const std::uint16_t port)
{
    try
    {
        asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));
        return true;
    }
    catch (const std::system_error &)
    {
        return false;
    }
}

bool testInvalidConfigurationHasNoSideEffects(const std::filesystem::path &server_executable)
{
    ScopedTestDirectory test_directory;
    const auto port = findAvailablePort();
    const auto database_path = test_directory.path() / "must-not-exist.db";
    ServerProcess process;

    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(port), L"--database-path", database_path.filename().wstring(),
                        L"--auth-timeout-ms", L"1"},
                       test_directory.path()))
    {
        return false;
    }

    DWORD exit_code = EXIT_SUCCESS;
    return process.waitForExit(kProcessStartupTimeout, exit_code) && exit_code != EXIT_SUCCESS &&
           !std::filesystem::exists(database_path) && canBindPort(port);
}

bool testCustomConfigurationReachesRealServer(const std::filesystem::path &server_executable)
{
    ScopedTestDirectory test_directory;
    const auto port = findAvailablePort();
    const auto database_path = test_directory.path() / "custom-runtime.db";
    ServerProcess process;

    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(port), L"--database-path", database_path.filename().wstring(),
                        L"--auth-timeout-ms", L"1000"},
                       test_directory.path()))
    {
        return false;
    }

    if (!process.waitsForStandardOutput(
            {"server_started", "database_path=\"custom-runtime.db\"", "auth_timeout_ms=1000"}, kProcessStartupTimeout))
    {
        return false;
    }

    asio::io_context client_io;
    tcp::socket client_socket(client_io);
    if (!connectBeforeDeadline(client_socket, port, kProcessStartupTimeout))
    {
        return false;
    }

    std::chrono::milliseconds timeout_elapsed{0};
    return std::filesystem::exists(database_path) && receivesAuthenticationTimeout(client_socket, timeout_elapsed) &&
           timeout_elapsed >= 800ms && timeout_elapsed <= 2500ms;
}

// 进程级启动失败测试：显式 MySQL 缺少密码时，Factory 必须在监听端口前失败。
bool testMissingMySqlPasswordExitsWithoutListener(const std::filesystem::path &server_executable)
{
    ScopedTestDirectory test_directory;
    const auto port = findAvailablePort();
    ServerProcess process;

    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(port), L"--storage-backend", L"mysql", L"--mysql-username",
                        L"chathub", L"--mysql-database", L"chathub_test"},
                       test_directory.path(), MySqlPasswordEnvironment::Remove))
    {
        return false;
    }

    DWORD exit_code = EXIT_SUCCESS;
    return process.waitsForStandardOutput({"configuration_error: missing_mysql_password"}, kProcessStartupTimeout) &&
           process.waitForExit(kProcessStartupTimeout, exit_code) && exit_code != EXIT_SUCCESS && canBindPort(port);
}

// 进程级启动失败测试：有密码但 MySQL TCP 连接失败时，也不能启动监听器。
bool testMySqlConnectionFailureExitsWithoutListener(const std::filesystem::path &server_executable)
{
    ScopedTestDirectory test_directory;
    const auto server_port = findAvailablePort();
    std::optional<ReservedClosedPort> mysql_port;
    try
    {
        mysql_port.emplace();
    }
    catch (const std::system_error &)
    {
        return false;
    }

    ServerProcess process;
    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", std::to_wstring(mysql_port->port()), L"--mysql-username",
                        L"chathub", L"--mysql-database", L"chathub_test"},
                       test_directory.path(), MySqlPasswordEnvironment::Set, L"test-only-child-password"))
    {
        return false;
    }

    DWORD exit_code = EXIT_SUCCESS;
    return process.waitsForStandardOutput({"repository_startup_error: mysql_connection_failed"},
                                          kProcessStartupTimeout) &&
           process.waitForExit(kProcessStartupTimeout, exit_code) && exit_code != EXIT_SUCCESS && canBindPort(server_port);
}

bool runTest(const char *name, bool passed);

// 进程级真实集成测试：Factory 成功连接并初始化专用 MySQL 后，Server 才允许监听端口。
bool testMySqlStartupReachesRealServer(const std::filesystem::path &server_executable,
                                       const MySqlStartupTestConfig &mysql_config)
{
    ScopedTestDirectory test_directory;
    const auto server_port = findAvailablePort();
    ServerProcess process;

    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", L"3306", L"--mysql-username", mysql_config.username,
                        L"--mysql-database", mysql_config.database},
                       test_directory.path(), MySqlPasswordEnvironment::Set, mysql_config.password))
    {
        return false;
    }

    if (!process.waitsForStandardOutput({"server_started"}, kProcessStartupTimeout))
    {
        return false;
    }

    asio::io_context client_io;
    tcp::socket client_socket(client_io);
    return connectBeforeDeadline(client_socket, server_port, kProcessStartupTimeout);
}

// 进程级真实集成测试：连接已经成功，但低权限账号不能创建 Schema 时，Factory 仍必须在监听前失败。
bool testMySqlSchemaFailureExitsWithoutListener(const std::filesystem::path &server_executable,
                                                const MySqlStartupTestConfig &mysql_config)
{
    ScopedTestDirectory test_directory;
    const auto server_port = findAvailablePort();
    ServerProcess process;
    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", L"3306", L"--mysql-username", mysql_config.username,
                        L"--mysql-database", mysql_config.database},
                       test_directory.path(), MySqlPasswordEnvironment::Set, mysql_config.password))
    {
        return false;
    }

    DWORD exit_code = EXIT_SUCCESS;
    return process.waitsForStandardOutput({"repository_startup_error: mysql_schema_initialization_failed"},
                                          kProcessStartupTimeout) &&
           process.waitForExit(kProcessStartupTimeout, exit_code) && exit_code != EXIT_SUCCESS && canBindPort(server_port);
}

// 进程级真实集成测试：两名由 Auth Service 签发 JWT 的用户通过 MySQL 后端完成一次聊天确认与转发。
bool testMySqlChatReachesAckAndRecipient(const std::filesystem::path &server_executable,
                                         const MySqlChatTestConfig &config)
{
    ScopedTestDirectory test_directory;
    const auto server_port = findAvailablePort();
    ServerProcess process;
    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", L"3306", L"--mysql-username", config.mysql.username,
                        L"--mysql-database", config.mysql.database},
                       test_directory.path(), MySqlPasswordEnvironment::Set, config.mysql.password) ||
        !process.waitsForStandardOutput({"server_started"}, kProcessStartupTimeout))
    {
        return false;
    }

    asio::io_context client_io;
    tcp::socket alice(client_io);
    tcp::socket bob(client_io);
    if (!connectBeforeDeadline(alice, server_port, kProcessStartupTimeout) ||
        !authenticateAndReceiveSnapshot(alice, config.alice_token, {config.alice_username}) ||
        !connectBeforeDeadline(bob, server_port, kProcessStartupTimeout) ||
        !authenticateAndReceiveSnapshot(bob, config.bob_token, {config.alice_username, config.bob_username}) ||
        !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {config.alice_username, config.bob_username}))
    {
        return false;
    }

    // 同一专用数据库会被多次运行；local_id 必须随本次运行变化，避免旧幂等记录只返回 ack 而不再次转发。
    const std::string local_id =
        "mysql-process-e2e-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    constexpr std::string_view content = "mysql-process-e2e-message";
    constexpr std::string_view send_at = "2026-08-24T12:00:00.000Z";
    boost::json::object request;
    request["to"] = config.bob_username;
    request["content"] = content;
    request["local_id"] = local_id;
    request["send_at"] = send_at;
    if (!sendFrameAndEnableNonBlocking(alice, protocol::MessageType::chat, boost::json::serialize(request)))
    {
        return false;
    }

    const auto forwarded = readFrameWithDeadline(bob, 2s);
    const auto ack = readFrameWithDeadline(alice, 2s);
    return hasMatchingAckAndForward(ack, forwarded, config, local_id, content, send_at);
}

// 进程级真实集成测试：第一次进程确认写入后结束，第二个独立进程必须从同一 MySQL 库读回该消息。
bool testMySqlRestartPreservesSingleHistoryMessage(const std::filesystem::path &server_executable,
                                                   const MySqlChatTestConfig &config)
{
    ScopedTestDirectory test_directory;
    const auto server_port = findAvailablePort();
    ServerProcess process;
    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", L"3306", L"--mysql-username", config.mysql.username,
                        L"--mysql-database", config.mysql.database},
                       test_directory.path(), MySqlPasswordEnvironment::Set, config.mysql.password) ||
        !process.waitsForStandardOutput({"server_started"}, kProcessStartupTimeout))
    {
        return false;
    }

    std::optional<PersistedChatExpectation> persisted;
    {
        asio::io_context client_io;
        tcp::socket alice(client_io);
        tcp::socket bob(client_io);
        if (!connectBeforeDeadline(alice, server_port, kProcessStartupTimeout) ||
            !authenticateAndReceiveSnapshot(alice, config.alice_token, {config.alice_username}) ||
            !connectBeforeDeadline(bob, server_port, kProcessStartupTimeout) ||
            !authenticateAndReceiveSnapshot(bob, config.bob_token, {config.alice_username, config.bob_username}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {config.alice_username, config.bob_username}))
        {
            return false;
        }

        const std::string local_id =
            "mysql-process-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        constexpr std::string_view content = "mysql-process-history-message";
        boost::json::object request;
        request["to"] = config.bob_username;
        request["content"] = content;
        request["local_id"] = local_id;
        request["send_at"] = "2026-08-24T12:00:00.000Z";
        if (!sendFrameAndEnableNonBlocking(alice, protocol::MessageType::chat, boost::json::serialize(request)))
        {
            return false;
        }

        // Bob 在线即可使服务端进入写入路径；本测试不重复验证已由 mysql_server_process_chat_test 覆盖的转发内容。
        persisted = readAcceptedChatAcknowledgement(readFrameWithDeadline(alice, 2s), local_id, content);
        if (!persisted)
        {
            return false;
        }
    }

    // 第一个进程与其内存状态已结束；第二个进程只能通过 MySQL 查询历史。
    process.stop();
    if (!process.start(server_executable,
                       {L"--port", std::to_wstring(server_port), L"--storage-backend", L"mysql", L"--mysql-host",
                        L"127.0.0.1", L"--mysql-port", L"3306", L"--mysql-username", config.mysql.username,
                        L"--mysql-database", config.mysql.database},
                       test_directory.path(), MySqlPasswordEnvironment::Set, config.mysql.password) ||
        !process.waitsForStandardOutput({"server_started"}, kProcessStartupTimeout))
    {
        return false;
    }

    asio::io_context restarted_client_io;
    tcp::socket restarted_alice(restarted_client_io);
    if (!connectBeforeDeadline(restarted_alice, server_port, kProcessStartupTimeout) ||
        !authenticateAndReceiveSnapshot(restarted_alice, config.alice_token, {config.alice_username}))
    {
        return false;
    }

    const std::string request_id =
        "mysql-process-history-query-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    boost::json::object query;
    query["request_id"] = request_id;
    query["limit"] = 1;
    if (!sendFrameAndEnableNonBlocking(restarted_alice, protocol::MessageType::history_query,
                                       boost::json::serialize(query)))
    {
        return false;
    }

    return hasExpectedSingleHistoryMessage(readFrameWithDeadline(restarted_alice, 2s), request_id, *persisted);
}

int runMySqlStartupTest(const std::filesystem::path &server_executable)
{
    const auto mysql_config = loadMySqlStartupTestConfig();
    if (!mysql_config)
    {
        std::cerr << "SKIP [mysql-server-process-startup]: missing MySQL startup test configuration\n";
        return 77;
    }

    return runTest("当真实 MySQL 启动配置完整时，ChatServer 应开始监听",
                   testMySqlStartupReachesRealServer(server_executable, *mysql_config))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

int runMySqlChatTest(const std::filesystem::path &server_executable)
{
    const auto config = loadMySqlChatTestConfig();
    if (!config)
    {
        std::cerr << "SKIP [mysql-server-process-chat]: missing MySQL or JWT test configuration\n";
        return 77;
    }

    return runTest("当真实 MySQL 保存聊天消息时，发送方应收到确认且接收方应收到消息",
                   testMySqlChatReachesAckAndRecipient(server_executable, *config))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

int runMySqlHistoryTest(const std::filesystem::path &server_executable)
{
    const auto config = loadMySqlChatTestConfig();
    if (!config)
    {
        std::cerr << "SKIP [mysql-server-process-history]: missing MySQL or JWT test configuration\n";
        return 77;
    }

    return runTest("当 ChatServer 重启后，真实 MySQL 历史消息应保持不变",
                   testMySqlRestartPreservesSingleHistoryMessage(server_executable, *config))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

int runMySqlSchemaFailureTest(const std::filesystem::path &server_executable)
{
    const auto mysql_config = loadMySqlSchemaFailureTestConfig();
    if (!mysql_config)
    {
        std::cerr << "SKIP [mysql-server-process-schema-failure]: missing MySQL schema-failure test configuration\n";
        return 77;
    }

    return runTest("当真实 MySQL schema 初始化失败时，ChatServer 应退出且不监听",
                   testMySqlSchemaFailureExitsWithoutListener(server_executable, *mysql_config))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

bool runTest(const char *name, const bool passed)
{
    std::cout << (passed ? "PASS: " : "FAIL: ") << name << '\n';
    return passed;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc != 2 && argc != 3)
    {
        std::cerr << "server_process_config_test requires the chat-server executable path and optional MySQL test mode\n";
        return EXIT_FAILURE;
    }

    // ChatServer 现在把 Auth introspection 配置作为启动必需依赖；
    // 这些仅用于进程测试的非真实凭证通过父环境传给每个子进程，
    // 测试仍然只验证启动/监听边界，不发起真实 introspection 请求。
    if (_putenv_s("CHATHUB_AUTH_INTROSPECTION_URL",
                  "http://127.0.0.1:3000/internal/auth/introspect") != 0 ||
        _putenv_s("CHATHUB_AUTH_INTERNAL_SERVICE_KEY",
                  "server-process-test-internal-key") != 0)
    {
        std::cerr << "failed to set Auth introspection test environment\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path server_executable = std::filesystem::absolute(argv[1]);
    if (argc == 3)
    {
        const std::string_view mode(argv[2]);
        if (mode == "--mysql-startup")
        {
            return runMySqlStartupTest(server_executable);
        }
        if (mode == "--mysql-chat")
        {
            return runMySqlChatTest(server_executable);
        }
        if (mode == "--mysql-history")
        {
            return runMySqlHistoryTest(server_executable);
        }
        if (mode == "--mysql-schema-failure")
        {
            return runMySqlSchemaFailureTest(server_executable);
        }
        else
        {
            std::cerr << "unknown test mode\n";
            return EXIT_FAILURE;
        }
    }

    const bool invalid_config_passed = runTest("当配置非法时，ChatServer 应退出且不监听或创建数据库",
                                               testInvalidConfigurationHasNoSideEffects(server_executable));
    const bool custom_config_passed =
        runTest("当端口、数据库路径和认证超时可配置时，ChatServer 应按配置启动",
                testCustomConfigurationReachesRealServer(server_executable));
    const bool missing_mysql_password_passed =
        runTest("当 MySQL 密码缺失时，ChatServer 应退出且不监听", testMissingMySqlPasswordExitsWithoutListener(server_executable));
    const bool mysql_connection_failure_passed =
        runTest("当 MySQL 连接失败时，ChatServer 应退出且不监听", testMySqlConnectionFailureExitsWithoutListener(server_executable));
    return invalid_config_passed && custom_config_passed && missing_mysql_password_passed && mysql_connection_failure_passed
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
