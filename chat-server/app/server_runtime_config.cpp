
#include "server_runtime_config.h"

#include <charconv>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
// 功能：只在整个无符号整数文本和范围均可解析时写入输出参数，避免失败污染调用方。
bool parseUnsignedInteger(std::string_view text, std::uint64_t &output)
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t value = 0;

    const char *const begin = text.data();
    const char *const end = text.data() + text.size();

    const auto [ptr, ec] = std::from_chars(begin, end, value);

    if (ec != std::errc{} || ptr != end)
    {
        return false;
    }
    output = value;
    return true;
}
// 功能：限制运行时配置允许的选项；后端选择和 MySQL 非秘密参数也必须在此白名单中声明。
bool isSupportedOption(std::string_view option)
{
    return  option == "--port" ||
            option == "--database-path" ||
            option == "--auth-timeout-ms" ||
            option == "--storage-backend" ||
            option == "--mysql-host" ||
            option == "--mysql-port" ||
            option == "--mysql-username" ||
            option == "--mysql-database";
}

// 功能：识别缺失值场景中的下一个选项标记，避免把选项名误当作前一个选项的值。
bool startsWithDoubleDash(std::string_view text)
{
    return text.size() >= 2 && text[0] == '-' && text[1] == '-';
}

// 功能：统一构造失败结果，确保失败路径不会泄露局部候选配置。
app::ParseRuntimeConfigResult makeError(const app::ServerRuntimeConfigError error)
{
    return {std::nullopt, error};
}
} // namespace

