
#include "net/server.h"
#include "protocol/chat_protocol.h"
#include "protocol/frame_decoder.h"
#include "repository/sqlite_message_repository.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <asio.hpp>
#include <boost/json.hpp>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using asio::ip::tcp;
using namespace std::chrono_literals;

// 功能：让既有的 Server/Session 集成场景使用确定的认证依赖，
//       将 HTTP introspection 客户端本身的测试与在线用户业务测试分离。
class TestIntrospectionRequest final : public auth::IAuthIntrospectionRequest
{
  public:
    explicit TestIntrospectionRequest(std::shared_ptr<std::atomic_bool> cancellation_observed)
        : m_cancellation_observed(std::move(cancellation_observed))
    {
    }

    void cancel() override
    {
        m_cancelled.store(true);
        if (m_cancellation_observed)
        {
            m_cancellation_observed->store(true);
        }
    }

    bool cancelled() const
    {
        return m_cancelled.load();
    }

  private:
    std::atomic_bool m_cancelled{false};
    std::shared_ptr<std::atomic_bool> m_cancellation_observed;
};

class TestIntrospectionClient final : public auth::IAuthIntrospectionClient
{
  public:
    explicit TestIntrospectionClient(asio::io_context &io_context,
                                     std::optional<auth::IntrospectionResult> forced_result = std::nullopt,
                                     const bool respond = true,
                                     std::shared_ptr<std::atomic_bool> cancellation_observed = nullptr,
                                     std::shared_ptr<std::atomic_bool> request_started = nullptr)
        : m_io_context(io_context),
          m_forced_result(std::move(forced_result)),
          m_respond(respond),
          m_cancellation_observed(std::move(cancellation_observed)),
          m_request_started(std::move(request_started))
    {
    }

    auth::IntrospectionRequestPtr introspect(std::string token,
                                             auth::IntrospectionHandler handler) override
    {
        if (m_request_started)
        {
            m_request_started->store(true);
        }
        const auto request = std::make_shared<TestIntrospectionRequest>(m_cancellation_observed);
        if (!m_respond)
        {
            return request;
        }

        asio::post(m_io_context,
                   [request, forced_result = m_forced_result, token = std::move(token),
                    handler = std::move(handler)]() mutable {
                       if (request->cancelled())
                       {
                           return;
                       }

                       if (forced_result.has_value())
                       {
                           handler(*forced_result);
                           return;
                       }

                       try
                       {
                           const auto decoded = jwt::decode(token);
                           const auto username = decoded.get_payload_claim("username").as_string();
                           handler({auth::IntrospectionStatus::active, username, {}});
                       }
                       catch (const std::exception &)
                       {
                           handler({auth::IntrospectionStatus::dependency_unavailable,
                                    {},
                                    "test_introspection_decode_failed"});
                       }
                   });
        return request;
    }

  private:
    asio::io_context &m_io_context;
    std::optional<auth::IntrospectionResult> m_forced_result;
    bool m_respond{true};
    std::shared_ptr<std::atomic_bool> m_cancellation_observed;
    std::shared_ptr<std::atomic_bool> m_request_started;
};

auth::AuthIntrospectionConfig makeTestAuthIntrospectionConfig()
{
    auth::AuthIntrospectionConfig config;
    config.host = "127.0.0.1";
    config.port = "1";
    config.target = "/internal/auth/introspect";
    config.internal_service_key = "test-internal-key";
    config.timeout = 100ms;
    config.max_response_body_bytes = 4096;
    return config;
}

// 功能：与 Session 的开发环境验证密钥一致地生成只包含测试用户名的 HS256 JWT。
std::string makeJwt(const std::string &username)
{
    return jwt::create()
        .set_payload_claim("username", jwt::claim(username))
        .sign(jwt::algorithm::hs256{"chathub-dev-secret"});
}

// 功能：保存从 TCP 字节流完整读取并校验过帧头的一条协议消息。
struct ReceivedFrame
{
    protocol::MessageType type;
    std::string body;
};

// 功能：以大端序读取帧头中的 16 位无符号整数。
std::uint16_t readUint16BigEndian(const char *data)
{
    return (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8U) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(data[1]));
}

// 功能：以大端序读取帧头中的 32 位无符号整数。
std::uint32_t readUint32BigEndian(const char *data)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[3]));
}

// 功能：在期限内读取指定字节数；超时、断开或读取错误时返回 false，避免 CTest 永久阻塞。
bool readExactlyWithDeadline(tcp::socket &socket, char *data, const std::size_t size,
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
        // 当前测试目标使用 Windows Socket 库；让出 1ms，避免非阻塞轮询占满 CPU。
        ::Sleep(1);
    }
    return true;
}

// 功能：在固定期限内读取、校验并解包一条完整 ChatHub 协议帧。
std::optional<ReceivedFrame> readFrameWithDeadline(tcp::socket &socket, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, protocol::kFrameHeaderLength> header{};
    if (!readExactlyWithDeadline(socket, header.data(), header.size(), deadline))
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
    if (body_length != 0 && !readExactlyWithDeadline(socket, frame.body.data(), frame.body.size(), deadline))
    {
        return std::nullopt;
    }
    return frame;
}

// 功能：将 online_users 正文严格解析为用户名数组；字段缺失或类型错误时返回空值。
std::optional<std::vector<std::string>> parseOnlineUsers(const std::string &body)
{
    try
    {
        const auto object = boost::json::parse(body).as_object();
        const auto *users_value = object.if_contains("users");
        if (users_value == nullptr || !users_value->is_array())
        {
            return std::nullopt;
        }

        std::vector<std::string> users;
        const auto &users_array = users_value->as_array();
        users.reserve(users_array.size());
        for (const auto &user_value : users_array)
        {
            if (!user_value.is_string())
            {
                return std::nullopt;
            }
            users.emplace_back(user_value.as_string().c_str());
        }
        return users;
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

// 功能：确认 auth 帧表示成功；测试不依赖 JSON 字段输出顺序。
bool isAuthOk(const std::optional<ReceivedFrame> &frame)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::auth)
    {
        return false;
    }

    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *ok = object.if_contains("ok");
        return ok != nullptr && ok->is_bool() && ok->as_bool();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：确认 online_users 帧的完整用户名列表与期望值完全一致。
bool hasExpectedOnlineUsers(const std::optional<ReceivedFrame> &frame, const std::vector<std::string> &expected_users)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::online_users)
    {
        return false;
    }
    const auto users = parseOnlineUsers(frame->body);
    return users.has_value() && *users == expected_users;
}

