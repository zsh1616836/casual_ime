#pragma once

#include "../common/broker_protocol.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class broker_client
{
public:
    using ui_action_callback = std::function<void(
        zime::broker_protocol::ui_action_type, std::uint32_t, POINT)>;
    using connection_state_callback = std::function<void(bool)>;
    using storage_result_callback = std::function<void(
        zime::broker_protocol::storage_operation,
        std::uint64_t,
        bool,
        const std::wstring&)>;
    using config_state_callback = std::function<void(
        const zime::broker_protocol::config_state&)>;
    using candidate_result_callback = std::function<void(
        std::uint64_t,
        const std::wstring&,
        const std::vector<std::wstring>&,
        const std::vector<std::wstring>&,
        const std::vector<bool>&,
        std::uint32_t,
        std::uint64_t)>;
    using dictionary_state_callback = std::function<void(std::uint64_t)>;

    broker_client();
    ~broker_client();

    broker_client(const broker_client&) = delete;
    broker_client& operator=(const broker_client&) = delete;

    void SetCallbacks(ui_action_callback action_callback,
                      connection_state_callback connection_callback);
    void SetStorageResultCallback(storage_result_callback callback);
    void SetConfigStateCallback(config_state_callback callback);
    void SetCandidateResultCallback(candidate_result_callback callback);
    void SetDictionaryStateCallback(dictionary_state_callback callback);
    void Start();
    void Stop();
    void SendContextSnapshot(HWND view_hwnd);
    void SendCandidateState(HWND owner_hwnd,
                            const RECT* anchor,
                            bool visible,
                            bool custom_ui_allowed,
                            const std::wstring& composition,
                            std::uint32_t selection_absolute,
                            std::uint32_t page_size,
                            std::uint32_t current_page,
                            std::uint32_t ui_font_percent,
                            bool presentation_pending = false);
    void SendStatusState(HWND owner_hwnd,
                         bool visible,
                         std::uint32_t state_flags,
                         int position_x,
                         int position_y,
                         std::uint32_t ui_font_percent,
                         std::uint32_t candidate_sort_mode);
    std::uint64_t SendStorageMutation(
        zime::broker_protocol::storage_operation operation,
        const std::wstring& code,
        const std::wstring& text,
        bool expects_result);
    std::uint64_t RequestCandidates(const std::wstring& code,
                                    bool apply_auto_commit);
    bool QueryCandidatesSync(const std::wstring& code,
                             std::vector<std::wstring>* candidates,
                             std::vector<std::wstring>* view_texts,
                             std::vector<bool>* pinyin_flags,
                             DWORD timeout_ms = 800,
                             bool apply_auto_commit = false,
                             std::uint32_t* result_flags = nullptr,
                             std::uint64_t* config_revision = nullptr);
    [[nodiscard]] bool IsConnected() const { return connected_.load(); }
    [[nodiscard]] bool HasStorageWriter() const
    {
        return storage_writer_available_.load();
    }

private:
    struct queued_frame
    {
        zime::broker_protocol::message_type type;
        std::vector<std::uint8_t> bytes;
        bool coalesce = false;
        bool priority = false;
#ifdef ZIME_PERF_DIAGNOSTIC
        std::int64_t perf_enqueued_qpc = 0;
#endif
    };

    enum class callback_kind
    {
        connection_state,
        ui_action,
        storage_result,
        config_state,
        candidate_result,
        dictionary_state,
    };

    struct callback_message
    {
        callback_kind kind;
        bool connected;
        zime::broker_protocol::ui_action action;
        zime::broker_protocol::storage_operation storage_operation;
        std::uint64_t storage_request_id;
        bool storage_success;
        std::wstring storage_error;
        zime::broker_protocol::config_state config;
        std::uint64_t candidate_query_generation;
        std::uint64_t candidate_config_revision;
        std::uint32_t candidate_result_flags;
        std::wstring candidate_code;
        std::vector<std::wstring> candidate_texts;
        std::vector<std::wstring> candidate_view_texts;
        std::vector<bool> candidate_pinyin_flags;
        std::uint64_t dictionary_revision;
#ifdef ZIME_PERF_DIAGNOSTIC
        std::int64_t perf_posted_qpc;
#endif
    };

    void Run();
    HANDLE ConnectAndHandshake();
    bool TryStartBroker();
    bool SendFrame(HANDLE pipe, const std::vector<std::uint8_t>& frame);
    bool ProcessIncoming(const std::uint8_t* bytes, std::size_t size);
    bool VerifyServerIdentity(HANDLE pipe, DWORD* server_process_id) const;
    void SetActivePipe(HANDLE pipe);
    void SetConnected(bool connected);
    bool EnqueueFrame(queued_frame frame);
    void PostAction(const zime::broker_protocol::ui_action& action);
    void PostStorageResult(zime::broker_protocol::storage_operation operation,
                           std::uint64_t request_id,
                           bool success,
                           std::wstring error);
    void PostConfigState(const zime::broker_protocol::config_state& state);
    void PostCandidateResult(std::uint64_t generation,
                             std::wstring code,
                             std::vector<std::wstring> candidates,
                             std::vector<std::wstring> view_texts,
                             std::vector<bool> pinyin_flags,
                             std::uint32_t flags,
                             std::uint64_t config_revision);
    void PostDictionaryState(std::uint64_t revision);
    bool CreateCallbackWindow();
    void DestroyCallbackWindow();
    static LRESULT CALLBACK CallbackWindowProc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    std::mutex mutex_;
    std::condition_variable sync_candidate_ready_;
    std::deque<queued_frame> queue_;
    std::thread worker_;
    HANDLE wake_event_;
    HANDLE stop_event_;
    HANDLE owner_thread_;
    HANDLE active_pipe_;
    bool stopping_;
    std::atomic<bool> connected_;
    std::atomic<bool> storage_writer_available_;
    DWORD client_process_id_;
    DWORD client_thread_id_;
    DWORD client_session_id_;
    std::uint64_t client_nonce_;
    std::uint64_t connection_id_;
    std::uint64_t next_request_id_;
    std::uint64_t next_candidate_generation_;
    std::uint64_t next_status_generation_;
    std::uint64_t next_query_generation_;
    std::atomic<std::uint64_t> latest_candidate_generation_;
    std::atomic<std::uint64_t> latest_status_generation_;
    std::atomic<std::uint64_t> latest_config_revision_;
    std::atomic<std::uint64_t> latest_query_generation_;
    std::atomic<std::uint64_t> latest_dictionary_revision_;
    std::uint64_t pending_sync_query_generation_;
    bool sync_candidate_completed_;
    bool sync_candidate_success_;
    std::vector<std::wstring> sync_candidate_texts_;
    std::vector<std::wstring> sync_candidate_view_texts_;
    std::vector<bool> sync_candidate_pinyin_flags_;
    std::uint32_t sync_candidate_result_flags_;
    std::uint64_t sync_candidate_config_revision_;
    HWND last_view_hwnd_;
    ULONGLONG last_snapshot_tick_;
    ULONGLONG last_start_attempt_tick_;
    HWND callback_window_;
    DWORD callback_thread_id_;
    ui_action_callback action_callback_;
    connection_state_callback connection_callback_;
    storage_result_callback storage_callback_;
    config_state_callback config_callback_;
    candidate_result_callback candidate_result_callback_;
    dictionary_state_callback dictionary_state_callback_;
};
