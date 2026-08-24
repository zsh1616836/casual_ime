#include "broker_client.h"

#include "ime_trace.h"
#include "perf_trace.h"
#include "tool.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kCallbackWindowClass[] = L"ZImeBrokerClientCallback";
constexpr UINT WM_BROKER_CALLBACK = WM_APP + 0x341;
constexpr wchar_t kBrokerRegistryKey[] = L"Software\\ZIme\\Broker";
constexpr wchar_t kBrokerRegistration64[] = L"Registration64";
constexpr wchar_t kBrokerRegistration32[] = L"Registration32";

bool overlapped_transfer(HANDLE pipe,
                         bool write,
                         void* buffer,
                         DWORD size,
                         DWORD* transferred,
                         DWORD timeout_ms,
                         HANDLE stop_event,
                         HANDLE owner_thread)
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
        ? WriteFile(pipe, buffer, size, &byte_count, &overlapped)
        : ReadFile(pipe, buffer, size, &byte_count, &overlapped);

    bool success = false;
    if (started)
    {
        success = true;
    }
    else if (GetLastError() == ERROR_IO_PENDING)
    {
        HANDLE waits[3] = {event, stop_event, owner_thread};
        DWORD wait_count = 1;
        if (stop_event)
            waits[wait_count++] = stop_event;
        if (owner_thread)
            waits[wait_count++] = owner_thread;
        const DWORD wait_result = WaitForMultipleObjects(
            wait_count, waits, FALSE, timeout_ms);
        if (wait_result == WAIT_OBJECT_0)
            success = GetOverlappedResult(
                pipe, &overlapped, &byte_count, FALSE) != FALSE;
        else
        {
            CancelIoEx(pipe, &overlapped);
            // The OVERLAPPED and event are stack-owned. Do not release them
            // until cancellation has completed.
            WaitForSingleObject(event, INFINITE);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
        }
    }

    if (success && transferred)
        *transferred = byte_count;
    CloseHandle(event);
    return success;
}

template <typename T>
bool transfer_struct(HANDLE pipe,
                     bool write,
                     T* value,
                     DWORD timeout_ms,
                     HANDLE stop_event,
                     HANDLE owner_thread)
{
    DWORD transferred = 0;
    return overlapped_transfer(pipe,
                               write,
                               value,
                               sizeof(T),
                               &transferred,
                               timeout_ms,
                               stop_event,
                               owner_thread) &&
        transferred == sizeof(T);
}

bool lifetime_ended(HANDLE stop_event, HANDLE owner_thread)
{
    if (stop_event &&
        WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0)
    {
        return true;
    }
    return owner_thread &&
        WaitForSingleObject(owner_thread, 0) == WAIT_OBJECT_0;
}

bool wait_for_named_pipe(HANDLE stop_event,
                         HANDLE owner_thread,
                         const wchar_t* pipe_name,
                         DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD last_error = ERROR_FILE_NOT_FOUND;
    for (;;)
    {
        if (lifetime_ended(stop_event, owner_thread))
        {
            SetLastError(ERROR_CANCELLED);
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            SetLastError(last_error);
            return false;
        }
        const DWORD slice = static_cast<DWORD>(
            std::min<ULONGLONG>(50, deadline - now));
        if (WaitNamedPipeW(pipe_name, slice))
            return true;
        last_error = GetLastError();
        if (last_error != ERROR_FILE_NOT_FOUND &&
            last_error != ERROR_SEM_TIMEOUT &&
            last_error != ERROR_PIPE_BUSY)
        {
            SetLastError(last_error);
            return false;
        }

        // WaitNamedPipe returns immediately while the server executable has
        // not created its first pipe instance. Avoid a busy retry while still
        // allowing host shutdown to cancel startup promptly.
        if (last_error == ERROR_FILE_NOT_FOUND)
        {
            HANDLE waits[2] = {};
            DWORD wait_count = 0;
            if (stop_event)
                waits[wait_count++] = stop_event;
            if (owner_thread)
                waits[wait_count++] = owner_thread;
            if (wait_count != 0 &&
                WaitForMultipleObjects(wait_count, waits, FALSE, slice) !=
                    WAIT_TIMEOUT)
            {
                SetLastError(ERROR_CANCELLED);
                return false;
            }
            if (wait_count == 0)
                Sleep(slice);
        }
    }
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

bool query_process_path(DWORD process_id, std::wstring* path)
{
    if (!path)
        return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process)
        return false;

    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!queried || size == 0)
    {
        SetLastError(query_error);
        return false;
    }

    path->assign(buffer.data(), size);
    return true;
}

bool current_process_is_appcontainer()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD is_appcontainer = 0;
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token,
                                             TokenIsAppContainer,
                                             &is_appcontainer,
                                             sizeof(is_appcontainer),
                                             &returned);
    CloseHandle(token);
    return queried && is_appcontainer != 0;
}

std::wstring normalized_path(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    std::wstring value = (error ? path : canonical).wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

void append_registered_broker_path(HKEY key,
                                   const wchar_t* value_name,
                                   std::vector<std::filesystem::path>* paths)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t))
    {
        return;
    }

    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key,
                        value_name,
                        nullptr,
                        &type,
                        reinterpret_cast<BYTE*>(value.data()),
                        &bytes) != ERROR_SUCCESS ||
        value[0] == L'\0')
    {
        return;
    }

    const std::filesystem::path path(value.data());
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error)
        paths->push_back(path);
}

std::vector<std::filesystem::path> trusted_broker_paths()
{
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path adjacent =
        tool::get_current_dll_path() / L"zime_broker.exe";
    std::error_code error;
    if (std::filesystem::is_regular_file(adjacent, error) && !error)
        paths.push_back(adjacent);

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      kBrokerRegistryKey,
                      0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) == ERROR_SUCCESS)
    {
        append_registered_broker_path(key, kBrokerRegistration64, &paths);
        append_registered_broker_path(key, kBrokerRegistration32, &paths);
        RegCloseKey(key);
    }

    std::vector<std::filesystem::path> unique_paths;
    for (const auto& path : paths)
    {
        const std::wstring normalized = normalized_path(path);
        const bool duplicate = std::any_of(
            unique_paths.begin(), unique_paths.end(), [&](const auto& existing) {
                return _wcsicmp(normalized.c_str(),
                                normalized_path(existing).c_str()) == 0;
            });
        if (!duplicate)
            unique_paths.push_back(path);
    }
    return unique_paths;
}

DWORD window_process_id(HWND hwnd)
{
    DWORD pid = 0;
    if (hwnd)
        GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}
}