// 功能：验证容量拒绝使用稳定的 error 协议字段，而不是依赖展示文案。
bool isOnlineUsersCapacityError(const std::optional<ReceivedFrame> &frame)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::error)
    {
        return false;
    }

    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *scope = object.if_contains("scope");
        const auto *code = object.if_contains("code");
        const auto *max_users = object.if_contains("max_users");
        if (scope == nullptr || code == nullptr || max_users == nullptr || !scope->is_string() || !code->is_string())
        {
            return false;
        }

        const bool max_users_matches =
            (max_users->is_int64() &&
             max_users->as_int64() == static_cast<std::int64_t>(protocol::kMaxOnlineUsersSnapshotCount)) ||
            (max_users->is_uint64() &&
             max_users->as_uint64() == static_cast<std::uint64_t>(protocol::kMaxOnlineUsersSnapshotCount));
        return scope->as_string() == "online_users" && code->as_string() == "online_snapshot_capacity_exceeded" &&
               max_users_matches;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：验证认证截止使用 scope=auth 和稳定的 authentication_timeout 错误码。
bool isAuthenticationTimeoutError(const std::optional<ReceivedFrame> &frame)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::error)
    {
        return false;
    }

    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *scope = object.if_contains("scope");
        const auto *code = object.if_contains("code");
        return scope != nullptr && code != nullptr && scope->is_string() && code->is_string() &&
               scope->as_string() == "auth" && code->as_string() == "authentication_timeout";
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：验证认证拒绝帧携带指定的稳定错误码，避免测试依赖展示文案。
bool isAuthenticationErrorWithCode(const std::optional<ReceivedFrame> &frame, const std::string_view expected_code)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::error)
    {
        return false;
    }

    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *scope = object.if_contains("scope");
        const auto *code = object.if_contains("code");
        return scope != nullptr && code != nullptr && scope->is_string() && code->is_string() &&
               scope->as_string() == "auth" && code->as_string() == expected_code;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：验证数据库不可用错误仍保留聊天作用域和发送者的 local_id，供客户端定位失败气泡。
