#include "broker_protocol.h"
#include "broker_log.h"
#include "broker_ui.h"
#include "broker_storage.h"
#include "globals.h"
#include "perf_trace.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr wchar_t kOwnedWindowClass[] = L"ZImeBrokerOwnedWindow";
constexpr DWORD kIdleConnectionTimeoutMs = 45000;
constexpr unsigned kMaxConnections = 32;

std::atomic<unsigned> g_connection_count = 0;
broker_storage g_storage;

void configure_process_dpi_awareness()
{
    using SetProcessDpiAwarenessContextFn =
        BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_context =
        reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"),
            "SetProcessDpiAwarenessContext"));
    if (set_context &&
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        broker_debug_log(L"dpi awareness=per-monitor-v2");
        return;
    }

    const DWORD context_error = GetLastError();
    if (context_error == ERROR_ACCESS_DENIED)
    {
        broker_debug_log(L"dpi awareness already configured");
        return;
    }
    if (SetProcessDPIAware())
    {
        broker_debug_log(L"dpi awareness=system fallback context_error=%lu",
                         context_error);
        return;
    }
    broker_error_log(L"dpi awareness configuration failed context_error=%lu fallback_error=%lu",
                     context_error,
                     GetLastError());
}

std::vector<BYTE> token_user_sid(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (size == 0 ||
        !GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    {
        return {};
    }
    return buffer;
}

std::wstring current_user_sid_string()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};

    const std::vector<BYTE> buffer = token_user_sid(token);
    CloseHandle(token);
    if (buffer.empty())
        return {};

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid_string = nullptr;
    std::wstring result;
    if (ConvertSidToStringSidW(token_user->User.Sid, &sid_string))
    {
        result = sid_string;
        LocalFree(sid_string);
    }
    return result;
}

bool process_belongs_to_current_user(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process)
        return false;

    HANDLE client_token = nullptr;
    const BOOL opened_client_token =
        OpenProcessToken(process, TOKEN_QUERY, &client_token);
    CloseHandle(process);
    if (!opened_client_token)
        return false;

    HANDLE server_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &server_token))
    {
        CloseHandle(client_token);
        return false;
    }

    const std::vector<BYTE> client_sid = token_user_sid(client_token);
    const std::vector<BYTE> server_sid = token_user_sid(server_token);
    CloseHandle(server_token);
    CloseHandle(client_token);
    if (client_sid.empty() || server_sid.empty())
        return false;

    const auto* client_user = reinterpret_cast<const TOKEN_USER*>(client_sid.data());
    const auto* server_user = reinterpret_cast<const TOKEN_USER*>(server_sid.data());
    return EqualSid(client_user->User.Sid, server_user->User.Sid) != FALSE;
}

#ifndef NDEBUG
std::wstring process_image_path(DWORD process_id)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process)
        return {};

    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    return queried ? std::wstring(buffer.data(), size) : std::wstring();
}
#endif

PSECURITY_DESCRIPTOR create_pipe_security_descriptor()
{
    const std::wstring user_sid = current_user_sid_string();
    if (user_sid.empty())
        return nullptr;

    const std::wstring sddl =
        L"D:P"
        L"(A;;GA;;;SY)"
        L"(A;;GA;;;" + user_sid + L")"
        // AppContainer clients open the duplex pipe with GENERIC_READ |
        // GENERIC_WRITE, so grant the complete generic mappings rather than
        // a partial file-specific mask that fails the access check.
        L"(A;;GRGW;;;S-1-15-2-1)"
        L"(A;;GRGW;;;S-1-15-2-2)"
        // Permit low-integrity AppContainer clients to write to the pipe;
        // the DACL and the server-side user/session checks remain authoritative.
        L"S:(ML;;NW;;;LW)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        return nullptr;
    }
    return descriptor;
}