broker_client::broker_client()
    : wake_event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      stop_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      owner_thread_(nullptr),
      active_pipe_(INVALID_HANDLE_VALUE),
      stopping_(false),
      connected_(false),
      storage_writer_available_(false),
      client_process_id_(0),
      client_thread_id_(0),
      client_session_id_(0),
      client_nonce_(0),
      connection_id_(0),
      next_request_id_(1),
      next_candidate_generation_(1),
      next_status_generation_(1),
      next_query_generation_(1),
      latest_candidate_generation_(0),
      latest_status_generation_(0),
      latest_config_revision_(0),
      latest_query_generation_(0),
      latest_dictionary_revision_(0),
      pending_sync_query_generation_(0),
      sync_candidate_completed_(false),
      sync_candidate_success_(false),
      sync_candidate_result_flags_(0),
      sync_candidate_config_revision_(0),
      last_view_hwnd_(nullptr),
      last_snapshot_tick_(0),
      last_start_attempt_tick_(0),
      callback_window_(nullptr),
      callback_thread_id_(0)
{
}

broker_client::~broker_client()
{
    Stop();
    if (wake_event_)
    {
        CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
    if (stop_event_)
    {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    if (owner_thread_)
    {
        CloseHandle(owner_thread_);
        owner_thread_ = nullptr;
    }
}

void broker_client::SetCallbacks(ui_action_callback action_callback,
                                 connection_state_callback connection_callback)
{
    action_callback_ = std::move(action_callback);
    connection_callback_ = std::move(connection_callback);
}

void broker_client::SetStorageResultCallback(storage_result_callback callback)
{
    storage_callback_ = std::move(callback);
}

void broker_client::SetConfigStateCallback(config_state_callback callback)
{
    config_callback_ = std::move(callback);
}

void broker_client::SetCandidateResultCallback(
    candidate_result_callback callback)
{
    candidate_result_callback_ = std::move(callback);
}

void broker_client::SetDictionaryStateCallback(
    dictionary_state_callback callback)
{
    dictionary_state_callback_ = std::move(callback);
}

void broker_client::Start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable())
        return;

    if (!wake_event_)
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_)
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_event_ || !stop_event_)
        return;
    ResetEvent(wake_event_);
    ResetEvent(stop_event_);

    if (owner_thread_)
    {
        CloseHandle(owner_thread_);
        owner_thread_ = nullptr;
    }
    if (!DuplicateHandle(GetCurrentProcess(),
                         GetCurrentThread(),
                         GetCurrentProcess(),
                         &owner_thread_,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        ime_errorf(L"BrokerClientStart",
                   L"owner thread duplication failed error=%lu",
                   GetLastError());
        return;
    }

    stopping_ = false;
    connected_.store(false);
    storage_writer_available_.store(false);
    client_process_id_ = GetCurrentProcessId();
    client_thread_id_ = GetCurrentThreadId();
    client_session_id_ = 0;
    ProcessIdToSessionId(client_process_id_, &client_session_id_);
    client_nonce_ = random_u64();
    connection_id_ = 0;
    next_request_id_ = 1;
    next_candidate_generation_ = 1;
    next_status_generation_ = 1;
    next_query_generation_ = 1;
    latest_candidate_generation_.store(0);
    latest_status_generation_.store(0);
    latest_config_revision_.store(0);
    latest_query_generation_.store(0);
    latest_dictionary_revision_.store(0);
    pending_sync_query_generation_ = 0;
    sync_candidate_completed_ = false;
    sync_candidate_success_ = false;
    sync_candidate_result_flags_ = 0;
    sync_candidate_config_revision_ = 0;
    last_start_attempt_tick_ = 0;
    callback_thread_id_ = GetCurrentThreadId();
    CreateCallbackWindow();
    worker_ = std::thread(&broker_client::Run, this);
}

void broker_client::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable())
        {
            DestroyCallbackWindow();
            if (owner_thread_)
            {
                CloseHandle(owner_thread_);
                owner_thread_ = nullptr;
            }
            return;
        }
        stopping_ = true;
        sync_candidate_ready_.notify_all();
        queue_.clear();
        if (stop_event_)
            SetEvent(stop_event_);
        if (active_pipe_ != INVALID_HANDLE_VALUE)
            CancelIoEx(active_pipe_, nullptr);
    }

    if (wake_event_)
        SetEvent(wake_event_);
    worker_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_pipe_ = INVALID_HANDLE_VALUE;
    }
    connected_.store(false);
    storage_writer_available_.store(false);
    DestroyCallbackWindow();
    if (owner_thread_)
    {
        CloseHandle(owner_thread_);
        owner_thread_ = nullptr;
    }
}

void broker_client::SendContextSnapshot(HWND view_hwnd)
{
    const ULONGLONG now = GetTickCount64();
    zime::broker_protocol::context_snapshot snapshot = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return;
        if (view_hwnd == last_view_hwnd_ && now - last_snapshot_tick_ < 2000)
            return;

        snapshot.header = zime::broker_protocol::make_header(
            zime::broker_protocol::message_type::context_snapshot,
            sizeof(snapshot),
            next_request_id_++);
        snapshot.reported_process_id = client_process_id_;
        snapshot.reported_thread_id = client_thread_id_;
        snapshot.reported_session_id = client_session_id_;
        snapshot.view_hwnd = reinterpret_cast<std::uintptr_t>(view_hwnd);
        snapshot.root_hwnd = reinterpret_cast<std::uintptr_t>(
            view_hwnd ? GetAncestor(view_hwnd, GA_ROOT) : nullptr);
        snapshot.foreground_hwnd =
            reinterpret_cast<std::uintptr_t>(GetForegroundWindow());
        last_view_hwnd_ = view_hwnd;
        last_snapshot_tick_ = now;
    }

    queued_frame frame = {};
    frame.type = zime::broker_protocol::message_type::context_snapshot;
    frame.bytes.resize(sizeof(snapshot));
    std::memcpy(frame.bytes.data(), &snapshot, sizeof(snapshot));
    EnqueueFrame(std::move(frame));
}

void broker_client::SendCandidateState(
    HWND owner_hwnd,
    const RECT* anchor,
    bool visible,
    bool custom_ui_allowed,
    const std::wstring& composition,
    std::uint32_t selection_absolute,
    std::uint32_t page_size,
    std::uint32_t current_page,
    std::uint32_t ui_font_percent,
    bool presentation_pending)
{
    using namespace zime::broker_protocol;
    std::uint64_t request_id = 0;
    std::uint64_t generation = 0;
    HWND view_hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return;
        request_id = next_request_id_++;
        generation = next_candidate_generation_++;
        view_hwnd = last_view_hwnd_;
    }
    latest_candidate_generation_.store(generation);

    std::wstring strings = composition.substr(0, 512);
    const std::uint32_t composition_chars =
        static_cast<std::uint32_t>(strings.size());
    // Candidate text already lives in the Broker query result.  State frames
    // carry only UI metadata so layout notifications never resend the list.
    const std::size_t frame_size = sizeof(candidate_state) +
        strings.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(frame_size);
    auto* state = reinterpret_cast<candidate_state*>(bytes.data());
    *state = {};
    state->header = make_header(message_type::candidate_state,
                                static_cast<std::uint32_t>(frame_size),
                                request_id);
    state->generation = generation;
    state->view_hwnd = reinterpret_cast<std::uintptr_t>(view_hwnd);
    state->owner_hwnd = reinterpret_cast<std::uintptr_t>(owner_hwnd);
    if (anchor)
    {
        state->anchor = {anchor->left, anchor->top, anchor->right, anchor->bottom};
        state->flags |= candidate_anchor_valid;
    }
    if (visible)
        state->flags |= candidate_visible;
    if (custom_ui_allowed)
        state->flags |= candidate_custom_ui_allowed;
    if (presentation_pending)
        state->flags |= candidate_presentation_pending;
    state->selection_absolute = selection_absolute;
    state->page_size = std::max<std::uint32_t>(1, page_size);
    state->current_page = current_page;
    state->ui_font_percent = std::clamp<std::uint32_t>(ui_font_percent, 80, 250);
    state->composition_chars = composition_chars;
    state->candidate_count = 0;
    state->string_chars = static_cast<std::uint32_t>(strings.size());

    if (!strings.empty())
    {
        std::memcpy(bytes.data() + sizeof(candidate_state),
                    strings.data(),
                    strings.size() * sizeof(wchar_t));
    }

    EnqueueFrame({message_type::candidate_state, std::move(bytes)});
}

