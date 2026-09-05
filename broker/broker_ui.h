#pragma once

#include "broker_protocol.h"
#include "candidate_form.h"
#include "status_window.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class broker_storage;

struct broker_candidate_update
{
    std::uint64_t connection_id;
    std::uint64_t generation;
    HWND view_hwnd;
    HWND owner_hwnd;
    RECT anchor;
    bool anchor_valid;
    bool visible;
    bool custom_ui_allowed;
    bool presentation_pending;
    std::uint32_t selection_absolute;
    std::uint32_t page_size;
    std::uint32_t current_page;
    std::uint32_t ui_font_percent;
    std::wstring composition;
    std::vector<std::wstring> candidates;
    std::function<void(const zime::broker_protocol::ui_action&)> send_action;
};

struct broker_candidate_result_update
{
    std::uint64_t connection_id;
    std::uint64_t query_generation;
    std::uint32_t result_flags;
    std::wstring composition;
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> view_texts;
};

struct broker_status_update
{
    std::uint64_t connection_id;
    std::uint64_t generation;
    HWND owner_hwnd;
    std::uint32_t flags;
    int position_x;
    int position_y;
    std::uint32_t ui_font_percent;
    std::uint32_t candidate_sort_mode;
    std::function<void(const zime::broker_protocol::ui_action&)> send_action;
};

class broker_ui_controller
{
public:
    broker_ui_controller();
    ~broker_ui_controller();

    bool Initialize(
        HINSTANCE instance,
        broker_storage* storage,
        std::function<void(const zime::broker_protocol::config_state&)>
            config_committed,
        std::function<void(std::uint64_t)> dictionary_committed);
    HWND hwnd() const { return controller_window_; }
    static bool PostCandidateUpdate(
        HWND controller_window,
        std::unique_ptr<broker_candidate_update> update);
    static bool PostCandidateResult(
        HWND controller_window,
        std::unique_ptr<broker_candidate_result_update> update);
    static bool PostStatusUpdate(
        HWND controller_window,
        std::unique_ptr<broker_status_update> update);
    static void PostConnectionClosed(HWND controller_window,
                                     std::uint64_t connection_id);

private:
    static LRESULT CALLBACK WindowProc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void ApplyCandidateUpdate(std::unique_ptr<broker_candidate_update> update);
    void ApplyCandidateResult(
        std::unique_ptr<broker_candidate_result_update> update);
    void ApplyStatusUpdate(std::unique_ptr<broker_status_update> update);
    void OnConnectionClosed(std::uint64_t connection_id);
    void EnsureCandidateWindow(HWND owner);
    void PositionCandidateWindow(const RECT& anchor);
    void SendCandidateAction(zime::broker_protocol::ui_action_type type,
                             std::uint32_t value,
                             POINT screen_point = {});
    void ShowCandidateContextMenu(int candidate_index, POINT screen_point);
    void EnsureStatusWindow();
    void SendStatusChange(int status_type);
    void CommitGlobalStatusChange(int status_type);
    void CommitStatusPosition(int x, int y);
    void ApplyGlobalConfigToWindows();
    bool SaveGlobalConfig();
    void ShowCreateWordWindow();
    void ExportRawDictionary();
    void SendStatusAction(zime::broker_protocol::ui_action_type type,
                          std::uint32_t value,
                          POINT point = {});

    HINSTANCE instance_;
    broker_storage* storage_;
    std::function<void(const zime::broker_protocol::config_state&)>
        config_committed_;
    std::function<void(std::uint64_t)> dictionary_committed_;
    zime::broker_protocol::config_state global_config_;
    HWND controller_window_;
    candidate_form candidate_window_;
    status_window status_window_;
    std::uint64_t active_connection_id_;
    std::uint64_t active_generation_;
    std::uint64_t active_query_generation_;
    std::uint64_t candidate_mutation_request_id_;
    std::wstring active_composition_;
    std::unordered_map<std::uint64_t, broker_candidate_result_update>
        cached_candidate_results_;
    RECT active_anchor_;
    bool active_anchor_valid_;
    std::function<void(const zime::broker_protocol::ui_action&)> active_sender_;
    std::uint64_t active_status_connection_id_;
    std::uint64_t active_status_generation_;
    std::function<void(const zime::broker_protocol::ui_action&)> active_status_sender_;
};
