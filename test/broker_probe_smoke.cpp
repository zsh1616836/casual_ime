#include "broker_protocol.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kSmokeWindowClass[] = L"ZImeBrokerSmokeWindow";

LRESULT CALLBACK smoke_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND create_smoke_window()
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = smoke_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kSmokeWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return nullptr;
    }

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW |
                                      WS_EX_NOACTIVATE |
                                      WS_EX_LAYERED,
                                  kSmokeWindowClass,
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

HANDLE connect_to_broker()
{
    if (!WaitNamedPipeW(zime::broker_protocol::kPipeName, 1000))
        return INVALID_HANDLE_VALUE;
    return CreateFileW(zime::broker_protocol::kPipeName,
                       FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                       0,
                       nullptr,
                       OPEN_EXISTING,
                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                       nullptr);
}

template <typename Request, typename Response>
bool transact(HANDLE pipe, const Request& request, Response* response)
{
    DWORD transferred = 0;
    auto writable_request = request;
    return response &&
        WriteFile(pipe,
                  &writable_request,
                  sizeof(writable_request),
                  &transferred,
                  nullptr) &&
        transferred == sizeof(writable_request) &&
        ReadFile(pipe,
                 response,
                 sizeof(*response),
                 &transferred,
                 nullptr) &&
        transferred == sizeof(*response);
}