void broker_client::SendStatusState(
    HWND owner_hwnd,
    bool visible,
    std::uint32_t state_flags,
    int position_x,
    int position_y,
    std::uint32_t ui_font_percent,
    std::uint32_t candidate_sort_mode)
{
    using namespace zime::broker_protocol;
    status_state state = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return;
        state.header = make_header(message_type::status_state,
                                   sizeof(state),
                                   next_request_id_++);
        state.generation = next_status_generation_++;
    }
    latest_status_generation_.store(state.generation);
    state.owner_hwnd = reinterpret_cast<std::uintptr_t>(owner_hwnd);
    state.flags = state_flags;
    if (visible)
        state.flags |= status_visible;
    state.position_x = position_x;
    state.position_y = position_y;
    state.ui_font_percent = std::clamp<std::uint32_t>(ui_font_percent, 80, 250);
    state.candidate_sort_mode = candidate_sort_mode;

    queued_frame frame = {};
    frame.type = message_type::status_state;
    frame.bytes.resize(sizeof(state));
    std::memcpy(frame.bytes.data(), &state, sizeof(state));
    EnqueueFrame(std::move(frame));
}

std::uint64_t broker_client::SendStorageMutation(
    zime::broker_protocol::storage_operation operation,
    const std::wstring& code,
    const std::wstring& text,
    bool expects_result)
{
    using namespace zime::broker_protocol;
    if (!HasStorageWriter())
        return 0;

    const std::wstring clipped_code = code.substr(0, 64);
    const std::wstring clipped_text = text.substr(0, 1024);
    std::uint64_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return 0;
        request_id = next_request_id_++;
    }

    const std::size_t frame_size = sizeof(storage_mutation) +
        (clipped_code.size() + clipped_text.size()) * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(frame_size);
    auto* mutation = reinterpret_cast<storage_mutation*>(bytes.data());
    *mutation = {};
    mutation->header = make_header(message_type::storage_mutation,
                                   static_cast<std::uint32_t>(frame_size),
                                   request_id);
    mutation->operation = operation;
    mutation->flags = expects_result ? storage_expects_result : 0;
    mutation->code_chars = static_cast<std::uint32_t>(clipped_code.size());
    mutation->text_chars = static_cast<std::uint32_t>(clipped_text.size());

    auto* strings = reinterpret_cast<wchar_t*>(
        bytes.data() + sizeof(storage_mutation));
    if (!clipped_code.empty())
        std::memcpy(strings,
                    clipped_code.data(),
                    clipped_code.size() * sizeof(wchar_t));
    if (!clipped_text.empty())
        std::memcpy(strings + clipped_code.size(),
                    clipped_text.data(),
                    clipped_text.size() * sizeof(wchar_t));

    if (!EnqueueFrame({message_type::storage_mutation, std::move(bytes)}))
        return 0;
    return request_id;
}

std::uint64_t broker_client::RequestCandidates(const std::wstring& code,
                                               bool apply_auto_commit)
{
    using namespace zime::broker_protocol;
    if (!IsConnected())
        return 0;
    const std::wstring clipped = code.substr(0, 64);
    std::uint64_t request_id = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return 0;
        request_id = next_request_id_++;
        generation = next_query_generation_++;
    }
    latest_query_generation_.store(generation);
    const std::size_t frame_size = sizeof(candidate_query) +
        clipped.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(frame_size);
    auto* request = reinterpret_cast<candidate_query*>(bytes.data());
    *request = {};
    request->header = make_header(message_type::candidate_query,
                                  static_cast<std::uint32_t>(frame_size),
                                  request_id);
    request->generation = generation;
    request->code_chars = static_cast<std::uint32_t>(clipped.size());
    if (apply_auto_commit)
        request->flags |= candidate_query_apply_auto_commit;
    if (!clipped.empty())
    {
        std::memcpy(bytes.data() + sizeof(candidate_query),
                    clipped.data(),
                    clipped.size() * sizeof(wchar_t));
    }
    if (!EnqueueFrame({message_type::candidate_query, std::move(bytes), true}))
        return 0;
    ZIME_PERF_RECORD("client.AsyncQueryQueued",
                     static_cast<std::int64_t>(generation),
                     static_cast<std::int64_t>(clipped.size()), 0);
    return generation;
}

