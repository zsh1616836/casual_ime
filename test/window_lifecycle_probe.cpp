#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{
struct module_info
{
    std::uintptr_t base;
    std::size_t size;
    std::wstring name;
    std::wstring path;
};

std::vector<DWORD> find_processes(const wchar_t* executable_name)
{
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, executable_name) == 0)
                result.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::vector<DWORD> process_threads(DWORD process_id)
{
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == process_id)
                result.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::vector<module_info> process_modules(DWORD process_id)
{
    std::vector<module_info> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            result.push_back({
                reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
                static_cast<std::size_t>(entry.modBaseSize),
                entry.szModule,
                entry.szExePath});
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

const module_info* module_for_address(const std::vector<module_info>& modules,
                                      std::uintptr_t address)
{
    const auto found = std::find_if(
        modules.begin(), modules.end(), [address](const module_info& module) {
            return address >= module.base && address < module.base + module.size;
        });
    return found == modules.end() ? nullptr : &*found;
}

void collect_window(HWND window, DWORD process_id, std::set<HWND>* windows)
{
    DWORD owner_process = 0;
    GetWindowThreadProcessId(window, &owner_process);
    if (owner_process == process_id)
        windows->insert(window);
}

void write_window(std::wostream& output,
                  HWND window,
                  const std::vector<module_info>& modules)
{
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    wchar_t class_name[256] = {};
    wchar_t title[512] = {};
    GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));

    SetLastError(ERROR_SUCCESS);
    const auto class_module = static_cast<std::uintptr_t>(
        GetClassLongPtrW(window, GCLP_HMODULE));
    const DWORD class_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const auto window_proc = static_cast<std::uintptr_t>(
        GetWindowLongPtrW(window, GWLP_WNDPROC));
    const DWORD proc_error = GetLastError();
    const module_info* class_owner = module_for_address(modules, class_module);
    const module_info* proc_owner = module_for_address(modules, window_proc);

    output << L"hwnd=0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(window)
           << L" tid=" << std::dec << thread_id
           << L" visible=" << (IsWindowVisible(window) ? 1 : 0)
           << L" parent=0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(GetParent(window))
           << L" owner=0x"
           << reinterpret_cast<std::uintptr_t>(GetWindow(window, GW_OWNER))
           << L" class_module=0x" << class_module
           << L" class_module_name="
           << (class_owner ? class_owner->name : L"<unknown>")
           << L" class_error=" << std::dec << class_error
           << L" wndproc=0x" << std::hex << window_proc
           << L" wndproc_module="
           << (proc_owner ? proc_owner->name : L"<unknown>")
           << L" wndproc_error=" << std::dec << proc_error
           << L" class=\"" << class_name << L"\""
           << L" title=\"" << title << L"\"\n";
}

void probe_process(std::wostream& output, DWORD process_id)
{
    const auto threads = process_threads(process_id);
    const auto modules = process_modules(process_id);
    std::set<HWND> windows;
    std::pair<DWORD, std::set<HWND>*> enum_state(process_id, &windows);

    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* state = reinterpret_cast<std::pair<DWORD, std::set<HWND>*>*>(
                parameter);
            collect_window(window, state->first, state->second);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&enum_state));

    for (const DWORD thread_id : threads)
    {
        EnumThreadWindows(
            thread_id,
            [](HWND window, LPARAM parameter) -> BOOL {
                reinterpret_cast<std::set<HWND>*>(parameter)->insert(window);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&windows));
    }

    for (HWND window = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr);
         window != nullptr;
         window = FindWindowExW(HWND_MESSAGE, window, nullptr, nullptr))
    {
        collect_window(window, process_id, &windows);
    }

    output << L"process=" << process_id << L" threads=" << threads.size()
           << L" modules=" << modules.size() << L" windows=" << windows.size()
           << L"\n";
    for (const auto& module : modules)
    {
        if (_wcsicmp(module.name.c_str(), L"casual_ime.dll") == 0)
        {
            output << L"casual_ime_base=0x" << std::hex << module.base
                   << L" size=0x" << module.size << L" path=" << module.path
                   << L"\n";
        }
    }
    for (const HWND window : windows)
        write_window(output, window, modules);
}
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* process_name = argc > 1 ? argv[1] : L"notepad.exe";
    const auto processes = find_processes(process_name);

    wchar_t executable_path[32768] = {};
    GetModuleFileNameW(nullptr,
                       executable_path,
                       static_cast<DWORD>(std::size(executable_path)));
    const std::filesystem::path output_path =
        std::filesystem::path(executable_path).parent_path() /
        L"zime_window_probe.txt";
    std::wofstream file(output_path, std::ios::trunc);
    if (!file)
        return 2;

    file << L"target=" << process_name << L" count=" << processes.size()
         << L"\n";
    for (const DWORD process_id : processes)
        probe_process(file, process_id);
    file.close();

    std::wcout << output_path.wstring() << std::endl;
    return processes.empty() ? 3 : 0;
}
