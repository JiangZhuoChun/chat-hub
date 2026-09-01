#pragma once

#include "auth/auth_introspection_client.h"

#include <optional>
#include <string_view>
namespace app
{
enum class AuthIntrospectionConfigError
{
    none,
    missing_url,
    invalid_url,
    missing_internal_service_key,
    invalid_timeout,
    invalid_internal_service_key
};

struct AuthIntrospectionConfigResult
{
    std::optional<auth::AuthIntrospectionConfig> config;
    AuthIntrospectionConfigError error{AuthIntrospectionConfigError::none};
};

AuthIntrospectionConfigResult loadAuthIntrospectionConfigFromEnvironment();

AuthIntrospectionConfigResult parseAuthIntrospectionConfig(std::string_view url_text,
                                                           std::string_view internal_service_key,
                                                           std::string_view timeout_text);

std::string_view authIntrospectionConfigErrorCode(AuthIntrospectionConfigError error) noexcept;
} // namespace app