bool broker_client::QueryCandidatesSync(
    const std::wstring& code,
    std::vector<std::wstring>* candidates,
    std::vector<std::wstring>* view_texts,
    std::vector<bool>* pinyin_flags,
    DWORD timeout_ms,
    bool apply_auto_commit,
    std::uint32_t* result_flags,
    std::uint64_t* config_revision)
{
    ZIME_PERF_SCOPE("client.QueryCandidatesSync",
                    static_cast<std::int64_t>(code.size()),
                    static_cast<std::int64_t>(timeout_ms));
    using namespace zime::broker_protocol;
    if (!candidates || !view_texts || !pinyin_flags || !IsConnected())
        return false;
    const std::wstring clipped = code.substr(0, 64);
    std::uint64_t request_id = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_ || pending_sync_query_generation_)
            return false;
        request_id = next_request_id_++;
        generation = next_query_generation_++;
        pending_sync_query_generation_ = generation;
        sync_candidate_completed_ = false;
        sync_candidate_success_ = false;
        sync_candidate_texts_.clear();
        sync_candidate_view_texts_.clear();
        sync_candidate_pinyin_flags_.clear();
        sync_candidate_result_flags_ = 0;
        sync_candidate_config_revision_ = 0;
    }
    latest_query_generation_.store(generation);

    const std::size_t frame_size = sizeof(candidate_query) +
        clipped.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> bytes(frame_size);
    auto* request = reinterpret_cast<candidate_query*>(bytes.data());
    *request = {};
    request->header = make_header(message_type::candidate_query,
                                  static_cast<std::uint32_t>(frame_size),
                                  request_id);
    request->generation = generation;
    request->code_chars = static_cast<std::uint32_t>(clipped.size());
    if (apply_auto_commit)
        request->flags |= candidate_query_apply_auto_commit;
    if (!clipped.empty())
    {
        std::memcpy(bytes.data() + sizeof(candidate_query),
                    clipped.data(),
                    clipped.size() * sizeof(wchar_t));
    }
    if (!EnqueueFrame({message_type::candidate_query,
                       std::move(bytes),
                       false,
                       true}))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_sync_query_generation_ = 0;
        return false;
    }
    ZIME_PERF_RECORD("client.SyncQueryQueued",
                     static_cast<std::int64_t>(generation),
                     static_cast<std::int64_t>(clipped.size()),
                     static_cast<std::int64_t>(timeout_ms));

    std::unique_lock<std::mutex> lock(mutex_);
    const bool completed = sync_candidate_ready_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this, generation]
        {
            return stopping_ ||
                (pending_sync_query_generation_ == generation &&
                 sync_candidate_completed_);
        });
    const bool success = completed && !stopping_ && sync_candidate_success_;
    if (success)
    {
        *candidates = sync_candidate_texts_;
        *view_texts = sync_candidate_view_texts_;
        *pinyin_flags = sync_candidate_pinyin_flags_;
        if (result_flags)
            *result_flags = sync_candidate_result_flags_;
        if (config_revision)
            *config_revision = sync_candidate_config_revision_;
    }
    if (pending_sync_query_generation_ == generation)
        pending_sync_query_generation_ = 0;
    ZIME_PERF_RECORD("client.QueryCandidatesSync.result",
                     success ? 1 : 0,
                     completed ? 1 : 0,
                     success ? static_cast<std::int64_t>(candidates->size()) : 0);
    return success;
}

void broker_client::Run()
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!read_event)
    {
        SetConnected(false);
        return;
    }

    OVERLAPPED read_overlapped = {};
    read_overlapped.hEvent = read_event;
    std::vector<std::uint8_t> read_buffer(
        zime::broker_protocol::kMaxMessageSize);
    bool read_pending = false;
    std::optional<queued_frame> pending;
    ULONGLONG last_keepalive_tick = 0;

    const auto disconnect = [&]() {
        if (pipe == INVALID_HANDLE_VALUE)
            return;
        if (read_pending)
        {
            CancelIoEx(pipe, &read_overlapped);
            WaitForSingleObject(read_event, INFINITE);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &read_overlapped, &ignored, FALSE);
            read_pending = false;
        }
        SetConnected(false);
        SetActivePipe(INVALID_HANDLE_VALUE);
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    };

    const auto finish_read = [&]() -> bool {
        DWORD transferred = 0;
        const BOOL completed = GetOverlappedResult(
            pipe, &read_overlapped, &transferred, FALSE);
        read_pending = false;
        return completed && transferred > 0 &&
            ProcessIncoming(read_buffer.data(), transferred);
    };

    for (;;)
    {
        bool stopping = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping = stopping_;
            if (pipe != INVALID_HANDLE_VALUE && !pending && !queue_.empty())
            {
                pending = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (stopping || lifetime_ended(stop_event_, owner_thread_))
            break;

        if (pipe == INVALID_HANDLE_VALUE)
        {
            pipe = ConnectAndHandshake();
            if (pipe == INVALID_HANDLE_VALUE)
            {
                WaitForSingleObject(wake_event_, 50);
                continue;
            }
            last_keepalive_tick = GetTickCount64();
        }

        if (read_pending &&
            WaitForSingleObject(read_event, 0) == WAIT_OBJECT_0)
        {
            if (!finish_read())
                disconnect();
            continue;
        }

        if (pending)
        {
            auto* header = reinterpret_cast<zime::broker_protocol::message_header*>(
                pending->bytes.data());
            header->connection_id = connection_id_;
#ifdef ZIME_PERF_DIAGNOSTIC
            if (pending->type ==
                    zime::broker_protocol::message_type::candidate_query &&
                pending->bytes.size() >=
                    sizeof(zime::broker_protocol::candidate_query))
            {
                const auto* query = reinterpret_cast<
                    const zime::broker_protocol::candidate_query*>(
                        pending->bytes.data());
                ZIME_PERF_RECORD(
                    "client.QuerySendDelay",
                    zime_perf_elapsed_us(pending->perf_enqueued_qpc),
                    static_cast<std::int64_t>(query->generation),
                    static_cast<std::int64_t>(query->code_chars));
            }
#endif
            if (!SendFrame(pipe, pending->bytes))
            {
                disconnect();
                continue;
            }
            pending.reset();
            last_keepalive_tick = GetTickCount64();
        }

        if (!read_pending)
        {
            ResetEvent(read_event);
            read_overlapped = {};
            read_overlapped.hEvent = read_event;
            DWORD transferred = 0;
            if (ReadFile(pipe,
                         read_buffer.data(),
                         static_cast<DWORD>(read_buffer.size()),
                         &transferred,
                         &read_overlapped))
            {
                if (transferred == 0 ||
                    !ProcessIncoming(read_buffer.data(), transferred))
                {
                    disconnect();
                }
                continue;
            }
            if (GetLastError() != ERROR_IO_PENDING)
            {
                disconnect();
                continue;
            }
            read_pending = true;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - last_keepalive_tick >= 15000)
        {
            zime::broker_protocol::message_header ping =
                zime::broker_protocol::make_header(
                    zime::broker_protocol::message_type::ping,
                    sizeof(ping),
                    GetTickCount64(),
                    connection_id_);
            std::vector<std::uint8_t> ping_bytes(sizeof(ping));
            std::memcpy(ping_bytes.data(), &ping, sizeof(ping));
            if (!SendFrame(pipe, ping_bytes))
            {
                disconnect();
                continue;
            }
            last_keepalive_tick = GetTickCount64();
        }

        bool queue_ready = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_ready = !queue_.empty();
        }
        if (queue_ready)
            continue;

        const ULONGLONG keepalive_elapsed =
            GetTickCount64() - last_keepalive_tick;
        const DWORD keepalive_wait = keepalive_elapsed >= 15000
            ? 0
            : static_cast<DWORD>(15000 - keepalive_elapsed);
        HANDLE events[4] = {};
        DWORD event_count = 0;
        const DWORD read_index = event_count;
        events[event_count++] = read_event;
        const DWORD wake_index = event_count;
        events[event_count++] = wake_event_;
        DWORD owner_index = MAXDWORD;
        if (owner_thread_)
        {
            owner_index = event_count;
            events[event_count++] = owner_thread_;
        }
        DWORD stop_index = MAXDWORD;
        if (stop_event_)
        {
            stop_index = event_count;
            events[event_count++] = stop_event_;
        }
        const DWORD wait_result = WaitForMultipleObjects(
            event_count,
            events,
            FALSE,
            keepalive_wait);
        if (wait_result == WAIT_OBJECT_0 + read_index)
        {
            if (!finish_read())
                disconnect();
        }
        else if ((owner_index != MAXDWORD &&
                  wait_result == WAIT_OBJECT_0 + owner_index) ||
                 (stop_index != MAXDWORD &&
                  wait_result == WAIT_OBJECT_0 + stop_index))
        {
            break;
        }
        else if (wait_result != WAIT_OBJECT_0 + wake_index &&
                 wait_result != WAIT_TIMEOUT)
        {
            disconnect();
        }
    }

    disconnect();
    CloseHandle(read_event);
}