LRESULT CALLBACK owned_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_owned_window_class(HINSTANCE instance)
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = owned_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kOwnedWindowClass;
    return RegisterClassExW(&window_class) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::uint64_t random_u64()
{
    std::uint64_t value = 0;
    if (BCryptGenRandom(nullptr,
                        reinterpret_cast<PUCHAR>(&value),
                        sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        value = (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^
            GetTickCount64();
    }
    return value ? value : 1;
}

bool overlapped_io(HANDLE pipe,
                   bool write,
                   void* buffer,
                   DWORD buffer_size,
                   DWORD* transferred,
                   DWORD timeout_ms)
{
    if (transferred)
        *transferred = 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        return false;

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;
    DWORD byte_count = 0;
    const BOOL started = write
        ? WriteFile(pipe, buffer, buffer_size, &byte_count, &overlapped)
        : ReadFile(pipe, buffer, buffer_size, &byte_count, &overlapped);

    bool success = false;
    if (started)
    {
        success = true;
    }
    else if (GetLastError() == ERROR_IO_PENDING)
    {
        const DWORD wait_result = WaitForSingleObject(event, timeout_ms);
        if (wait_result == WAIT_OBJECT_0)
            success = GetOverlappedResult(pipe, &overlapped, &byte_count, FALSE) != FALSE;
        else
        {
            CancelIoEx(pipe, &overlapped);
            WaitForSingleObject(event, 50);
        }
    }

    if (success && transferred)
        *transferred = byte_count;
    CloseHandle(event);
    return success;
}

bool read_message(HANDLE pipe,
                  std::array<std::uint8_t, zime::broker_protocol::kMaxMessageSize>* buffer,
                  DWORD* size,
                  DWORD timeout_ms)
{
    if (!buffer || !size ||
        !overlapped_io(pipe,
                       false,
                       buffer->data(),
                       static_cast<DWORD>(buffer->size()),
                       size,
                       timeout_ms))
    {
        return false;
    }
    if (*size < sizeof(zime::broker_protocol::message_header))
        return false;

    const auto* header = reinterpret_cast<const zime::broker_protocol::message_header*>(
        buffer->data());
    return header->magic == zime::broker_protocol::kMagic &&
        header->size == *size &&
        header->size <= zime::broker_protocol::kMaxMessageSize;
}

template <typename T>
bool write_message(HANDLE pipe, T* message)
{
    DWORD written = 0;
    return overlapped_io(pipe,
                         true,
                         message,
                         sizeof(T),
                         &written,
                         zime::broker_protocol::kIoTimeoutMs) &&
        written == sizeof(T);
}

bool connect_pipe(HANDLE pipe)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        return false;

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;
    BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (!connected)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            connected = TRUE;
        }
        else if (error == ERROR_IO_PENDING)
        {
            DWORD transferred = 0;
            connected = WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 &&
                GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
        }
    }
    CloseHandle(event);
    return connected != FALSE;
}

DWORD window_process_id(HWND hwnd)
{
    DWORD pid = 0;
    if (hwnd)
        GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

HWND foreground_root_for_process(DWORD process_id)
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return nullptr;
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (root)
        foreground = root;
    if (window_process_id(foreground) == process_id)
        return foreground;

    struct child_search
    {
        DWORD process_id;
        bool found;
    } search = {process_id, false};
    EnumChildWindows(
        foreground,
        [](HWND child, LPARAM parameter) -> BOOL
        {
            auto* search = reinterpret_cast<child_search*>(parameter);
            if (window_process_id(child) == search->process_id)
            {
                search->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found ? foreground : nullptr;
}

bool is_valid_client_owner(HWND owner, DWORD client_process_id)
{
    if (!owner || !IsWindow(owner))
        return false;
    if (window_process_id(owner) == client_process_id)
        return true;
    return owner == foreground_root_for_process(client_process_id);
}

zime::broker_protocol::context_result process_context_snapshot(
    const zime::broker_protocol::context_snapshot& request,
    DWORD client_process_id,
    DWORD client_session_id,
    std::uint64_t connection_id,
    HINSTANCE instance)
{
    using namespace zime::broker_protocol;
    context_result result = {};
    result.header = make_header(message_type::context_result,
                                sizeof(result),
                                request.header.request_id,
                                connection_id);

    if (request.header.connection_id != connection_id ||
        request.reported_process_id != client_process_id)
    {
        result.status = status_code::identity_mismatch;
        return result;
    }
    if (request.reported_session_id != client_session_id)
    {
        result.status = status_code::session_mismatch;
        return result;
    }

    const HWND requested_owner = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(request.view_hwnd));
    result.owner_process_id = window_process_id(requested_owner);
    if (!requested_owner ||
        !IsWindow(requested_owner) ||
        result.owner_process_id != client_process_id)
    {
        result.status = status_code::invalid_window;
        return result;
    }

    SetLastError(ERROR_SUCCESS);
    HWND owned_window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kOwnedWindowClass,
        L"",
        WS_POPUP,
        -32000,
        -32000,
        1,
        1,
        requested_owner,
        nullptr,
        instance,
        nullptr);
    result.owner_window_created = owned_window != nullptr;
    if (!owned_window)
    {
        result.status = status_code::internal_error;
        result.win32_error = GetLastError();
        return result;
    }

    SetLayeredWindowAttributes(owned_window, 0, 0, LWA_ALPHA);
    ShowWindow(owned_window, SW_SHOWNOACTIVATE);
    result.owner_relationship_matches =
        GetWindow(owned_window, GW_OWNER) == requested_owner;
    result.owner_window_visible = IsWindowVisible(owned_window) != FALSE;
    result.status = result.owner_relationship_matches && result.owner_window_visible
        ? status_code::ok
        : status_code::internal_error;
    DestroyWindow(owned_window);
    return result;
}

struct client_channel
{
    HANDLE pipe;
    std::uint64_t connection_id;
    std::mutex send_mutex;
    bool closed;

    client_channel(HANDLE pipe_handle, std::uint64_t id)
        : pipe(pipe_handle), connection_id(id), closed(false)
    {
    }

    template <typename T>
    bool Send(T* message)
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        return !closed && write_message(pipe, message);
    }

    bool SendBytes(void* bytes, DWORD size)
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (closed)
            return false;
        DWORD written = 0;
        return overlapped_io(pipe,
                             true,
                             bytes,
                             size,
                             &written,
                             zime::broker_protocol::kIoTimeoutMs) &&
            written == size;
    }

    bool SendConfigState(const zime::broker_protocol::config_state& source)
    {
        zime::broker_protocol::config_state state = source;
        state.header = zime::broker_protocol::make_header(
            zime::broker_protocol::message_type::config_state,
            sizeof(state),
            source.header.request_id,
            connection_id);
        return Send(&state);
    }

    bool SendDictionaryState(std::uint64_t revision)
    {
        zime::broker_protocol::dictionary_state state = {};
        state.header = zime::broker_protocol::make_header(
            zime::broker_protocol::message_type::dictionary_state,
            sizeof(state),
            revision,
            connection_id);
        state.revision = revision;
        return Send(&state);
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        if (closed)
            return;
        closed = true;
        CancelIoEx(pipe, nullptr);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
};