HWND find_owned_window(const wchar_t* expected_class,
                       HWND expected_owner,
                       DWORD expected_process_id)
{
    struct search_state
    {
        HWND owner;
        DWORD process_id;
        const wchar_t* class_name;
        HWND result;
    } state = {expected_owner, expected_process_id, expected_class, nullptr};
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

zime::broker_protocol::status_state make_status_state(
    std::uint64_t connection_id,
    std::uint64_t generation,
    HWND owner,
    bool visible)
{
    using namespace zime::broker_protocol;
    status_state state = {};
    state.header = make_header(message_type::status_state,
                               sizeof(state),
                               generation + 20,
                               connection_id);
    state.generation = generation;
    state.owner_hwnd = reinterpret_cast<std::uintptr_t>(owner);
    state.flags = status_custom_ui_allowed |
        status_chinese_mode;
    if (visible)
        state.flags |= status_visible;
    state.ui_font_percent = 100;
    state.candidate_sort_mode = 2;
    return state;
}

std::vector<std::uint8_t> make_candidate_state(
    std::uint64_t connection_id,
    std::uint64_t generation,
    HWND view,
    bool visible)
{
    using namespace zime::broker_protocol;
    const std::wstring composition = L"test";
    const std::vector<std::wstring> candidates = {L"candidate-one", L"candidate-two"};
    std::wstring strings = composition;
    std::vector<candidate_string_record> records;
    for (const auto& candidate : candidates)
    {
        records.push_back({static_cast<std::uint32_t>(strings.size()),
                           static_cast<std::uint32_t>(candidate.size())});
        strings += candidate;
    }

    const std::size_t size = sizeof(candidate_state) +
        records.size() * sizeof(candidate_string_record) +
        strings.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(size);
    auto* state = reinterpret_cast<candidate_state*>(bytes.data());
    *state = {};
    state->header = make_header(message_type::candidate_state,
                                static_cast<std::uint32_t>(size),
                                generation + 10,
                                connection_id);
    state->generation = generation;
    state->view_hwnd = reinterpret_cast<std::uintptr_t>(view);
    state->owner_hwnd = reinterpret_cast<std::uintptr_t>(view);
    state->anchor = {-32000, -32000, -31998, -31980};
    state->flags = candidate_custom_ui_allowed | candidate_anchor_valid;
    if (visible)
        state->flags |= candidate_visible;
    state->page_size = 9;
    state->ui_font_percent = 100;
    state->composition_chars = static_cast<std::uint32_t>(composition.size());
    state->candidate_count = static_cast<std::uint32_t>(records.size());
    state->string_chars = static_cast<std::uint32_t>(strings.size());

    std::uint8_t* cursor = bytes.data() + sizeof(candidate_state);
    std::memcpy(cursor,
                records.data(),
                records.size() * sizeof(candidate_string_record));
    cursor += records.size() * sizeof(candidate_string_record);
    std::memcpy(cursor, strings.data(), strings.size() * sizeof(wchar_t));
    return bytes;
}

bool write_bytes(HANDLE pipe, std::vector<std::uint8_t>* bytes)
{
    if (!bytes)
        return false;
    DWORD transferred = 0;
    return WriteFile(pipe,
                     bytes->data(),
                     static_cast<DWORD>(bytes->size()),
                     &transferred,
                     nullptr) &&
        transferred == bytes->size();
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
}

int wmain()
{
    using namespace zime::broker_protocol;
    HANDLE pipe = connect_to_broker();
    if (pipe == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"broker pipe unavailable: " << GetLastError() << std::endl;
        return 1;
    }

    const DWORD process_id = GetCurrentProcessId();
    DWORD session_id = 0;
    ProcessIdToSessionId(process_id, &session_id);
    hello hello_request = {};
    hello_request.header = make_header(message_type::hello, sizeof(hello_request), 1);
    hello_request.reported_process_id = process_id;
    hello_request.reported_thread_id = GetCurrentThreadId();
    hello_request.reported_session_id = session_id;
    hello_request.pointer_size = sizeof(void*);
    hello_request.client_nonce = 0x123456789abcdef0ull;
    hello_request.capabilities = capability_context_snapshot |
        capability_owned_windows |
        capability_candidate_window |
        capability_ui_actions |
        capability_status_window |
        capability_storage_writer;

    hello_result hello_reply = {};
    if (!transact(pipe, hello_request, &hello_reply) ||
        !has_valid_envelope(hello_reply.header,
                            message_type::hello_result,
                            sizeof(hello_reply)) ||
        hello_reply.status != status_code::ok ||
        hello_reply.actual_client_process_id != process_id ||
        hello_reply.actual_client_session_id != session_id ||
        hello_reply.echoed_client_nonce != hello_request.client_nonce ||
        hello_reply.header.connection_id == 0)
    {
        std::wcerr << L"broker hello failed status="
                   << static_cast<DWORD>(hello_reply.status) << std::endl;
        CloseHandle(pipe);
        return 2;
    }

    config_state initial_config = {};
    DWORD config_bytes = 0;
    if (!ReadFile(pipe,
                  &initial_config,
                  sizeof(initial_config),
                  &config_bytes,
                  nullptr) ||
        config_bytes != sizeof(initial_config) ||
        !has_valid_envelope(initial_config.header,
                            message_type::config_state,
                            sizeof(initial_config)) ||
        initial_config.header.connection_id !=
            hello_reply.header.connection_id ||
        initial_config.revision == 0)
    {
        std::wcerr << L"initial config snapshot missing" << std::endl;
        CloseHandle(pipe);
        return 13;
    }

    const std::wstring storage_code = L"zimeprob";
    const std::wstring storage_text = L"broker-storage-smoke";
    const std::size_t mutation_size = sizeof(storage_mutation) +
        (storage_code.size() + storage_text.size()) * sizeof(wchar_t);
    std::vector<std::uint8_t> mutation_bytes(mutation_size);
    auto* mutation = reinterpret_cast<storage_mutation*>(mutation_bytes.data());
    *mutation = {};
    mutation->header = make_header(message_type::storage_mutation,
                                   static_cast<std::uint32_t>(mutation_size),
                                   5,
                                   hello_reply.header.connection_id);
    mutation->operation = storage_operation::record_selection;
    mutation->flags = storage_expects_result;
    mutation->code_chars = static_cast<std::uint32_t>(storage_code.size());
    mutation->text_chars = static_cast<std::uint32_t>(storage_text.size());
    auto* mutation_strings = reinterpret_cast<wchar_t*>(
        mutation_bytes.data() + sizeof(storage_mutation));
    std::memcpy(mutation_strings,
                storage_code.data(),
                storage_code.size() * sizeof(wchar_t));
    std::memcpy(mutation_strings + storage_code.size(),
                storage_text.data(),
                storage_text.size() * sizeof(wchar_t));
    storage_result mutation_result = {};
    DWORD mutation_result_bytes = 0;
    if (!write_bytes(pipe, &mutation_bytes) ||
        !ReadFile(pipe,
                  &mutation_result,
                  sizeof(mutation_result),
                  &mutation_result_bytes,
                  nullptr) ||
        mutation_result_bytes != sizeof(mutation_result) ||
        !has_valid_envelope(mutation_result.header,
                            message_type::storage_result,
                            sizeof(mutation_result)) ||
        mutation_result.header.request_id != mutation->header.request_id ||
        mutation_result.header.connection_id !=
            hello_reply.header.connection_id ||
        mutation_result.operation != storage_operation::record_selection ||
        mutation_result.status != status_code::ok)
    {
        std::wcerr << L"storage mutation failed" << std::endl;
        CloseHandle(pipe);
        return 14;
    }

    dictionary_state dictionary_update = {};
    DWORD dictionary_update_bytes = 0;
    if (!ReadFile(pipe,
                  &dictionary_update,
                  sizeof(dictionary_update),
                  &dictionary_update_bytes,
                  nullptr) ||
        dictionary_update_bytes != sizeof(dictionary_update) ||
        !has_valid_envelope(dictionary_update.header,
                            message_type::dictionary_state,
                            sizeof(dictionary_update)) ||
        dictionary_update.revision == 0)
    {
        std::wcerr << L"dictionary revision broadcast failed" << std::endl;
        CloseHandle(pipe);
        return 15;
    }

    const HWND view = create_smoke_window();
    if (!view)
    {
        std::wcerr << L"smoke window creation failed: " << GetLastError() << std::endl;
        CloseHandle(pipe);
        return 3;
    }

    context_snapshot request = {};
    request.header = make_header(message_type::context_snapshot,
                                 sizeof(request),
                                 2,
                                 hello_reply.header.connection_id);
    request.reported_process_id = process_id;
    request.reported_thread_id = GetCurrentThreadId();
    request.reported_session_id = session_id;
    request.view_hwnd = reinterpret_cast<std::uintptr_t>(view);
    request.root_hwnd = reinterpret_cast<std::uintptr_t>(GetAncestor(view, GA_ROOT));
    request.foreground_hwnd = reinterpret_cast<std::uintptr_t>(GetForegroundWindow());

    context_result result = {};
    const bool transacted = transact(pipe, request, &result);

    const bool valid = transacted &&
        has_valid_envelope(result.header,
                           message_type::context_result,
                           sizeof(result)) &&
        result.header.request_id == request.header.request_id &&
        result.header.connection_id == hello_reply.header.connection_id;
    if (!valid ||
        result.status != status_code::ok ||
        !result.owner_window_created ||
        !result.owner_relationship_matches ||
        !result.owner_window_visible)
    {
        std::wcerr << L"context rejected: valid=" << valid
                   << L" status=" << static_cast<DWORD>(result.status)
                   << L" owner_created=" << result.owner_window_created
                   << L" owner_match=" << result.owner_relationship_matches
                   << L" visible=" << result.owner_window_visible
                   << L" error=" << result.win32_error << std::endl;
        return 4;
    }

    constexpr std::uint64_t candidate_generation = 42;
    auto candidate_frame = make_candidate_state(
        hello_reply.header.connection_id, candidate_generation, view, true);
    if (!write_bytes(pipe, &candidate_frame))
    {
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"candidate state write failed" << std::endl;
        return 5;
    }

    HWND candidate_window = nullptr;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        pump_messages();
        Sleep(20);
        candidate_window = find_owned_window(
            L"SimpleTSFCandidateWindow", nullptr, hello_reply.server_process_id);
        if (candidate_window && IsWindowVisible(candidate_window))
            break;
    }
    if (!candidate_window || !IsWindowVisible(candidate_window))
    {
        const LONG_PTR owner_style = GetWindowLongPtrW(view, GWL_STYLE);
        const BOOL owner_visible = IsWindowVisible(view);
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"broker candidate window was not created; owner_visible="
                   << owner_visible
                   << L" owner_style=0x" << std::hex << owner_style
                   << L" candidate=0x" << reinterpret_cast<std::uintptr_t>(candidate_window)
                   << std::dec << std::endl;
        return 6;
    }

    SendMessageW(candidate_window, WM_LBUTTONUP, 0, MAKELPARAM(10, 40));
    ui_action action = {};
    DWORD transferred = 0;
    if (!ReadFile(pipe, &action, sizeof(action), &transferred, nullptr) ||
        transferred != sizeof(action) ||
        !has_valid_envelope(action.header,
                            message_type::ui_action,
                            sizeof(action)) ||
        action.header.connection_id != hello_reply.header.connection_id ||
        action.generation != candidate_generation ||
        action.action != ui_action_type::candidate_select ||
        action.value != 0)
    {
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"candidate click action was not returned" << std::endl;
        return 7;
    }

    auto hide_frame = make_candidate_state(
        hello_reply.header.connection_id, candidate_generation + 1, view, false);
    write_bytes(pipe, &hide_frame);

    constexpr std::uint64_t status_generation = 77;
    status_state status_frame = make_status_state(
        hello_reply.header.connection_id, status_generation, view, true);
    DWORD status_written = 0;
    if (!WriteFile(pipe,
                   &status_frame,
                   sizeof(status_frame),
                   &status_written,
                   nullptr) ||
        status_written != sizeof(status_frame))
    {
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"status state write failed" << std::endl;
        return 8;
    }

    HWND status_window = nullptr;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        pump_messages();
        Sleep(20);
        status_window = find_owned_window(
            L"SimpleTSFStatusWindow", view, hello_reply.server_process_id);
        if (status_window && IsWindowVisible(status_window))
            break;
    }
    if (!status_window || !IsWindowVisible(status_window))
    {
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"broker status window was not created" << std::endl;
        return 9;
    }

    SendMessageW(status_window, WM_LBUTTONDOWN, 0, MAKELPARAM(50, 10));
    action = {};
    transferred = 0;
    if (!ReadFile(pipe, &action, sizeof(action), &transferred, nullptr) ||
        transferred != sizeof(action) ||
        action.header.connection_id != hello_reply.header.connection_id ||
        action.generation != status_generation ||
        action.action != ui_action_type::status_change ||
        action.value != 1 ||
        action.screen_x != 0)
    {
        CloseHandle(pipe);
        DestroyWindow(view);
        std::wcerr << L"status change action was not returned" << std::endl;
        return 10;
    }

    status_frame = make_status_state(
        hello_reply.header.connection_id, status_generation + 1, view, false);
    WriteFile(pipe,
              &status_frame,
              sizeof(status_frame),
              &status_written,
              nullptr);
    CloseHandle(pipe);
    DestroyWindow(view);

    HANDLE forged_pipe = connect_to_broker();
    hello forged_request = hello_request;
    forged_request.header.request_id = 3;
    forged_request.reported_process_id = process_id + 1;
    hello_result forged_reply = {};
    if (forged_pipe == INVALID_HANDLE_VALUE ||
        !transact(forged_pipe, forged_request, &forged_reply) ||
        forged_reply.status != status_code::identity_mismatch)
    {
        if (forged_pipe != INVALID_HANDLE_VALUE)
            CloseHandle(forged_pipe);
        std::wcerr << L"forged process identity was not rejected" << std::endl;
        return 11;
    }
    CloseHandle(forged_pipe);

    HANDLE version_pipe = connect_to_broker();
    hello version_request = hello_request;
    version_request.header.request_id = 4;
    version_request.header.major_version = kMajorVersion + 1;
    hello_result version_reply = {};
    if (version_pipe == INVALID_HANDLE_VALUE ||
        !transact(version_pipe, version_request, &version_reply) ||
        version_reply.status != status_code::unsupported_version)
    {
        if (version_pipe != INVALID_HANDLE_VALUE)
            CloseHandle(version_pipe);
        std::wcerr << L"future protocol version was not rejected" << std::endl;
        return 12;
    }
    CloseHandle(version_pipe);

    std::wcout << L"broker protocol passed; server_pid="
               << hello_reply.server_process_id
               << L" client_pid=" << hello_reply.actual_client_process_id
               << L" connection=" << hello_reply.header.connection_id
               << std::endl;
    return 0;
}
