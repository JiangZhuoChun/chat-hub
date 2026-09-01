#include "message_repository_factory.h"
#include "server_runtime_config.h"

#include "repository/mysql_connection_config.h"
#include "repository/mysql_message_repository.h"
#include "repository/sqlite_message_repository.h"

#include <cstdlib>
#include <utility>

namespace app
{
CreateMessageRepositoryResult createMessageRepository(const ServerRuntimeConfig &config)
{
    if (config.storage_backend == StorageBackend::Sqlite)
    {
        if (auto repo = repository::createSqliteMessageRepository(config.database_path.string()); repo)
        {
            return {std::move(repo), MessageRepositoryStartupError::None};
        }
        return {nullptr, MessageRepositoryStartupError::SqliteOpenFailed};
    }
    // 如果是 MySQL，先读取环境变量
    const char *const password = std::getenv("CHATHUB_MYSQL_PASSWORD");
    if (password == nullptr || password[0] == '\0')
    {
        return {nullptr, MessageRepositoryStartupError::MissingMySqlPassword};
    }
    // 把非秘密运行配置和密码组合成短生命周期的连接配置
    repository::MySqlConnectionConfig connection_config;
    connection_config.host = config.mysql.host;
    connection_config.port = config.mysql.port;
    connection_config.username = config.mysql.username;
    connection_config.password = password;
    connection_config.database = config.mysql.database;

    // 创建候选对象并打开
    auto candidate = std::make_unique<repository::MySqlMessageRepository>();
    const auto status = candidate->open(connection_config);
    switch (status)
    {
    case repository::MySqlConnectionStatus::Opened: {
        if (!candidate->initializeSchema())
        {
            return {nullptr, MessageRepositoryStartupError::MySqlSchemaInitializationFailed};
        }

        return {std::move(candidate), MessageRepositoryStartupError::None};
    }
    case repository::MySqlConnectionStatus::AlreadyOpen:
        return {nullptr, MessageRepositoryStartupError::MySqlConnectionFailed};
    case repository::MySqlConnectionStatus::InvalidConfig:
        return {nullptr, MessageRepositoryStartupError::MySqlInvalidConfig};
    case repository::MySqlConnectionStatus::PluginUnavailable:
        return {nullptr, MessageRepositoryStartupError::MySqlPluginUnavailable};
    case repository::MySqlConnectionStatus::InitFailed:
    case repository::MySqlConnectionStatus::OptionsFailed:
        return {nullptr, MessageRepositoryStartupError::MySqlClientInitializationFailed};
    case repository::MySqlConnectionStatus::ConnectionFailed:
        return {nullptr, MessageRepositoryStartupError::MySqlConnectionFailed};
    }
    return {nullptr, MessageRepositoryStartupError::MySqlConnectionFailed};
}
std::string_view messageRepositoryStartupErrorCode(MessageRepositoryStartupError error) noexcept
{
    switch (error)
    {
    case MessageRepositoryStartupError::None:
        return {};
    case MessageRepositoryStartupError::SqliteOpenFailed:
        return "sqlite_open_failed";
    case MessageRepositoryStartupError::MissingMySqlPassword:
        return "missing_mysql_password";
    case MessageRepositoryStartupError::MySqlInvalidConfig:
        return "invalid_mysql_config";
    case MessageRepositoryStartupError::MySqlPluginUnavailable:
        return "mysql_plugin_unavailable";
    case MessageRepositoryStartupError::MySqlClientInitializationFailed:
        return "mysql_client_initialization_failed";
    case MessageRepositoryStartupError::MySqlConnectionFailed:
        return "mysql_connection_failed";
    case MessageRepositoryStartupError::MySqlSchemaInitializationFailed:
        return "mysql_schema_initialization_failed";
    }
    return "invalid_repository_startup_error";
}
} // namespace app