std::mutex g_channels_mutex;
std::vector<std::weak_ptr<client_channel>> g_channels;

void register_channel(const std::shared_ptr<client_channel>& channel)
{
    std::lock_guard<std::mutex> lock(g_channels_mutex);
    g_channels.erase(
        std::remove_if(g_channels.begin(),
                       g_channels.end(),
                       [](const std::weak_ptr<client_channel>& item)
                       {
                           return item.expired();
                       }),
        g_channels.end());
    g_channels.push_back(channel);
}

struct config_job
{
    std::shared_ptr<client_channel> channel;
    zime::broker_protocol::config_state state;
};

void CALLBACK send_config_callback(PTP_CALLBACK_INSTANCE, void* context)
{
    std::unique_ptr<config_job> job(static_cast<config_job*>(context));
    if (job && job->channel)
        job->channel->SendConfigState(job->state);
}

void broadcast_config_state(
    const zime::broker_protocol::config_state& state)
{
    std::vector<std::shared_ptr<client_channel>> channels;
    {
        std::lock_guard<std::mutex> lock(g_channels_mutex);
        for (auto iterator = g_channels.begin(); iterator != g_channels.end();)
        {
            if (auto channel = iterator->lock())
            {
                channels.push_back(std::move(channel));
                ++iterator;
            }
            else
            {
                iterator = g_channels.erase(iterator);
            }
        }
    }
    for (const auto& channel : channels)
    {
        auto* job = new (std::nothrow) config_job{channel, state};
        if (!job)
            continue;
        if (!TrySubmitThreadpoolCallback(send_config_callback, job, nullptr))
            delete job;
    }
}

void broadcast_dictionary_state(std::uint64_t revision)
{
    std::vector<std::shared_ptr<client_channel>> channels;
    {
        std::lock_guard<std::mutex> lock(g_channels_mutex);
        for (auto iterator = g_channels.begin(); iterator != g_channels.end();)
        {
            if (auto channel = iterator->lock())
            {
                channels.push_back(std::move(channel));
                ++iterator;
            }
            else
            {
                iterator = g_channels.erase(iterator);
            }
        }
    }
    for (const auto& channel : channels)
        channel->SendDictionaryState(revision);
}

struct action_job
{
    std::shared_ptr<client_channel> channel;
    zime::broker_protocol::ui_action action;
};

void CALLBACK send_action_callback(PTP_CALLBACK_INSTANCE, void* context)
{
    std::unique_ptr<action_job> job(static_cast<action_job*>(context));
    if (job && job->channel)
        job->channel->Send(&job->action);
}

void enqueue_ui_action(const std::weak_ptr<client_channel>& weak_channel,
                       const zime::broker_protocol::ui_action& action)
{
    const std::shared_ptr<client_channel> channel = weak_channel.lock();
    if (!channel)
        return;
    auto* job = new (std::nothrow) action_job{channel, action};
    if (!job)
        return;
    if (!TrySubmitThreadpoolCallback(send_action_callback, job, nullptr))
        delete job;
}

