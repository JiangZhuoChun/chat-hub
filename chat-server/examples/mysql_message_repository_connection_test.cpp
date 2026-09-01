#include "repository/mysql_message_repository.h"

#include <cstdlib>
#include <iostream>

namespace
{
// 定义跳过码
constexpr int kSkipMissingMySqlConfiguration = 77;

// 测试内容：通过 MySqlMessageRepository 转发真实 MySQL 配置，
// 验证 Opened → AlreadyOpen → close() 生命周期。
bool testOpenAlreadyOpenAndClose(const repository::MySqlConnectionConfig &config)
{
    repository::MySqlMessageRepository repository;
    const auto first_open_result = repository.open(config);

    if (first_open_result != repository::MySqlConnectionStatus::Opened)
    {
        std::cerr << "FAIL [mysql-message-repository-connection-integration]: ""首次连接没有返回 Opened\n";
        return false;
    }

    if (!repository.isOpen())
    {
        std::cerr<< "FAIL [mysql-message-repository-connection-integration]: ""首次连接成功后连接状态无效\n";
        return false;
    }

    const auto second_open_result = repository.open(config);
    if (second_open_result != repository::MySqlConnectionStatus::AlreadyOpen)
    {
        std::cerr<< "FAIL [mysql-message-repository-connection-integration]: ""重复连接没有返回 AlreadyOpen\n";
        return false;
    }
    repository.close();

    if (repository.isOpen())
    {
        std::cerr<< "FAIL [mysql-message-repository-connection-integration]: ""close 后连接仍处于打开状态\n";
        return false;
    }

    // 验证 close() 的幂等性。
    repository.close();
    if (repository.isOpen())
    {
        std::cerr<< "FAIL [mysql-message-repository-connection-integration]: ""重复 close 后连接状态无效\n";
        return false;
    }

    std::cout<< "PASS [mysql-message-repository-connection-integration]: ""Opened -> AlreadyOpen -> close lifecycle\n";
    return true;
}
}// namespace

int main()
{
    const char* username = std::getenv("CHATHUB_MYSQL_USERNAME");
    const char* password = std::getenv("CHATHUB_MYSQL_PASSWORD");
    const char* database = std::getenv("CHATHUB_MYSQL_DATABASE");

    if (username == nullptr || *username == '\0' ||
        password == nullptr || *password == '\0' ||
        database == nullptr || *database == '\0')
    {
        std::cout << "SKIP [mysql-message-repository-connection-integration]: ""missing MySQL environment configuration\n";
        return kSkipMissingMySqlConfiguration;
    }

    repository::MySqlConnectionConfig config;
    config.username = username;
    config.password = password;
    config.database = database;

    if (!testOpenAlreadyOpenAndClose(config))
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}