#include "repository/mysql_connection.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace
{
bool isClosed(const repository::MySqlConnection& connection)
{
    return !connection.isOpen() && connection.nativeHandle() == nullptr;
}
}

repository::MySqlConnectionConfig makeCompleteConfig(const std::filesystem::path& plugin_dir)
{
    repository::MySqlConnectionConfig config;

    config.username = "test_user";
    config.password = "test_password";
    config.database = "test_database";
    config.plugin_directory = plugin_dir;

    return config;
}

// 测试类型：构建与部署集成测试。
// 测试内容：默认插件目录必须与 CMake 传入的 chat-server 部署目录一致，且认证插件 DLL 存在。
bool testDefaultPluginDirectory(const std::filesystem::path& expect_dir)
{
    const auto actual_dir = repository::defaultMySqlPluginDirectory();

    if (expect_dir.lexically_normal() != actual_dir.lexically_normal())
    {
        std::cerr  << "default plugin directory mismatch\n"
                    << "expected: " << expect_dir << '\n'
                    << "actual: " << actual_dir  << '\n';
        return false;
    }

    const auto plugin_file = actual_dir / "caching_sha2_password.dll";
    if (!std::filesystem::is_regular_file(plugin_file))
    {
        std::cerr
            << "caching_sha2_password.dll is missing or not a regular file: "
            << plugin_file << '\n';
        return false;
    }

    return true;
}

// 测试类型：MySqlConnection 单元测试。
// 测试内容：用户名为空时，open() 必须在创建 MYSQL* 前返回 InvalidConfig，连接保持关闭。
bool testEmptyUsernameIsInvalidConfig(const std::filesystem::path& plugin_dir)
{
    repository::MySqlConnection connection;

    auto config = makeCompleteConfig(plugin_dir);
    config.username.clear();

    const auto result = connection.open(config);
    if (result != repository::MySqlConnectionStatus::InvalidConfig)
    {
        std::cerr<< "empty username did not return InvalidConfig\n";
        return false;
    }

    if (!isClosed(connection))
    {
        std::cerr<< "connection is not closed after invalid config\n";
        return false;
    }

    return true;
}

// 测试类型：MySqlConnection 单元测试。
// 测试内容：完整配置指向不含认证插件的目录时，open() 必须返回 PluginUnavailable，连接保持关闭。
bool testMissingPluginIsRejected(const std::filesystem::path& expected_dir)
{
    repository::MySqlConnection connection;

    auto config = makeCompleteConfig(expected_dir);
    config.plugin_directory = expected_dir / "__missing_plugin_directory__";

    const auto result = connection.open(config);

    if (result != repository::MySqlConnectionStatus::PluginUnavailable)
    {
        std::cerr<< "missing plugin directory did not return PluginUnavailable\n";
        return false;
    }

    if (!isClosed(connection))
    {
        std::cerr<< "connection is not closed after missing plugin\n";
        return false;
    }
    return true;
}

bool reportTest(const char* name,const bool passed)
{
    if (passed)
    {
        std::cout << name << " passed\n";
        return true;
    }
    else
    {
        std::cout << name << " failed\n";
        return false;
    }
}

int main(int argc,char* argv[])
{
    if (argc != 2)
    {
        std::cerr
            << "FAIL: expected exactly one plugin directory argument\n";
        return EXIT_FAILURE;
    }

    const fs::path expected_dir = argv[1];
    bool passed = true;


    passed &= reportTest(
        "default mysql plugin directory",
        testDefaultPluginDirectory(expected_dir));

    passed &= reportTest(
        "empty username is invalid config",
        testEmptyUsernameIsInvalidConfig(expected_dir));

    passed &= reportTest(
        "missing plugin is rejected",
        testMissingPluginIsRejected(expected_dir));

    return  passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
