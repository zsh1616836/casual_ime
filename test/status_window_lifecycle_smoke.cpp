#include "status_window.h"

#include <windows.h>
#include <tlhelp32.h>

#include <iostream>
#include <string>
#include <vector>

namespace
{
DWORD process_thread_count()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    DWORD count = 0;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
                ++count;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

int run_lifecycle_child()
{
    g_hInst = GetModuleHandleW(nullptr);
    const DWORD baseline_threads = process_thread_count();
    auto* window = new status_window();
    if (!window->create(nullptr))
        return 2;
    window->show(true);
    UpdateWindow(window->get_hwnd());
    const DWORD painted_threads = process_thread_count();
    if (baseline_threads == 0 || painted_threads <= baseline_threads)
        return 3;
    delete window;

    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (process_thread_count() > baseline_threads &&
           GetTickCount64() < deadline)
    {
        Sleep(10);
    }
    if (process_thread_count() > baseline_threads)
        return 4;

    // Classic Notepad can finish its UI thread without forcing ExitProcess.
    // A leaked GDI+ background thread would keep this child alive.
    ExitThread(0);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--lifecycle-child") == 0)
        return run_lifecycle_child();

    wchar_t executable_path[32768] = {};
    if (GetModuleFileNameW(nullptr,
                           executable_path,
                           static_cast<DWORD>(std::size(executable_path))) == 0)
    {
        return 2;
    }

    std::wstring command_line = L"\"";
    command_line += executable_path;
    command_line += L"\" --lifecycle-child";
    std::vector<wchar_t> mutable_command(command_line.begin(),
                                          command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable_path,
                        mutable_command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &process))
    {
        return 3;
    }

    CloseHandle(process.hThread);
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 5000);
    DWORD exit_code = 0;
    const BOOL got_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait_result == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 100);
    CloseHandle(process.hProcess);

    if (wait_result != WAIT_OBJECT_0)
    {
        std::wcerr << L"status window held process after destruction"
                   << std::endl;
        return 5;
    }
    if (!got_exit_code || exit_code != 0)
    {
        std::wcerr << L"status window lifecycle child failed: " << exit_code
                   << std::endl;
        return 6;
    }
    std::wcout << L"status window lifecycle passed" << std::endl;
    return 0;
}
