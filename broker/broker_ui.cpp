#include "broker_ui.h"

#include "broker_log.h"
#include "broker_storage.h"

#include <algorithm>
#include <commdlg.h>
#include <cwctype>
#include <new>

namespace
{
constexpr wchar_t kControllerWindowClass[] = L"ZImeBrokerUiController";
constexpr UINT WM_BROKER_CANDIDATE_UPDATE = WM_APP + 0x411;
constexpr UINT WM_BROKER_CONNECTION_CLOSED = WM_APP + 0x412;
constexpr UINT WM_BROKER_STATUS_UPDATE = WM_APP + 0x413;
constexpr UINT WM_BROKER_CANDIDATE_RESULT = WM_APP + 0x414;
constexpr UINT ID_CANDIDATE_DELETE = 3101;
constexpr UINT ID_CANDIDATE_MARK_UNCOMMON = 3102;
constexpr UINT ID_CREATE_WORD_TEXT = 3201;
constexpr UINT ID_CREATE_WORD_CODE = 3202;
constexpr UINT ID_CREATE_WORD_OK = 3203;
constexpr wchar_t kCreateWordWindowClass[] = L"ZImeBrokerCreateWord";

struct create_word_context
{
    broker_storage* storage;
    std::function<void()> dictionary_changed;
};

LRESULT CALLBACK create_word_window_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* context = reinterpret_cast<create_word_context*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        context = static_cast<create_word_context*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(context));
    }
    if (message == WM_CREATE)
    {
        CreateWindowW(L"STATIC", L"\u65b0\u8bcd\uff1a", WS_CHILD | WS_VISIBLE,
                      20, 22, 70, 24, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        92, 18, 260, 28, hwnd,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(ID_CREATE_WORD_TEXT)),
                        nullptr, nullptr);
        CreateWindowW(L"STATIC", L"\u7f16\u7801\uff1a", WS_CHILD | WS_VISIBLE,
                      20, 66, 70, 24, hwnd, nullptr, nullptr, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        92, 62, 260, 28, hwnd,
                        reinterpret_cast<HMENU>(
                            static_cast<INT_PTR>(ID_CREATE_WORD_CODE)),
                        nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"\u786e\u5b9a",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      184, 116, 80, 30, hwnd,
                      reinterpret_cast<HMENU>(
                          static_cast<INT_PTR>(ID_CREATE_WORD_OK)),
                      nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"\u53d6\u6d88",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      272, 116, 80, 30, hwnd,
                      reinterpret_cast<HMENU>(
                          static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        SetFocus(GetDlgItem(hwnd, ID_CREATE_WORD_TEXT));
        return 0;
    }
    if (message == WM_COMMAND)
    {
        const int command = LOWORD(wparam);
        if (command == IDCANCEL)
        {
            DestroyWindow(hwnd);
            return 0;
        }
        if (command == ID_CREATE_WORD_OK && context && context->storage)
        {
            wchar_t word[257] = {};
            wchar_t code[65] = {};
            GetWindowTextW(GetDlgItem(hwnd, ID_CREATE_WORD_TEXT),
                           word, static_cast<int>(std::size(word)));
            GetWindowTextW(GetDlgItem(hwnd, ID_CREATE_WORD_CODE),
                           code, static_cast<int>(std::size(code)));
            if (!word[0] || !code[0])
            {
                MessageBoxW(hwnd, L"\u65b0\u8bcd\u548c\u7f16\u7801\u4e0d\u80fd\u4e3a\u7a7a\u3002", L"ZIme",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            std::wstring normalized_code(code);
            bool valid = true;
            for (wchar_t& character : normalized_code)
            {
                character = static_cast<wchar_t>(towlower(character));
                if (character < L'a' || character > L'y')
                    valid = false;
            }
            if (!valid)
            {
                MessageBoxW(hwnd, L"\u7f16\u7801\u53ea\u80fd\u5305\u542b a-y\u3002", L"ZIme",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            std::wstring error;
            const bool success = context->storage->ApplyMutation(
                0x42524f4b45525549ull,
                GetTickCount64(),
                zime::broker_protocol::storage_operation::add_custom_word,
                normalized_code,
                word,
                &error);
            if (!success)
            {
                MessageBoxW(hwnd,
                            error.empty() ? L"\u9020\u8bcd\u5931\u8d25\u3002" : error.c_str(),
                            L"ZIme", MB_OK | MB_ICONERROR);
                return 0;
            }
            if (context->dictionary_changed)
                context->dictionary_changed();
            DestroyWindow(hwnd);
            return 0;
        }
    }
    if (message == WM_CLOSE)
    {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool try_get_window_thread_caret(HWND view, HWND owner, RECT* anchor)
{
    if (!anchor)
        return false;
    HWND thread_window = view && IsWindow(view) ? view : owner;
    const DWORD thread_id = thread_window
        ? GetWindowThreadProcessId(thread_window, nullptr)
        : 0;
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    if (!thread_id || !GetGUIThreadInfo(thread_id, &info) || !info.hwndCaret)
        return false;

    RECT rect = info.rcCaret;
    POINT top_left = {rect.left, rect.top};
    POINT bottom_right = {rect.right, rect.bottom};
    if (!ClientToScreen(info.hwndCaret, &top_left) ||
        !ClientToScreen(info.hwndCaret, &bottom_right))
    {
        return false;
    }
    rect = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    if (rect.right <= rect.left)
        rect.right = rect.left + 2;
    if (rect.bottom <= rect.top)
        rect.bottom = rect.top + 20;
    if (rect.left <= 1 && rect.top <= 1 &&
        rect.right <= 4 && rect.bottom <= 40)
    {
        return false;
    }
    *anchor = rect;
    return true;
}

bool try_get_visible_window_anchor(HWND view, HWND owner, RECT* anchor)
{
    if (!anchor)
        return false;
    HWND window = view && IsWindow(view) ? view : owner;
    RECT rect = {};
    if (!window || !GetWindowRect(window, &rect) ||
        rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return false;
    }
    anchor->left = rect.left + 4;
    anchor->top = rect.top + 4;
    anchor->right = anchor->left + 2;
    anchor->bottom = (std::min)(rect.bottom, anchor->top + 20);
    return anchor->bottom > anchor->top;
}
}

broker_ui_controller::broker_ui_controller()
    : instance_(nullptr),
      storage_(nullptr),
      controller_window_(nullptr),
      active_connection_id_(0),
      active_generation_(0),
      active_query_generation_(0),
      active_anchor_{},
      active_anchor_valid_(false),
      active_status_connection_id_(0),
      active_status_generation_(0)
{
}

broker_ui_controller::~broker_ui_controller()
{
    candidate_window_.destroy();
    status_window_.destroy();
    if (controller_window_)
        DestroyWindow(controller_window_);
}

bool broker_ui_controller::Initialize(
    HINSTANCE instance,
    broker_storage* storage,
    std::function<void(const zime::broker_protocol::config_state&)>
        config_committed,
    std::function<void(std::uint64_t)> dictionary_committed)
{
    instance_ = instance;
    storage_ = storage;
    config_committed_ = std::move(config_committed);
    dictionary_committed_ = std::move(dictionary_committed);
    if (storage_)
        global_config_ = storage_->LoadConfig();
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kControllerWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    controller_window_ = CreateWindowExW(0,
                                         kControllerWindowClass,
                                         L"",
                                         0,
                                         0,
                                         0,
                                         0,
                                         0,
                                         HWND_MESSAGE,
                                         nullptr,
                                         instance,
                                         this);
    return controller_window_ != nullptr;
}

bool broker_ui_controller::PostCandidateUpdate(
    HWND controller_window,
    std::unique_ptr<broker_candidate_update> update)
{
    if (!controller_window || !update)
        return false;
    broker_candidate_update* raw_update = update.release();
    if (!PostMessageW(controller_window,
                      WM_BROKER_CANDIDATE_UPDATE,
                      0,
                      reinterpret_cast<LPARAM>(raw_update)))
    {
        delete raw_update;
        return false;
    }
    return true;
}

bool broker_ui_controller::PostCandidateResult(
    HWND controller_window,
    std::unique_ptr<broker_candidate_result_update> update)
{
    if (!controller_window || !update)
        return false;
    broker_candidate_result_update* raw_update = update.release();
    if (!PostMessageW(controller_window,
                      WM_BROKER_CANDIDATE_RESULT,
                      0,
                      reinterpret_cast<LPARAM>(raw_update)))
    {
        delete raw_update;
        return false;
    }
    return true;
}

bool broker_ui_controller::PostStatusUpdate(
    HWND controller_window,
    std::unique_ptr<broker_status_update> update)
{
    if (!controller_window || !update)
        return false;
    broker_status_update* raw_update = update.release();
    if (!PostMessageW(controller_window,
                      WM_BROKER_STATUS_UPDATE,
                      0,
                      reinterpret_cast<LPARAM>(raw_update)))
    {
        delete raw_update;
        return false;
    }
    return true;
}

void broker_ui_controller::PostConnectionClosed(
    HWND controller_window,
    std::uint64_t connection_id)
{
    if (controller_window)
    {
        PostMessageW(controller_window,
                     WM_BROKER_CONNECTION_CLOSED,
                     static_cast<WPARAM>(connection_id),
                     static_cast<LPARAM>(connection_id >> 32));
    }
}

LRESULT CALLBACK broker_ui_controller::WindowProc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    broker_ui_controller* controller = reinterpret_cast<broker_ui_controller*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        controller = static_cast<broker_ui_controller*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd,
                          GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(controller));
    }
    else if (message == WM_BROKER_CANDIDATE_UPDATE)
    {
        std::unique_ptr<broker_candidate_update> update(
            reinterpret_cast<broker_candidate_update*>(lparam));
        if (controller)
            controller->ApplyCandidateUpdate(std::move(update));
        return 0;
    }
    else if (message == WM_BROKER_CONNECTION_CLOSED)
    {
        const std::uint64_t connection_id =
            static_cast<std::uint32_t>(wparam) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(lparam)) << 32);
        if (controller)
            controller->OnConnectionClosed(connection_id);
        return 0;
    }
    else if (message == WM_BROKER_CANDIDATE_RESULT)
    {
        std::unique_ptr<broker_candidate_result_update> update(
            reinterpret_cast<broker_candidate_result_update*>(lparam));
        if (controller)
            controller->ApplyCandidateResult(std::move(update));
        return 0;
    }
    else if (message == WM_BROKER_STATUS_UPDATE)
    {
        std::unique_ptr<broker_status_update> update(
            reinterpret_cast<broker_status_update*>(lparam));
        if (controller)
            controller->ApplyStatusUpdate(std::move(update));
        return 0;
    }
    else if (message == WM_NCDESTROY)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void broker_ui_controller::ApplyCandidateUpdate(
    std::unique_ptr<broker_candidate_update> update)
{
    if (!update)
        return;

    if (!update->anchor_valid)
    {
        update->anchor_valid = try_get_window_thread_caret(
            update->view_hwnd, update->owner_hwnd, &update->anchor);
        if (!update->anchor_valid)
        {
            update->anchor_valid = try_get_visible_window_anchor(
                update->view_hwnd, update->owner_hwnd, &update->anchor);
        }
    }

    if (update->presentation_pending)
    {
        // Keep the previous stable frame until the TIP confirms that both
        // host layout and the matching candidate snapshot are ready.
        if (active_connection_id_ != 0 &&
            active_connection_id_ != update->connection_id)
        {
            candidate_window_.show(false);
        }
        return;
    }

    const bool should_show = update->visible &&
        update->custom_ui_allowed &&
        update->anchor_valid &&
        update->owner_hwnd &&
        IsWindow(update->owner_hwnd);
    broker_debug_log(L"candidate update connection=%llu generation=%llu visible=%d allowed=%d pending=%d anchor=%d owner=0x%p owner_valid=%d count=%zu",
           static_cast<unsigned long long>(update->connection_id),
           static_cast<unsigned long long>(update->generation),
           update->visible ? 1 : 0,
           update->custom_ui_allowed ? 1 : 0,
           update->presentation_pending ? 1 : 0,
           update->anchor_valid ? 1 : 0,
           update->owner_hwnd,
           update->owner_hwnd && IsWindow(update->owner_hwnd) ? 1 : 0,
           update->candidates.size());
    if (!should_show)
    {
        if (active_connection_id_ == update->connection_id)
            candidate_window_.show(false);
        return;
    }

    if (active_connection_id_ != update->connection_id ||
        active_composition_ != update->composition)
    {
        active_query_generation_ = 0;
    }
    active_connection_id_ = update->connection_id;
    active_generation_ = update->generation;
    active_composition_ = update->composition;
    active_anchor_ = update->anchor;
    active_anchor_valid_ = update->anchor_valid;
    active_sender_ = std::move(update->send_action);
    EnsureCandidateWindow(update->owner_hwnd);
    if (!candidate_window_.get_hwnd())
        return;

    candidate_window_.set_ui_font_percent(static_cast<int>(
        std::clamp<std::uint32_t>(global_config_.ui_font_percent + 20, 80, 250)));
    candidate_window_.set_composition_text(update->composition);
    const auto cached = cached_candidate_results_.find(update->connection_id);
    const bool has_cached_result =
        cached != cached_candidate_results_.end() &&
        cached->second.composition == update->composition &&
        cached->second.query_generation >= active_query_generation_ &&
        (cached->second.result_flags &
         zime::broker_protocol::candidate_result_auto_commit_first) == 0;
    if (has_cached_result)
    {
        if (active_query_generation_ < cached->second.query_generation)
        {
            active_query_generation_ = cached->second.query_generation;
            candidate_window_.set_candidates(cached->second.candidates);
        }
    }
    else
    {
        candidate_window_.set_candidates(update->candidates);
    }
    while (candidate_window_.get_current_page() <
           static_cast<int>(update->current_page))
    {
        candidate_window_.page_down();
    }
    const int page_size = (std::max)(1, candidate_window_.get_page_size());
    candidate_window_.set_selection(
        static_cast<int>(update->selection_absolute % page_size));
    PositionCandidateWindow(update->anchor);
    candidate_window_.show(true);
    SetWindowPos(candidate_window_.get_hwnd(),
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
#ifndef NDEBUG
    RECT candidate_rect = {};
    GetWindowRect(candidate_window_.get_hwnd(), &candidate_rect);
    broker_debug_log(L"candidate shown hwnd=0x%p visible=%d owner_visible=%d style=0x%llx exstyle=0x%llx rect=(%ld,%ld,%ld,%ld)",
           candidate_window_.get_hwnd(),
           IsWindowVisible(candidate_window_.get_hwnd()) ? 1 : 0,
           IsWindowVisible(update->owner_hwnd) ? 1 : 0,
           static_cast<unsigned long long>(GetWindowLongPtrW(
               candidate_window_.get_hwnd(), GWL_STYLE)),
           static_cast<unsigned long long>(GetWindowLongPtrW(
               candidate_window_.get_hwnd(), GWL_EXSTYLE)),
           candidate_rect.left,
           candidate_rect.top,
           candidate_rect.right,
           candidate_rect.bottom);
#endif
}

void broker_ui_controller::ApplyCandidateResult(
    std::unique_ptr<broker_candidate_result_update> update)
{
    using namespace zime::broker_protocol;
    if (!update)
    {
        return;
    }

    if (update->result_flags & candidate_result_auto_commit_first)
    {
        const auto cached = cached_candidate_results_.find(
            update->connection_id);
        if (cached != cached_candidate_results_.end() &&
            cached->second.composition == update->composition &&
            cached->second.query_generation <= update->query_generation)
        {
            cached_candidate_results_.erase(cached);
        }
        return;
    }
    const auto previous = cached_candidate_results_.find(
        update->connection_id);
    if (previous != cached_candidate_results_.end() &&
        previous->second.composition == update->composition &&
        update->query_generation < previous->second.query_generation)
    {
        return;
    }

    const std::uint64_t connection_id = update->connection_id;
    auto cached = cached_candidate_results_.insert_or_assign(
        connection_id, std::move(*update)).first;
    const broker_candidate_result_update& result = cached->second;

    if (result.connection_id != active_connection_id_ ||
        result.composition != active_composition_ ||
        result.query_generation < active_query_generation_ ||
        !candidate_window_.get_hwnd() ||
        !IsWindowVisible(candidate_window_.get_hwnd()))
    {
        return;
    }

    active_query_generation_ = result.query_generation;
    candidate_window_.set_candidates(result.candidates);
    candidate_window_.set_selection(0);
    if (active_anchor_valid_)
        PositionCandidateWindow(active_anchor_);
    SetWindowPos(candidate_window_.get_hwnd(),
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void broker_ui_controller::OnConnectionClosed(std::uint64_t connection_id)
{
    if (active_connection_id_ == connection_id)
    {
        candidate_window_.show(false);
        active_connection_id_ = 0;
        active_generation_ = 0;
        active_query_generation_ = 0;
        active_composition_.clear();
        active_anchor_valid_ = false;
        active_sender_ = nullptr;
    }
    cached_candidate_results_.erase(connection_id);
    if (active_status_connection_id_ == connection_id)
    {
        status_window_.show(false);
        active_status_connection_id_ = 0;
        active_status_generation_ = 0;
        active_status_sender_ = nullptr;
    }
}

void broker_ui_controller::ApplyStatusUpdate(
    std::unique_ptr<broker_status_update> update)
{
    using namespace zime::broker_protocol;
    if (!update)
        return;
    const bool should_show =
        (update->flags & status_visible) != 0 &&
        (update->flags & status_custom_ui_allowed) != 0 &&
        update->owner_hwnd &&
        IsWindow(update->owner_hwnd);
    broker_debug_log(L"status update connection=%llu generation=%llu visible=%d allowed=%d owner=0x%p owner_valid=%d show=%d",
           static_cast<unsigned long long>(update->connection_id),
           static_cast<unsigned long long>(update->generation),
           (update->flags & status_visible) != 0 ? 1 : 0,
           (update->flags & status_custom_ui_allowed) != 0 ? 1 : 0,
           update->owner_hwnd,
           update->owner_hwnd && IsWindow(update->owner_hwnd) ? 1 : 0,
           should_show ? 1 : 0);
    if (!should_show)
    {
        if (active_status_connection_id_ == update->connection_id)
            status_window_.show(false);
        return;
    }

    active_status_connection_id_ = update->connection_id;
    active_status_generation_ = update->generation;
    active_status_sender_ = std::move(update->send_action);
    EnsureStatusWindow(update->owner_hwnd);
    if (!status_window_.get_hwnd())
    {
        broker_error_log(L"status create failed owner=0x%p error=%lu",
                         update->owner_hwnd,
                         GetLastError());
        return;
    }

    status_window_.set_chinese_mode((update->flags & status_chinese_mode) != 0);
    status_window_.set_full_width((update->flags & status_full_width) != 0);
    status_window_.set_chinese_punctuation(
        (update->flags & status_chinese_punctuation) != 0);
    ApplyGlobalConfigToWindows();

    if ((global_config_.flags & setting_status_position_customized) != 0)
    {
        status_window_.move(global_config_.status_position_x,
                            global_config_.status_position_y);
    }
    else
    {
        const HMONITOR monitor = MonitorFromWindow(
            update->owner_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        RECT work_area = {};
        if (monitor && GetMonitorInfoW(monitor, &info))
            work_area = info.rcWork;
        else
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
        int width = 0;
        int height = 0;
        status_window_.get_window_size(width, height);
        status_window_.move(work_area.right - width - 10,
                            work_area.bottom - height - 10);
    }
    status_window_.show(true);
#ifndef NDEBUG
    RECT status_rect = {};
    GetWindowRect(status_window_.get_hwnd(), &status_rect);
    broker_debug_log(L"status shown hwnd=0x%p visible=%d owner=0x%p actual_owner=0x%p rect=(%ld,%ld,%ld,%ld)",
           status_window_.get_hwnd(),
           IsWindowVisible(status_window_.get_hwnd()) ? 1 : 0,
           update->owner_hwnd,
           GetWindow(status_window_.get_hwnd(), GW_OWNER),
           status_rect.left,
           status_rect.top,
           status_rect.right,
           status_rect.bottom);
#endif
}

void broker_ui_controller::EnsureCandidateWindow(HWND owner)
{
    HWND candidate = candidate_window_.get_hwnd();
    if (!candidate)
    {
        // A shell/AppContainer HWND cannot always become the owner of an
        // out-of-process popup. Reuse one ownerless topmost window and govern
        // its lifetime through the validated Broker connection instead.
        if (!candidate_window_.create(nullptr))
        {
            broker_error_log(L"candidate create failed owner=0x%p error=%lu", owner, GetLastError());
            return;
        }
        broker_debug_log(L"candidate created hwnd=0x%p owner=0x%p actual_owner=0x%p",
               candidate_window_.get_hwnd(),
               owner,
               GetWindow(candidate_window_.get_hwnd(), GW_OWNER));
        candidate_window_.set_click_callback(
            [this](int candidate_index)
            {
                POINT point = {};
                SendCandidateAction(
                    zime::broker_protocol::ui_action_type::candidate_select,
                    static_cast<std::uint32_t>(candidate_index),
                    point);
            });
        candidate_window_.set_page_change_callback(
            [this](int page)
            {
                SendCandidateAction(
                    zime::broker_protocol::ui_action_type::candidate_page,
                    static_cast<std::uint32_t>((std::max)(0, page)));
            });
        candidate_window_.set_context_menu_callback(
            [this](int candidate_index, POINT point)
            {
                ShowCandidateContextMenu(candidate_index, point);
            });
    }
}

void broker_ui_controller::PositionCandidateWindow(const RECT& anchor)
{
    int width = 0;
    int height = 0;
    candidate_window_.get_window_size(width, height);
    const HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return;

    const RECT& work_area = info.rcWork;
    constexpr int gap = 2;
    int x = anchor.left;
    if (width > work_area.right - work_area.left)
        x = work_area.left;
    else
        x = std::clamp(x,
                       static_cast<int>(work_area.left),
                       static_cast<int>(work_area.right) - width);

    const int below = anchor.bottom + gap;
    const int above = anchor.top - gap - height;
    int y = below;
    if (below + height > work_area.bottom && above >= work_area.top)
        y = above;
    if (height > work_area.bottom - work_area.top)
        y = work_area.top;
    else
        y = std::clamp(y,
                       static_cast<int>(work_area.top),
                       static_cast<int>(work_area.bottom) - height);
    candidate_window_.move(x, y);
}

void broker_ui_controller::SendCandidateAction(
    zime::broker_protocol::ui_action_type type,
    std::uint32_t value,
    POINT screen_point)
{
    if (!active_sender_ || active_connection_id_ == 0 || active_generation_ == 0)
        return;
    zime::broker_protocol::ui_action action = {};
    action.header = zime::broker_protocol::make_header(
        zime::broker_protocol::message_type::ui_action,
        sizeof(action),
        GetTickCount64(),
        active_connection_id_);
    action.generation = active_generation_;
    action.action = type;
    action.value = value;
    action.screen_x = screen_point.x;
    action.screen_y = screen_point.y;
    active_sender_(action);
}

void broker_ui_controller::ShowCandidateContextMenu(
    int candidate_index,
    POINT screen_point)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, ID_CANDIDATE_DELETE, L"\u5220\u9664");
    AppendMenuW(menu,
                MF_STRING,
                ID_CANDIDATE_MARK_UNCOMMON,
                L"\u6807\u8bb0\u4e0d\u5e38\u7528");
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD |
            TPM_NONOTIFY |
            TPM_LEFTALIGN |
            TPM_TOPALIGN |
            TPM_RIGHTBUTTON,
        screen_point.x,
        screen_point.y,
        0,
        candidate_window_.get_hwnd(),
        nullptr);
    DestroyMenu(menu);

    if (command == ID_CANDIDATE_DELETE)
    {
        SendCandidateAction(
            zime::broker_protocol::ui_action_type::candidate_delete,
            static_cast<std::uint32_t>(candidate_index),
            screen_point);
    }
    else if (command == ID_CANDIDATE_MARK_UNCOMMON)
    {
        SendCandidateAction(
            zime::broker_protocol::ui_action_type::candidate_mark_uncommon,
            static_cast<std::uint32_t>(candidate_index),
            screen_point);
    }
}

void broker_ui_controller::EnsureStatusWindow(HWND owner)
{
    HWND status = status_window_.get_hwnd();
    if (status && GetWindow(status, GW_OWNER) != owner)
    {
        status_window_.destroy();
        status = nullptr;
    }
    if (status)
        return;
    if (!status_window_.create(owner))
        return;

    status_window_.set_status_change_callback(
        [this](int status_type)
        {
            SendStatusChange(status_type);
        });
    status_window_.set_menu_popup_state_callback(
        [this](bool showing)
        {
            SendStatusAction(
                zime::broker_protocol::ui_action_type::status_menu_popup,
                showing ? 1u : 0u);
        });
    status_window_.set_position_changed_callback(
        [this](int x, int y)
        {
            CommitStatusPosition(x, y);
        });
}

void broker_ui_controller::SendStatusChange(int status_type)
{
    if (status_type != status_window::STATUS_FULL_WIDTH &&
        status_type != status_window::STATUS_CHINESE_MODE &&
        status_type != status_window::STATUS_PUNCTUATION)
    {
        CommitGlobalStatusChange(status_type);
        return;
    }

    int state_value = 0;
    switch (status_type)
    {
    case status_window::STATUS_FULL_WIDTH:
        state_value = status_window_.is_full_width() ? 1 : 0;
        break;
    case status_window::STATUS_CHINESE_MODE:
        state_value = status_window_.is_chinese_mode() ? 1 : 0;
        break;
    case status_window::STATUS_PUNCTUATION:
        state_value = status_window_.is_chinese_punctuation() ? 1 : 0;
        break;
    case status_window::STATUS_AUTO_COMMIT_FOUR_UNIQUE:
        state_value = status_window_.is_auto_commit_four_code_unique() ? 1 : 0;
        break;
    case status_window::STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE:
        state_value = status_window_.is_commit_first_candidate_on_fifth_code() ? 1 : 0;
        break;
    case status_window::STATUS_SHOW_UNCOMMON_CANDIDATES:
        state_value = status_window_.is_show_uncommon_candidates() ? 1 : 0;
        break;
    case status_window::STATUS_REPLACE_DOT_AFTER_DIGIT:
        state_value = status_window_.is_replace_dot_after_digit() ? 1 : 0;
        break;
    case status_window::STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE:
        state_value = status_window_.is_use_english_punctuation_in_chinese_mode() ? 1 : 0;
        break;
    case status_window::STATUS_DISABLE_CHINESE_DASH:
        state_value = status_window_.is_disable_chinese_dash() ? 1 : 0;
        break;
    case status_window::STATUS_UI_FONT_CHANGED:
        state_value = status_window_.get_ui_font_percent();
        break;
    case status_window::STATUS_CANDIDATE_SORT_MODE_CHANGED:
        state_value = static_cast<int>(status_window_.get_candidate_sort_mode());
        break;
    default:
        break;
    }
    SendStatusAction(
        zime::broker_protocol::ui_action_type::status_change,
        static_cast<std::uint32_t>(status_type),
        POINT{state_value, 0});
}

void broker_ui_controller::CommitGlobalStatusChange(int status_type)
{
    using namespace zime::broker_protocol;
    auto set_flag = [this](std::uint32_t flag, bool enabled)
    {
        if (enabled)
            global_config_.flags |= flag;
        else
            global_config_.flags &= ~flag;
    };
    switch (status_type)
    {
    case status_window::STATUS_SETTINGS:
        ShowCreateWordWindow();
        return;
    case status_window::STATUS_EXPORT_RAW_DICT:
        ExportRawDictionary();
        return;
    case status_window::STATUS_AUTO_COMMIT_FOUR_UNIQUE:
        set_flag(setting_auto_commit_four_unique,
                 status_window_.is_auto_commit_four_code_unique());
        break;
    case status_window::STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE:
        set_flag(setting_commit_first_on_fifth,
                 status_window_.is_commit_first_candidate_on_fifth_code());
        break;
    case status_window::STATUS_SHOW_UNCOMMON_CANDIDATES:
        set_flag(setting_show_uncommon,
                 status_window_.is_show_uncommon_candidates());
        break;
    case status_window::STATUS_REPLACE_DOT_AFTER_DIGIT:
        set_flag(setting_replace_dot_after_digit,
                 status_window_.is_replace_dot_after_digit());
        break;
    case status_window::STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE:
        set_flag(setting_use_english_punctuation,
                 status_window_.is_use_english_punctuation_in_chinese_mode());
        break;
    case status_window::STATUS_DISABLE_CHINESE_DASH:
        set_flag(setting_disable_chinese_dash,
                 status_window_.is_disable_chinese_dash());
        break;
    case status_window::STATUS_UI_FONT_CHANGED:
        global_config_.ui_font_percent = static_cast<std::uint32_t>(
            std::clamp(status_window_.get_ui_font_percent(), 80, 250));
        break;
    case status_window::STATUS_CANDIDATE_SORT_MODE_CHANGED:
        global_config_.candidate_sort_mode = static_cast<std::uint32_t>(
            status_window_.get_candidate_sort_mode());
        break;
    default:
        return;
    }
    SaveGlobalConfig();
}

void broker_ui_controller::CommitStatusPosition(int x, int y)
{
    global_config_.flags |=
        zime::broker_protocol::setting_status_position_customized;
    global_config_.status_position_x = x;
    global_config_.status_position_y = y;
    SaveGlobalConfig();
}

bool broker_ui_controller::SaveGlobalConfig()
{
    if (!storage_)
        return false;
    zime::broker_protocol::config_state committed = {};
    std::wstring error;
    if (!storage_->SaveConfig(global_config_, &committed, &error))
    {
        MessageBoxW(status_window_.get_hwnd(),
                    error.empty() ? L"保存设置失败" : error.c_str(),
                    L"ZIme",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    global_config_ = committed;
    ApplyGlobalConfigToWindows();
    if (config_committed_)
        config_committed_(global_config_);
    return true;
}

void broker_ui_controller::ApplyGlobalConfigToWindows()
{
    using namespace zime::broker_protocol;
    status_window_.set_auto_commit_four_code_unique(
        (global_config_.flags & setting_auto_commit_four_unique) != 0);
    status_window_.set_commit_first_candidate_on_fifth_code(
        (global_config_.flags & setting_commit_first_on_fifth) != 0);
    status_window_.set_show_uncommon_candidates(
        (global_config_.flags & setting_show_uncommon) != 0);
    status_window_.set_replace_dot_after_digit(
        (global_config_.flags & setting_replace_dot_after_digit) != 0);
    status_window_.set_use_english_punctuation_in_chinese_mode(
        (global_config_.flags & setting_use_english_punctuation) != 0);
    status_window_.set_disable_chinese_dash(
        (global_config_.flags & setting_disable_chinese_dash) != 0);
    status_window_.set_candidate_sort_mode(
        static_cast<status_window::CandidateSortMode>(
            (std::min)(global_config_.candidate_sort_mode, 2u)));
    status_window_.set_ui_font_percent(static_cast<int>(
        std::clamp<std::uint32_t>(global_config_.ui_font_percent, 80, 250)));
    candidate_window_.set_ui_font_percent(static_cast<int>(
        std::clamp<std::uint32_t>(global_config_.ui_font_percent + 20, 80, 250)));
}

void broker_ui_controller::ShowCreateWordWindow()
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = create_word_window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kCreateWordWindowClass;
    if (!RegisterClassExW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return;
    }
    create_word_context context = {
        storage_,
        [this]()
        {
            if (storage_ && dictionary_committed_)
                dictionary_committed_(storage_->DictionaryRevision());
        }};
    HWND owner = status_window_.get_hwnd();
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kCreateWordWindowClass,
        L"\u9020\u8bcd",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 390, 205,
        owner, nullptr, instance_, &context);
    if (!window)
        return;
    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    MSG message = {};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner && IsWindow(owner))
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}

void broker_ui_controller::ExportRawDictionary()
{
    if (!storage_)
        return;
    wchar_t path[MAX_PATH] = L"zime_dictionary.dic";
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = status_window_.get_hwnd();
    dialog.lpstrFilter = L"Dictionary Files (*.dic)\0*.dic\0All Files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"dic";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog))
        return;
    std::wstring error;
    if (!storage_->ExportRawDictionary(path, &error))
    {
        MessageBoxW(dialog.hwndOwner,
                    error.empty() ? L"\u5bfc\u51fa\u8bcd\u5e93\u5931\u8d25\u3002" : error.c_str(),
                    L"ZIme", MB_OK | MB_ICONERROR);
    }
}

void broker_ui_controller::SendStatusAction(
    zime::broker_protocol::ui_action_type type,
    std::uint32_t value,
    POINT point)
{
    if (!active_status_sender_ ||
        active_status_connection_id_ == 0 ||
        active_status_generation_ == 0)
    {
        return;
    }
    zime::broker_protocol::ui_action action = {};
    action.header = zime::broker_protocol::make_header(
        zime::broker_protocol::message_type::ui_action,
        sizeof(action),
        GetTickCount64(),
        active_status_connection_id_);
    action.generation = active_status_generation_;
    action.action = type;
    action.value = value;
    action.screen_x = point.x;
    action.screen_y = point.y;
    active_status_sender_(action);
}
