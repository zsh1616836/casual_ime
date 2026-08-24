#include "broker_log.h"

#include <windows.h>

#include <array>
#include <cstdarg>
#include <cwchar>
#include <string>

namespace
{
SRWLOCK g_log_lock = SRWLOCK_INIT;

std::wstring temp_log_path(const wchar_t* file_name)
{
    wchar_t temp[MAX_PATH * 4] = {};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(std::size(temp)), temp);
    if (length > 0 && length < std::size(temp))
        return std::wstring(temp, length) + file_name;
    return file_name;
}

void write_line(const wchar_t* file_name,
                const wchar_t* level,
                const wchar_t* format,
                va_list args,
                bool limit_size)
{
    wchar_t detail[2048] = {};
    _vsnwprintf_s(detail, std::size(detail), _TRUNCATE, format, args);

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t line[2304] = {};
    swprintf_s(line,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu level=%s %s\r\n",
               time.wYear,
               time.wMonth,
               time.wDay,
               time.wHour,
               time.wMinute,
               time.wSecond,
               time.wMilliseconds,
               GetCurrentProcessId(),
               level,
               detail);
    OutputDebugStringW(line);

    const std::wstring path = temp_log_path(file_name);
    AcquireSRWLockExclusive(&g_log_lock);
    HANDLE file = CreateFileW(path.c_str(),
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        constexpr LONGLONG kMaxErrorLogBytes = 2LL * 1024 * 1024;
        LARGE_INTEGER size = {};
        if (limit_size && GetFileSizeEx(file, &size) &&
            size.QuadPart >= kMaxErrorLogBytes)
        {
            CloseHandle(file);
            DeleteFileW(path.c_str());
            file = CreateFileW(path.c_str(),
                               FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        }
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file,
                      line,
                      static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)),
                      &written,
                      nullptr);
            CloseHandle(file);
        }
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

#ifndef NDEBUG
bool trace_enabled()
{
    static const bool enabled = []() {
        wchar_t value[16] = {};
        const DWORD length = GetEnvironmentVariableW(
            L"ZIME_TRACE", value, static_cast<DWORD>(std::size(value)));
        return length > 0 && length < std::size(value) &&
            (_wcsicmp(value, L"1") == 0 ||
             _wcsicmp(value, L"true") == 0 ||
             _wcsicmp(value, L"yes") == 0);
    }();
    return enabled;
}
#endif
}

#ifndef NDEBUG
void broker_debug_log(const wchar_t* format, ...)
{
    if (!trace_enabled())
        return;
    va_list args;
    va_start(args, format);
    write_line(L"zime_broker_trace.log", L"debug", format, args, false);
    va_end(args);
}
#endif

void broker_error_log(const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    write_line(L"zime_broker_error.log", L"error", format, args, true);
    va_end(args);
}
