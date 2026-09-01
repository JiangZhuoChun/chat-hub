#include "auth_introspection_config.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using app::AuthIntrospectionConfigError;
using app::AuthIntrospectionConfigResult;

constexpr std::string_view kValidUrl =
    "http://127.0.0.1:3000/internal/auth/introspect";
constexpr std::string_view kValidKey = "test-internal-key";

bool hasError(const AuthIntrospectionConfigResult &result,
              AuthIntrospectionConfigError expected) {
  return !result.config.has_value() && result.error == expected;
}

bool testValidConfigAndDefaults() {
  const auto explicit_result = app::parseAuthIntrospectionConfig(
      kValidUrl, kValidKey, "1500");
  if (!explicit_result.config.has_value() ||
      explicit_result.error != AuthIntrospectionConfigError::none) {
    return false;
  }

  const auto &explicit_config = *explicit_result.config;
  if (explicit_config.host != "127.0.0.1" || explicit_config.port != "3000" ||
      explicit_config.target != "/internal/auth/introspect" ||
      explicit_config.internal_service_key != kValidKey ||
      explicit_config.timeout != std::chrono::milliseconds{1500} ||
      explicit_config.max_response_body_bytes != 4096) {
    return false;
  }

  const auto default_result =
      app::parseAuthIntrospectionConfig("http://localhost", kValidKey, {});
  return default_result.config.has_value() &&
         default_result.config->port == "3000" &&
         default_result.config->target == "/internal/auth/introspect" &&
         default_result.config->timeout == std::chrono::milliseconds{2000};
}

bool testMissingAndInvalidValues() {
  if (!hasError(app::parseAuthIntrospectionConfig("", kValidKey, {}),
                AuthIntrospectionConfigError::missing_url) ||
      !hasError(app::parseAuthIntrospectionConfig("   ", kValidKey, {}),
                AuthIntrospectionConfigError::missing_url) ||
      !hasError(app::parseAuthIntrospectionConfig(kValidUrl, "", {}),
                AuthIntrospectionConfigError::missing_internal_service_key) ||
      !hasError(app::parseAuthIntrospectionConfig(kValidUrl, "key\nvalue", {}),
                AuthIntrospectionConfigError::invalid_internal_service_key)) {
    return false;
  }

  const std::string_view invalid_urls[] = {
      "https://127.0.0.1:3000/introspect",
      "http://127.0.0.1:0/introspect",
      "http://127.0.0.1:65536/introspect",
      "http://127.0.0.1:abc/introspect",
      "http://127.0.0.1:3000/introspect?debug=1",
      "http://127.0.0.1:3000/introspect#fragment",
      "http://user@127.0.0.1:3000/introspect",
      "http://127.0.0.1:3000:1/introspect",
      "http://127.0.0.1:3000/introspect path"};
  for (const auto url : invalid_urls) {
    if (!hasError(app::parseAuthIntrospectionConfig(url, kValidKey, {}),
                  AuthIntrospectionConfigError::invalid_url)) {
      return false;
    }
  }

  const std::string_view invalid_timeouts[] = {"99", "5001", "1000ms",
                                                "+1000"};
  for (const auto timeout : invalid_timeouts) {
    if (!hasError(app::parseAuthIntrospectionConfig(kValidUrl, kValidKey,
                                                    timeout),
                  AuthIntrospectionConfigError::invalid_timeout)) {
      return false;
    }
  }
  return true;
}

bool testStableErrorCodeMapping() {
  return app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::none)
             .empty() &&
         app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::missing_url) ==
             "missing_auth_introspection_url" &&
         app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::invalid_url) ==
             "invalid_auth_introspection_url" &&
         app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::missing_internal_service_key) ==
             "missing_auth_internal_service_key" &&
         app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::invalid_internal_service_key) ==
             "invalid_auth_internal_service_key" &&
         app::authIntrospectionConfigErrorCode(
             AuthIntrospectionConfigError::invalid_timeout) ==
             "invalid_auth_introspection_timeout" &&
         app::authIntrospectionConfigErrorCode(
             static_cast<AuthIntrospectionConfigError>(999)) == "unknown";
}

bool testEnvironmentLoader() {
  // 环境变量读取层只负责借用文本，最终仍由同一个解析器校验并复制配置。
  if (_putenv_s("CHATHUB_AUTH_INTROSPECTION_URL",
                "http://127.0.0.1:3100/introspect") != 0 ||
      _putenv_s("CHATHUB_AUTH_INTERNAL_SERVICE_KEY",
                "environment-test-key") != 0 ||
      _putenv_s("CHATHUB_AUTH_INTROSPECTION_TIMEOUT_MS", "1200") != 0) {
    return false;
  }

  const auto loaded = app::loadAuthIntrospectionConfigFromEnvironment();
  return loaded.config.has_value() &&
         loaded.config->host == "127.0.0.1" && loaded.config->port == "3100" &&
         loaded.config->target == "/introspect" &&
         loaded.config->internal_service_key == "environment-test-key" &&
         loaded.config->timeout == std::chrono::milliseconds{1200};
}

bool runTest(const char *name, bool passed) {
  if (passed) {
    std::cout << "PASS: " << name << '\n';
    return true;
  }
  std::cerr << "FAIL: " << name << '\n';
  return false;
}

}  // namespace

int main() {
  const bool valid_passed =
      runTest("当 introspection 配置合法或省略可选项时，解析应返回规范化配置",
              testValidConfigAndDefaults());
  const bool invalid_passed =
      runTest("当 introspection URL、密钥或超时非法时，配置解析应拒绝启动",
              testMissingAndInvalidValues());
  const bool mapping_passed =
      runTest("当 introspection 配置解析失败时，错误码映射应保持稳定",
              testStableErrorCodeMapping());
  const bool environment_passed =
      runTest("当环境变量提供 introspection 配置时，加载器应返回已校验副本",
              testEnvironmentLoader());
  return valid_passed && invalid_passed && mapping_passed && environment_passed
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
