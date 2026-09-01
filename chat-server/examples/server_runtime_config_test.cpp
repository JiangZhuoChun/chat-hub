#include "server_runtime_config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

// 功能：使用拥有参数文本的 vector 构造 argc/argv，避免测试传入悬空 char*。
app::ParseRuntimeConfigResult parseArguments(std::vector<std::string> arguments)
{
    std::vector<char *> argv;
    argv.reserve(arguments.size());
    for (auto &argument : arguments)
    {
        argv.push_back(argument.data());
    }
    return app::parseServerRuntimeConfig(static_cast<int>(argv.size()), argv.data());
}

// 功能：集中确认失败结果不携带半成品配置，并保持预期的内部错误枚举。
bool hasError(const app::ParseRuntimeConfigResult &result, const app::ServerRuntimeConfigError expected_error)
{
    return !result.config.has_value() && result.error == expected_error;
}

bool testDefaults()
{
    // 测试类型：默认配置回归。验证旧启动命令仍选择 SQLite，并保留 MySQL 非秘密默认值。
    const auto result = parseArguments({"chat-server"});
    return result.config.has_value() && result.error == app::ServerRuntimeConfigError::none &&
           result.config->port == 9000 && result.config->database_path == "chathub.db" &&
           result.config->authentication_timeout.count() == 5000 &&
           result.config->storage_backend == app::StorageBackend::Sqlite &&
           result.config->mysql.host == "127.0.0.1" && result.config->mysql.port == 3306 &&
           result.config->mysql.username.empty() && result.config->mysql.database.empty();
}

bool testValidOverrides()
{
    // 测试类型：SQLite 覆盖配置。验证已有 SQLite 参数仍可独立覆盖默认值。
    const auto result = parseArguments(
        {"chat-server", "--port", "9001", "--database-path", "test-data.db", "--auth-timeout-ms", "1200"});
    return result.config.has_value() && result.error == app::ServerRuntimeConfigError::none &&
           result.config->port == 9001 && result.config->database_path == "test-data.db" &&
           result.config->authentication_timeout.count() == 1200;
}

bool testValidMySqlConfig()
{
    // 测试类型：MySQL 合法配置。MySQL 参数故意在后端选择前出现，验证解析不依赖参数顺序。
    const auto result = parseArguments({"chat-server", "--mysql-username", "chathub", "--mysql-database",
                                        "chathub_test", "--storage-backend", "mysql", "--mysql-host",
                                        "192.0.2.10", "--mysql-port", "1"});
    return result.config.has_value() && result.error == app::ServerRuntimeConfigError::none &&
           result.config->storage_backend == app::StorageBackend::MySql &&
           result.config->mysql.host == "192.0.2.10" && result.config->mysql.port == 1 &&
           result.config->mysql.username == "chathub" && result.config->mysql.database == "chathub_test";
}

bool testInvalidArguments()
{
    // 测试类型：通用命令行错误。验证既有未知项、重复项、缺值与数值边界错误保持原语义。
    return hasError(parseArguments({"chat-server", "--unknown", "value"}),
                    app::ServerRuntimeConfigError::unknownOption) &&
           hasError(parseArguments({"chat-server", "--max-online-users", "88"}),
                    app::ServerRuntimeConfigError::unknownOption) &&
           hasError(parseArguments({"chat-server", "--port", "9000", "--port", "9001"}),
                    app::ServerRuntimeConfigError::duplicateOption) &&
           hasError(parseArguments({"chat-server", "--port"}), app::ServerRuntimeConfigError::missingOptionValue) &&
           hasError(parseArguments({"chat-server", "--database-path"}),
                    app::ServerRuntimeConfigError::missingOptionValue) &&
           hasError(parseArguments({"chat-server", "--auth-timeout-ms"}),
                    app::ServerRuntimeConfigError::missingOptionValue) &&
           hasError(parseArguments({"chat-server", "--port", "--auth-timeout-ms", "5000"}),
                    app::ServerRuntimeConfigError::missingOptionValue) &&
           hasError(parseArguments({"chat-server", "--database-path", "--port", "9000"}),
                    app::ServerRuntimeConfigError::missingOptionValue) &&
           hasError(parseArguments({"chat-server", "--port", "0"}), app::ServerRuntimeConfigError::invalidPort) &&
           hasError(parseArguments({"chat-server", "--port", "65536"}), app::ServerRuntimeConfigError::invalidPort) &&
           hasError(parseArguments({"chat-server", "--port", "9000abc"}), app::ServerRuntimeConfigError::invalidPort) &&
           hasError(parseArguments({"chat-server", "--database-path", ""}),
                    app::ServerRuntimeConfigError::invalidDatabasePath) &&
           hasError(parseArguments({"chat-server", "--auth-timeout-ms", "999"}),
                    app::ServerRuntimeConfigError::invalidAuthTimeout) &&
           hasError(parseArguments({"chat-server", "--auth-timeout-ms", "30001"}),
                    app::ServerRuntimeConfigError::invalidAuthTimeout) &&
           hasError(parseArguments({"chat-server", "--auth-timeout-ms", "1200ms"}),
                    app::ServerRuntimeConfigError::invalidAuthTimeout);
}

