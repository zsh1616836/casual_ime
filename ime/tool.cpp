#include "tool.h"
#include <Windows.h>

std::filesystem::path tool::get_current_dll_path()
{
    wchar_t path[MAX_PATH];
    HMODULE hm = nullptr;

    if (GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&get_current_dll_path),
        &hm)) {

        GetModuleFileNameW(hm, path, static_cast<DWORD>(_countof(path)));
        return std::filesystem::path(path).parent_path();
    }

    return std::filesystem::current_path();
}