namespace app
{
// 功能：在局部 candidate 中完成全部参数试错；只有循环成功结束后才提交有效配置。
ParseRuntimeConfigResult parseServerRuntimeConfig(int argc, char *argv[])
{

    // 功能：先累积候选配置，所有单项和组合校验通过后才作为成功结果返回。
    ServerRuntimeConfig candidate;

    // 功能：记录每个出现过的选项，用于拒绝重复项，并在循环后判断跨后端参数是否混用。
    std::unordered_set<std::string> seen_options;

    for (int index = 1; index < argc; ++index)
    {
        const std::string option{argv[index]};

        if (!isSupportedOption(option))
        {
            return makeError(ServerRuntimeConfigError::unknownOption);
        }

        if (seen_options.find(option) != seen_options.end())
        {
            return makeError(ServerRuntimeConfigError::duplicateOption);
        }

        seen_options.emplace(option);

        if (index + 1 >= argc)
        {
            return makeError(ServerRuntimeConfigError::missingOptionValue);
        }
        if (const std::string_view next{argv[index + 1]}; startsWithDoubleDash(next))
        {
            return makeError(ServerRuntimeConfigError::missingOptionValue);
        }

        const std::string_view value{argv[++index]};

        if (option == "--port")
        {
            std::uint64_t parsed = 0;

            if (!parseUnsignedInteger(value, parsed) || parsed < 1 || parsed > 65535)
            {
                return makeError(ServerRuntimeConfigError::invalidPort);
            }
            candidate.port = static_cast<std::uint16_t>(parsed);
            continue;
        }

        if (option == "--database-path")
        {
            if (value.empty())
            {
                return makeError(ServerRuntimeConfigError::invalidDatabasePath);
            }
            candidate.database_path = std::filesystem::path{value};
            continue;
        }

        if (option == "--auth-timeout-ms")
        {
            std::uint64_t parsed = 0;

            if (!parseUnsignedInteger(value, parsed) || parsed < 1000 || parsed > 30000)
            {
                return makeError(ServerRuntimeConfigError::invalidAuthTimeout);
            }
            candidate.authentication_timeout = std::chrono::milliseconds{parsed};
        }

        if (option == "--storage-backend")
        {
            if (value == "sqlite")
            {
                candidate.storage_backend = StorageBackend::Sqlite;
                continue;
            }
            if (value == "mysql")
            {
                candidate.storage_backend = StorageBackend::MySql;
                continue;
            }
            return makeError(ServerRuntimeConfigError::invalidStorageBackend);
        }

        if (option == "--mysql-host")
        {
            if (value.empty())
            {
                return makeError(ServerRuntimeConfigError::invalidMySqlHost);
            }
            candidate.mysql.host = std::string{value};
            continue;
        }
        if (option == "--mysql-port")
        {
            std::uint64_t parsed = 0;

            if (!parseUnsignedInteger(value, parsed) || parsed < 1 || parsed > 65535)
            {
                return makeError(ServerRuntimeConfigError::invalidMySqlPort);
            }
            candidate.mysql.port = static_cast<std::uint16_t>(parsed);
            continue;
        }
        if (option == "--mysql-username")
        {
            if (value.empty())
            {
                return makeError(ServerRuntimeConfigError::invalidMySqlUsername);
            }
            candidate.mysql.username = std::string{value};
            continue;
        }
        if (option == "--mysql-database")
        {
            if (value.empty())
            {
                return makeError(ServerRuntimeConfigError::invalidMySqlDatabase);
            }
            candidate.mysql.database = std::string{value};
        }
    }

    // 功能：单个参数可合法，但 SQLite/MySQL 参数组合可能冲突，故必须在完整读取后统一校验。
    const bool has_mysql_option =
        seen_options.find("--mysql-host") != seen_options.end() ||
        seen_options.find("--mysql-port") != seen_options.end() ||
        seen_options.find("--mysql-username") != seen_options.end() ||
        seen_options.find("--mysql-database") != seen_options.end();
    if (candidate.storage_backend == StorageBackend::Sqlite)
    {
        if (has_mysql_option)
        {
            return makeError(ServerRuntimeConfigError::incompatibleStorageOptions);
        }
    }else
    {
        if (seen_options.find("--database-path") != seen_options.end())
        {
            return makeError(ServerRuntimeConfigError::incompatibleStorageOptions);
        }
        if (candidate.mysql.username.empty())
        {
            return makeError(ServerRuntimeConfigError::invalidMySqlUsername);
        }
        if (candidate.mysql.database.empty())
        {
            return makeError(ServerRuntimeConfigError::invalidMySqlDatabase);
        }
    }

    return {std::move(candidate), ServerRuntimeConfigError::none};
}

// 功能：集中维护错误枚举与外部稳定文本的唯一映射，避免解析逻辑散落字符串字面量。
std::string_view serverRuntimeConfigErrorCode(const ServerRuntimeConfigError error) noexcept
{
    switch (error)
    {
    case ServerRuntimeConfigError::none:
        return "";
    case ServerRuntimeConfigError::missingOptionValue:
        return "missing_option_value";
    case ServerRuntimeConfigError::duplicateOption:
        return "duplicate_option";
    case ServerRuntimeConfigError::unknownOption:
        return "unknown_option";
    case ServerRuntimeConfigError::invalidPort:
        return "invalid_port";
    case ServerRuntimeConfigError::invalidDatabasePath:
        return "invalid_database_path";
    case ServerRuntimeConfigError::invalidAuthTimeout:
        return "invalid_auth_timeout";
    case ServerRuntimeConfigError::invalidStorageBackend:
        return "invalid_storage_backend";
    case ServerRuntimeConfigError::invalidMySqlHost:
        return "invalid_mysql_host";
    case ServerRuntimeConfigError::invalidMySqlPort:
        return "invalid_mysql_port";
    case ServerRuntimeConfigError::invalidMySqlUsername:
        return "invalid_mysql_username";
    case ServerRuntimeConfigError::invalidMySqlDatabase:
        return "invalid_mysql_database";
    case ServerRuntimeConfigError::incompatibleStorageOptions:
        return "incompatible_storage_options";
    }
    return "invalid_runtime_config_error";
}
} // namespace app