std::unique_ptr<broker_candidate_update> decode_candidate_update(
    const std::uint8_t* bytes,
    DWORD size,
    DWORD client_process_id,
    std::uint64_t connection_id,
    const std::shared_ptr<client_channel>& channel)
{
    using namespace zime::broker_protocol;
    if (!bytes || size < sizeof(candidate_state))
        return nullptr;

    const auto& state = *reinterpret_cast<const candidate_state*>(bytes);
    if (!has_valid_envelope(state.header, message_type::candidate_state, size) ||
        state.header.connection_id != connection_id ||
        state.candidate_count > 512 ||
        state.composition_chars > state.string_chars)
    {
        return nullptr;
    }

    const std::size_t expected_size = sizeof(candidate_state) +
        static_cast<std::size_t>(state.candidate_count) *
            sizeof(candidate_string_record) +
        static_cast<std::size_t>(state.string_chars) * sizeof(wchar_t);
    if (expected_size != size)
        return nullptr;

    HWND owner = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(state.owner_hwnd));
    const HWND view = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(state.view_hwnd));
    if ((owner && !is_valid_client_owner(owner, client_process_id)) ||
        (view && (!IsWindow(view) || window_process_id(view) != client_process_id)))
    {
        return nullptr;
    }
    if (!owner)
        owner = foreground_root_for_process(client_process_id);

    const auto* records = reinterpret_cast<const candidate_string_record*>(
        bytes + sizeof(candidate_state));
    const auto* strings = reinterpret_cast<const wchar_t*>(
        bytes + sizeof(candidate_state) +
        static_cast<std::size_t>(state.candidate_count) *
            sizeof(candidate_string_record));

    auto update = std::make_unique<broker_candidate_update>();
    update->connection_id = connection_id;
    update->generation = state.generation;
    update->view_hwnd = view;
    update->owner_hwnd = owner;
    update->anchor = {state.anchor.left,
                      state.anchor.top,
                      state.anchor.right,
                      state.anchor.bottom};
    update->anchor_valid = (state.flags & candidate_anchor_valid) != 0 &&
        update->anchor.bottom > update->anchor.top;
    update->visible = (state.flags & candidate_visible) != 0;
    update->custom_ui_allowed =
        (state.flags & candidate_custom_ui_allowed) != 0;
    update->presentation_pending =
        (state.flags & candidate_presentation_pending) != 0;
    update->selection_absolute = state.selection_absolute;
    update->page_size = state.page_size;
    update->current_page = state.current_page;
    update->ui_font_percent = state.ui_font_percent;
    update->composition.assign(strings, state.composition_chars);
    update->candidates.reserve(state.candidate_count);
    for (std::uint32_t index = 0; index < state.candidate_count; ++index)
    {
        const auto& record = records[index];
        if (record.offset_chars > state.string_chars ||
            record.length_chars > state.string_chars - record.offset_chars)
        {
            return nullptr;
        }
        update->candidates.emplace_back(strings + record.offset_chars,
                                        record.length_chars);
    }
    const std::weak_ptr<client_channel> weak_channel = channel;
    update->send_action = [weak_channel](const ui_action& action)
    {
        enqueue_ui_action(weak_channel, action);
    };
    return update;
}

std::unique_ptr<broker_status_update> decode_status_update(
    const std::uint8_t* bytes,
    DWORD size,
    DWORD client_process_id,
    std::uint64_t connection_id,
    const std::shared_ptr<client_channel>& channel)
{
    using namespace zime::broker_protocol;
    if (!bytes || size != sizeof(status_state))
        return nullptr;
    const auto& state = *reinterpret_cast<const status_state*>(bytes);
    if (!has_valid_envelope(state.header,
                            message_type::status_state,
                            sizeof(state)) ||
        state.header.connection_id != connection_id)
    {
        return nullptr;
    }

    HWND owner = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(state.owner_hwnd));
    if (owner && !is_valid_client_owner(owner, client_process_id))
        return nullptr;
    if (!owner)
        owner = foreground_root_for_process(client_process_id);

    auto update = std::make_unique<broker_status_update>();
    update->connection_id = connection_id;
    update->generation = state.generation;
    update->owner_hwnd = owner;
    update->flags = state.flags;
    update->position_x = state.position_x;
    update->position_y = state.position_y;
    update->ui_font_percent = state.ui_font_percent;
    update->candidate_sort_mode = state.candidate_sort_mode;
    const std::weak_ptr<client_channel> weak_channel = channel;
    update->send_action = [weak_channel](const ui_action& action)
    {
        enqueue_ui_action(weak_channel, action);
    };
    return update;
}