bool isDatabaseUnavailableError(const std::optional<ReceivedFrame> &frame, const std::string &expected_local_id)
{
    if (!frame.has_value() || frame->type != protocol::MessageType::error)
    {
        return false;
    }

    try
    {
        const auto object = boost::json::parse(frame->body).as_object();
        const auto *scope = object.if_contains("scope");
        const auto *code = object.if_contains("code");
        const auto *local_id = object.if_contains("local_id");
        return scope != nullptr && code != nullptr && local_id != nullptr && scope->is_string() && code->is_string() &&
               local_id->is_string() && scope->as_string() == "chat" && code->as_string() == "database_unavailable" &&
               local_id->as_string() == expected_local_id;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 功能：在读取到拒绝 error 后确认服务端确实关闭候选连接。
bool waitForSocketClosed(tcp::socket &socket, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 1> byte{};
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::error_code error;
        const auto bytes_read = socket.read_some(asio::buffer(byte), error);
        if (error == asio::error::eof || error == asio::error::connection_reset ||
            error == asio::error::operation_aborted)
        {
            return true;
        }
        if (!error && bytes_read != 0)
        {
            return false;
        }
        if (error != asio::error::would_block && error != asio::error::try_again)
        {
            return false;
        }
        ::Sleep(1);
    }
    return false;
}

// 功能：生成恰好 20B 的合法用户名，使容量边界不受短用户名干扰。
std::string makeMaxLengthUsername(const std::size_t index)
{
    const std::string index_text = std::to_string(index);
    return "u" + std::string(protocol::kMaxUsernameBytes - 1 - index_text.size(), '0') + index_text;
}

// 功能：临时绑定端口 0 取得本机可用端口，供单次集成测试的 Server 监听。
std::uint16_t findAvailablePort()
{
    asio::io_context probe_io;
    tcp::acceptor probe(probe_io, tcp::endpoint(tcp::v4(), 0));
    return probe.local_endpoint().port();
}

// 功能：在独立临时目录运行测试，隔离 Server 自动创建的 chathub.db。
class ScopedTestDirectory
{
  public:
    ScopedTestDirectory()
        : m_previous_path(std::filesystem::current_path()),
          m_test_path(
              std::filesystem::temp_directory_path() /
              ("chathub-online-users-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(m_test_path);
        std::filesystem::current_path(m_test_path);
    }

    ~ScopedTestDirectory()
    {
        std::error_code error;
        std::filesystem::current_path(m_previous_path, error);
        std::filesystem::remove_all(m_test_path, error);
    }

    ScopedTestDirectory(const ScopedTestDirectory &) = delete;
    ScopedTestDirectory &operator=(const ScopedTestDirectory &) = delete;

    const std::filesystem::path &path() const
    {
        return m_test_path;
    }

  private:
    std::filesystem::path m_previous_path;
    std::filesystem::path m_test_path;
};

// 功能：在 Server 线程启动前接管 cout，并在 Server 停止后读取本用例产生的运行日志。
class ScopedCoutCapture
{
  public:
    ScopedCoutCapture() : m_previous_buffer(std::cout.rdbuf(m_buffer.rdbuf()))
    {
    }

    ~ScopedCoutCapture()
    {
        std::cout.rdbuf(m_previous_buffer);
    }

    ScopedCoutCapture(const ScopedCoutCapture &) = delete;
    ScopedCoutCapture &operator=(const ScopedCoutCapture &) = delete;

    std::string text() const
    {
        return m_buffer.str();
    }

  private:
    std::ostringstream m_buffer;
    std::streambuf *m_previous_buffer;
};

// 功能：从捕获文本中找到一条可按空格拆分的结构化 Session 日志。
bool containsStructuredSessionLog(const std::string &logs, const std::string_view expected_phase,
                                  const std::string_view expected_event, const std::string_view expected_code)
{
    std::istringstream lines(logs);
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream fields(line);
        std::string field;
        std::string session_id;
        std::string phase;
        std::string event;
        std::string code;
        bool malformed = false;

        while (fields >> field)
        {
            const auto separator = field.find('=');
            if (separator == std::string::npos)
            {
                malformed = true;
                break;
            }

            const auto key = field.substr(0, separator);
            const auto value = field.substr(separator + 1);
            if (key == "session_id")
            {
                session_id = value;
            }
            else if (key == "phase")
            {
                phase = value;
            }
            else if (key == "event")
            {
                event = value;
            }
            else if (key == "code")
            {
                code = value;
            }
        }

        if (!malformed && !session_id.empty() && phase == expected_phase && event == expected_event &&
            code == expected_code)
        {
            return true;
        }
    }
    return false;
}

// 功能：启动独立 Server 线程，并在析构时停止 io_context、等待线程退出后销毁 Server。
class ServerFixture
{
  public:
    explicit ServerFixture(const std::string &database_path = "chathub.db",
                           const std::chrono::milliseconds authentication_timeout = 5000ms,
                           std::optional<auth::IntrospectionResult> forced_result = std::nullopt,
                           const bool respond_to_introspection = true,
                           std::shared_ptr<std::atomic_bool> cancellation_observed = nullptr,
                           std::shared_ptr<std::atomic_bool> request_started = nullptr)
        : m_port(findAvailablePort()),
          m_auth_introspection_client(std::make_shared<TestIntrospectionClient>(
              m_server_io, std::move(forced_result), respond_to_introspection,
              std::move(cancellation_observed), std::move(request_started))),
          // 测试夹具显式复用生产默认配置；ScopedTestDirectory 已隔离该数据库文件。
          m_server(std::make_unique<net::Server>(m_server_io, m_port, database_path, authentication_timeout,
                                                  makeTestAuthIntrospectionConfig(),
                                                  repository::createSqliteMessageRepository(database_path),
                                                  m_auth_introspection_client))
    {
        m_server->start();
        m_server_thread = std::thread([this] { m_server_io.run(); });
    }

    ~ServerFixture()
    {
        m_server_io.stop();
        if (m_server_thread.joinable())
        {
            m_server_thread.join();
        }
        m_server.reset();
    }

    ServerFixture(const ServerFixture &) = delete;
    ServerFixture &operator=(const ServerFixture &) = delete;

    std::uint16_t port() const
    {
        return m_port;
    }

  private:
    asio::io_context m_server_io;
    std::uint16_t m_port;
    std::shared_ptr<TestIntrospectionClient> m_auth_introspection_client;
    std::unique_ptr<net::Server> m_server;
    std::thread m_server_thread;
};

// 功能：连接本测试启动的本机 Server；连接失败抛出异常并由用例报告。
tcp::socket connectClient(asio::io_context &client_io, const std::uint16_t port)
{
    tcp::socket socket(client_io);
    socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
    return socket;
}

// 功能：发送认证帧后切换为非阻塞读取模式，供带期限的读帧函数使用。
bool sendAuth(tcp::socket &socket, const std::string &username)
{
    const auto frame = protocol::makeFrame(protocol::MessageType::auth, makeJwt(username));
    std::error_code error;
    asio::write(socket, asio::buffer(frame), error);
    if (error)
    {
        return false;
    }
    socket.non_blocking(true, error);
    return !error;
}

// 功能：发送一帧后恢复非阻塞读取，供认证完成后的心跳与原始客户端场景复用。
bool sendFrameAndEnableNonBlocking(tcp::socket &socket, const protocol::MessageType type, const std::string &body)
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

// 功能：确认候选会话严格先收到 auth.ok，再收到指定的完整在线快照。
bool authenticateAndReceiveSnapshot(tcp::socket &socket, const std::string &username,
                                    const std::vector<std::string> &expected_users)
{
    return sendAuth(socket, username) && isAuthOk(readFrameWithDeadline(socket, 2s)) &&
           hasExpectedOnlineUsers(readFrameWithDeadline(socket, 2s), expected_users);
}

// 功能：验证第二名不同用户认证时，两端收到同一完整快照且候选用户不重复接收快照。
bool testTwoUsersReceiveOneSharedSnapshotInOrder()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;

        auto alice = connectClient(client_io, server.port());
        if (!sendAuth(alice, "alice"))
        {
            return false;
        }
        if (!isAuthOk(readFrameWithDeadline(alice, 2s)) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {"alice"}))
        {
            return false;
        }

        auto bob = connectClient(client_io, server.port());
        if (!sendAuth(bob, "bob"))
        {
            return false;
        }
        if (!isAuthOk(readFrameWithDeadline(bob, 2s)) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(bob, 2s), {"alice", "bob"}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {"alice", "bob"}))
        {
            return false;
        }

        // 候选 Session 已由 completeAuthentication() 收到快照；随后广播不得再投递第二份。
        return !readFrameWithDeadline(bob, 150ms).has_value();
    }
    catch (const std::exception &error)
    {
        std::cerr << "双用户在线快照测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：覆盖 88 人允许、89 人拒绝、断开后重新准入三个容量边界。
bool testCapacityBoundaryAndRecovery()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;
        std::vector<tcp::socket> clients;
        clients.reserve(protocol::kMaxOnlineUsersSnapshotCount);
        std::vector<std::string> expected_users;
        expected_users.reserve(protocol::kMaxOnlineUsersSnapshotCount);

        for (std::size_t index = 0; index < protocol::kMaxOnlineUsersSnapshotCount; ++index)
        {
            const std::string username = makeMaxLengthUsername(index);
            auto candidate = connectClient(client_io, server.port());
            expected_users.push_back(username);

            if (!authenticateAndReceiveSnapshot(candidate, username, expected_users))
            {
                return false;
            }
            for (auto &existing_client : clients)
            {
                if (!hasExpectedOnlineUsers(readFrameWithDeadline(existing_client, 2s), expected_users))
                {
                    return false;
                }
            }
            clients.push_back(std::move(candidate));
        }

        const auto final_88_snapshot = readFrameWithDeadline(clients.front(), 150ms);
        if (final_88_snapshot.has_value())
        {
            return false;
        }

        auto rejected_candidate = connectClient(client_io, server.port());
        if (!sendAuth(rejected_candidate, makeMaxLengthUsername(protocol::kMaxOnlineUsersSnapshotCount)) ||
            !isOnlineUsersCapacityError(readFrameWithDeadline(rejected_candidate, 2s)) ||
            !waitForSocketClosed(rejected_candidate, 2s))
        {
            return false;
        }

        // 关闭第 88 名后，剩余 87 个会话应先收到不含旧用户名的完整快照。
        std::error_code close_error;
        clients.back().close(close_error);
        if (close_error)
        {
            return false;
        }
        clients.pop_back();
        expected_users.pop_back();
        for (auto &existing_client : clients)
        {
            if (!hasExpectedOnlineUsers(readFrameWithDeadline(existing_client, 2s), expected_users))
            {
                return false;
            }
        }

        // 新的第 88 名必须可被准入，且恢复后的快照不保留已断开用户名。
        const std::string recovered_username = makeMaxLengthUsername(protocol::kMaxOnlineUsersSnapshotCount);
        auto recovered_client = connectClient(client_io, server.port());
        expected_users.push_back(recovered_username);
        if (!authenticateAndReceiveSnapshot(recovered_client, recovered_username, expected_users))
        {
            return false;
        }
        for (auto &existing_client : clients)
        {
            if (!hasExpectedOnlineUsers(readFrameWithDeadline(existing_client, 2s), expected_users))
            {
                return false;
            }
        }
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "在线用户容量边界测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：把数据库路径预先创建为目录，验证不可用数据库只拒绝当前聊天请求而不影响已认证会话。
bool testDatabaseUnavailableKeepsAuthenticatedSessionsAlive()
{
    try
    {
        ScopedTestDirectory test_directory;
        const auto unavailable_database_path = test_directory.path() / "database-is-a-directory";
        std::error_code directory_error;
        if (!std::filesystem::create_directory(unavailable_database_path, directory_error) || directory_error)
        {
            return false;
        }

        ServerFixture server(unavailable_database_path.string());
        asio::io_context client_io;
        auto alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(alice, "alice", {"alice"}))
        {
            return false;
        }

        auto bob = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(bob, "bob", {"alice", "bob"}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {"alice", "bob"}))
        {
            return false;
        }

        constexpr std::string_view local_id{"database-unavailable-local-id"};
        boost::json::object request;
        request["to"] = "bob";
        request["content"] = "must not be persisted";
        request["local_id"] = local_id;
        request["send_at"] = "2026-08-20T12:00:00.000Z";
        if (!sendFrameAndEnableNonBlocking(alice, protocol::MessageType::chat, boost::json::serialize(request)) ||
            !isDatabaseUnavailableError(readFrameWithDeadline(alice, 2s), std::string(local_id)))
        {
            return false;
        }

        const auto expect_pong = [](tcp::socket &socket, const std::string &body) {
            return sendFrameAndEnableNonBlocking(socket, protocol::MessageType::ping, body) && [&] {
                const auto pong = readFrameWithDeadline(socket, 2s);
                return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == body;
            }();
        };
        return expect_pong(alice, "alice-still-alive") && expect_pong(bob, "bob-still-alive");
    }
    catch (const std::exception &error)
    {
        std::cerr << "数据库不可用隔离测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：覆盖同名接管后旧会话关闭不应删除新映射的回归路径。
bool testSameUsernameTakeoverKeepsCurrentMapping()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;

        auto old_alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(old_alice, "alice", {"alice"}))
        {
            return false;
        }

        auto current_alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(current_alice, "alice", {"alice"}) || !waitForSocketClosed(old_alice, 2s))
        {
            return false;
        }

        auto bob = connectClient(client_io, server.port());
        const std::vector<std::string> expected_users{"alice", "bob"};
        if (!authenticateAndReceiveSnapshot(bob, "bob", expected_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(current_alice, 2s), expected_users))
        {
            return false;
        }
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "同名接管测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证未认证连接发送 ping 不续期，仍收到认证超时 error 后关闭。
bool testUnauthenticatedPingTimesOut()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;
        auto client = connectClient(client_io, server.port());

        const auto started_at = std::chrono::steady_clock::now();
        if (!sendFrameAndEnableNonBlocking(client, protocol::MessageType::ping, "ping"))
        {
            return false;
        }
        const auto timeout_frame = readFrameWithDeadline(client, 7s);
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        return elapsed >= 4s && isAuthenticationTimeoutError(timeout_frame) && waitForSocketClosed(client, 2s);
    }
    catch (const std::exception &error)
    {
        std::cerr << "未认证心跳超时测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证正常认证取消截止；超过认证期限后连接仍能响应心跳。
bool testAuthenticatedSessionCancelsDeadline()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;
        auto client = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(client, "alice", {"alice"}))
        {
            return false;
        }

        if (readFrameWithDeadline(client, 5500ms).has_value() ||
            !sendFrameAndEnableNonBlocking(client, protocol::MessageType::ping, "alive"))
        {
            return false;
        }
        const auto pong = readFrameWithDeadline(client, 2s);
        return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == "alive";
    }
    catch (const std::exception &error)
    {
        std::cerr << "认证成功取消截止测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证会话错误日志使用可解析字段、记录认证拒绝原因，并且不回显入站 error 正文。
bool testSessionStructuredErrorLogs()
{
    try
    {
        ScopedTestDirectory test_directory;
        std::string logs;
        bool invalid_username_rejected = false;
        bool authentication_timed_out = false;
        bool peer_error_ignored = false;
        constexpr std::string_view peer_error_body{"peer-error-body-must-not-be-logged"};

        {
            ScopedCoutCapture log_capture;
            {
                ServerFixture server("chathub.db", 1000ms);
                asio::io_context client_io;

                auto invalid_username_client = connectClient(client_io, server.port());
                invalid_username_rejected =
                    sendAuth(invalid_username_client, "x") &&
                    isAuthenticationErrorWithCode(readFrameWithDeadline(invalid_username_client, 2s),
                                                  "invalid_username_claim") &&
                    waitForSocketClosed(invalid_username_client, 2s);

                auto timeout_client = connectClient(client_io, server.port());
                authentication_timed_out =
                    sendFrameAndEnableNonBlocking(timeout_client, protocol::MessageType::ping, "probe") &&
                    isAuthenticationTimeoutError(readFrameWithDeadline(timeout_client, 2500ms)) &&
                    waitForSocketClosed(timeout_client, 2s);

                auto authenticated_client = connectClient(client_io, server.port());
                peer_error_ignored =
                    authenticateAndReceiveSnapshot(authenticated_client, "alice", {"alice"}) &&
                    sendFrameAndEnableNonBlocking(authenticated_client, protocol::MessageType::error,
                                                  std::string(peer_error_body)) &&
                    sendFrameAndEnableNonBlocking(authenticated_client, protocol::MessageType::ping, "alive") && [&] {
                        const auto pong = readFrameWithDeadline(authenticated_client, 2s);
                        return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == "alive";
                    }();
            }
            logs = log_capture.text();
        }

        return invalid_username_rejected && authentication_timed_out && peer_error_ignored &&
               containsStructuredSessionLog(logs, "auth", "authentication_rejected", "invalid_username_claim") &&
               containsStructuredSessionLog(logs, "auth", "authentication_rejected", "authentication_timeout") &&
               containsStructuredSessionLog(logs, "dispatch", "peer_error_ignored", "inbound_error") &&
               logs.find(peer_error_body) == std::string::npos;
    }
    catch (const std::exception &error)
    {
        std::cerr << "会话结构化错误日志测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证 Auth introspection 的“确定拒绝”和“依赖故障”都不会进入在线用户表，
//       并且分别使用稳定的 auth 错误码后关闭当前连接。
bool testIntrospectionStatusMapping()
{
    struct StatusCase
    {
        auth::IntrospectionStatus status;
        const char *expected_code;
        const char *diagnostic_code;
    };

    const StatusCase cases[] = {
        {auth::IntrospectionStatus::authentication_rejected,
         "authentication_rejected", ""},
        {auth::IntrospectionStatus::dependency_unavailable,
         "authentication_dependency_unavailable", "stub_unavailable"},
    };

    try
    {
        for (const auto &test_case : cases)
        {
            ScopedTestDirectory test_directory;
            ServerFixture server(
                "chathub.db", 1000ms,
                auth::IntrospectionResult{test_case.status, {}, test_case.diagnostic_code});
            asio::io_context client_io;
            auto client = connectClient(client_io, server.port());

            if (!sendAuth(client, "alice") ||
                !isAuthenticationErrorWithCode(readFrameWithDeadline(client, 2s),
                                               test_case.expected_code) ||
                !waitForSocketClosed(client, 2s))
            {
                return false;
            }
        }
        return true;
    }
    catch (const std::exception &error)
    {
        std::cerr << "introspection 状态映射测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证 Session 在 introspection 尚未返回时关闭，会取消请求句柄，
//       迟到的结果不能再把已关闭会话推进到认证成功。
bool testSessionCloseCancelsPendingIntrospection()
{
    try
    {
        ScopedTestDirectory test_directory;
        const auto request_started = std::make_shared<std::atomic_bool>(false);
        const auto cancellation_observed = std::make_shared<std::atomic_bool>(false);
        ServerFixture server("chathub.db", 5000ms, std::nullopt, false,
                             cancellation_observed, request_started);
        asio::io_context client_io;
        auto client = connectClient(client_io, server.port());
        if (!sendAuth(client, "alice"))
        {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!request_started->load() && std::chrono::steady_clock::now() < deadline)
        {
            ::Sleep(1);
        }
        if (!request_started->load())
        {
            return false;
        }

        std::error_code close_error;
        client.shutdown(tcp::socket::shutdown_both, close_error);
        client.close(close_error);

        while (!cancellation_observed->load() && std::chrono::steady_clock::now() < deadline)
        {
            ::Sleep(1);
        }
        return cancellation_observed->load();
    }
    catch (const std::exception &error)
    {
        std::cerr << "introspection 取消测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证三名已认证用户连续私聊时，chat、chat_ack、delivery_receipt 和心跳均保持各自会话边界。
bool testThreeAccountRelay()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;

        auto alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(alice, "alice", {"alice"}))
        {
            return false;
        }

        auto bob = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(bob, "bob", {"alice", "bob"}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {"alice", "bob"}))
        {
            return false;
        }

        auto carol = connectClient(client_io, server.port());
        const std::vector<std::string> three_users{"alice", "bob", "carol"};
        if (!authenticateAndReceiveSnapshot(carol, "carol", three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(bob, 2s), three_users))
        {
            return false;
        }

        auto parseObject = [](const std::string &body, boost::json::object &object) {
            boost::system::error_code error;
            const auto value = boost::json::parse(body, error);
            if (error || !value.is_object())
            {
                return false;
            }
            object = value.as_object();
            return true;
        };

        auto readStringField = [](const boost::json::object &object, const char *name, std::string &value) {
            const auto *field = object.if_contains(name);
            if (field == nullptr || !field->is_string())
            {
                return false;
            }
            value = field->as_string().c_str();
            return true;
        };

        auto readPositiveTimestamp = [](const boost::json::object &object, const char *name, std::int64_t &timestamp) {
            const auto *field = object.if_contains(name);
            if (field == nullptr)
            {
                return false;
            }
            if (field->is_int64())
            {
                timestamp = field->as_int64();
            }
            else if (field->is_uint64() &&
                     field->as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                timestamp = static_cast<std::int64_t>(field->as_uint64());
            }
            else
            {
                return false;
            }
            return timestamp > 0;
        };

        auto expectJsonFrame = [&](tcp::socket &socket, const protocol::MessageType expected_type,
                                   boost::json::object &object) {
            const auto frame = readFrameWithDeadline(socket, 2s);
            return frame.has_value() && frame->type == expected_type && parseObject(frame->body, object);
        };

        std::vector<std::string> message_ids;
        auto relayAndConfirm = [&](tcp::socket &sender, const std::string &sender_name, tcp::socket &recipient,
                                   const std::string &recipient_name, tcp::socket &observer,
                                   const std::string &local_id, const std::string &content) {
            const std::string send_at{"2026-08-19T12:00:00.000Z"};
            boost::json::object request;
            request["to"] = recipient_name;
            request["content"] = content;
            request["local_id"] = local_id;
            request["send_at"] = send_at;
            if (!sendFrameAndEnableNonBlocking(sender, protocol::MessageType::chat, boost::json::serialize(request)))
            {
                return false;
            }

            boost::json::object forwarded;
            boost::json::object acknowledgement;
            if (!expectJsonFrame(recipient, protocol::MessageType::chat, forwarded) ||
                !expectJsonFrame(sender, protocol::MessageType::chat_ack, acknowledgement))
            {
                return false;
            }

            std::string message_id;
            std::string acknowledgement_local_id;
            std::string acknowledgement_status;
            std::string forwarded_message_id;
            std::string forwarded_local_id;
            std::string forwarded_from;
            std::string forwarded_to;
            std::string forwarded_content;
            std::string forwarded_send_at;
            std::int64_t acknowledgement_received_at = 0;
            std::int64_t forwarded_received_at = 0;
            if (!readStringField(acknowledgement, "message_id", message_id) || message_id.empty() ||
                !readStringField(acknowledgement, "local_id", acknowledgement_local_id) ||
                !readStringField(acknowledgement, "status", acknowledgement_status) ||
                !readPositiveTimestamp(acknowledgement, "server_received_at_ms", acknowledgement_received_at) ||
                !readStringField(forwarded, "message_id", forwarded_message_id) ||
                !readStringField(forwarded, "local_id", forwarded_local_id) ||
                !readStringField(forwarded, "from", forwarded_from) ||
                !readStringField(forwarded, "to", forwarded_to) ||
                !readStringField(forwarded, "content", forwarded_content) ||
                !readStringField(forwarded, "send_at", forwarded_send_at) ||
                !readPositiveTimestamp(forwarded, "server_received_at_ms", forwarded_received_at) ||
                acknowledgement_local_id != local_id || acknowledgement_status != "accepted" ||
                forwarded_message_id != message_id || forwarded_local_id != local_id || forwarded_from != sender_name ||
                forwarded_to != recipient_name || forwarded_content != content || forwarded_send_at != send_at ||
                forwarded_received_at != acknowledgement_received_at)
            {
                return false;
            }

            // 第三名用户不是此消息的接收者，不得收到聊天、确认或其他残留协议帧。
            if (readFrameWithDeadline(observer, 150ms).has_value())
            {
                return false;
            }

            boost::json::object receipt;
            receipt["message_id"] = message_id;
            if (!sendFrameAndEnableNonBlocking(recipient, protocol::MessageType::delivery_receipt,
                                               boost::json::serialize(receipt)))
            {
                return false;
            }

            boost::json::object delivered;
            std::string delivered_local_id;
            std::string delivered_status;
            if (!expectJsonFrame(sender, protocol::MessageType::delivery_receipt, delivered) ||
                !readStringField(delivered, "local_id", delivered_local_id) ||
                !readStringField(delivered, "status", delivered_status) || delivered_local_id != local_id ||
                delivered_status != "delivered")
            {
                return false;
            }

            message_ids.push_back(message_id);
            return true;
        };

        if (!relayAndConfirm(alice, "alice", bob, "bob", carol, "relay-alice-bob", "alice to bob") ||
            !relayAndConfirm(bob, "bob", carol, "carol", alice, "relay-bob-carol", "bob to carol") ||
            !relayAndConfirm(carol, "carol", alice, "alice", bob, "relay-carol-alice", "carol to alice") ||
            message_ids.size() != 3 || message_ids[0] == message_ids[1] || message_ids[0] == message_ids[2] ||
            message_ids[1] == message_ids[2])
        {
            return false;
        }

        auto expectPong = [&](tcp::socket &socket, const std::string &body) {
            return sendFrameAndEnableNonBlocking(socket, protocol::MessageType::ping, body) && [&] {
                const auto pong = readFrameWithDeadline(socket, 2s);
                return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == body;
            }();
        };

        return expectPong(alice, "alice-alive") && expectPong(bob, "bob-alive") && expectPong(carol, "carol-alive");
    }
    catch (const std::exception &error)
    {
        std::cerr << "三账户连续消息测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证三人在线期间同名接管只替换 alice 的会话，旧会话关闭后消息仍路由到新会话。
bool testThreeAccountSameUsernameTakeover()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;
        const std::vector<std::string> expected_users{"alice", "bob", "carol"};

        auto old_alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(old_alice, "alice", {"alice"}))
        {
            return false;
        }

        auto bob = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(bob, "bob", {"alice", "bob"}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(old_alice, 2s), {"alice", "bob"}))
        {
            return false;
        }

        auto carol = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(carol, "carol", expected_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(old_alice, 2s), expected_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(bob, 2s), expected_users))
        {
            return false;
        }

        auto current_alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(current_alice, "alice", expected_users) ||
            !waitForSocketClosed(old_alice, 2s) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(bob, 2s), expected_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(carol, 2s), expected_users) ||
            readFrameWithDeadline(bob, 150ms).has_value() || readFrameWithDeadline(carol, 150ms).has_value())
        {
            return false;
        }

        const std::string local_id{"takeover-bob-alice"};
        boost::json::object request;
        request["to"] = "alice";
        request["content"] = "route to current alice";
        request["local_id"] = local_id;
        request["send_at"] = "2026-08-19T12:00:00.000Z";
        if (!sendFrameAndEnableNonBlocking(bob, protocol::MessageType::chat, boost::json::serialize(request)))
        {
            return false;
        }

        const auto forwarded = readFrameWithDeadline(current_alice, 2s);
        const auto acknowledgement = readFrameWithDeadline(bob, 2s);
        if (!forwarded.has_value() || forwarded->type != protocol::MessageType::chat || !acknowledgement.has_value() ||
            acknowledgement->type != protocol::MessageType::chat_ack)
        {
            return false;
        }

        const auto forwarded_body = boost::json::parse(forwarded->body).as_object();
        const auto acknowledgement_body = boost::json::parse(acknowledgement->body).as_object();
        const auto *forwarded_from = forwarded_body.if_contains("from");
        const auto *forwarded_to = forwarded_body.if_contains("to");
        const auto *forwarded_content = forwarded_body.if_contains("content");
        const auto *forwarded_local_id = forwarded_body.if_contains("local_id");
        const auto *acknowledgement_local_id = acknowledgement_body.if_contains("local_id");
        const auto *acknowledgement_status = acknowledgement_body.if_contains("status");
        const auto *acknowledgement_message_id = acknowledgement_body.if_contains("message_id");
        if (forwarded_from == nullptr || forwarded_to == nullptr || forwarded_content == nullptr ||
            forwarded_local_id == nullptr || acknowledgement_local_id == nullptr || acknowledgement_status == nullptr ||
            acknowledgement_message_id == nullptr || !forwarded_from->is_string() || !forwarded_to->is_string() ||
            !forwarded_content->is_string() || !forwarded_local_id->is_string() ||
            !acknowledgement_local_id->is_string() || !acknowledgement_status->is_string() ||
            !acknowledgement_message_id->is_string() || forwarded_from->as_string() != "bob" ||
            forwarded_to->as_string() != "alice" || forwarded_content->as_string() != "route to current alice" ||
            forwarded_local_id->as_string() != local_id || acknowledgement_local_id->as_string() != local_id ||
            acknowledgement_status->as_string() != "accepted" || acknowledgement_message_id->as_string().empty())
        {
            return false;
        }

        boost::json::object receipt;
        receipt["message_id"] = acknowledgement_message_id->as_string();
        if (!sendFrameAndEnableNonBlocking(current_alice, protocol::MessageType::delivery_receipt,
                                           boost::json::serialize(receipt)))
        {
            return false;
        }
        const auto delivered = readFrameWithDeadline(bob, 2s);
        if (!delivered.has_value() || delivered->type != protocol::MessageType::delivery_receipt)
        {
            return false;
        }
        const auto delivered_body = boost::json::parse(delivered->body).as_object();
        const auto *delivered_local_id = delivered_body.if_contains("local_id");
        const auto *delivered_status = delivered_body.if_contains("status");
        if (delivered_local_id == nullptr || delivered_status == nullptr || !delivered_local_id->is_string() ||
            !delivered_status->is_string() || delivered_local_id->as_string() != local_id ||
            delivered_status->as_string() != "delivered")
        {
            return false;
        }

        auto expectPong = [&](tcp::socket &socket, const std::string &body) {
            if (!sendFrameAndEnableNonBlocking(socket, protocol::MessageType::ping, body))
            {
                return false;
            }
            const auto pong = readFrameWithDeadline(socket, 2s);
            return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == body;
        };
        return expectPong(current_alice, "current-alice-alive") && expectPong(bob, "bob-alive") &&
               expectPong(carol, "carol-alive");
    }
    catch (const std::exception &error)
    {
        std::cerr << "三账户同名接管测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：验证三人在线时 Bob 主动断开并重新认证后，快照、路由与其余会话均恢复正常。
bool testThreeAccountReconnectAfterDisconnect()
{
    try
    {
        ScopedTestDirectory test_directory;
        ServerFixture server;
        asio::io_context client_io;
        const std::vector<std::string> three_users{"alice", "bob", "carol"};
        const std::vector<std::string> users_without_bob{"alice", "carol"};

        auto alice = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(alice, "alice", {"alice"}))
        {
            return false;
        }
        auto old_bob = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(old_bob, "bob", {"alice", "bob"}) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), {"alice", "bob"}))
        {
            return false;
        }
        auto carol = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(carol, "carol", three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(old_bob, 2s), three_users))
        {
            return false;
        }

        std::error_code close_error;
        old_bob.close(close_error);
        if (close_error || !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), users_without_bob) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(carol, 2s), users_without_bob))
        {
            return false;
        }

        auto current_bob = connectClient(client_io, server.port());
        if (!authenticateAndReceiveSnapshot(current_bob, "bob", three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(alice, 2s), three_users) ||
            !hasExpectedOnlineUsers(readFrameWithDeadline(carol, 2s), three_users) ||
            readFrameWithDeadline(alice, 150ms).has_value() || readFrameWithDeadline(carol, 150ms).has_value())
        {
            return false;
        }

        const std::string local_id{"reconnect-alice-bob"};
        boost::json::object request;
        request["to"] = "bob";
        request["content"] = "route to reconnected bob";
        request["local_id"] = local_id;
        request["send_at"] = "2026-08-19T12:00:00.000Z";
        if (!sendFrameAndEnableNonBlocking(alice, protocol::MessageType::chat, boost::json::serialize(request)))
        {
            return false;
        }

        const auto forwarded = readFrameWithDeadline(current_bob, 2s);
        const auto acknowledgement = readFrameWithDeadline(alice, 2s);
        if (!forwarded.has_value() || forwarded->type != protocol::MessageType::chat || !acknowledgement.has_value() ||
            acknowledgement->type != protocol::MessageType::chat_ack)
        {
            return false;
        }

        const auto forwarded_body = boost::json::parse(forwarded->body).as_object();
        const auto acknowledgement_body = boost::json::parse(acknowledgement->body).as_object();
        const auto *forwarded_from = forwarded_body.if_contains("from");
        const auto *forwarded_to = forwarded_body.if_contains("to");
        const auto *forwarded_content = forwarded_body.if_contains("content");
        const auto *forwarded_local_id = forwarded_body.if_contains("local_id");
        const auto *acknowledgement_local_id = acknowledgement_body.if_contains("local_id");
        const auto *acknowledgement_status = acknowledgement_body.if_contains("status");
        const auto *acknowledgement_message_id = acknowledgement_body.if_contains("message_id");
        if (forwarded_from == nullptr || forwarded_to == nullptr || forwarded_content == nullptr ||
            forwarded_local_id == nullptr || acknowledgement_local_id == nullptr || acknowledgement_status == nullptr ||
            acknowledgement_message_id == nullptr || !forwarded_from->is_string() || !forwarded_to->is_string() ||
            !forwarded_content->is_string() || !forwarded_local_id->is_string() ||
            !acknowledgement_local_id->is_string() || !acknowledgement_status->is_string() ||
            !acknowledgement_message_id->is_string() || forwarded_from->as_string() != "alice" ||
            forwarded_to->as_string() != "bob" || forwarded_content->as_string() != "route to reconnected bob" ||
            forwarded_local_id->as_string() != local_id || acknowledgement_local_id->as_string() != local_id ||
            acknowledgement_status->as_string() != "accepted" || acknowledgement_message_id->as_string().empty())
        {
            return false;
        }

        boost::json::object receipt;
        receipt["message_id"] = acknowledgement_message_id->as_string();
        if (!sendFrameAndEnableNonBlocking(current_bob, protocol::MessageType::delivery_receipt,
                                           boost::json::serialize(receipt)))
        {
            return false;
        }
        const auto delivered = readFrameWithDeadline(alice, 2s);
        if (!delivered.has_value() || delivered->type != protocol::MessageType::delivery_receipt)
        {
            return false;
        }
        const auto delivered_body = boost::json::parse(delivered->body).as_object();
        const auto *delivered_local_id = delivered_body.if_contains("local_id");
        const auto *delivered_status = delivered_body.if_contains("status");
        if (delivered_local_id == nullptr || delivered_status == nullptr || !delivered_local_id->is_string() ||
            !delivered_status->is_string() || delivered_local_id->as_string() != local_id ||
            delivered_status->as_string() != "delivered")
        {
            return false;
        }

        auto expectPong = [&](tcp::socket &socket, const std::string &body) {
            if (!sendFrameAndEnableNonBlocking(socket, protocol::MessageType::ping, body))
            {
                return false;
            }
            const auto pong = readFrameWithDeadline(socket, 2s);
            return pong.has_value() && pong->type == protocol::MessageType::pong && pong->body == body;
        };
        return expectPong(alice, "alice-alive") && expectPong(current_bob, "current-bob-alive") &&
               expectPong(carol, "carol-alive");
    }
    catch (const std::exception &error)
    {
        std::cerr << "三账户断开重连测试异常：" << error.what() << '\n';
        return false;
    }
}

