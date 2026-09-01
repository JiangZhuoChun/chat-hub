#pragma once

#include "repository/message_repository_contract.h"


#include <memory>
#include <string_view>
namespace app
{
struct ServerRuntimeConfig;
enum class MessageRepositoryStartupError
{
    None,
    SqliteOpenFailed,
    MissingMySqlPassword,
    MySqlInvalidConfig,
    MySqlPluginUnavailable,
    MySqlClientInitializationFailed,
    MySqlConnectionFailed,
    MySqlSchemaInitializationFailed,
};

struct CreateMessageRepositoryResult
{
    std::unique_ptr<repository::IMessageRepository> repository;
    MessageRepositoryStartupError error = MessageRepositoryStartupError::None;
};

CreateMessageRepositoryResult createMessageRepository(const ServerRuntimeConfig & config);

std::string_view messageRepositoryStartupErrorCode(MessageRepositoryStartupError error) noexcept;
}