bool process_storage_mutation(
    const std::uint8_t* bytes,
    DWORD size,
    std::uint64_t client_nonce,
    std::uint64_t connection_id,
    const std::shared_ptr<client_channel>& channel)
{
    using namespace zime::broker_protocol;
    if (!bytes || size < sizeof(storage_mutation))
        return false;
    const auto& request = *reinterpret_cast<const storage_mutation*>(bytes);
    if (request.header.connection_id != connection_id ||
        request.code_chars > 1024 ||
        request.text_chars > 4096)
    {
        return false;
    }
    const std::size_t expected_size = sizeof(storage_mutation) +
        (static_cast<std::size_t>(request.code_chars) + request.text_chars) *
            sizeof(wchar_t);
    if (expected_size != size ||
        !has_valid_envelope(request.header,
                            message_type::storage_mutation,
                            size))
    {
        return false;
    }

    const auto* strings = reinterpret_cast<const wchar_t*>(
        bytes + sizeof(storage_mutation));
    const std::wstring code(strings, request.code_chars);
    const std::wstring text(strings + request.code_chars, request.text_chars);
    std::wstring error;
    const bool success = g_storage.ApplyMutation(
        client_nonce,
        request.header.request_id,
        request.operation,
        code,
        text,
        &error);
    if (success)
    {
        broker_debug_log(L"storage operation=%lu code_chars=%lu text_chars=%lu",
                         static_cast<DWORD>(request.operation),
                         request.code_chars,
                         request.text_chars);
    }
    else
    {
        broker_error_log(L"storage operation=%lu failed code_chars=%lu text_chars=%lu error=%s",
                         static_cast<DWORD>(request.operation),
                         request.code_chars,
                         request.text_chars,
                         error.c_str());
    }

    if ((request.flags & storage_expects_result) == 0)
    {
        if (success)
            broadcast_dictionary_state(g_storage.DictionaryRevision());
        return true;
    }
    if (error.size() > 2048)
        error.resize(2048);
    const std::size_t result_size = sizeof(storage_result) +
        error.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> result_bytes(result_size);
    auto* result = reinterpret_cast<storage_result*>(result_bytes.data());
    *result = {};
    result->header = make_header(message_type::storage_result,
                                 static_cast<std::uint32_t>(result_size),
                                 request.header.request_id,
                                 connection_id);
    result->status = success ? status_code::ok : status_code::internal_error;
    result->operation = request.operation;
    result->error_chars = static_cast<std::uint32_t>(error.size());
    if (!error.empty())
    {
        std::memcpy(result_bytes.data() + sizeof(storage_result),
                    error.data(),
                    error.size() * sizeof(wchar_t));
    }
    const bool sent = channel->SendBytes(
        result_bytes.data(), static_cast<DWORD>(result_bytes.size()));
    if (sent && success)
        broadcast_dictionary_state(g_storage.DictionaryRevision());
    return sent;
}

bool process_candidate_query(
    const std::uint8_t* bytes,
    DWORD size,
    std::uint64_t connection_id,
    const std::shared_ptr<client_channel>& channel,
    HWND ui_controller)
{
    using namespace zime::broker_protocol;
    if (!bytes || size < sizeof(candidate_query))
        return false;
    const auto& request = *reinterpret_cast<const candidate_query*>(bytes);
    if (request.header.connection_id != connection_id ||
        request.code_chars > 64)
    {
        return false;
    }
    const std::size_t expected_size = sizeof(candidate_query) +
        static_cast<std::size_t>(request.code_chars) * sizeof(wchar_t);
    if (expected_size != size ||
        !has_valid_envelope(request.header, message_type::candidate_query, size))
    {
        return false;
    }
    ZIME_PERF_SCOPE("broker.ProcessCandidateQuery",
                    static_cast<std::int64_t>(request.generation),
                    static_cast<std::int64_t>(request.code_chars));
    ZIME_PERF_RECORD("broker.QueryReceived",
                     static_cast<std::int64_t>(request.generation),
                     static_cast<std::int64_t>(request.code_chars), 0);

    const auto* code_text = reinterpret_cast<const wchar_t*>(
        bytes + sizeof(candidate_query));
    const std::wstring code(code_text, request.code_chars);
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> view_texts;
    std::vector<bool> pinyin_flags;
    std::uint32_t result_flags = 0;
    std::uint64_t config_revision = 0;
    std::wstring error;
    const bool success = g_storage.GetCandidates(
        code,
        (request.flags & candidate_query_apply_auto_commit) != 0,
        &candidates,
        &view_texts,
        &pinyin_flags,
        &result_flags,
        &config_revision,
        &error);

    std::wstring strings = code;
    std::vector<engine_candidate_record> records;
    records.reserve((std::min<std::size_t>)(candidates.size(), 512));
    for (std::size_t index = 0;
         index < candidates.size() && records.size() < 512;
         ++index)
    {
        const std::wstring text = candidates[index].substr(0, 256);
        const std::wstring view = index < view_texts.size()
            ? view_texts[index].substr(0, 256)
            : text;
        const std::size_t projected = sizeof(candidate_result) +
            (records.size() + 1) * sizeof(engine_candidate_record) +
            (strings.size() + text.size() + view.size()) * sizeof(wchar_t);
        if (projected > kMaxMessageSize)
            break;
        engine_candidate_record record = {};
        record.text_offset_chars = static_cast<std::uint32_t>(strings.size());
        record.text_length_chars = static_cast<std::uint32_t>(text.size());
        strings.append(text);
        record.view_offset_chars = static_cast<std::uint32_t>(strings.size());
        record.view_length_chars = static_cast<std::uint32_t>(view.size());
        strings.append(view);
        if (index < pinyin_flags.size() && pinyin_flags[index])
            record.flags |= candidate_record_pinyin;
        records.push_back(record);
    }

    const std::size_t result_size = sizeof(candidate_result) +
        records.size() * sizeof(engine_candidate_record) +
        strings.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> result_bytes(result_size);
    auto* result = reinterpret_cast<candidate_result*>(result_bytes.data());
    *result = {};
    result->header = make_header(message_type::candidate_result,
                                 static_cast<std::uint32_t>(result_size),
                                 request.header.request_id,
                                 connection_id);
    result->generation = request.generation;
    result->config_revision = config_revision;
    result->status = success ? status_code::ok : status_code::internal_error;
    result->flags = success ? result_flags : 0;
    result->code_chars = request.code_chars;
    result->candidate_count = static_cast<std::uint32_t>(records.size());
    result->string_chars = static_cast<std::uint32_t>(strings.size());
    auto* cursor = result_bytes.data() + sizeof(candidate_result);
    if (!records.empty())
    {
        const std::size_t record_bytes = records.size() * sizeof(records[0]);
        std::memcpy(cursor, records.data(), record_bytes);
        cursor += record_bytes;
    }
    if (!strings.empty())
        std::memcpy(cursor, strings.data(), strings.size() * sizeof(wchar_t));

    if (success)
    {
        auto ui_result = std::make_unique<broker_candidate_result_update>();
        ui_result->connection_id = connection_id;
        ui_result->query_generation = request.generation;
        ui_result->result_flags = result_flags;
        ui_result->composition = code;
        ui_result->candidates = view_texts;
        broker_ui_controller::PostCandidateResult(
            ui_controller, std::move(ui_result));
    }
    const bool sent = channel->SendBytes(
        result_bytes.data(), static_cast<DWORD>(result_bytes.size()));
    ZIME_PERF_RECORD("broker.ResultSent",
                     static_cast<std::int64_t>(request.generation),
                     sent ? 1 : 0,
                     static_cast<std::int64_t>(result->candidate_count));

    if (success)
    {
        broker_debug_log(L"candidate query generation=%llu code_chars=%lu candidates=%lu flags=0x%lx",
                         static_cast<unsigned long long>(request.generation),
                         request.code_chars,
                         result->candidate_count,
                         result->flags);
    }
    else
    {
        broker_error_log(L"candidate query failed generation=%llu code_chars=%lu error=%s",
                         static_cast<unsigned long long>(request.generation),
                         request.code_chars,
                         error.c_str());
    }
    return sent;
}

