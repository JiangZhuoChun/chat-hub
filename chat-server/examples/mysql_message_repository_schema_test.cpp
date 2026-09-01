#include "repository/mysql_connection.h"
#include "repository/mysql_message_repository.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <mysql.h>

namespace
{
constexpr int kSkipMissingMySqlConfiguration = 77;
constexpr  char kExpectedDatabase[] = "chathub_schema_test";

bool resetSchema(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        return false;
    }

    MYSQL* handle = connection.nativeHandle();
    const bool messages_removed =
        handle != nullptr && mysql_query(handle,"DROP TABLE IF EXISTS messages") == 0;
    const bool migrations_removed =
        messages_removed && mysql_query(handle,"DROP TABLE IF EXISTS schema_migrations") == 0;

    connection.close();
    return migrations_removed;
}
}

bool testSchemaInitialization(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlMessageRepository repository;
    // 未建立连接时，不允许初始化 Schema。
    if (repository.initializeSchema())
    {
        std::cerr
            << "FAIL [mysql-message-repository-schema]: "
               "initializeSchema succeeded before open\n";
        return false;
    }

    if (repository.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        std::cerr
            << "FAIL [mysql-message-repository-schema]: "
               "repository open did not return Opened\n";
        return false;
    }
    // 首次调用应创建 Schema 并记录 V1 migration。
    if (!repository.initializeSchema())
    {
        std::cerr
            << "FAIL [mysql-message-repository-schema]: "
               "first initializeSchema failed\n";
        return false;
    }

    // Schema 初始化必须可重复执行。
    if (!repository.initializeSchema())
    {
        std::cerr
            << "FAIL [mysql-message-repository-schema]: "
               "second initializeSchema failed\n";
        return false; // 重复调用必须可重跑
    }

    repository.close();
    if (repository.isOpen())
    {
        std::cerr
            << "FAIL [mysql-message-repository-schema]: "
               "repository remained open after close\n";
        return false;
    }

    return true;
}
int main()
{
    const char* username = std::getenv("CHATHUB_MYSQL_SCHEMA_TEST_USERNAME");
    const char* password =std::getenv("CHATHUB_MYSQL_SCHEMA_TEST_PASSWORD");
    const char* database =std::getenv("CHATHUB_MYSQL_SCHEMA_TEST_DATABASE");

    if (username == nullptr || password == nullptr || database == nullptr ||
        *username == '\0' || *password == '\0' || *database == '\0')
    {
        std::cerr<< "SKIP [mysql-message-repository-schema]: ""missing MySQL schema test configuration\n";
        return kSkipMissingMySqlConfiguration;
    }

    if (std::string(database) != kExpectedDatabase)
    {
        std::cerr<< "FAIL [mysql-message-repository-schema]: ""unexpected schema test database name\n";
        return EXIT_FAILURE;
    }

    repository::MySqlConnectionConfig config;
    config.username = username;
    config.password = password;
    config.database = database;

    if (!resetSchema(config))
    {
        std::cerr<< "FAIL [mysql-message-repository-schema]: ""initial schema reset failed\n";
        return EXIT_FAILURE;
    }

    const bool test_passed = testSchemaInitialization(config);
    const bool cleanup_passed = resetSchema(config);
    // 无论核心测试是否成功，都尝试执行最终清理。
    if (!test_passed)
    {
        std::cerr<< "FAIL [mysql-message-repository-schema]: ""schema initialization test failed\n";
    }
    if (!cleanup_passed)
    {
        std::cerr << "FAIL [mysql-message-repository-schema]: " "final schema reset failed\n";
    }
    if (!test_passed || !cleanup_passed)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;

}