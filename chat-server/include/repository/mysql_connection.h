#pragma once
#include "repository/mysql_connection_config.h"

struct st_mysql;
using MYSQL = st_mysql;

namespace repository
{
enum class MySqlConnectionStatus
{
    Opened,
    AlreadyOpen,
    InvalidConfig,
    PluginUnavailable,
    InitFailed,
    OptionsFailed,
    ConnectionFailed,
};

class MySqlConnection final
{
public:
    MySqlConnection() = default;
    ~MySqlConnection();

    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    MySqlConnectionStatus open(const MySqlConnectionConfig& config);
    void close() noexcept;//只释放资源，不应该抛异常，因此声明为 noexcept

    bool isOpen() const noexcept;
    MYSQL* nativeHandle() const noexcept;

private:
    MYSQL* m_connection = nullptr;

};

}