void handle_client(HANDLE pipe, HINSTANCE instance, HWND ui_controller)
{
    struct connection_guard
    {
        ~connection_guard() { --g_connection_count; }
    } guard;

    using namespace zime::broker_protocol;
    ULONG client_process_id = 0;
    ULONG client_session_id = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_process_id) ||
        !GetNamedPipeClientSessionId(pipe, &client_session_id) ||
        !process_belongs_to_current_user(client_process_id))
    {
        broker_error_log(L"connection rejected pid=%lu session=%lu error=%lu",
                         client_process_id,
                         client_session_id,
                         GetLastError());
        CloseHandle(pipe);
        return;
    }

    DWORD server_session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &server_session_id);
    std::array<std::uint8_t, kMaxMessageSize> buffer = {};
    DWORD size = 0;
    if (!read_message(pipe, &buffer, &size, kHelloTimeoutMs))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
            broker_debug_log(L"hello probe closed pid=%lu error=%lu",
                             client_process_id, error);
        else
            broker_error_log(L"hello read failed pid=%lu error=%lu",
                             client_process_id, error);
        CloseHandle(pipe);
        return;
    }

    const auto* header = reinterpret_cast<const message_header*>(buffer.data());
    hello_result hello_reply = {};
    hello_reply.header = make_header(message_type::hello_result,
                                     sizeof(hello_reply),
                                     header->request_id);
    hello_reply.actual_client_process_id = client_process_id;
    hello_reply.actual_client_session_id = client_session_id;
    hello_reply.server_process_id = GetCurrentProcessId();
    hello_reply.server_session_id = server_session_id;

    if (size != sizeof(hello) ||
        header->type != message_type::hello ||
        header->size != sizeof(hello))
    {
        hello_reply.status = status_code::malformed_message;
        write_message(pipe, &hello_reply);
        CloseHandle(pipe);
        return;
    }

    const auto& hello_request = *reinterpret_cast<const hello*>(buffer.data());
    hello_reply.echoed_client_nonce = hello_request.client_nonce;
    if (header->major_version != kMajorVersion ||
        header->minor_version > kMinorVersion)
    {
        hello_reply.status = status_code::unsupported_version;
    }
    else if (hello_request.reported_process_id != client_process_id)
    {
        hello_reply.status = status_code::identity_mismatch;
    }
    else if (hello_request.reported_session_id != client_session_id ||
             client_session_id != server_session_id)
    {
        hello_reply.status = status_code::session_mismatch;
    }
    else
    {
        hello_reply.status = status_code::ok;
        hello_reply.header.connection_id = random_u64();
        hello_reply.server_nonce = random_u64();
        hello_reply.capabilities = capability_context_snapshot |
            capability_owned_windows |
            capability_candidate_window |
            capability_ui_actions |
            capability_status_window |
            capability_storage_writer;
    }

    if (!write_message(pipe, &hello_reply) || hello_reply.status != status_code::ok)
    {
        broker_error_log(L"hello rejected pid=%lu status=%lu",
                         client_process_id,
                         static_cast<DWORD>(hello_reply.status));
        CloseHandle(pipe);
        return;
    }

    const std::uint64_t connection_id = hello_reply.header.connection_id;
    const auto channel = std::make_shared<client_channel>(pipe, connection_id);
    register_channel(channel);
    const config_state initial_config = g_storage.LoadConfig();
    if (!channel->SendConfigState(initial_config))
    {
        channel->Close();
        return;
    }
