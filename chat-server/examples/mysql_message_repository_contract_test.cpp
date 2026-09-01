#include "message_repository_contract_test_runner.h"
#include "repository/mysql_connection.h"
#include "repository/mysql_message_repository.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <mysql.h>
#include <string>

namespace
{
constexpr int kSkipMissingMySqlConfiguration = 77;
constexpr char kExpectedDatabase[] = "chathub_schema_test";

// 测试辅助函数：只在专用测试库中删除本测试创建的两张表，保证每个合同场景独立。
bool resetSchema(const repository::MySqlConnectionConfig& config)
{
    repository::MySqlConnection connection;
    if (connection.open(config) != repository::MySqlConnectionStatus::Opened)
    {
        return false;
    }

    MYSQL* const handle = connection.nativeHandle();
    const bool messages_removed =
        handle != nullptr && mysql_query(handle, "DROP TABLE IF EXISTS messages") == 0;
    const bool migrations_removed =
        messages_removed && mysql_query(handle, "DROP TABLE IF EXISTS schema_migrations") == 0;

    connection.close();
    return migrations_removed;
}

// 测试类型：MySQL 后端夹具 Factory。
// 测试内容：为共享合同 Runner 的每一个场景返回独立、干净、已初始化的接口对象。
repository::test::RepositoryFixture createMySqlRepositoryFixture(
    const repository::MySqlConnectionConfig& config)
{
    if (!resetSchema(config))
    {
        std::cerr << "无法清理 MySQL 合同测试 Schema\n";
        return {};
    }

    auto repository = std::make_unique<repository::MySqlMessageRepository>();
    if (repository->open(config) != repository::MySqlConnectionStatus::Opened ||
        !repository->initializeSchema())
    {
        std::cerr << "无法创建已初始化的 MySQL 合同测试 Repository\n";
        repository->close();
        resetSchema(config);
        return {};
    }

    // 共享 Runner 会先销毁接口对象，再调用 cleanup；因此这里可以安全地重新连接并删除测试表。
    const auto cleanup = [config]() { return resetSchema(config); };
    return {std::move(repository), cleanup};
}
} // namespace

// 测试入口：运行与 SQLite 相同的业务合同；缺少本地 MySQL 配置时安全跳过。
int main()
{
    const char* const username = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_USERNAME");
    const char* const password = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_PASSWORD");
    const char* const database = std::getenv("CHATHUB_MYSQL_REPOSITORY_TEST_DATABASE");
    if (username == nullptr || password == nullptr || database == nullptr ||
        *username == '\0' || *password == '\0' || *database == '\0')
    {
        std::cerr << "SKIP [mysql-message-repository-contract]: "
                     "missing MySQL repository test configuration\n";
        return kSkipMissingMySqlConfiguration;
    }

    if (std::string(database) != kExpectedDatabase)
    {
        std::cerr << "FAIL [mysql-message-repository-contract]: "
                     "unexpected repository test database name\n";
        return EXIT_FAILURE;
    }

    repository::MySqlConnectionConfig config;
    config.username = username;
    config.password = password;
    config.database = database;

    // Lambda 是测试层的 Factory；共享 Runner 不依赖 MySqlMessageRepository 或 MySQL C API。
    const repository::test::RepositoryFactory factory = [config]() {
        return createMySqlRepositoryFixture(config);
    };
    return repository::test::runMessageRepositoryContractTests(factory)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