bool testInvalidMySqlArguments()
{
    // 测试类型：MySQL 参数及组合错误。验证 MySQL 必填项、端口边界和 SQLite/MySQL 参数互斥。
    return hasError(parseArguments({"chat-server", "--storage-backend", "postgres"}),
                    app::ServerRuntimeConfigError::invalidStorageBackend) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-database",
                                    "chathub_test"}),
                    app::ServerRuntimeConfigError::invalidMySqlUsername) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-username", "chathub"}),
                    app::ServerRuntimeConfigError::invalidMySqlDatabase) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-host", "",
                                    "--mysql-username", "chathub", "--mysql-database", "chathub_test"}),
                    app::ServerRuntimeConfigError::invalidMySqlHost) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "sqlite", "--mysql-host", "127.0.0.1"}),
                    app::ServerRuntimeConfigError::incompatibleStorageOptions) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-username", "chathub",
                                    "--mysql-database", "chathub_test", "--database-path", "local.db"}),
                    app::ServerRuntimeConfigError::incompatibleStorageOptions) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-username", "chathub",
                                    "--mysql-database", "chathub_test", "--mysql-port", "0"}),
                    app::ServerRuntimeConfigError::invalidMySqlPort) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-username", "chathub",
                                    "--mysql-database", "chathub_test", "--mysql-port", "65536"}),
                    app::ServerRuntimeConfigError::invalidMySqlPort) &&
           hasError(parseArguments({"chat-server", "--storage-backend", "mysql", "--mysql-username", "chathub",
                                    "--mysql-database", "chathub_test", "--mysql-port", "abc"}),
                    app::ServerRuntimeConfigError::invalidMySqlPort);
}

bool testStableErrorCodeMapping()
{
    // 测试类型：外部错误码合同。验证新增枚举不会因重构而改变 main() 可观察的错误文本。
    return app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::none).empty() &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::missingOptionValue) ==
               "missing_option_value" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::duplicateOption) == "duplicate_option" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::unknownOption) == "unknown_option" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidPort) == "invalid_port" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidDatabasePath) ==
               "invalid_database_path" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidAuthTimeout) ==
               "invalid_auth_timeout" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidStorageBackend) ==
               "invalid_storage_backend" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidMySqlHost) ==
               "invalid_mysql_host" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidMySqlPort) ==
               "invalid_mysql_port" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidMySqlUsername) ==
               "invalid_mysql_username" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::invalidMySqlDatabase) ==
               "invalid_mysql_database" &&
           app::serverRuntimeConfigErrorCode(app::ServerRuntimeConfigError::incompatibleStorageOptions) ==
               "incompatible_storage_options" &&
           app::serverRuntimeConfigErrorCode(static_cast<app::ServerRuntimeConfigError>(999)) ==
               "invalid_runtime_config_error";
}

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
    const bool defaults_passed = runTest("runtime config defaults", testDefaults());
    const bool overrides_passed = runTest("runtime config valid overrides", testValidOverrides());
    const bool mysql_config_passed = runTest("runtime config MySQL options", testValidMySqlConfig());
    const bool invalid_arguments_passed = runTest("runtime config invalid arguments", testInvalidArguments());
    const bool invalid_mysql_arguments_passed =
        runTest("runtime config invalid MySQL arguments", testInvalidMySqlArguments());
    const bool error_code_mapping_passed =
        runTest("runtime config stable error code mapping", testStableErrorCodeMapping());
    return defaults_passed && overrides_passed && mysql_config_passed && invalid_arguments_passed &&
                   invalid_mysql_arguments_passed && error_code_mapping_passed
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
