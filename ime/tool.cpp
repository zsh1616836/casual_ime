#include "tool.h"
#include <Windows.h>
#include <ShlObj.h>
#include <iterator>

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

std::filesystem::path tool::get_user_data_path()
{
    wchar_t override_path[32768] = {};
    const DWORD override_length = GetEnvironmentVariableW(
        L"ZIME_DATA_DIR",
        override_path,
        static_cast<DWORD>(std::size(override_path)));
    if (override_length > 0 && override_length < std::size(override_path))
        return std::filesystem::path(override_path);

    PWSTR local_app_data = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData,
        KF_FLAG_NO_PACKAGE_REDIRECTION,
        nullptr,
        &local_app_data);
    if (SUCCEEDED(result) && local_app_data)
    {
        std::filesystem::path path(local_app_data);
        CoTaskMemFree(local_app_data);
        return path / L"ZIme";
    }
    if (local_app_data)
        CoTaskMemFree(local_app_data);
    return get_current_dll_path() / L"data";
}
