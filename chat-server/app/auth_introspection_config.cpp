#include "auth_introspection_config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <utility>
#include <cstdint>
#include <cstdlib>
namespace
{
constexpr std::string_view HTTP_PREFIX = "http://";
constexpr std::string_view DEFAULT_PORT = "3000";
constexpr std::string_view DEFAULT_TARGET ="/internal/auth/introspect";
constexpr std::uint64_t DEFAULT_TIMEOUT_MS = 2000;
constexpr std::uint64_t MIN_TIMEOUT_MS = 100;
constexpr std::uint64_t MAX_TIMEOUT_MS = 5000;
constexpr std::size_t MAX_URL_LENGTH = 512;

constexpr auto AUTH_INTROSPECTION_URL_ENV = "CHATHUB_AUTH_INTROSPECTION_URL";
constexpr auto AUTH_INTERNAL_SERVICE_KEY_ENV = "CHATHUB_AUTH_INTERNAL_SERVICE_KEY";
constexpr auto AUTH_INTROSPECTION_TIMEOUT_MS_ENV = "CHATHUB_AUTH_INTROSPECTION_TIMEOUT_MS";

bool isBlank(const std::string_view text)
{
    if (text.empty())
    {
        return true;
    }
    return std::all_of(text.begin(),text.end(),[](const char c) {
        return std::isspace(static_cast<unsigned char>(c));
    }) != 0;
}
bool hasWhitespaceOrControl(const std::string_view text)
{
    return std::any_of(text.begin(),text.end(),[](const char c) {
       const auto value =static_cast<unsigned char>(c);
        return std::isspace(value) != 0 || std::iscntrl(value) != 0;
    });
}
bool hasControlCharacter(const std::string_view text)
{
    return std::any_of(text.begin(),text.end(),[](const char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::iscntrl(value) != 0;
    });
}
std::string_view readEnvironmentValue(const char *variable_name)
{
    const auto raw_value = std::getenv(variable_name);
    if (raw_value == nullptr)
    {
        return {};
    }
    return std::string_view{raw_value};
}

app::AuthIntrospectionConfigResult makeError(const app::AuthIntrospectionConfigError error)
{
    return {std::nullopt,error};
}
} // namespace

