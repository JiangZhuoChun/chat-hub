#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
namespace app
{
// 功能：表示启动时选定的消息存储后端；该值只描述选择，不保存连接或密码。
enum class StorageBackend
{
    Sqlite,
    MySql,
};

// 功能：保存 MySQL 的非秘密运行参数；密码由后续组装层单独从环境变量读取。
struct MySqlRuntimeConfig
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string username;
    std::string database;
};

// 功能：枚举配置解析失败的稳定分类，供解析器和外部错误文本映射共同使用。
enum class ServerRuntimeConfigError
{
    none,
    missingOptionValue,
    duplicateOption,
    unknownOption,
    invalidPort,
    invalidDatabasePath,
    invalidAuthTimeout,

    invalidStorageBackend,
    invalidMySqlHost,
    invalidMySqlPort,
    invalidMySqlUsername,
    invalidMySqlDatabase,
    incompatibleStorageOptions,
};

// 功能：保存全部合法的 ChatServer 启动参数及其默认值。
struct ServerRuntimeConfig
{
    std::uint16_t port{9000};
    std::filesystem::path database_path{"chathub.db"};
    std::chrono::milliseconds authentication_timeout{5000};
    // 功能：默认保持 SQLite 兼容，只有显式命令行参数才切换到 MySQL。
    StorageBackend storage_backend{StorageBackend::Sqlite};
    // 功能：仅在 storage_backend 为 MySql 时参与后端组合校验和后续连接组装。
    MySqlRuntimeConfig mysql;
};

// 功能：区分有效配置与稳定失败分类；成功时 config 有值且 error 为 none。
struct ParseRuntimeConfigResult
{
    std::optional<ServerRuntimeConfig> config{std::nullopt};
    ServerRuntimeConfigError error{ServerRuntimeConfigError::none};
};

// 功能：仅解析和校验命令行参数，不创建监听器、不访问数据库。
ParseRuntimeConfigResult parseServerRuntimeConfig(int argc, char *argv[]);

// 功能：将内部错误枚举转换为 main() 可输出和测试可断言的稳定错误码文本。
std::string_view serverRuntimeConfigErrorCode(ServerRuntimeConfigError error) noexcept;
} // namespace app