HANDLE broker_client::ConnectAndHandshake()
{
    const bool appcontainer = current_process_is_appcontainer();
    const wchar_t* pipe_names[2] = {
        appcontainer
            ? zime::broker_protocol::kAppContainerPipeName
            : zime::broker_protocol::kPipeName,
        appcontainer ? zime::broker_protocol::kPipeName : nullptr,
    };
    DWORD initial_errors[2] = {ERROR_SUCCESS, ERROR_SUCCESS};
    DWORD final_errors[2] = {ERROR_SUCCESS, ERROR_SUCCESS};
    const wchar_t* selected_pipe_name = nullptr;

    for (std::size_t index = 0; index < std::size(pipe_names); ++index)
    {
        if (!pipe_names[index])
            continue;
        if (wait_for_named_pipe(
                stop_event_, owner_thread_, pipe_names[index], 50))
        {
            selected_pipe_name = pipe_names[index];
            break;
        }
        initial_errors[index] = GetLastError();
    }

    bool start_requested = false;
    if (!selected_pipe_name)
    {
        // An AppContainer cannot launch the unpackaged Broker itself. Always
        // retry the pipe even when startup fails or is throttled: another
        // full-trust TIP client may already be starting the per-user Broker.
        start_requested = TryStartBroker();
        if (lifetime_ended(stop_event_, owner_thread_))
            return INVALID_HANDLE_VALUE;
        for (std::size_t index = 0; index < std::size(pipe_names); ++index)
        {
            if (!pipe_names[index])
                continue;
            if (wait_for_named_pipe(
                    stop_event_, owner_thread_, pipe_names[index], 1500))
            {
                selected_pipe_name = pipe_names[index];
                break;
            }
            final_errors[index] = GetLastError();
        }
    }

    if (!selected_pipe_name)
    {
        ime_tracef(L"BrokerWait",
                   L"failed appcontainer=%d initial=(%lu,%lu) final=(%lu,%lu) start_requested=%d",
                   appcontainer ? 1 : 0,
                   initial_errors[0],
                   initial_errors[1],
                   final_errors[0],
                   final_errors[1],
                   start_requested ? 1 : 0);
        return INVALID_HANDLE_VALUE;
    }

    if (initial_errors[0] != ERROR_SUCCESS ||
        initial_errors[1] != ERROR_SUCCESS)
    {
        ime_tracef(L"BrokerWait",
                   L"recovered appcontainer=%d initial=(%lu,%lu) endpoint=%s start_requested=%d",
                   appcontainer ? 1 : 0,
                   initial_errors[0],
                   initial_errors[1],
                   selected_pipe_name,
                   start_requested ? 1 : 0);
    }

    HANDLE pipe = CreateFileW(selected_pipe_name,
                              FILE_READ_DATA |
                                  FILE_WRITE_DATA |
                                  FILE_WRITE_ATTRIBUTES |
                                  SYNCHRONIZE,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED |
                                  SECURITY_SQOS_PRESENT |
                                  SECURITY_IDENTIFICATION,
                              nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        ime_tracef(L"BrokerConnect",
                   L"open error=%lu endpoint=%s appcontainer=%d",
                   GetLastError(),
                   selected_pipe_name,
                   appcontainer ? 1 : 0);
        return INVALID_HANDLE_VALUE;
    }

    DWORD read_mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &read_mode, nullptr, nullptr))
    {
        ime_errorf(L"BrokerConnect",
                   L"set message mode error=%lu", GetLastError());
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }

    DWORD server_process_id = 0;
    if (!VerifyServerIdentity(pipe, &server_process_id))
    {
        ime_errorf(L"BrokerIdentity", L"rejected server_pid=%lu", server_process_id);
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }

    SetActivePipe(pipe);
    zime::broker_protocol::hello request = {};
    request.header = zime::broker_protocol::make_header(
        zime::broker_protocol::message_type::hello,
        sizeof(request),
        client_nonce_);
    request.reported_process_id = client_process_id_;
    request.reported_thread_id = client_thread_id_;
    request.reported_session_id = client_session_id_;
    request.pointer_size = sizeof(void*);
    request.client_nonce = client_nonce_;
    request.capabilities = zime::broker_protocol::capability_context_snapshot |
        zime::broker_protocol::capability_owned_windows |
        zime::broker_protocol::capability_candidate_window |
        zime::broker_protocol::capability_ui_actions |
        zime::broker_protocol::capability_status_window |
        zime::broker_protocol::capability_storage_writer;

    zime::broker_protocol::hello_result result = {};
    const bool wrote = transfer_struct(
        pipe,
        true,
        &request,
        zime::broker_protocol::kHelloTimeoutMs,
        stop_event_,
        owner_thread_);
    const bool read = wrote && transfer_struct(
        pipe,
        false,
        &result,
        zime::broker_protocol::kHelloTimeoutMs,
        stop_event_,
        owner_thread_);
    const bool valid = read &&
        zime::broker_protocol::has_valid_envelope(
            result.header,
            zime::broker_protocol::message_type::hello_result,
            sizeof(result)) &&
        result.header.request_id == request.header.request_id &&
        result.status == zime::broker_protocol::status_code::ok &&
        result.actual_client_process_id == client_process_id_ &&
        result.actual_client_session_id == client_session_id_ &&
        result.server_process_id == server_process_id &&
        result.server_session_id == client_session_id_ &&
        result.echoed_client_nonce == client_nonce_ &&
        result.header.connection_id != 0 &&
        (result.capabilities &
         zime::broker_protocol::capability_candidate_window) != 0;
    if (!valid)
    {
        ime_errorf(L"BrokerHello",
                   L"rejected server_pid=%lu read=%d status=%lu error=%lu",
                   server_process_id,
                   read ? 1 : 0,
                   read ? static_cast<DWORD>(result.status) : 0,
                   GetLastError());
        SetActivePipe(INVALID_HANDLE_VALUE);
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }

    connection_id_ = result.header.connection_id;
    // Broker revisions are process-local and restart from one. Never compare
    // a new Broker instance against revision counters from the old process.
    latest_config_revision_.store(0);
    latest_dictionary_revision_.store(0);
    storage_writer_available_.store(
        (result.capabilities &
         zime::broker_protocol::capability_storage_writer) != 0);
    SetConnected(true);
    ime_tracef(L"BrokerHello",
               L"connected server_pid=%lu session=%lu connection=%llu capabilities=0x%llx",
               server_process_id,
               result.server_session_id,
               static_cast<unsigned long long>(connection_id_),
               static_cast<unsigned long long>(result.capabilities));
    return pipe;
}