#ifndef NDEBUG
    const std::wstring image_path = process_image_path(client_process_id);
    broker_debug_log(L"hello accepted pid=%lu session=%lu pointer=%lu connection=%llu image=%s",
                     client_process_id,
                     client_session_id,
                     hello_request.pointer_size,
                     static_cast<unsigned long long>(connection_id),
                     image_path.c_str());
#endif

    for (;;)
    {
        size = 0;
        if (!read_message(pipe, &buffer, &size, kIdleConnectionTimeoutMs))
            break;

        const auto* request_header =
            reinterpret_cast<const message_header*>(buffer.data());
        if (request_header->type == message_type::context_snapshot &&
            size == sizeof(context_snapshot) &&
            has_valid_envelope(*request_header,
                               message_type::context_snapshot,
                               sizeof(context_snapshot)))
        {
            const auto& request =
                *reinterpret_cast<const context_snapshot*>(buffer.data());
            context_result result = process_context_snapshot(request,
                                                             client_process_id,
                                                             client_session_id,
                                                             connection_id,
                                                             instance);
            broker_debug_log(L"context pid=%lu view=0x%p view_pid=%lu status=%lu created=%lu owner_match=%lu visible=%lu error=%lu",
                             client_process_id,
                             reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.view_hwnd)),
                             result.owner_process_id,
                             static_cast<DWORD>(result.status),
                             result.owner_window_created,
                             result.owner_relationship_matches,
                             result.owner_window_visible,
                             result.win32_error);
            if (!channel->Send(&result))
                break;
            continue;
        }

        if (request_header->type == message_type::candidate_state)
        {
            auto update = decode_candidate_update(buffer.data(),
                                                  size,
                                                  client_process_id,
                                                  connection_id,
                                                  channel);
            if (!update ||
                !broker_ui_controller::PostCandidateUpdate(
                    ui_controller, std::move(update)))
            {
                broker_error_log(L"candidate state rejected pid=%lu size=%lu",
                                 client_process_id,
                                 size);
                break;
            }
            continue;
        }

        if (request_header->type == message_type::status_state)
        {
            auto update = decode_status_update(buffer.data(),
                                               size,
                                               client_process_id,
                                               connection_id,
                                               channel);
            if (!update ||
                !broker_ui_controller::PostStatusUpdate(
                    ui_controller, std::move(update)))
            {
                broker_error_log(L"status state rejected pid=%lu size=%lu",
                                 client_process_id,
                                 size);
                break;
            }
            continue;
        }

        if (request_header->type == message_type::ping &&
            size == sizeof(message_header) &&
            has_valid_envelope(*request_header,
                               message_type::ping,
                               sizeof(message_header)) &&
            request_header->connection_id == connection_id)
        {
            message_header pong = make_header(message_type::pong,
                                              sizeof(pong),
                                              request_header->request_id,
                                              connection_id);
            if (!channel->Send(&pong))
                break;
            continue;
        }

        if (request_header->type == message_type::storage_mutation)
        {
            if (!process_storage_mutation(buffer.data(),
                                          size,
                                          hello_request.client_nonce,
                                          connection_id,
                                          channel))
            {
                broker_error_log(L"storage mutation rejected pid=%lu size=%lu",
                                 client_process_id,
                                 size);
                break;
            }
            continue;
        }

        if (request_header->type == message_type::candidate_query)
        {
            if (!process_candidate_query(buffer.data(),
                                         size,
                                         connection_id,
                                         channel,
                                         ui_controller))
            {
                broker_error_log(L"candidate query rejected pid=%lu size=%lu",
                                 client_process_id,
                                 size);
                break;
            }
            continue;
        }

        broker_error_log(L"message rejected pid=%lu type=%u size=%lu",
                         client_process_id,
                         static_cast<unsigned>(request_header->type),
                         size);
        break;
    }

    broker_ui_controller::PostConnectionClosed(ui_controller, connection_id);
    channel->Close();
    broker_debug_log(L"connection closed pid=%lu connection=%llu",
                     client_process_id,
                     static_cast<unsigned long long>(connection_id));
}

