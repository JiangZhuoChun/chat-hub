#include "message_repository_contract_test_runner.h"
#include "repository/sqlite_message_repository.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fs = std::filesystem;

namespace
{

// 为测试创建独占临时目录；create_directory 的原子结果可避免并行测试进程使用同一路径。
fs::path createUniqueTestDirectory(const char *label, std::error_code &error)
{
    const fs::path temp_directory = fs::temp_directory_path(error);
    if (error)
    {
        return {};
    }

    const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (unsigned int attempt = 0; attempt < 100; ++attempt)
    {
        const fs::path candidate =
            temp_directory / (std::string(label) + "_" + std::to_string(seed) + "_" + std::to_string(attempt));
        error.clear();
        if (fs::create_directory(candidate, error))
        {
            return candidate;
        }
        if (error)
        {
            return {};
        }
    }

    error = std::make_error_code(std::errc::file_exists);
    return {};
}

// 测试类型：SQLite 后端专项测试。
// 测试内容：目录不能被当作 SQLite 数据库文件打开，open() 必须返回 false。
bool testSqliteOpenFailsForDirectory()
{
    std::error_code error;
    const fs::path database_directory = createUniqueTestDirectory("chathub_repository_open_failure", error);
    if (database_directory.empty())
    {
        std::cerr << "无法创建测试目录：" << error.message() << '\n';
        return false;
    }

    repository::SqliteMessageRepository repository;
    const bool opened = repository.open(database_directory.string());

    std::error_code cleanup_error;
    fs::remove_all(database_directory, cleanup_error);
    if (cleanup_error)
    {
        std::cerr << "无法清理测试目录：" << cleanup_error.message() << '\n';
        return false;
    }

    return !opened;
}

// 测试夹具 Factory：为每个公共合同场景创建独立、干净、已初始化的 SQLite Repository。
repository::test::RepositoryFixture createSqliteRepositoryFixture()
{
    std::error_code error;
    const fs::path fixture_directory = createUniqueTestDirectory("chathub_repository_contract", error);
    if (fixture_directory.empty())
    {
        std::cerr << "无法创建合同测试临时目录：" << error.message() << '\n';
        return {};
    }

    const fs::path database_path = fixture_directory / "messages.db";
    auto repository = repository::createSqliteMessageRepository(database_path.string());
    const auto cleanup = [fixture_directory]() {
        std::error_code cleanup_error;
        fs::remove_all(fixture_directory, cleanup_error);
        if (cleanup_error)
        {
            std::cerr << "无法删除合同测试目录：" << cleanup_error.message() << '\n';
            return false;
        }
        return true;
    };

    return {std::move(repository), cleanup};
}

// 测试类型：SQLite 后端专项编译期测试。
// 测试内容：持有 sqlite3* 的 Repository 禁止复制构造，避免两个对象重复关闭同一数据库连接。
static_assert(!std::is_copy_constructible_v<repository::SqliteMessageRepository>,
              "SqliteMessageRepository must not be copy constructible");

// 测试类型：SQLite 后端专项编译期测试。
// 测试内容：持有 sqlite3* 的 Repository 禁止复制赋值，避免连接所有权被浅复制覆盖。
static_assert(!std::is_copy_assignable_v<repository::SqliteMessageRepository>,
              "SqliteMessageRepository must not be copy assignable");

// 输出 SQLite 后端专项测试结果；公共合同测试由共享 Runner 输出。
bool reportSqliteTest(const char *name, const bool passed)
{
    if (passed)
    {
        std::cout << "PASS [sqlite-specific]: " << name << '\n';
    }
    else
    {
        std::cerr << "FAIL [sqlite-specific]: " << name << '\n';
    }
    return passed;
}

} // namespace

// 测试入口同时运行两类测试：共享业务合同测试，以及 SQLite 后端专项测试。
int main()
{
    const bool contract_tests_passed =
        repository::test::runMessageRepositoryContractTests(createSqliteRepositoryFixture);
    const bool sqlite_tests_passed = reportSqliteTest("directory path open failure", testSqliteOpenFailsForDirectory());

    return contract_tests_passed && sqlite_tests_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
