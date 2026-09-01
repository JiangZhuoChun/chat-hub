#include "repository/mysql_connection_config.h"

#include <string>
#include <windows.h>

namespace repository
{

std::filesystem::path defaultMySqlPluginDirectory()
{
    constexpr DWORD buffer_size = 32768;
    std::wstring executable_path(buffer_size, L'\0');

    const DWORD path_length =
        GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));

    if (path_length == 0 || path_length >= executable_path.size())
    {
        return {};
    }

    executable_path.resize(path_length);
    const auto executable_dir = std::filesystem::path{executable_path}.parent_path();

    if (executable_dir.empty())
    {
        return {};
    }
    return executable_dir / "plugins" / "libmariadb";
}
} // namespace repository