void accept_clients(const wchar_t* pipe_name,
                    HINSTANCE instance,
                    SECURITY_ATTRIBUTES* attributes,
                    HWND ui_controller)
{
    bool first_instance = true;
    for (;;)
    {
        const DWORD open_mode = PIPE_ACCESS_DUPLEX |
            FILE_FLAG_OVERLAPPED |
            (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(
            pipe_name,
            open_mode,
            PIPE_TYPE_MESSAGE |
                PIPE_READMODE_MESSAGE |
                PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            kMaxConnections,
            zime::broker_protocol::kMaxMessageSize,
            zime::broker_protocol::kMaxMessageSize,
            0,
            attributes);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            broker_error_log(L"CreateNamedPipe failed pipe=%s first=%d error=%lu",
                             pipe_name,
                             first_instance ? 1 : 0,
                             GetLastError());
            Sleep(250);
            continue;
        }
        first_instance = false;

        if (!connect_pipe(pipe))
        {
            CloseHandle(pipe);
            continue;
        }
        if (g_connection_count.fetch_add(1) >= kMaxConnections)
        {
            --g_connection_count;
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        std::thread(handle_client, pipe, instance, ui_controller).detach();
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int)
{
    if (command_line && _wcsicmp(command_line, L"--shutdown") == 0)
    {
        HANDLE shutdown_event = OpenEventW(
            EVENT_MODIFY_STATE, FALSE, zime::broker_protocol::kShutdownEventName);
        if (shutdown_event)
        {
            SetEvent(shutdown_event);
            CloseHandle(shutdown_event);
        }

        HANDLE broker_mutex = OpenMutexW(
            SYNCHRONIZE, FALSE, zime::broker_protocol::kMutexName);
        if (!broker_mutex)
            return 0;
        const DWORD wait_result = WaitForSingleObject(broker_mutex, 10000);
        CloseHandle(broker_mutex);
        return (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED)
            ? 0
            : 2;
    }

    configure_process_dpi_awareness();
    g_hInst = instance;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, zime::broker_protocol::kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!mutex)
            broker_error_log(L"broker mutex creation failed error=%lu", GetLastError());
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }
    HANDLE shutdown_event = CreateEventW(
        nullptr, TRUE, FALSE, zime::broker_protocol::kShutdownEventName);
    if (!shutdown_event)
    {
        broker_error_log(L"shutdown event creation failed error=%lu", GetLastError());
        CloseHandle(mutex);
        return 5;
    }

    if (!register_owned_window_class(instance))
    {
        broker_error_log(L"owned window class registration failed error=%lu", GetLastError());
        CloseHandle(shutdown_event);
        CloseHandle(mutex);
        return 1;
    }

    std::wstring storage_error;
    if (!g_storage.Prepare(&storage_error))
    {
        broker_error_log(L"storage preparation failed error=%s", storage_error.c_str());
        CloseHandle(shutdown_event);
        CloseHandle(mutex);
        return 4;
    }

    broker_ui_controller ui_controller;
    if (!ui_controller.Initialize(instance,
                                  &g_storage,
                                  [](const zime::broker_protocol::config_state& state)
                                  {
                                      broadcast_config_state(state);
                                  },
                                  [](std::uint64_t revision)
                                  {
                                      broadcast_dictionary_state(revision);
                                  }))
    {
        broker_error_log(L"UI controller initialization failed error=%lu", GetLastError());
        CloseHandle(shutdown_event);
        CloseHandle(mutex);
        return 2;
    }

    PSECURITY_DESCRIPTOR descriptor = create_pipe_security_descriptor();
    if (!descriptor)
    {
        broker_error_log(L"security descriptor failed error=%lu", GetLastError());
        CloseHandle(shutdown_event);
        CloseHandle(mutex);
        return 3;
    }

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    broker_debug_log(L"broker started desktop_pipe=%s appcontainer_pipe=%s",
                     zime::broker_protocol::kPipeName,
                     zime::broker_protocol::kAppContainerPipeName);

    std::thread(accept_clients,
                zime::broker_protocol::kPipeName,
                instance,
                &attributes,
                ui_controller.hwnd()).detach();
    std::thread(accept_clients,
                zime::broker_protocol::kAppContainerPipeName,
                instance,
                &attributes,
                ui_controller.hwnd()).detach();

    MSG message = {};
    bool running = true;
    while (running)
    {
        const DWORD wait_result = MsgWaitForMultipleObjects(
            1, &shutdown_event, FALSE, INFINITE, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0)
            break;
        if (wait_result != WAIT_OBJECT_0 + 1)
            break;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    broker_debug_log(L"broker stopping");
    ZIME_PERF_FLUSH(L"broker");
    CloseHandle(shutdown_event);
    CloseHandle(mutex);
    return 0;
}
