#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
namespace repository
{
std::filesystem::path defaultMySqlPluginDirectory();

struct MySqlConnectionConfig
 {
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string username;
    std::string password;
    std::string database;
    std::filesystem::path plugin_directory {defaultMySqlPluginDirectory()};
    unsigned int connect_timeout_seconds{2};
 };
}