bool broker_client::TryStartBroker()
{
    const ULONGLONG now = GetTickCount64();
    if (last_start_attempt_tick_ != 0 &&
        now - last_start_attempt_tick_ < 15000)
    {
        return false;
    }
    last_start_attempt_tick_ = now;

    const std::filesystem::path broker_path =
        tool::get_current_dll_path() / L"zime_broker.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(broker_path, error) || error)
    {
        ime_errorf(L"BrokerStart", L"missing path=%s", broker_path.c_str());
        return false;
    }

    std::wstring command_line = L"\"" + broker_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const std::wstring working_directory = broker_path.parent_path().wstring();
    const BOOL created = CreateProcessW(
        broker_path.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process);
    if (!created)
    {
        ime_errorf(L"BrokerStart", L"error=%lu path=%s", GetLastError(), broker_path.c_str());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ime_tracef(L"BrokerStart", L"created pid=%lu path=%s", process.dwProcessId, broker_path.c_str());
    return true;
}

bool broker_client::SendFrame(HANDLE pipe,
                              const std::vector<std::uint8_t>& frame)
{
    if (frame.size() < sizeof(zime::broker_protocol::message_header) ||
        frame.size() > zime::broker_protocol::kMaxMessageSize)
    {
        return false;
    }
    DWORD transferred = 0;
    return overlapped_transfer(pipe,
                               true,
                               const_cast<std::uint8_t*>(frame.data()),
                               static_cast<DWORD>(frame.size()),
                               &transferred,
                               zime::broker_protocol::kIoTimeoutMs,
                               stop_event_,
                               owner_thread_) &&
        transferred == frame.size();
}

bool broker_client::ProcessIncoming(const std::uint8_t* bytes, std::size_t size)
{
    using namespace zime::broker_protocol;
    if (!bytes || size < sizeof(message_header))
        return false;

    const auto* header = reinterpret_cast<const message_header*>(bytes);
    if (header->magic != kMagic ||
        header->major_version != kMajorVersion ||
        header->minor_version > kMinorVersion ||
        header->size != size ||
        header->connection_id != connection_id_)
    {
        return false;
    }

    if (header->type == message_type::context_result)
    {
        if (size != sizeof(context_result))
            return false;
        const auto& result = *reinterpret_cast<const context_result*>(bytes);
        ime_tracef(L"BrokerContextResult",
                   L"status=%lu owner_pid=%lu owner_created=%lu owner_match=%lu visible=%lu error=%lu",
                   static_cast<DWORD>(result.status),
                   result.owner_process_id,
                   result.owner_window_created,
                   result.owner_relationship_matches,
                   result.owner_window_visible,
                   result.win32_error);
        return true;
    }

    if (header->type == message_type::ui_action)
    {
        if (size != sizeof(ui_action) ||
            !has_valid_envelope(*header, message_type::ui_action, sizeof(ui_action)))
        {
            return false;
        }
        const auto& action = *reinterpret_cast<const ui_action*>(bytes);
        const bool candidate_action =
            action.action == ui_action_type::candidate_select ||
            action.action == ui_action_type::candidate_page ||
            action.action == ui_action_type::candidate_delete ||
            action.action == ui_action_type::candidate_mark_uncommon;
        const std::uint64_t expected_generation = candidate_action
            ? latest_candidate_generation_.load()
            : latest_status_generation_.load();
        if (action.generation != expected_generation)
            return true;
        PostAction(action);
        return true;
    }

    if (header->type == message_type::storage_result)
    {
        if (size < sizeof(storage_result))
            return false;
        const auto& result = *reinterpret_cast<const storage_result*>(bytes);
        const std::size_t expected_size = sizeof(storage_result) +
            static_cast<std::size_t>(result.error_chars) * sizeof(wchar_t);
        if (expected_size != size || result.error_chars > 1024)
            return false;

        std::wstring error;
        if (result.error_chars != 0)
        {
            const auto* error_text = reinterpret_cast<const wchar_t*>(
                bytes + sizeof(storage_result));
            error.assign(error_text, error_text + result.error_chars);
        }
        PostStorageResult(result.operation,
                          result.header.request_id,
                          result.status == status_code::ok,
                          std::move(error));
        return true;
    }

    if (header->type == message_type::config_state)
    {
        if (!has_valid_envelope(
                *header, message_type::config_state, sizeof(config_state)))
        {
            return false;
        }
        const auto& state = *reinterpret_cast<const config_state*>(bytes);
        if (state.revision == 0)
            return false;
        std::uint64_t previous = latest_config_revision_.load();
        while (state.revision > previous &&
               !latest_config_revision_.compare_exchange_weak(
                   previous, state.revision))
        {
        }
        if (state.revision > previous)
            PostConfigState(state);
        return true;
    }

    if (header->type == message_type::candidate_result)
    {
        if (size < sizeof(candidate_result))
            return false;
        const auto& result = *reinterpret_cast<const candidate_result*>(bytes);
        if (result.code_chars > result.string_chars ||
            result.candidate_count > 512)
        {
            return false;
        }
        const std::size_t expected_size = sizeof(candidate_result) +
            static_cast<std::size_t>(result.candidate_count) *
                sizeof(engine_candidate_record) +
            static_cast<std::size_t>(result.string_chars) * sizeof(wchar_t);
        if (expected_size != size)
            return false;
        bool synchronous_query = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            synchronous_query =
                result.generation == pending_sync_query_generation_;
        }
        ZIME_PERF_RECORD("client.ResultReceived",
                         static_cast<std::int64_t>(result.generation),
                         synchronous_query ? 1 : 0,
                         static_cast<std::int64_t>(result.candidate_count));
        if (!synchronous_query &&
            result.generation != latest_query_generation_.load())
            return true;

        const auto* records = reinterpret_cast<const engine_candidate_record*>(
            bytes + sizeof(candidate_result));
        const auto* strings = reinterpret_cast<const wchar_t*>(
            bytes + sizeof(candidate_result) +
            static_cast<std::size_t>(result.candidate_count) *
                sizeof(engine_candidate_record));
        std::wstring code(strings, result.code_chars);
        std::vector<std::wstring> candidates;
        std::vector<std::wstring> view_texts;
        std::vector<bool> pinyin_flags;
        candidates.reserve(result.candidate_count);
        view_texts.reserve(result.candidate_count);
        pinyin_flags.reserve(result.candidate_count);
        for (std::uint32_t index = 0; index < result.candidate_count; ++index)
        {
            const auto& record = records[index];
            if (record.text_offset_chars > result.string_chars ||
                record.text_length_chars >
                    result.string_chars - record.text_offset_chars ||
                record.view_offset_chars > result.string_chars ||
                record.view_length_chars >
                    result.string_chars - record.view_offset_chars)
            {
                return false;
            }
            candidates.emplace_back(strings + record.text_offset_chars,
                                    record.text_length_chars);
            view_texts.emplace_back(strings + record.view_offset_chars,
                                    record.view_length_chars);
            pinyin_flags.push_back(
                (record.flags & candidate_record_pinyin) != 0);
        }
        if (synchronous_query)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result.generation == pending_sync_query_generation_)
            {
                sync_candidate_texts_ = std::move(candidates);
                sync_candidate_view_texts_ = std::move(view_texts);
                sync_candidate_pinyin_flags_ = std::move(pinyin_flags);
                sync_candidate_result_flags_ = result.flags;
                sync_candidate_config_revision_ = result.config_revision;
                sync_candidate_success_ = result.status == status_code::ok;
                sync_candidate_completed_ = true;
                sync_candidate_ready_.notify_all();
            }
            return true;
        }
        PostCandidateResult(result.generation,
                            std::move(code),
                            std::move(candidates),
                            std::move(view_texts),
                            std::move(pinyin_flags),
                            result.status == status_code::ok ? result.flags : 0,
                            result.config_revision);
        return true;
    }

    if (header->type == message_type::dictionary_state)
    {
        if (size != sizeof(dictionary_state))
            return false;
        const auto& state = *reinterpret_cast<const dictionary_state*>(bytes);
        std::uint64_t previous = latest_dictionary_revision_.load();
        while (state.revision > previous &&
               !latest_dictionary_revision_.compare_exchange_weak(
                   previous, state.revision))
        {
        }
        if (state.revision > previous)
            PostDictionaryState(state.revision);
        return true;
    }

    if (header->type == message_type::pong)
        return size == sizeof(message_header);

    return false;
}

