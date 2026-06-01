#include "ime_trace.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
std::once_flag g_log_path_once;
std::wstring g_log_path;
SRWLOCK g_log_lock = SRWLOCK_INIT;

std::wstring vformat_string(const wchar_t* fmt, va_list args)
{
    if (!fmt || !*fmt)
        return std::wstring();

    va_list args_copy;
    va_copy(args_copy, args);
    const int len = _vscwprintf(fmt, args_copy);
    va_end(args_copy);
    if (len <= 0)
        return std::wstring();

    std::wstring text;
    text.resize(static_cast<size_t>(len) + 1);
    va_copy(args_copy, args);
    _vsnwprintf_s(text.data(), text.size(), _TRUNCATE, fmt, args_copy);
    va_end(args_copy);
    text.resize(static_cast<size_t>(len));
    return text;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();

    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return std::string();

    std::string utf8;
    utf8.resize(static_cast<size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        utf8.data(), len, nullptr, nullptr);
    return utf8;
}

std::wstring get_process_path()
{
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD copied = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (copied == 0 || copied >= std::size(buffer))
        return L"<unknown-process>";
    return std::wstring(buffer, copied);
}

std::wstring basename_from_path(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return path;
    return path.substr(pos + 1);
}

std::wstring resolve_log_path()
{
    wchar_t temp_path[MAX_PATH * 4] = {};
    const DWORD temp_len = GetTempPathW(static_cast<DWORD>(std::size(temp_path)), temp_path);
    if (temp_len > 0 && temp_len < std::size(temp_path))
        return std::wstring(temp_path, temp_len) + L"zime_ui_trace.log";
    return L"C:\\Windows\\Temp\\zime_ui_trace.log";
}

std::wstring make_prefix()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    const std::wstring process_name = basename_from_path(get_process_path());

    wchar_t prefix[512] = {};
    swprintf_s(prefix,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu proc=%s",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               GetCurrentProcessId(), GetCurrentThreadId(), process_name.c_str());
    return prefix;
}

void append_line_to_file(const std::wstring& line)
{
    std::call_once(g_log_path_once, []() {
        g_log_path = resolve_log_path();
    });

    HANDLE file = CreateFileW(g_log_path.c_str(),
                              FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    std::wstring text = line;
    text.append(L"\r\n");
    const std::string utf8 = wide_to_utf8(text);
    if (!utf8.empty())
    {
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}
}

void ime_tracef(const wchar_t* event, const wchar_t* fmt, ...)
{
    std::wstring detail;
    if (fmt && *fmt)
    {
        va_list args;
        va_start(args, fmt);
        detail = vformat_string(fmt, args);
        va_end(args);
    }

    std::wstring line = make_prefix() + L" event=" + (event ? event : L"<null>");
    if (!detail.empty())
        line.append(L" ").append(detail);

    OutputDebugStringW((L"[zime-ui] " + line + L"\n").c_str());

    AcquireSRWLockExclusive(&g_log_lock);
    append_line_to_file(line);
    ReleaseSRWLockExclusive(&g_log_lock);
}