// 功能：输出单项测试结论，并聚合为 CTest 可识别的进程退出状态。
bool runTest(const char *name, const bool passed)
{
    if (passed)
    {
        std::cout << "PASS: " << name << '\n';
        return true;
    }

    std::cerr << "FAIL: " << name << '\n';
    return false;
}

} // namespace

int main()
{
    const bool two_users_passed =
        runTest("当两个用户在线时，双方应按顺序收到同一份在线快照", testTwoUsersReceiveOneSharedSnapshotInOrder());
    const bool capacity_passed =
        runTest("当在线快照达到容量边界并恢复时，服务应拒绝超限并恢复发送", testCapacityBoundaryAndRecovery());
    const bool database_unavailable_passed = runTest("当数据库不可用时，已认证会话应保持连接",
                                                     testDatabaseUnavailableKeepsAuthenticatedSessionsAlive());
    const bool takeover_passed =
        runTest("当同一用户名重复登录时，服务应只保留当前会话映射", testSameUsernameTakeoverKeepsCurrentMapping());
    const bool unauthenticated_timeout_passed =
        runTest("当会话未完成认证时，超时应关闭连接", testUnauthenticatedPingTimesOut());
    const bool authenticated_deadline_cancelled =
        runTest("当会话完成认证时，认证截止定时器应被取消", testAuthenticatedSessionCancelsDeadline());
    const bool structured_error_logs = runTest("当会话发生协议错误时，日志应包含结构化字段", testSessionStructuredErrorLogs());
    const bool introspection_status_mapping =
        runTest("当 introspection 返回拒绝或依赖故障时，会话应映射错误并关闭", testIntrospectionStatusMapping());
    const bool introspection_cancelled =
        runTest("当会话关闭时，未完成的 introspection 请求应被取消", testSessionCloseCancelsPendingIntrospection());
    const bool three_account_relay = runTest("当三个账户在线通信时，消息应正确转发", testThreeAccountRelay());
    const bool three_account_takeover =
        runTest("当三个账户出现同名登录时，旧会话应被接管", testThreeAccountSameUsernameTakeover());
    const bool three_account_reconnect =
        runTest("当三个账户断开后重连时，在线映射应恢复", testThreeAccountReconnectAfterDisconnect());
    const bool passed = two_users_passed && capacity_passed && database_unavailable_passed && takeover_passed &&
                        unauthenticated_timeout_passed && authenticated_deadline_cancelled && structured_error_logs &&
                        introspection_status_mapping && introspection_cancelled && three_account_relay &&
                        three_account_takeover && three_account_reconnect;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