namespace app
{
AuthIntrospectionConfigResult loadAuthIntrospectionConfigFromEnvironment()
{
    return parseAuthIntrospectionConfig(
        readEnvironmentValue(AUTH_INTROSPECTION_URL_ENV),
        readEnvironmentValue(AUTH_INTERNAL_SERVICE_KEY_ENV),
        readEnvironmentValue(AUTH_INTROSPECTION_TIMEOUT_MS_ENV));
}

AuthIntrospectionConfigResult parseAuthIntrospectionConfig(const std::string_view url_text,
                                                           const std::string_view internal_service_key,
                                                           const std::string_view timeout_text)
{
    // 1. URL 不能为空
    if (isBlank(url_text))
    {
        return makeError(AuthIntrospectionConfigError::missing_url);
    }
    // 2. 内部服务密钥不能为空
    if (isBlank(internal_service_key))
    {
        return makeError(AuthIntrospectionConfigError::missing_internal_service_key);
    }
    if (hasControlCharacter(internal_service_key))
    {
        return makeError(AuthIntrospectionConfigError::invalid_internal_service_key);
    }
    // 3. URL 长度限制
    if (url_text.length() > MAX_URL_LENGTH)
    {
        return makeError(AuthIntrospectionConfigError::invalid_url);
    }
    // 4. 只接受 http://
    //substr()：截取字符串的一部分
    if (url_text.size() < HTTP_PREFIX.size() ||
        url_text.substr(0,HTTP_PREFIX.size()) != HTTP_PREFIX)
    {
        return makeError(AuthIntrospectionConfigError::invalid_url);
    }
    // 去掉 http://
    const std::string_view after_scheme = url_text.substr(HTTP_PREFIX.size());

    //5.分割 authority 和 target
    const std::size_t slash_pos = after_scheme.find('/');
    std::string_view authority;
    std::string_view target_text;
    //没找到返回：std::string_view::npos
    if (slash_pos == std::string_view::npos)
    {
        authority = after_scheme;
        target_text = DEFAULT_TARGET;
    }else
    {
        authority = after_scheme.substr(0,slash_pos);
        target_text = after_scheme.substr(slash_pos);
    }
    // 不接受 userinfo / query / fragment 出现在 authority 中
    if (authority.empty() ||
        authority.find('@') != std::string_view::npos ||
        authority.find('?') != std::string_view::npos ||
        authority.find('#') != std::string_view::npos ||
        target_text.empty() ||
        target_text.front() != '/' ||
        target_text.find('?') != std::string_view::npos ||
        target_text.find('#') != std::string_view::npos ||
        hasWhitespaceOrControl(target_text))
    {
        return makeError(AuthIntrospectionConfigError::invalid_url);
    }
    // 6. 分割 host 和 port
    std::string_view host_text;
    std::string_view port_text;

    if (const std::size_t colon_pos = authority.find(':');
        colon_pos == std::string_view::npos)
    {
        host_text = authority;
        port_text = DEFAULT_PORT;
    }else
    {   // 当前只接受 host:port。
        // 如果有多个 ':'，很可能是未加 [] 的 IPv6，直接拒绝。
        if (authority.find(':',colon_pos + 1) != std::string_view::npos)
        {
            return makeError(AuthIntrospectionConfigError::invalid_url);
        }
        host_text = authority.substr(0,colon_pos);
        port_text = authority.substr(colon_pos + 1);
    }
    if (host_text.empty() || hasWhitespaceOrControl(host_text) ||
        port_text.empty())
    {
        return makeError(AuthIntrospectionConfigError::invalid_url);
    }

    // 7. 解析 port
    std::uint64_t port_value = 0;
    const auto [port_end,port_error] =
        //把字符序列解析成数字
        std::from_chars(port_text.data(),port_text.data() +port_text.size(),port_value);

    if (port_error != std::errc{} ||
        port_end != port_text.data() + port_text.size() ||
       port_value < 1 || port_value > 65535)
    {
        return makeError(AuthIntrospectionConfigError::invalid_url);
    }

    // 8. 解析 timeout
    std::uint64_t timeout_value = DEFAULT_TIMEOUT_MS;
    if (!timeout_text.empty())
    {
        timeout_value = 0;
        const auto [timeout_end,timeout_error] =
            std::from_chars(timeout_text.data(),timeout_text.data() + timeout_text.size(),timeout_value);

        if (timeout_error != std::errc{} ||
            timeout_end != timeout_text.data() + timeout_text.size() ||
            timeout_value < MIN_TIMEOUT_MS ||
            timeout_value > MAX_TIMEOUT_MS)
        {
            return makeError(AuthIntrospectionConfigError::invalid_timeout);
        }
    }

    // 9. 所有校验成功后，才复制到最终配置
    auth::AuthIntrospectionConfig config;
    config.host = std::string(host_text);
    config.port = std::string(port_text);
    config.target = std::string(target_text);
    config.internal_service_key = std::string(internal_service_key);
    config.timeout = std::chrono::milliseconds(timeout_value);
    config.max_response_body_bytes = 4096;
    return {std::move(config),AuthIntrospectionConfigError::none};
}

std::string_view authIntrospectionConfigErrorCode(const AuthIntrospectionConfigError error) noexcept
{
    switch (error)
    {
        case AuthIntrospectionConfigError::none:
            return "";
        case AuthIntrospectionConfigError::invalid_timeout:
            return "invalid_auth_introspection_timeout";
        case AuthIntrospectionConfigError::invalid_url:
            return "invalid_auth_introspection_url";
        case AuthIntrospectionConfigError::missing_internal_service_key:
            return "missing_auth_internal_service_key";
        case AuthIntrospectionConfigError::missing_url:
            return "missing_auth_introspection_url";
        case AuthIntrospectionConfigError::invalid_internal_service_key:
            return "invalid_auth_internal_service_key";
        }
    return "unknown";
}
} // namespace app