bool broker_client::VerifyServerIdentity(HANDLE pipe, DWORD* server_process_id) const
{
    if (!server_process_id || !GetNamedPipeServerProcessId(pipe, server_process_id))
    {
        ime_errorf(L"BrokerIdentity",
                   L"server pid query failed error=%lu", GetLastError());
        return false;
    }

    const std::vector<std::filesystem::path> expected_paths =
        trusted_broker_paths();
    if (expected_paths.empty())
    {
        ime_errorf(L"BrokerIdentity", L"no trusted broker path");
        return false;
    }

    std::wstring actual_path;
    if (!query_process_path(*server_process_id, &actual_path))
    {
        const DWORD path_error = GetLastError();
        ULONG server_session_id = 0;
        const bool queried_server_session =
            GetNamedPipeServerSessionId(pipe, &server_session_id) != FALSE;
        const DWORD session_error =
            queried_server_session ? ERROR_SUCCESS : GetLastError();
        const bool same_session = queried_server_session &&
            server_session_id == client_session_id_;

        // AppContainer tokens cannot normally open a medium-integrity process
        // to query its image path.  Keep exact path verification everywhere
        // else; here the secured pipe plus the nonce/PID/session-bound hello is
        // the only usable identity chain available to an in-container TIP.
        if (path_error == ERROR_ACCESS_DENIED &&
            current_process_is_appcontainer() &&
            same_session)
        {
            ime_tracef(L"BrokerIdentity",
                       L"appcontainer fallback server_pid=%lu session=%lu trusted_paths=%llu",
                       *server_process_id,
                       server_session_id,
                       static_cast<unsigned long long>(expected_paths.size()));
            return true;
        }

        ime_errorf(L"BrokerIdentity",
                   L"server path query failed server_pid=%lu error=%lu appcontainer=%d session_query_error=%lu server_session=%lu client_session=%lu same_session=%d",
                   *server_process_id,
                   path_error,
                   current_process_is_appcontainer() ? 1 : 0,
                   session_error,
                   server_session_id,
                   client_session_id_,
                   same_session ? 1 : 0);
        return false;
    }

    const std::wstring actual = normalized_path(actual_path);
    const auto matching_path = std::find_if(
        expected_paths.begin(), expected_paths.end(), [&](const auto& expected_path) {
            const std::wstring expected = normalized_path(expected_path);
            return _wcsicmp(expected.c_str(), actual.c_str()) == 0;
        });
    const bool matches = matching_path != expected_paths.end();
    if (!matches)
    {
        ime_errorf(L"BrokerIdentity",
                   L"path mismatch actual=%s trusted_paths=%llu",
                   actual.c_str(),
                   static_cast<unsigned long long>(expected_paths.size()));
    }
    return matches;
}

void broker_client::SetActivePipe(HANDLE pipe)
{
    std::lock_guard<std::mutex> lock(mutex_);
    active_pipe_ = pipe;
}

