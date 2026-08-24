#include "broker_client.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kWindowClass[] = L"ZImeBrokerClientSmokeWindow";

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND create_owner_window()
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return nullptr;
    }

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW |
                                      WS_EX_NOACTIVATE |
                                      WS_EX_LAYERED,
                                  kWindowClass,
                                  L"",
                                  WS_OVERLAPPED | WS_VISIBLE,
                                  100,
                                  100,
                                  8,
                                  8,
                                  nullptr,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr);
    if (window)
    {
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
    }
    return window;
}

void pump_messages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND find_owned_window(const wchar_t* expected_class,
                       HWND expected_owner,
                       DWORD expected_process_id)
{
    struct search_state
    {
        const wchar_t* class_name;
        HWND owner;
        DWORD process_id;
        HWND result;
    } state = {expected_class, expected_owner, expected_process_id, nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL
        {
            auto* state = reinterpret_cast<search_state*>(parameter);
            wchar_t class_name[128] = {};
            DWORD process_id = 0;
            GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
            GetWindowThreadProcessId(window, &process_id);
            if (wcscmp(class_name, state->class_name) == 0 &&
                process_id == state->process_id &&
                GetWindow(window, GW_OWNER) == state->owner)
            {
                state->result = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    return state.result;
}

DWORD broker_process_id()
{
    if (!WaitNamedPipeW(zime::broker_protocol::kPipeName, 1000))
        return 0;
    HANDLE pipe = CreateFileW(zime::broker_protocol::kPipeName,
                              FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return 0;
    ULONG process_id = 0;
    GetNamedPipeServerProcessId(pipe, &process_id);
    CloseHandle(pipe);
    return process_id;
}

bool wait_for_condition(const std::function<bool()>& condition, DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline)
    {
        pump_messages();
        if (condition())
            return true;
        Sleep(20);
    }
    pump_messages();
    return condition();
}

bool request_broker_shutdown()
{
    HANDLE shutdown_event = OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        zime::broker_protocol::kShutdownEventName);
    if (!shutdown_event)
        return false;
    const bool signaled = SetEvent(shutdown_event) != FALSE;
    CloseHandle(shutdown_event);
    return signaled;
}

bool start_local_broker()
{
    wchar_t executable_path[32768] = {};
    if (GetModuleFileNameW(nullptr,
                           executable_path,
                           static_cast<DWORD>(std::size(executable_path))) == 0)
    {
        return false;
    }

    std::wstring broker_path(executable_path);
    const std::size_t slash = broker_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return false;
    broker_path.resize(slash + 1);
    broker_path += L"zime_broker.exe";

    std::wstring command_line = L"\"" + broker_path + L"\"";
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(broker_path.c_str(),
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
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool owner_exit_does_not_hold_process()
{
    wchar_t executable_path[32768] = {};
    if (GetModuleFileNameW(nullptr,
                           executable_path,
                           static_cast<DWORD>(std::size(executable_path))) == 0)
    {
        return false;
    }

    std::wstring command_line = L"\"";
    command_line += executable_path;
    command_line += L"\" --owner-exit-child";
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
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 5000);
    if (wait_result == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 100);
    CloseHandle(process.hProcess);
    return wait_result == WAIT_OBJECT_0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--owner-exit-child") == 0)
    {
        // Deliberately bypass C++ destruction and broker_client::Stop(), as
        // old Notepad can terminate its TSF/UI thread without Deactivate().
        auto* client = new broker_client();
        client->Start();
        ExitThread(0);
    }

    wchar_t executable_path[32768] = {};
    if (GetModuleFileNameW(nullptr,
                           executable_path,
                           static_cast<DWORD>(std::size(executable_path))) > 0)
    {
        std::wstring data_path(executable_path);
        const std::size_t slash = data_path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            data_path.resize(slash + 1);
        data_path += L"zime-test-data";
        SetEnvironmentVariableW(L"ZIME_DATA_DIR", data_path.c_str());
    }
    DWORD broker_pid = 0;

    const HWND owner = create_owner_window();
    if (!owner)
    {
        std::wcerr << L"owner window creation failed" << std::endl;
        return 2;
    }

    std::atomic<bool> connected = false;
    std::atomic<int> connection_count = 0;
    std::atomic<int> disconnection_count = 0;
    std::atomic<int> action_type = 0;
    std::atomic<std::uint32_t> action_value = 0;
    std::atomic<int> action_state_value = 0;
    std::atomic<std::uint64_t> config_revision = 0;
    std::atomic<int> config_callback_count = 0;
    std::atomic<std::uint64_t> storage_result_request = 0;
    std::atomic<bool> storage_result_success = false;
    std::uint64_t latest_candidate_generation = 0;
    std::wstring latest_candidate_code;
    int candidate_callback_count = 0;
    broker_client client;
    client.SetCallbacks(
        [&](zime::broker_protocol::ui_action_type type,
            std::uint32_t value,
            POINT point)
        {
            action_value.store(value);
            action_state_value.store(point.x);
            action_type.store(static_cast<int>(type));
        },
        [&](bool is_connected)
        {
            connected.store(is_connected);
            if (is_connected)
                ++connection_count;
            else
                ++disconnection_count;
        });
    client.SetConfigStateCallback(
        [&](const zime::broker_protocol::config_state& state)
        {
            config_revision.store(state.revision);
            ++config_callback_count;
        });
    client.SetStorageResultCallback(
        [&](zime::broker_protocol::storage_operation,
            std::uint64_t request_id,
            bool success,
            const std::wstring&)
        {
            storage_result_success.store(success);
            storage_result_request.store(request_id);
        });
    client.SetCandidateResultCallback(
        [&](std::uint64_t generation,
            const std::wstring& code,
            const std::vector<std::wstring>&,
            const std::vector<std::wstring>&,
            const std::vector<bool>&,
            std::uint32_t,
            std::uint64_t)
        {
            latest_candidate_generation = generation;
            latest_candidate_code = code;
            ++candidate_callback_count;
        });
    client.Start();
    client.SendContextSnapshot(owner);
    RECT anchor = {-32000, -32000, -31998, -31980};
    client.SendCandidateState(owner,
                              &anchor,
                              true,
                              true,
                              L"test",
                              0,
                              9,
                              0,
                              100);

    HWND candidate_window = nullptr;
    const bool candidate_ready = wait_for_condition(
        [&]()
        {
            if (connected.load() && broker_pid == 0)
                broker_pid = broker_process_id();
            candidate_window = find_owned_window(
                L"SimpleTSFCandidateWindow", nullptr, broker_pid);
            return connected.load() &&
                config_revision.load() != 0 &&
                client.HasStorageWriter() &&
                candidate_window &&
                IsWindowVisible(candidate_window);
        },
        5000);
    if (!candidate_ready)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"production client did not show candidate UI" << std::endl;
        return 3;
    }

    RECT stable_candidate_rect = {};
    GetWindowRect(candidate_window, &stable_candidate_rect);
    RECT deferred_anchor = {
        stable_candidate_rect.left + 360,
        stable_candidate_rect.top + 240,
        stable_candidate_rect.left + 362,
        stable_candidate_rect.top + 260};
    client.SendCandidateState(owner,
                              &deferred_anchor,
                              true,
                              true,
                              L"a",
                              0,
                              9,
                              0,
                              100,
                              true);
    const std::uint64_t deferred_query_generation =
        client.RequestCandidates(L"a", false);
    const bool deferred_result_ready = deferred_query_generation != 0 &&
        wait_for_condition(
            [&]()
            {
                return latest_candidate_generation == deferred_query_generation;
            },
            1500);
    RECT pending_candidate_rect = {};
    GetWindowRect(candidate_window, &pending_candidate_rect);
    if (!deferred_result_ready ||
        !IsWindowVisible(candidate_window) ||
        pending_candidate_rect.left != stable_candidate_rect.left ||
        pending_candidate_rect.top != stable_candidate_rect.top)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"pending candidate presentation changed the stable window"
                   << std::endl;
        return 16;
    }

    client.SendCandidateState(owner,
                              &deferred_anchor,
                              true,
                              true,
                              L"a",
                              0,
                              9,
                              0,
                              100);
    const bool deferred_presentation_applied = wait_for_condition(
        [&]()
        {
            RECT rect = {};
            GetWindowRect(candidate_window, &rect);
            return IsWindowVisible(candidate_window) &&
                rect.left == deferred_anchor.left &&
                rect.top == deferred_anchor.bottom + 2;
        },
        1500);
    if (!deferred_presentation_applied)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"ready candidate presentation was not applied"
                   << std::endl;
        return 17;
    }

    const std::uint64_t storage_request = client.SendStorageMutation(
        zime::broker_protocol::storage_operation::record_selection,
        L"zimeprob",
        L"broker-client-storage-smoke",
        true);
    const bool storage_ready = storage_request != 0 && wait_for_condition(
        [&]()
        {
            return storage_result_request.load() == storage_request &&
                storage_result_success.load();
        },
        3000);
    if (!storage_ready)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"production client storage request failed" << std::endl;
        return 7;
    }

    latest_candidate_generation = 0;
    latest_candidate_code.clear();
    candidate_callback_count = 0;
    for (int index = 0; index < 500; ++index)
    {
        client.RequestCandidates(
            (index % 3) == 0 ? L"t" : (index % 3) == 1 ? L"te" : L"tes",
            true);
    }
    const ULONGLONG rapid_query_started = GetTickCount64();
    const std::uint64_t rapid_query_generation =
        client.RequestCandidates(L"test", true);
    const bool rapid_query_ready = rapid_query_generation != 0 &&
        wait_for_condition(
            [&]()
            {
                return latest_candidate_generation == rapid_query_generation &&
                    latest_candidate_code == L"test";
            },
            1500);
    const ULONGLONG rapid_query_elapsed =
        GetTickCount64() - rapid_query_started;
    if (!rapid_query_ready || candidate_callback_count != 1)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"rapid candidate queries were not coalesced; elapsed="
                   << rapid_query_elapsed << L"ms callbacks="
                   << candidate_callback_count << std::endl;
        return 10;
    }

    latest_candidate_generation = 0;
    latest_candidate_code.clear();
    candidate_callback_count = 0;
    const ULONGLONG layout_query_started = GetTickCount64();
    const std::uint64_t layout_query_generation =
        client.RequestCandidates(L"a", false);
    for (int update = 0; update < 500; ++update)
    {
        RECT layout_anchor = {
            220 + update,
            260 + update,
            222 + update,
            280 + update};
        client.SendCandidateState(owner,
                                  &layout_anchor,
                                  true,
                                  true,
                                  L"a",
                                  0,
                                  9,
                                  0,
                                  100);
    }
    const bool layout_query_ready = layout_query_generation != 0 &&
        wait_for_condition(
            [&]()
            {
                return latest_candidate_generation == layout_query_generation &&
                    latest_candidate_code == L"a";
            },
            1500);
    const ULONGLONG layout_query_elapsed =
        GetTickCount64() - layout_query_started;
    if (!layout_query_ready || layout_query_elapsed > 200)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"layout updates starved candidate query; elapsed="
                   << layout_query_elapsed << L"ms" << std::endl;
        return 16;
    }

    LARGE_INTEGER performance_frequency = {};
    QueryPerformanceFrequency(&performance_frequency);
    std::vector<double> critical_query_times;
    constexpr int kTypingRounds = 40;
    critical_query_times.reserve(kTypingRounds);
    for (int round = 0; round < kTypingRounds; ++round)
    {
        const auto queue_layout_burst = [&](const wchar_t* code)
        {
            for (int update = 0; update < 16; ++update)
            {
                RECT typing_anchor = {
                    120 + update,
                    160 + update,
                    122 + update,
                    180 + update};
                client.SendCandidateState(owner,
                                          &typing_anchor,
                                          true,
                                          true,
                                          code,
                                          0,
                                          9,
                                          0,
                                          100);
            }
        };

        client.RequestCandidates(L"a", false);
        queue_layout_burst(L"a");
        Sleep(3);
        client.RequestCandidates(L"aa", false);
        queue_layout_burst(L"aa");
        Sleep(3);
        client.RequestCandidates(L"aaa", false);
        queue_layout_burst(L"aaa");
        Sleep(3);

        std::vector<std::wstring> typing_candidates;
        std::vector<std::wstring> typing_view_texts;
        std::vector<bool> typing_pinyin;
        std::uint32_t typing_flags = 0;
        std::uint64_t typing_config_revision = 0;
        LARGE_INTEGER started = {};
        LARGE_INTEGER finished = {};
        QueryPerformanceCounter(&started);
        const bool typing_query_ready = client.QueryCandidatesSync(
            L"aaad",
            &typing_candidates,
            &typing_view_texts,
            &typing_pinyin,
            100,
            true,
            &typing_flags,
            &typing_config_revision);
        QueryPerformanceCounter(&finished);
        if (!typing_query_ready)
        {
            client.Stop();
            DestroyWindow(owner);
            std::wcerr << L"critical candidate query timed out" << std::endl;
            return 11;
        }
        if ((typing_flags & zime::broker_protocol::candidate_result_auto_commit_first) == 0 ||
            typing_config_revision == 0)
        {
            client.Stop();
            DestroyWindow(owner);
            std::wcerr << L"critical candidate query lost decision metadata"
                       << std::endl;
            return 13;
        }
        critical_query_times.push_back(
            static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 /
            static_cast<double>(performance_frequency.QuadPart));
    }
    std::sort(critical_query_times.begin(), critical_query_times.end());
    double critical_total = 0.0;
    for (const double elapsed : critical_query_times)
        critical_total += elapsed;
    const double critical_average =
        critical_total / critical_query_times.size();
    const double critical_p95 = critical_query_times[
        static_cast<std::size_t>(critical_query_times.size() * 95 / 100)];
    if (critical_average > 5.0 || critical_p95 > 15.0)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"critical candidate query is too slow; average="
                   << critical_average << L"ms p95=" << critical_p95
                   << L"ms" << std::endl;
        return 12;
    }

    std::vector<std::wstring> queried_candidates;
    std::vector<std::wstring> queried_view_texts;
    std::vector<bool> queried_pinyin;
    if (!client.QueryCandidatesSync(L"test",
                                    &queried_candidates,
                                    &queried_view_texts,
                                    &queried_pinyin,
                                    3000))
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"production client candidate query failed" << std::endl;
        return 8;
    }

    SendMessageW(candidate_window, WM_LBUTTONUP, 0, MAKELPARAM(10, 40));
    const bool candidate_action = wait_for_condition(
        [&]()
        {
            return action_type.load() == static_cast<int>(
                       zime::broker_protocol::ui_action_type::candidate_select) &&
                action_value.load() == 0;
        },
        3000);
    if (!candidate_action)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"production client did not receive candidate action" << std::endl;
        return 4;
    }

    // A synchronous fourth-code query can reach the Broker UI before the
    // matching state/anchor frame.  The Broker must retain that result and
    // attach it when the state frame arrives instead of showing an empty UI.
    std::vector<std::wstring> cached_candidates;
    std::vector<std::wstring> cached_view_texts;
    std::vector<bool> cached_pinyin;
    if (!client.QueryCandidatesSync(L"a",
                                    &cached_candidates,
                                    &cached_view_texts,
                                    &cached_pinyin,
                                    3000) ||
        cached_candidates.empty())
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"candidate cache race setup failed" << std::endl;
        return 14;
    }
    action_type.store(0);
    action_value.store(0);
    client.SendCandidateState(owner,
                              &anchor,
                              true,
                              true,
                              L"a",
                              0,
                              9,
                              0,
                              100);
    const bool cached_candidate_action = wait_for_condition(
        [&]()
        {
            SendMessageW(candidate_window,
                         WM_LBUTTONUP,
                         0,
                         MAKELPARAM(10, 40));
            return action_type.load() == static_cast<int>(
                       zime::broker_protocol::ui_action_type::candidate_select) &&
                action_value.load() == 0;
        },
        3000);
    if (!cached_candidate_action)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"Broker lost query result received before UI state"
                   << std::endl;
        return 15;
    }

    client.SendCandidateState(owner,
                              nullptr,
                              false,
                              true,
                              L"",
                              0,
                              9,
                              0,
                              100);
    action_type.store(0);
    const std::uint32_t status_flags =
        zime::broker_protocol::status_custom_ui_allowed |
        zime::broker_protocol::status_chinese_mode;
    client.SendStatusState(owner, true, status_flags, 0, 0, 100, 2);

    HWND status_window = nullptr;
    const bool status_ready = wait_for_condition(
        [&]()
        {
            status_window = find_owned_window(
                L"SimpleTSFStatusWindow", owner, broker_pid);
            return status_window && IsWindowVisible(status_window);
        },
        3000);
    if (!status_ready)
    {
        client.Stop();
        DestroyWindow(owner);
        std::wcerr << L"production client did not show status UI" << std::endl;
        return 5;
    }

    SendMessageW(status_window, WM_LBUTTONDOWN, 0, MAKELPARAM(50, 10));
    const bool status_action = wait_for_condition(
        [&]()
        {
            return action_type.load() == static_cast<int>(
                       zime::broker_protocol::ui_action_type::status_change) &&
                action_value.load() == 1 &&
                action_state_value.load() == 0;
        },
            3000);

    action_type.store(0);
    const std::uint64_t punctuation_config_revision = config_revision.load();
    SendMessageW(status_window, WM_LBUTTONDOWN, 0, MAKELPARAM(90, 10));
    const bool punctuation_action = wait_for_condition(
        [&]()
        {
            return action_type.load() == static_cast<int>(
                       zime::broker_protocol::ui_action_type::status_change) &&
                action_value.load() == 2 &&
                action_state_value.load() == 1;
        },
        3000);
    Sleep(100);
    const bool punctuation_stayed_session_local =
        config_revision.load() == punctuation_config_revision;

    client.SendStatusState(owner, false, status_flags, 0, 0, 100, 2);
    const int previous_connections = connection_count.load();
    const int previous_disconnections = disconnection_count.load();
    const int previous_config_callbacks = config_callback_count.load();
    const DWORD previous_broker_pid = broker_pid;
    const bool shutdown_requested = request_broker_shutdown();
    const bool disconnected_after_restart = shutdown_requested &&
        wait_for_condition(
            [&]()
            {
                return disconnection_count.load() > previous_disconnections;
            },
            5000);
    bool local_start_attempted = false;
    const bool broker_started = disconnected_after_restart &&
        wait_for_condition(
            [&]()
            {
                if (broker_process_id() != 0)
                    return true;
                if (!local_start_attempted)
                {
                    local_start_attempted = true;
                    start_local_broker();
                }
                return false;
            },
            5000);
    const bool reconnected = broker_started && wait_for_condition(
        [&]()
        {
            broker_pid = broker_process_id();
            return connected.load() &&
                connection_count.load() > previous_connections &&
                config_callback_count.load() > previous_config_callbacks &&
                broker_pid != 0 && broker_pid != previous_broker_pid;
        },
        5000);

    std::vector<std::wstring> restarted_candidates;
    std::vector<std::wstring> restarted_view_texts;
    std::vector<bool> restarted_pinyin;
    const bool restarted_query = reconnected && client.QueryCandidatesSync(
        L"test",
        &restarted_candidates,
        &restarted_view_texts,
        &restarted_pinyin,
        3000);
    constexpr int kHostQuitCode = 73;
    PostQuitMessage(kHostQuitCode);
    client.Stop();
    MSG quit_message = {};
    const bool host_quit_preserved =
        PeekMessageW(&quit_message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) &&
        quit_message.message == WM_QUIT &&
        static_cast<int>(quit_message.wParam) == kHostQuitCode;
    const bool owner_exit_cleanup = owner_exit_does_not_hold_process();
    DestroyWindow(owner);
    if (!status_action || !punctuation_action ||
        !punctuation_stayed_session_local)
    {
        std::wcerr << L"production client did not receive status action" << std::endl;
        return 6;
    }
    if (!reconnected || !restarted_query)
    {
        std::wcerr << L"production client did not recover from broker restart" << std::endl;
        return 9;
    }
    if (!owner_exit_cleanup)
    {
        std::wcerr << L"Broker client held process after owner thread exit"
                   << std::endl;
        return 16;
    }
    if (!host_quit_preserved)
    {
        std::wcerr << L"Broker client consumed the host WM_QUIT message"
                   << std::endl;
        return 18;
    }

    std::wcout << L"broker client passed; broker_pid=" << broker_pid
               << L" rapid_query=" << rapid_query_elapsed << L"ms"
               << L" layout_query=" << layout_query_elapsed << L"ms"
               << L" critical_average=" << critical_average << L"ms"
               << L" critical_p95=" << critical_p95 << L"ms"
               << std::endl;
    return 0;
}
