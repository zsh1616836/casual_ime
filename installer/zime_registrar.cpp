#include <windows.h>

#include <cstdio>
#include <cstring>

namespace
{
using registration_function = HRESULT(STDAPICALLTYPE*)();

bool write_status_file(const wchar_t* path, const char* contents)
{
    if (!path || !*path)
        return true;

    HANDLE file = CreateFileW(path,
                              GENERIC_WRITE,
                              FILE_SHARE_READ,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const DWORD content_size = static_cast<DWORD>(strlen(contents));
    DWORD written = 0;
    const bool result = WriteFile(file,
                                  contents,
                                  content_size,
                                  &written,
                                  nullptr) != FALSE &&
        written == content_size;
    CloseHandle(file);
    return result;
}

int finish_with_status(int exit_code, const wchar_t* path)
{
    if (!path || !*path)
        return exit_code;

    char contents[64] = {};
    sprintf_s(contents, "exit=%d\r\n", exit_code);
    return write_status_file(path, contents) ? exit_code : 7;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3 ||
        (wcscmp(argv[1], L"--register") != 0 &&
         wcscmp(argv[1], L"--unregister") != 0))
    {
        fwprintf(stderr,
                 L"usage: zime_registrar --register|--unregister dll [success-marker]\n");
        return 2;
    }

    const wchar_t* status_path = argc >= 4 ? argv[3] : nullptr;
    if (status_path)
    {
        char running[64] = {};
        sprintf_s(running, "running=%lu\r\n", GetCurrentProcessId());
        if (!write_status_file(status_path, running))
            return 7;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
    {
        fwprintf(stderr,
                 L"COM initialization failed: 0x%08lx\n",
                 static_cast<unsigned long>(com_result));
        return finish_with_status(3, status_path);
    }

    HMODULE module = LoadLibraryExW(argv[2],
                                    nullptr,
                                    LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        fwprintf(stderr,
                 L"LoadLibraryExW failed: %lu, dll=%ls\n",
                 GetLastError(),
                 argv[2]);
        if (SUCCEEDED(com_result))
            CoUninitialize();
        return finish_with_status(4, status_path);
    }

    const char* export_name = wcscmp(argv[1], L"--register") == 0
        ? "DllRegisterServer"
        : "DllUnregisterServer";
    const auto function = reinterpret_cast<registration_function>(
        GetProcAddress(module, export_name));
    if (!function)
    {
        fwprintf(stderr,
                 L"GetProcAddress failed: %lu, export=%hs\n",
                 GetLastError(),
                 export_name);
        FreeLibrary(module);
        if (SUCCEEDED(com_result))
            CoUninitialize();
        return finish_with_status(5, status_path);
    }

    const HRESULT result = function();
    fwprintf(stdout,
             L"%hs completed: hresult=0x%08lx, dll=%ls\n",
             export_name,
             static_cast<unsigned long>(result),
             argv[2]);
    FreeLibrary(module);
    if (SUCCEEDED(com_result))
        CoUninitialize();

    if (FAILED(result))
        return finish_with_status(6, status_path);
    return finish_with_status(0, status_path);
}
