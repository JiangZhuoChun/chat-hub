#include "repository/mysql_connection.h"

#include <filesystem>
#include <memory>
#include <mysql.h>
#include <string>
#include <system_error>

namespace repository
{
MySqlConnection::~MySqlConnection()
{
    close();
}

MySqlConnectionStatus MySqlConnection::open(const MySqlConnectionConfig &config)
{
    if (isOpen())
    {
        return MySqlConnectionStatus::AlreadyOpen;
    }

    if (config.host.empty() || config.port == 0 || config.username.empty() || config.password.empty() ||
        config.database.empty() || config.plugin_directory.empty() || config.connect_timeout_seconds == 0)
    {
        return MySqlConnectionStatus::InvalidConfig;
    }

    std::error_code filesystem_error;
    const auto plugin_path = config.plugin_directory / "caching_sha2_password.dll";

    if (!std::filesystem::is_regular_file(plugin_path, filesystem_error))
    {
        return MySqlConnectionStatus::PluginUnavailable;
    }

    using MysqlHandle = std::unique_ptr<MYSQL, decltype(&mysql_close)>;

    MysqlHandle candidate{mysql_init(nullptr), &mysql_close};
    if (!candidate)
    {
        return MySqlConnectionStatus::InitFailed;
    }

    unsigned int timeout_seconds = config.connect_timeout_seconds;
    if (mysql_optionsv(candidate.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds) != 0)
    {
        return MySqlConnectionStatus::OptionsFailed;
    }

    const std::string plugin_dir = config.plugin_directory.string();
    if (mysql_optionsv(candidate.get(), MYSQL_PLUGIN_DIR, plugin_dir.c_str()) != 0)
    {
        return MySqlConnectionStatus::OptionsFailed;
    }

    if (mysql_real_connect(candidate.get(), config.host.c_str(), config.username.c_str(), config.password.c_str(),
                           config.database.c_str(), config.port, nullptr, 0) == nullptr)
    {
        return MySqlConnectionStatus::ConnectionFailed;
    }

    m_connection = candidate.release();
    return MySqlConnectionStatus::Opened;
}

void MySqlConnection::close() noexcept
{
    if (m_connection == nullptr)
    {
        return;
    }
    mysql_close(m_connection);
    m_connection = nullptr;
}

bool MySqlConnection::isOpen() const noexcept
{
    return m_connection != nullptr;
}

MYSQL *MySqlConnection::nativeHandle() const noexcept
{
    return m_connection;
}

} // namespace repository