void broker_client::SetConnected(bool connected)
{
    if (!connected)
    {
        storage_writer_available_.store(false);
        latest_config_revision_.store(0);
        latest_dictionary_revision_.store(0);
    }
    const bool previous = connected_.exchange(connected);
    if (previous == connected || !callback_window_ ||
        lifetime_ended(stop_event_, owner_thread_))
        return;

    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::connection_state;
    callback->connected = connected;
    if (!PostMessageW(callback_window_,
                      WM_BROKER_CALLBACK,
                      0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

bool broker_client::EnqueueFrame(queued_frame frame)
{
#ifdef ZIME_PERF_DIAGNOSTIC
    frame.perf_enqueued_qpc = zime_perf_now();
#endif
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_.joinable() || stopping_)
            return false;

        const bool coalesce = frame.coalesce ||
            frame.type == zime::broker_protocol::message_type::context_snapshot ||
            frame.type == zime::broker_protocol::message_type::candidate_state ||
            frame.type == zime::broker_protocol::message_type::status_state;
        if (coalesce)
        {
            queue_.erase(std::remove_if(queue_.begin(),
                                        queue_.end(),
                                        [&frame](const queued_frame& existing)
                                        {
                                            return existing.type == frame.type;
                                        }),
                         queue_.end());
        }
        constexpr std::size_t kMaxStorageFrames = 64;
        if (frame.type ==
            zime::broker_protocol::message_type::storage_mutation)
        {
            const std::size_t storage_frames = static_cast<std::size_t>(
                std::count_if(queue_.begin(), queue_.end(),
                [](const queued_frame& existing)
                {
                    return existing.type ==
                        zime::broker_protocol::message_type::storage_mutation;
                }));
            if (storage_frames >= kMaxStorageFrames)
                return false;
        }
        if (frame.priority ||
            frame.type ==
                zime::broker_protocol::message_type::candidate_query)
            queue_.push_front(std::move(frame));
        else
            queue_.push_back(std::move(frame));
    }
    if (wake_event_)
        SetEvent(wake_event_);
    return true;
}

void broker_client::PostAction(const zime::broker_protocol::ui_action& action)
{
    if (!callback_window_)
        return;
    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::ui_action;
    callback->connected = true;
    callback->action = action;
    if (!PostMessageW(callback_window_,
                      WM_BROKER_CALLBACK,
                      0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

void broker_client::PostStorageResult(
    zime::broker_protocol::storage_operation operation,
    std::uint64_t request_id,
    bool success,
    std::wstring error)
{
    if (!callback_window_)
        return;
    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::storage_result;
    callback->connected = true;
    callback->storage_operation = operation;
    callback->storage_request_id = request_id;
    callback->storage_success = success;
    callback->storage_error = std::move(error);
    if (!PostMessageW(callback_window_,
                      WM_BROKER_CALLBACK,
                      0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

void broker_client::PostConfigState(
    const zime::broker_protocol::config_state& state)
{
    if (!callback_window_)
        return;
    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::config_state;
    callback->connected = true;
    callback->config = state;
    if (!PostMessageW(callback_window_,
                      WM_BROKER_CALLBACK,
                      0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

void broker_client::PostCandidateResult(
    std::uint64_t generation,
    std::wstring code,
    std::vector<std::wstring> candidates,
    std::vector<std::wstring> view_texts,
    std::vector<bool> pinyin_flags,
    std::uint32_t flags,
    std::uint64_t config_revision)
{
    if (!callback_window_)
        return;
    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::candidate_result;
    callback->candidate_query_generation = generation;
    callback->candidate_config_revision = config_revision;
    callback->candidate_result_flags = flags;
    callback->candidate_code = std::move(code);
    callback->candidate_texts = std::move(candidates);
    callback->candidate_view_texts = std::move(view_texts);
    callback->candidate_pinyin_flags = std::move(pinyin_flags);
#ifdef ZIME_PERF_DIAGNOSTIC
    callback->perf_posted_qpc = zime_perf_now();
#endif
    if (!PostMessageW(callback_window_,
                      WM_BROKER_CALLBACK,
                      0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

void broker_client::PostDictionaryState(std::uint64_t revision)
{
    if (!callback_window_)
        return;
    auto* callback = new (std::nothrow) callback_message();
    if (!callback)
        return;
    callback->kind = callback_kind::dictionary_state;
    callback->dictionary_revision = revision;
    if (!PostMessageW(callback_window_, WM_BROKER_CALLBACK, 0,
                      reinterpret_cast<LPARAM>(callback)))
    {
        delete callback;
    }
}

bool broker_client::CreateCallbackWindow()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CallbackWindowProc),
                       &module);
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = CallbackWindowProc;
    window_class.hInstance = module;
    window_class.lpszClassName = kCallbackWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    callback_window_ = CreateWindowExW(0,
                                       kCallbackWindowClass,
                                       L"",
                                       0,
                                       0,
                                       0,
                                       0,
                                       0,
                                       HWND_MESSAGE,
                                       nullptr,
                                       module,
                                       this);
    return callback_window_ != nullptr;
}

void broker_client::DestroyCallbackWindow()
{
    if (!callback_window_)
        return;
    if (owner_thread_ &&
        WaitForSingleObject(owner_thread_, 0) == WAIT_OBJECT_0)
    {
        // Windows already destroyed windows owned by the terminated thread.
    }
    else if (GetCurrentThreadId() == callback_thread_id_)
    {
        MSG message = {};
        bool repost_quit = false;
        int quit_code = 0;
        while (PeekMessageW(&message,
                            callback_window_,
                            WM_BROKER_CALLBACK,
                            WM_BROKER_CALLBACK,
                            PM_REMOVE))
        {
            // WM_QUIT bypasses both the window and message-range filters. Never
            // consume the host thread's quit request while draining callbacks.
            if (message.message == WM_QUIT)
            {
                repost_quit = true;
                quit_code = static_cast<int>(message.wParam);
                break;
            }
            delete reinterpret_cast<callback_message*>(message.lParam);
        }
        DestroyWindow(callback_window_);
        if (repost_quit)
            PostQuitMessage(quit_code);
    }
    else
    {
        PostMessageW(callback_window_, WM_CLOSE, 0, 0);
    }
    callback_window_ = nullptr;
}

LRESULT CALLBACK broker_client::CallbackWindowProc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    broker_client* client = reinterpret_cast<broker_client*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        client = static_cast<broker_client*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(client));
    }
    else if (message == WM_BROKER_CALLBACK)
    {
        std::unique_ptr<callback_message> callback(
            reinterpret_cast<callback_message*>(lparam));
        if (client && callback)
        {
            if (callback->kind == callback_kind::connection_state)
            {
                if (client->connection_callback_)
                    client->connection_callback_(callback->connected);
            }
            else if (callback->kind == callback_kind::storage_result)
            {
                if (client->storage_callback_)
                {
                    client->storage_callback_(callback->storage_operation,
                                              callback->storage_request_id,
                                              callback->storage_success,
                                              callback->storage_error);
                }
            }
            else if (callback->kind == callback_kind::config_state)
            {
                if (client->config_callback_)
                    client->config_callback_(callback->config);
            }
            else if (callback->kind == callback_kind::candidate_result)
            {
#ifdef ZIME_PERF_DIAGNOSTIC
                ZIME_PERF_RECORD(
                    "client.CandidateCallbackDelay",
                    zime_perf_elapsed_us(callback->perf_posted_qpc),
                    static_cast<std::int64_t>(callback->candidate_code.size()),
                    static_cast<std::int64_t>(callback->candidate_texts.size()));
#endif
                if (client->candidate_result_callback_ &&
                    callback->candidate_query_generation ==
                        client->latest_query_generation_.load())
                {
                    client->candidate_result_callback_(
                        callback->candidate_query_generation,
                        callback->candidate_code,
                        callback->candidate_texts,
                        callback->candidate_view_texts,
                        callback->candidate_pinyin_flags,
                        callback->candidate_result_flags,
                        callback->candidate_config_revision);
                }
            }
            else if (callback->kind == callback_kind::dictionary_state)
            {
                if (client->dictionary_state_callback_)
                    client->dictionary_state_callback_(
                        callback->dictionary_revision);
            }
            else if (client->action_callback_)
            {
                const bool candidate_action =
                    callback->action.action ==
                        zime::broker_protocol::ui_action_type::candidate_select ||
                    callback->action.action ==
                        zime::broker_protocol::ui_action_type::candidate_page ||
                    callback->action.action ==
                        zime::broker_protocol::ui_action_type::candidate_delete ||
                    callback->action.action ==
                        zime::broker_protocol::ui_action_type::candidate_mark_uncommon;
                const std::uint64_t expected_generation = candidate_action
                    ? client->latest_candidate_generation_.load()
                    : client->latest_status_generation_.load();
                if (callback->action.generation == expected_generation)
                {
                    POINT point = {callback->action.screen_x,
                                   callback->action.screen_y};
                    client->action_callback_(callback->action.action,
                                             callback->action.value,
                                             point);
                }
            }
        }
        return 0;
    }
    else if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}
