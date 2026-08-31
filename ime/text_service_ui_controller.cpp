#include "text_service.h"

#include "ime_trace.h"
#include "perf_trace.h"

#include <Windows.h>
#include <shellapi.h>
#include <map>
#include <algorithm>
#include <commdlg.h>
#include <filesystem>
#include <string>
#include <cwctype>

#include "tool.h"

namespace
{
constexpr int IDC_NEW_WORD_EDIT = 2001;
constexpr int IDC_NEW_CODE_EDIT = 2002;
constexpr int IDC_CREATE_OK = 2003;
constexpr int IDC_CREATE_CANCEL = 2004;
constexpr int IDC_NEW_WORD_LABEL = 2005;
constexpr int IDC_NEW_CODE_LABEL = 2006;
constexpr int IDC_NEW_CODE_HINT = 2007;
constexpr int IDC_CAND_MENU_DELETE = 2101;
constexpr int IDC_CAND_MENU_MARK_UNCOMMON = 2102;
constexpr auto CREATE_WORD_WINDOW_CLASS = L"ZImeCreateWordWindow";
constexpr auto CREATE_WORD_FONT_PROP = L"zime_create_word_font";
constexpr auto CREATE_WORD_BG_BRUSH_PROP = L"zime_create_word_bg_brush";
constexpr auto CREATE_WORD_EDIT_BRUSH_PROP = L"zime_create_word_edit_brush";
constexpr UINT WM_CREATE_WORD_APPLY_LAYOUT = WM_APP + 101;
constexpr int CANDIDATE_FONT_BOOST_PERCENT = 10;

bool prefer_in_process_shell_candidate_window()
{
    static const bool prefer_local = []
    {
        wchar_t path[32768] = {};
        const DWORD length = GetModuleFileNameW(
            nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        return _wcsicmp(name, L"SearchHost.exe") == 0 ||
            _wcsicmp(name, L"SearchApp.exe") == 0;
    }();
    return prefer_local;
}

UINT resolve_window_dpi(HWND hwnd)
{
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pGetDpiForWindow && hwnd)
        return pGetDpiForWindow(hwnd);
    return 96;
}

int scale_ui_px(int base_px, UINT dpi, int font_percent)
{
    return MulDiv(base_px * font_percent, static_cast<int>(dpi), 96 * 100);
}

bool is_shift_vk(WPARAM vk)
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

bool is_likely_fullscreen_foreground_window()
{
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    if (FAILED(SHQueryUserNotificationState(&state)))
        return false;

    // Geometry/style tests misclassify maximized custom-titlebar apps such as
    // VS Code and modern Notepad.  These two states are the system's explicit
    // signal that nonessential UI should stay out of a real fullscreen session.
    return state == QUNS_RUNNING_D3D_FULL_SCREEN ||
        state == QUNS_PRESENTATION_MODE;
}

HWND get_foreground_ui_owner_window()
{
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindow(hwnd))
        return nullptr;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && IsWindow(root) && !IsIconic(root))
        return root;
    return (!IsIconic(hwnd)) ? hwnd : nullptr;
}

DWORD window_process_id(HWND hwnd)
{
    DWORD pid = 0;
    if (hwnd)
        GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

HWND trace_context_view_relationship(ITfContextView* view)
{
    HWND view_hwnd = nullptr;
#ifndef NDEBUG
    const HRESULT get_wnd_hr = view ? view->GetWnd(&view_hwnd) : E_INVALIDARG;
    const HWND root_hwnd = view_hwnd ? GetAncestor(view_hwnd, GA_ROOT) : nullptr;
    const HWND foreground_hwnd = GetForegroundWindow();
    ime_tracef(L"ContextView",
               L"getwnd_hr=0x%08lx view=0x%p view_pid=%lu root=0x%p root_pid=%lu fg=0x%p fg_pid=%lu",
               static_cast<unsigned long>(get_wnd_hr),
               view_hwnd,
               window_process_id(view_hwnd),
               root_hwnd,
               window_process_id(root_hwnd),
               foreground_hwnd,
               window_process_id(foreground_hwnd));
#else
    if (view)
        view->GetWnd(&view_hwnd);
#endif
    return view_hwnd;
}

bool try_get_caret_fallback_rect(RECT* rc_out)
{
    if (!rc_out)
        return false;

    const auto is_reasonable_caret_rect = [](const RECT& rc) {
        const bool has_vertical_span = rc.bottom > rc.top;
        const bool looks_uninitialized_rect =
            (rc.left == 0 && rc.top == 0 && rc.right == 0 && rc.bottom == 0);
        const bool looks_origin_fallback_rect =
            (rc.left <= 1 && rc.top <= 1 && rc.right <= 4 && rc.bottom <= 40);
        return has_vertical_span &&
               !looks_uninitialized_rect &&
               !looks_origin_fallback_rect;
    };

    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret)
    {
        RECT rc = gti.rcCaret;
        POINT lt = { rc.left, rc.top };
        POINT rb = { rc.right, rc.bottom };
        if (ClientToScreen(gti.hwndCaret, &lt) && ClientToScreen(gti.hwndCaret, &rb))
        {
            rc.left = lt.x;
            rc.top = lt.y;
            rc.right = rb.x;
            rc.bottom = rb.y;
            if (rc.right <= rc.left)
                rc.right = rc.left + 2;
            if (rc.bottom <= rc.top)
                rc.bottom = rc.top + 20;
            if (is_reasonable_caret_rect(rc))
            {
                *rc_out = rc;
                return true;
            }
        }
    }

    return false;
}

bool try_build_client_anchor_rect(HWND hwnd, RECT* rc_out)
{
    if (!hwnd || !rc_out || !IsWindow(hwnd))
        return false;

    RECT rc_client = {};
    if (!GetClientRect(hwnd, &rc_client))
        return false;

    POINT anchor = { rc_client.left + 4, rc_client.top + 4 };
    if (!ClientToScreen(hwnd, &anchor))
        return false;

    rc_out->left = anchor.x;
    rc_out->top = anchor.y;
    rc_out->right = anchor.x + 2;
    rc_out->bottom = anchor.y + 20;
    return true;
}

bool try_get_focus_window_fallback_rect(RECT* rc_out)
{
    if (!rc_out)
        return false;

    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(0, &gti) && gti.hwndFocus)
        return try_build_client_anchor_rect(gti.hwndFocus, rc_out);

    return false;
}

bool try_get_view_fallback_rect(ITfContextView* pView, RECT* rc_out)
{
    if (!pView || !rc_out)
        return false;

    RECT rc_view = {};
    if (FAILED(pView->GetScreenExt(&rc_view)))
        return false;

    if (rc_view.right <= rc_view.left || rc_view.bottom <= rc_view.top)
        return false;

    const int anchor_x = rc_view.left + 4;
    const int anchor_y = rc_view.top + 4;
    rc_out->left = anchor_x;
    rc_out->top = anchor_y;
    rc_out->right = anchor_x + 2;
    rc_out->bottom = min(rc_view.bottom, anchor_y + 20);
    if (rc_out->bottom <= rc_out->top)
        rc_out->bottom = rc_out->top + 20;
    return true;
}

bool try_get_owner_window_fallback_rect(RECT* rc_out)
{
    return try_build_client_anchor_rect(get_foreground_ui_owner_window(), rc_out);
}

bool is_valid_text_ext_rect(const RECT& rc, BOOL fClipped)
{
    const bool has_vertical_span = rc.bottom > rc.top;
    const bool looks_uninitialized_rect =
        (rc.left == 0 && rc.top == 0 && rc.right == 0 && rc.bottom == 0);
    const bool looks_origin_fallback_rect =
        (rc.left <= 1 && rc.top <= 1 && rc.right <= 4 && rc.bottom <= 40);
    return !fClipped &&
           has_vertical_span &&
           !looks_uninitialized_rect &&
           !looks_origin_fallback_rect;
}

bool is_window_in_foreground_tree(HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;
    HWND foreground_root = GetAncestor(foreground, GA_ROOT);
    HWND window_root = GetAncestor(window, GA_ROOT);
    if (foreground_root && window_root && foreground_root == window_root)
        return true;
    return IsChild(foreground, window) || IsChild(window, foreground);
}

RECT resolve_primary_work_area()
{
    RECT work_area = {};
    const POINT primary_anchor = { 0, 0 };
    const HMONITOR hMon = MonitorFromPoint(primary_anchor, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (hMon && GetMonitorInfoW(hMon, &mi))
        return mi.rcWork;

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    return work_area;
}

POINT center_point_in_work_area(const RECT& work_area, int width, int height)
{
    const int work_width = max(0, work_area.right - work_area.left);
    const int work_height = max(0, work_area.bottom - work_area.top);

    POINT pt = {};
    pt.x = work_area.left + max(0, (work_width - width) / 2);
    pt.y = work_area.top + max(0, (work_height - height) / 2);
    return pt;
}

void reset_shift_track(bool& shift_pressed, bool& other_key_pressed)
{
    shift_pressed = FALSE;
    other_key_pressed = FALSE;
}

ime_dict::candidate_sort_mode candidate_sort_mode_from_int(int value)
{
    switch (value)
    {
    case static_cast<int>(ime_dict::candidate_sort_mode::fixed_order):
        return ime_dict::candidate_sort_mode::fixed_order;
    case static_cast<int>(ime_dict::candidate_sort_mode::recent):
        return ime_dict::candidate_sort_mode::recent;
    case static_cast<int>(ime_dict::candidate_sort_mode::frequency):
    default:
        return ime_dict::candidate_sort_mode::frequency;
    }
}

status_window::CandidateSortMode to_status_sort_mode(ime_dict::candidate_sort_mode mode)
{
    switch (mode)
    {
    case ime_dict::candidate_sort_mode::fixed_order:
        return status_window::CandidateSortMode::FixedOrder;
    case ime_dict::candidate_sort_mode::recent:
        return status_window::CandidateSortMode::Recent;
    case ime_dict::candidate_sort_mode::frequency:
    default:
        return status_window::CandidateSortMode::Frequency;
    }
}

ime_dict::candidate_sort_mode from_status_sort_mode(status_window::CandidateSortMode mode)
{
    switch (mode)
    {
    case status_window::CandidateSortMode::FixedOrder:
        return ime_dict::candidate_sort_mode::fixed_order;
    case status_window::CandidateSortMode::Recent:
        return ime_dict::candidate_sort_mode::recent;
    case status_window::CandidateSortMode::Frequency:
    default:
        return ime_dict::candidate_sort_mode::frequency;
    }
}

void ApplyCreateWordLayout(HWND hWnd, int font_percent)
{
    if (!hWnd)
        return;

    const UINT dpi = resolve_window_dpi(hWnd);
    auto sx = [&](int value) { return scale_ui_px(value, dpi, font_percent); };

    HFONT old_font = reinterpret_cast<HFONT>(GetPropW(hWnd, CREATE_WORD_FONT_PROP));
    if (old_font)
    {
        DeleteObject(old_font);
        RemovePropW(hWnd, CREATE_WORD_FONT_PROP);
    }

    HFONT hFont = CreateFontW(
        sx(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SetPropW(hWnd, CREATE_WORD_FONT_PROP, hFont);

    if (hFont)
    {
        const WPARAM wp = reinterpret_cast<WPARAM>(hFont);
        EnumChildWindows(hWnd,
            [](HWND child, LPARAM lp)->BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
                return TRUE;
            },
            static_cast<LPARAM>(wp));
    }

    MoveWindow(GetDlgItem(hWnd, IDC_NEW_WORD_LABEL), sx(18), sx(20), sx(56), sx(24), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_NEW_WORD_EDIT), sx(78), sx(16), sx(266), sx(32), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_NEW_CODE_LABEL), sx(18), sx(64), sx(56), sx(24), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_NEW_CODE_EDIT), sx(78), sx(60), sx(266), sx(32), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_NEW_CODE_HINT), sx(78), sx(98), sx(200), sx(20), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_CREATE_OK), sx(170), sx(130), sx(86), sx(34), TRUE);
    MoveWindow(GetDlgItem(hWnd, IDC_CREATE_CANCEL), sx(264), sx(130), sx(86), sx(34), TRUE);

    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, sx(372), sx(206), SWP_NOMOVE | SWP_NOACTIVATE);
}

std::wstring trim_ws(const std::wstring& s)
{
    size_t begin = 0;
    while (begin < s.size() && iswspace(s[begin]))
        ++begin;
    size_t end = s.size();
    while (end > begin && iswspace(s[end - 1]))
        --end;
    return s.substr(begin, end - begin);
}

}

void text_service::ShowCandidates(ITfContext* pContext)
{
    ZIME_PERF_SCOPE("tip.ShowCandidates",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    m_candidateWindow.get_candidate_count());
    const HWND hwnd_before_show = m_candidateWindow.get_hwnd();
    const bool was_visible = hwnd_before_show && IsWindowVisible(hwnd_before_show);

    UpdateCandidateUIElement(pContext);
    const bool own_candidate_ui = ShouldShowOwnCandidateWindow();
    const bool broker_connected = m_brokerClient.IsConnected();
    const bool broker_candidate_ui = ShouldShowBrokerCandidateWindow();
    if (broker_candidate_ui && broker_connected)
    {
        // The Broker keeps this generation pending until OnLayoutChange has
        // produced a fresh anchor and the matching result reached this thread.
        ApplyCandidateWindowVisibility();
    }
    else
    {
        if (own_candidate_ui)
        {
            EnsureCandidateWindow();
            UpdateCandidateWindowPosition(pContext);
        }
        ApplyCandidateWindowVisibility();
    }

    const HWND hwnd_after_show = m_candidateWindow.get_hwnd();
    const bool has_content =
        m_bInComposition &&
        (!m_compositionText.empty() || m_candidateWindow.get_candidate_count() > 0);
    if (!was_visible &&
        has_content &&
        hwnd_after_show &&
        IsWindowVisible(hwnd_after_show))
    {
        UpdateCandidateWindowPosition(pContext);
    }
}

void text_service::HideCandidates()
{
    EndCandidateUIElement();
    m_candidateWindow.show(FALSE);
    SyncBrokerCandidateState();
}

void text_service::OnHostCandidateUiShowChanged(BOOL bShow)
{
    m_hostWantsCandidateWindow = (bShow != FALSE);
    ApplyCandidateWindowVisibility();
}

bool text_service::ShouldShowOwnCandidateWindow() const
{
    return m_hostWantsCandidateWindow;
}

bool text_service::ShouldShowBrokerCandidateWindow() const
{
    // Search UI is hosted in a privileged shell window band. A normal
    // out-of-process topmost Broker HWND remains below that band, while the
    // in-process TIP window is placed correctly by the shell host.
    if (prefer_in_process_shell_candidate_window())
        return false;

    // UIElement-only hosts cannot host our HWND, but the per-user Broker can.
    // Keep honoring explicit suppression in regular hosts to avoid duplicate UI.
    return m_hostWantsCandidateWindow || m_uiElementOnlyMode;
}

bool text_service::ShouldShowStatusWindow() const
{
    return true;
}

bool text_service::ShouldShowLocalStatusWindow() const
{
    return !m_uiElementOnlyMode;
}

void text_service::ApplyCandidateWindowVisibility()
{
    ZIME_PERF_SCOPE("tip.ApplyCandidateVisibility",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    m_candidateWindow.get_candidate_count());
    const bool has_content =
        m_bInComposition &&
        (!m_compositionText.empty() || m_candidateWindow.get_candidate_count() > 0);
    const bool should_show = has_content && ShouldShowOwnCandidateWindow();
    const bool broker_owns_window =
        m_brokerClient.IsConnected() && ShouldShowBrokerCandidateWindow();
    if (broker_owns_window)
    {
        // Candidate data remains in candidate_form, but its HWND is redundant
        // once the Broker owns presentation. Keeping a hidden HWND causes GDI
        // measurement and SetWindowPos work on every result in some VM hosts.
        m_candidateWindow.destroy();
    }
    else
    {
        m_candidateWindow.show(should_show ? TRUE : FALSE);
    }
    SyncBrokerCandidateState();
}

void text_service::SyncBrokerCandidateState(const RECT* anchor_override)
{
    const int candidate_count = m_candidateWindow.get_candidate_count();

    HWND owner = m_brokerViewHwnd;
    if (owner && IsWindow(owner))
    {
        HWND root = GetAncestor(owner, GA_ROOT);
        if (root && IsWindow(root) &&
            window_process_id(root) == GetCurrentProcessId())
            owner = root;
        else if (root && window_process_id(root) != GetCurrentProcessId())
            owner = nullptr;
    }
    const bool has_content = m_bInComposition &&
        (!m_compositionText.empty() || candidate_count > 0);
    const bool custom_ui_allowed = ShouldShowBrokerCandidateWindow();
    const bool broker_owns_window =
        m_brokerClient.IsConnected() && custom_ui_allowed;
    const bool candidate_snapshot_ready =
        !m_compositionText.empty() &&
        m_brokerCandidateCode == m_compositionText;
    const bool presentation_pending = broker_owns_window && has_content &&
        (!m_brokerCandidateLayoutReady || !candidate_snapshot_ready);
    const RECT* anchor = anchor_override;
    RECT cached_anchor = {};
    if (!anchor && TryGetCachedCandidateAnchorRect(&cached_anchor))
        anchor = &cached_anchor;
    m_brokerClient.SendCandidateState(
        owner,
        anchor,
        has_content && custom_ui_allowed && !presentation_pending,
        custom_ui_allowed,
        m_compositionText,
        static_cast<std::uint32_t>(max(0, GetCandidateSelectionAbsolute())),
        static_cast<std::uint32_t>(max(1, m_candidateWindow.get_page_size())),
        static_cast<std::uint32_t>(max(0, m_candidateWindow.get_current_page())),
        static_cast<std::uint32_t>(min(250, m_uiFontPercent + CANDIDATE_FONT_BOOST_PERCENT)),
        presentation_pending);
}

void text_service::OnBrokerConnectionChanged(bool connected)
{
    ime_tracef(L"BrokerConnectionState", L"connected=%d", connected ? 1 : 0);
    // Config revisions belong to one Broker process and may restart at one
    // after a crash, upgrade, or explicit shutdown.
    m_hasBrokerConfig = false;
    m_brokerConfigRevision = 0;
    if (connected && ShouldShowBrokerCandidateWindow())
        m_candidateWindow.destroy();
    ApplyCandidateWindowVisibility();
    if (connected)
    {
        m_statusWindow.show(false);
        SyncBrokerStatusState();
        if (m_bInComposition && !m_compositionText.empty())
            RequestBrokerCandidates();
    }
    else
    {
        if (m_pendingCreateWordRequest != 0)
        {
            m_pendingCreateWordRequest = 0;
            if (m_hCreateWordWnd && IsWindow(m_hCreateWordWnd))
                EnableWindow(GetDlgItem(m_hCreateWordWnd, IDC_CREATE_OK), TRUE);
        }
        m_pendingCandidateStorageRequests.clear();
        if (m_statusWindowDesiredVisible && ShouldShowStatusWindow())
            ShowStatusWindow();
    }
}

void text_service::OnBrokerStorageResult(
    zime::broker_protocol::storage_operation operation,
    std::uint64_t request_id,
    bool success,
    const std::wstring& error)
{
    using zime::broker_protocol::storage_operation;
    if (operation == storage_operation::add_custom_word &&
        request_id == m_pendingCreateWordRequest)
    {
        m_pendingCreateWordRequest = 0;
        if (!m_hCreateWordWnd || !IsWindow(m_hCreateWordWnd))
            return;
        EnableWindow(GetDlgItem(m_hCreateWordWnd, IDC_CREATE_OK), TRUE);
        if (!success)
        {
            const std::wstring detail = error.empty() ? L"后台写入失败" : error;
            MessageBoxW(m_hCreateWordWnd,
                        (L"写入词库失败:\n" + detail).c_str(),
                        L"错误",
                        MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(m_hCreateWordWnd,
                    L"造词成功，已写入用户词库并即时生效。",
                    L"提示",
                    MB_OK | MB_ICONINFORMATION);
        DestroyWindow(m_hCreateWordWnd);
        return;
    }

    const auto pending = m_pendingCandidateStorageRequests.find(request_id);
    if (pending == m_pendingCandidateStorageRequests.end())
        return;
    const bool operation_matches = pending->second == operation;
    m_pendingCandidateStorageRequests.erase(pending);
    if (!success || !operation_matches)
    {
        const std::wstring detail = error.empty() ? L"后台写入失败" : error;
        MessageBoxW(m_brokerViewHwnd,
                    (L"更新用户词库失败:\n" + detail).c_str(),
                    L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }
    RefreshCandidatesAfterStorageMutation();
}

void text_service::OnBrokerConfigState(
    const zime::broker_protocol::config_state& state)
{
    using namespace zime::broker_protocol;
    const bool refresh_candidates =
        m_commitFirstCandidateOnFifthCode !=
            ((state.flags & setting_commit_first_on_fifth) != 0) ||
        m_showUncommonCandidates !=
            ((state.flags & setting_show_uncommon) != 0) ||
        static_cast<std::uint32_t>(m_candidateSortMode) !=
            (std::min)(state.candidate_sort_mode, 2u) ||
        m_uiFontPercent != static_cast<int>(std::clamp<std::uint32_t>(
            state.ui_font_percent, 80, 250));

    const bool punctuation_preference_changed = m_hasBrokerConfig &&
        m_useEnglishPunctuationInChineseMode !=
            ((state.flags & setting_use_english_punctuation) != 0);
    m_autoCommitFourCodeUnique =
        (state.flags & setting_auto_commit_four_unique) != 0;
    m_commitFirstCandidateOnFifthCode =
        (state.flags & setting_commit_first_on_fifth) != 0;
    m_showUncommonCandidates =
        (state.flags & setting_show_uncommon) != 0;
    m_replaceDotAfterDigit =
        (state.flags & setting_replace_dot_after_digit) != 0;
    m_useEnglishPunctuationInChineseMode =
        (state.flags & setting_use_english_punctuation) != 0;
    m_disableChineseDash =
        (state.flags & setting_disable_chinese_dash) != 0;
    m_candidateSortMode = candidate_sort_mode_from_int(
        static_cast<int>((std::min)(state.candidate_sort_mode, 2u)));
    m_uiFontPercent = static_cast<int>(std::clamp<std::uint32_t>(
        state.ui_font_percent, 80, 250));
    m_statusWindowPositionCustomized =
        (state.flags & setting_status_position_customized) != 0;
    m_statusWindowPosX = state.status_position_x;
    m_statusWindowPosY = state.status_position_y;
    if (!m_hasBrokerConfig || punctuation_preference_changed)
        SyncPunctuationModeWithLanguageMode();
    m_hasBrokerConfig = true;
    m_brokerConfigRevision = state.revision;
    UpdateStatusWindow(false);
    if (is_window_in_foreground_tree(m_brokerViewHwnd))
        SyncBrokerStatusState();
    if (m_hCreateWordWnd && IsWindow(m_hCreateWordWnd))
        PostMessageW(m_hCreateWordWnd, WM_CREATE_WORD_APPLY_LAYOUT, 0, 0);
    if (refresh_candidates)
        RefreshCandidatesAfterStorageMutation();

    ime_tracef(L"BrokerConfigState",
               L"revision=%llu flags=0x%08lx font=%lu sort=%lu",
               static_cast<unsigned long long>(state.revision),
               static_cast<unsigned long>(state.flags),
               static_cast<unsigned long>(state.ui_font_percent),
               static_cast<unsigned long>(state.candidate_sort_mode));
}

void text_service::RequestBrokerCandidates(bool apply_auto_commit)
{
    m_brokerCommitFirstOnNextCode = false;
    m_firstCandidateIsPinyin = false;
    m_brokerCandidateCode.clear();
    if (m_compositionText.empty())
        return;
    if (m_brokerClient.IsConnected())
    {
        m_brokerClient.RequestCandidates(
            m_compositionText, apply_auto_commit);
        return;
    }

    // 断线期间保留组合串，Broker 重连后会立即重新查询。
}

bool text_service::ResolveBrokerCandidatesSynchronously(bool apply_auto_commit)
{
    ZIME_PERF_SCOPE("tip.ResolveBrokerSync",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    apply_auto_commit ? 1 : 0);
    if (m_compositionText.empty() || !m_brokerClient.IsConnected())
        return false;

    const std::wstring code = m_compositionText;
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> view_texts;
    std::vector<bool> pinyin_flags;
    std::uint32_t result_flags = 0;
    std::uint64_t config_revision = 0;
    constexpr DWORD kCriticalCandidateTimeoutMs = 20;
    if (!m_brokerClient.QueryCandidatesSync(code,
                                            &candidates,
                                            &view_texts,
                                            &pinyin_flags,
                                            kCriticalCandidateTimeoutMs,
                                            apply_auto_commit,
                                            &result_flags,
                                            &config_revision))
    {
        return false;
    }

    OnBrokerCandidateResult(0,
                            code,
                            candidates,
                            view_texts,
                            pinyin_flags,
                            result_flags,
                            config_revision);
    return true;
}

void text_service::OnBrokerCandidateResult(
    std::uint64_t generation,
    const std::wstring& code,
    const std::vector<std::wstring>& candidates,
    const std::vector<std::wstring>& view_texts,
    const std::vector<bool>& pinyin_flags,
    std::uint32_t result_flags,
    std::uint64_t config_revision)
{
    ZIME_PERF_SCOPE("tip.OnBrokerCandidateResult",
                    static_cast<std::int64_t>(code.size()),
                    static_cast<std::int64_t>(candidates.size()));
    using namespace zime::broker_protocol;
    if (!m_bInComposition || code != m_compositionText)
        return;

    std::wstring pending_characters;
    pending_characters.swap(m_pendingCodeCharacters);
    pending_candidate_action deferred_action = pending_candidate_action::none;
    int deferred_number = 0;
    if (m_pendingCandidateActionCode == code)
    {
        deferred_action = m_pendingCandidateAction;
        deferred_number = m_pendingCandidateNumber;
    }
    m_pendingCandidateAction = pending_candidate_action::none;
    m_pendingCandidateActionCode.clear();
    m_pendingCandidateNumber = 0;

    if (config_revision != 0 &&
        m_brokerConfigRevision != 0 &&
        config_revision < m_brokerConfigRevision)
    {
        // A lower revision identifies a replacement Broker. Accept this
        // result instead of creating an unbounded stale-result retry loop.
        ime_tracef(L"BrokerConfigRevisionReset",
                   L"previous=%llu current=%llu generation=%llu",
                   static_cast<unsigned long long>(m_brokerConfigRevision),
                   static_cast<unsigned long long>(config_revision),
                   static_cast<unsigned long long>(generation));
        m_hasBrokerConfig = false;
        m_brokerConfigRevision = config_revision;
    }
    m_brokerCandidateCode = code;
    m_firstCandidateIsPinyin = !pinyin_flags.empty() && pinyin_flags[0];
    m_brokerCommitFirstOnNextCode =
        (result_flags & candidate_result_commit_first_on_next_code) != 0;
    {
        ZIME_PERF_SCOPE("tip.StoreCandidateSnapshot",
                        static_cast<std::int64_t>(code.size()),
                        static_cast<std::int64_t>(candidates.size()));
        m_candidateWindow.set_composition_text(code);
        m_candidateWindow.set_candidates(candidates, view_texts);
    }

    ITfDocumentMgr* document_manager = nullptr;
    ITfContext* context = nullptr;
    if (m_pThreadMgr &&
        SUCCEEDED(m_pThreadMgr->GetFocus(&document_manager)) &&
        document_manager)
    {
        document_manager->GetTop(&context);
        document_manager->Release();
    }

    if ((result_flags & candidate_result_auto_commit_first) != 0 &&
        !candidates.empty() && !candidates[0].empty())
    {
        const std::wstring display = view_texts.empty()
            ? candidates[0]
            : view_texts[0];
        RecordCandidateSelection(code, candidates[0], display);
        InsertText(context, candidates[0]);
        ClearComposition();
        HideCandidates();
    }
    else if (deferred_action == pending_candidate_action::space)
    {
        if (!candidates.empty())
        {
            HandleNumber(context, 1);
        }
        else
        {
            InsertText(context, code);
            ClearComposition();
            HideCandidates();
        }
    }
    else if (deferred_action == pending_candidate_action::number)
    {
        if (deferred_number > 0 &&
            deferred_number <= m_candidateWindow.get_candidate_count())
        {
            HandleNumber(context, deferred_number);
        }
        else
        {
            ShowCandidates(context);
        }
    }
    else
    {
        ShowCandidates(context);
    }
    if (context)
        context->Release();

    if (!pending_characters.empty())
    {
        ITfDocumentMgr* pending_document_manager = nullptr;
        ITfContext* pending_context = nullptr;
        if (m_pThreadMgr &&
            SUCCEEDED(m_pThreadMgr->GetFocus(&pending_document_manager)) &&
            pending_document_manager)
        {
            pending_document_manager->GetTop(&pending_context);
            pending_document_manager->Release();
        }
        for (const wchar_t character : pending_characters)
            HandleCharacter(pending_context, character);
        if (pending_context)
            pending_context->Release();
    }
    ime_tracef(L"BrokerCandidateResult",
               L"generation=%llu config_revision=%llu code_chars=%lu candidates=%lu flags=0x%lx",
               static_cast<unsigned long long>(generation),
               static_cast<unsigned long long>(config_revision),
               static_cast<unsigned long>(code.size()),
               static_cast<unsigned long>(candidates.size()),
               static_cast<unsigned long>(result_flags));
}

void text_service::RefreshCandidatesAfterStorageMutation()
{
    if (!m_bInComposition || m_compositionText.empty())
        return;

    RequestBrokerCandidates();
}

void text_service::OnBrokerUiAction(
    zime::broker_protocol::ui_action_type action,
    std::uint32_t value,
    POINT screen_point)
{
    using zime::broker_protocol::ui_action_type;
    switch (action)
    {
    case ui_action_type::candidate_select:
        if (m_bInComposition &&
            m_brokerCandidateCode != m_compositionText)
        {
            if (!ResolveBrokerCandidatesSynchronously(false))
            {
                m_pendingCandidateAction = pending_candidate_action::number;
                m_pendingCandidateActionCode = m_compositionText;
                m_pendingCandidateNumber = static_cast<int>(value) + 1;
                return;
            }
            if (!m_bInComposition)
                return;
        }
        OnCandidateClicked(static_cast<int>(value));
        return;
    case ui_action_type::candidate_delete:
        OnCandidateContextCommand(static_cast<int>(value), true);
        return;
    case ui_action_type::candidate_mark_uncommon:
        OnCandidateContextCommand(static_cast<int>(value), false);
        return;
    case ui_action_type::candidate_page:
    {
        const int target_page = static_cast<int>(value);
        while (m_candidateWindow.get_current_page() < target_page)
            m_candidateWindow.page_down();
        while (m_candidateWindow.get_current_page() > target_page)
            m_candidateWindow.page_up();
        m_candidateWindow.set_selection(0);

        ITfDocumentMgr* document_manager = nullptr;
        ITfContext* context = nullptr;
        if (m_pThreadMgr &&
            SUCCEEDED(m_pThreadMgr->GetFocus(&document_manager)) &&
            document_manager)
        {
            document_manager->GetTop(&context);
            document_manager->Release();
        }
        UpdateCandidateUIElement(context);
        if (context)
            context->Release();
        SyncBrokerCandidateState();
        return;
    }
    case ui_action_type::status_change:
    {
        const int status_type = static_cast<int>(value);
        const int state_value = screen_point.x;
        switch (status_type)
        {
        case status_window::STATUS_FULL_WIDTH:
            m_statusWindow.set_full_width(state_value != 0);
            break;
        case status_window::STATUS_CHINESE_MODE:
            m_statusWindow.set_chinese_mode(state_value != 0);
            break;
        case status_window::STATUS_PUNCTUATION:
            m_statusWindow.set_chinese_punctuation(state_value != 0);
            break;
        case status_window::STATUS_AUTO_COMMIT_FOUR_UNIQUE:
            m_statusWindow.set_auto_commit_four_code_unique(state_value != 0);
            break;
        case status_window::STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE:
            m_statusWindow.set_commit_first_candidate_on_fifth_code(state_value != 0);
            break;
        case status_window::STATUS_SHOW_UNCOMMON_CANDIDATES:
            m_statusWindow.set_show_uncommon_candidates(state_value != 0);
            break;
        case status_window::STATUS_REPLACE_DOT_AFTER_DIGIT:
            m_statusWindow.set_replace_dot_after_digit(state_value != 0);
            break;
        case status_window::STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE:
            m_statusWindow.set_use_english_punctuation_in_chinese_mode(
                state_value != 0);
            break;
        case status_window::STATUS_DISABLE_CHINESE_DASH:
            m_statusWindow.set_disable_chinese_dash(state_value != 0);
            break;
        case status_window::STATUS_UI_FONT_CHANGED:
            m_statusWindow.set_ui_font_percent(state_value);
            break;
        case status_window::STATUS_CANDIDATE_SORT_MODE_CHANGED:
            m_statusWindow.set_candidate_sort_mode(
                static_cast<status_window::CandidateSortMode>(
                    min(2, max(0, state_value))));
            break;
        default:
            break;
        }
        OnStatusChanged(status_type);
        SyncBrokerStatusState();
        return;
    }
    case ui_action_type::status_position:
        OnStatusWindowMoved(screen_point.x, screen_point.y);
        SyncBrokerStatusState();
        return;
    case ui_action_type::status_menu_popup:
        m_inStatusMenuPopup = value != 0;
        return;
    default:
        return;
    }
}

void text_service::UpdateCandidateUIElement(ITfContext *pContext)
{
    ZIME_PERF_SCOPE("tip.UpdateCandidateUIElement",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    m_candidateWindow.get_candidate_count());
    if (!m_pThreadMgr)
        return;

    const int candidate_count = m_candidateWindow.get_candidate_count();

    if (!m_candidateUIElement)
    {
        m_candidateUIElement = new candidate_ui_element();
        if (m_candidateUIElement)
        {
            m_candidateUIElement->set_show_callback(
                [this](BOOL bShow)
                {
                    this->OnHostCandidateUiShowChanged(bShow);
                });
            m_candidateUIElement->set_selection_callback(
                [this](UINT index)
                {
                    this->SetCandidateSelectionAbsolute(index);
                });
            m_candidateUIElement->set_finalize_callback(
                [this]()
                {
                    this->FinalizeCurrentCandidateSelection();
                });
            m_candidateUIElement->set_abort_callback(
                [this]()
                {
                    this->AbortCurrentCandidateUi();
                });
            m_candidateUIElement->set_key_down_callback(
                [this](WPARAM wParam, LPARAM lParam) -> bool
                {
                    return this->HandleCandidateUiKeyDown(wParam, lParam) != FALSE;
                });
            m_candidateUIElement->set_finalize_exact_callback(
                [this]()
                {
                    this->FinalizeExactCompositionStringFromUi();
                });
        }
    }
    if (!m_candidateUIElement)
        return;

    std::vector<std::wstring> candidates;
    if (candidate_count > 0)
    {
        candidates.reserve(static_cast<size_t>(candidate_count));
        for (int i = 0; i < candidate_count; ++i)
            candidates.push_back(m_candidateWindow.get_candidate(i));
    }

    const UINT page_size = static_cast<UINT>(max(1, m_candidateWindow.get_page_size()));
    std::vector<UINT> page_index;
    if (candidate_count > 0)
    {
        for (UINT i = 0; i < static_cast<UINT>(candidate_count); i += page_size)
            page_index.push_back(i);
    }

    UINT current_page = 0;
    UINT selection = 0;
    if (candidate_count > 0)
    {
        current_page = static_cast<UINT>(max(0, m_candidateWindow.get_current_page()));
        if (!page_index.empty() && current_page >= page_index.size())
            current_page = static_cast<UINT>(page_index.size() - 1);
        const UINT page_start = (current_page < page_index.size()) ? page_index[current_page] : 0;
        UINT relative_selection = static_cast<UINT>(max(0, m_candidateWindow.get_selection()));
        selection = page_start + relative_selection;
        if (!candidates.empty() && selection >= candidates.size())
            selection = static_cast<UINT>(candidates.size() - 1);
    }

    ITfDocumentMgr *pDocMgr = nullptr;
    if (pContext)
        pContext->GetDocumentMgr(&pDocMgr);

    m_candidateUIElement->update_state(
        candidates,
        page_index,
        current_page,
        selection,
        pDocMgr,
        TF_CLUIE_DOCUMENTMGR |
            TF_CLUIE_COUNT |
            TF_CLUIE_SELECTION |
            TF_CLUIE_STRING |
            TF_CLUIE_PAGEINDEX |
            TF_CLUIE_CURRENTPAGE);

    if (pDocMgr)
        pDocMgr->Release();

    ITfUIElementMgr *pUIMgr = nullptr;
    if (FAILED(m_pThreadMgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void **>(&pUIMgr))))
        return;

    if (m_candidateUIElementId == TF_INVALID_UIELEMENTID)
    {
        if (candidate_count <= 0)
        {
            pUIMgr->Release();
            return;
        }
        BOOL bShow = TRUE;
        const HRESULT hrBegin = pUIMgr->BeginUIElement(m_candidateUIElement, &bShow, &m_candidateUIElementId);
        ime_tracef(L"BeginUIElement",
                   L"hr=0x%08lx show=%d id=%lu count=%d uielement_only=%d",
                   static_cast<unsigned long>(hrBegin),
                   bShow ? 1 : 0,
                   static_cast<unsigned long>(m_candidateUIElementId),
                   candidate_count,
                   m_uiElementOnlyMode ? 1 : 0);
        if (FAILED(hrBegin))
        {
            if (m_uiElementOnlyMode)
                m_hostWantsCandidateWindow = false;
            pUIMgr->Release();
            return;
        }
        else
        {
            m_hostWantsCandidateWindow = (bShow != FALSE);
            m_candidateUIElement->set_initial_show_state(bShow);
        }
    }
    else if (candidate_count <= 0)
    {
        const HRESULT hrEnd = pUIMgr->EndUIElement(m_candidateUIElementId);
        ime_tracef(L"EndUIElement", L"empty hr=0x%08lx id=%lu",
                   static_cast<unsigned long>(hrEnd),
                   static_cast<unsigned long>(m_candidateUIElementId));
        m_candidateUIElementId = TF_INVALID_UIELEMENTID;
        pUIMgr->Release();
        return;
    }

    const HRESULT hrUpdate = pUIMgr->UpdateUIElement(m_candidateUIElementId);
    ime_tracef(L"UpdateUIElement", L"hr=0x%08lx id=%lu count=%d show_own=%d",
               static_cast<unsigned long>(hrUpdate),
               static_cast<unsigned long>(m_candidateUIElementId),
               candidate_count,
               m_hostWantsCandidateWindow ? 1 : 0);
    pUIMgr->Release();
}

void text_service::EndCandidateUIElement()
{
    if (!m_pThreadMgr || m_candidateUIElementId == TF_INVALID_UIELEMENTID)
        return;

    ITfUIElementMgr *pUIMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void **>(&pUIMgr))))
    {
        const HRESULT hrEnd = pUIMgr->EndUIElement(m_candidateUIElementId);
        ime_tracef(L"EndUIElement", L"hr=0x%08lx id=%lu",
                   static_cast<unsigned long>(hrEnd),
                   static_cast<unsigned long>(m_candidateUIElementId));
        pUIMgr->Release();
    }
    m_candidateUIElementId = TF_INVALID_UIELEMENTID;
}

void text_service::EnsureCandidateWindow()
{
    const HWND owner = get_foreground_ui_owner_window();
    HWND hwnd = m_candidateWindow.get_hwnd();
    if (hwnd)
    {
        const HWND current_owner = GetWindow(hwnd, GW_OWNER);
        if (current_owner == owner)
            return;
        m_candidateWindow.destroy();
    }

    const BOOL created = m_candidateWindow.create(owner);
    ime_tracef(L"CandidateWindowCreate",
               L"created=%d hwnd=0x%p owner=0x%p owner_pid=%lu",
               created ? 1 : 0,
               m_candidateWindow.get_hwnd(),
               owner,
               window_process_id(owner));
    m_candidateWindow.set_ui_font_percent(min(250, m_uiFontPercent + CANDIDATE_FONT_BOOST_PERCENT));
    m_candidateWindow.set_click_callback(
        [this](int candidate_index) {
            this->OnCandidateClicked(candidate_index);
        });
    m_candidateWindow.set_context_menu_callback(
        [this](int candidate_index, POINT pt) {
            this->OnCandidateContextMenu(candidate_index, pt);
        });
}

void text_service::UpdateCandidateWindowPosition(ITfContext* pContext,
                                                 bool asynchronous,
                                                 bool confirms_layout)
{
    ZIME_PERF_SCOPE("tip.UpdateCandidatePosition",
                    asynchronous ? 1 : 0,
                    m_candidatePositionEditPending ? 1 : 0);
    if (!pContext || m_candidatePositionEditPending)
        return;

    // 为了遵循 TSF 规范，在 EditSession 中通过 ITfContextView::GetTextExt
    // 获取插入符号的屏幕坐标，然后再移动候选窗口。
    class CandidatePosEditSession : public ITfEditSession
    {
    public:
        CandidatePosEditSession(text_service* service,
                                ITfContext* context,
                                bool confirms_layout)
            : _ref(1),
              _service(service),
              _context(context),
              _confirms_layout(confirms_layout),
              _composition(service ? service->m_compositionText : L""),
              _executed(false)
        {
            if (_context)
                _context->AddRef();
            if (_service)
                _service->AddRef();
        }

        virtual ~CandidatePosEditSession()
        {
            const bool retry_current_layout =
                _executed && _confirms_layout && _service && _context &&
                !_service->m_compositionText.empty() &&
                _service->m_compositionText != _composition;
            if (_service)
                _service->m_candidatePositionEditPending = false;
            if (retry_current_layout)
                _service->UpdateCandidateWindowPosition(_context, true, true);
            if (_context)
                _context->Release();
            if (_service)
            {
                _service->Release();
            }
        }

        STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
        {
            if (!ppvObj)
                return E_INVALIDARG;

            *ppvObj = nullptr;
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
            {
                *ppvObj = static_cast<ITfEditSession*>(this);
            }
            if (*ppvObj)
            {
                AddRef();
    return S_OK;
}

            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&_ref));
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const LONG cr = InterlockedDecrement(&_ref);
            if (cr == 0)
            {
                delete this;
            }
            return static_cast<ULONG>(cr);
        }

        STDMETHODIMP DoEditSession(TfEditCookie ec) override
        {
            ZIME_PERF_SCOPE("tip.CandidatePositionEdit", 0, 0);
            if (!_service || !_context)
                return E_FAIL;
            _executed = true;

            ITfContextView* pView = nullptr;
            bool positioned = false;
            const auto confirm_current_layout = [&]()
            {
                if (_confirms_layout &&
                    _service->m_compositionText == _composition)
                {
                    _service->m_brokerCandidateLayoutReady = true;
                }
            };

            const auto fallback_position = [&]() -> bool
            {
                RECT fallback = {};
                if (try_get_caret_fallback_rect(&fallback))
                {
                    _service->RememberCandidateAnchorRect(fallback);
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (_service->TryGetCachedCandidateAnchorRect(&fallback))
                {
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_focus_window_fallback_rect(&fallback))
                {
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_view_fallback_rect(pView, &fallback))
                {
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_owner_window_fallback_rect(&fallback))
                {
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                return false;
            };

            const auto position_from_range = [&](ITfRange* pRange) -> bool
            {
                if (!pView || !pRange)
                    return false;

                RECT rc = { 0 };
                BOOL fClipped = FALSE;
                const HRESULT hrExt = pView->GetTextExt(ec, pRange, &rc, &fClipped);
                const bool valid = SUCCEEDED(hrExt) && is_valid_text_ext_rect(rc, fClipped);
                ime_tracef(L"GetTextExt",
                           L"hr=0x%08lx clipped=%d valid=%d rect=(%ld,%ld,%ld,%ld)",
                           static_cast<unsigned long>(hrExt),
                           fClipped ? 1 : 0,
                           valid ? 1 : 0,
                           rc.left,
                           rc.top,
                           rc.right,
                           rc.bottom);
                if (valid)
                {
                    _service->RememberCandidateAnchorRect(rc);
                    confirm_current_layout();
                    _service->UpdateCandidateWindowPositionFromRect(rc);
                    return true;
                }
                return false;
            };

            HRESULT hr = _context->GetActiveView(&pView);
            if (FAILED(hr) || !pView)
            {
                fallback_position();
                return S_OK; // 无法获取视图时使用兜底定位，避免影响输入
            }
            const HWND view_hwnd = trace_context_view_relationship(pView);
            _service->SendBrokerContextSnapshot(view_hwnd);

            if (_service->m_pComposition)
            {
                ITfRange* pCompositionRange = nullptr;
                if (SUCCEEDED(_service->m_pComposition->GetRange(&pCompositionRange)) && pCompositionRange)
                {
                    ITfRange* pCaretRange = nullptr;
                    if (SUCCEEDED(pCompositionRange->Clone(&pCaretRange)) && pCaretRange)
                    {
                        if (SUCCEEDED(pCaretRange->Collapse(ec, TF_ANCHOR_END)))
                            positioned = position_from_range(pCaretRange);
                        pCaretRange->Release();
                    }
                    pCompositionRange->Release();
                }
            }

            if (!positioned)
            {
                ITfInsertAtSelection* pInsert = nullptr;
                ITfRange* pInsertRange = nullptr;
                if (SUCCEEDED(_context->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&pInsert))) && pInsert)
                {
                    if (SUCCEEDED(pInsert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &pInsertRange)) && pInsertRange)
                    {
                        positioned = position_from_range(pInsertRange);
                        pInsertRange->Release();
                    }
                    pInsert->Release();
                }
            }

            if (!positioned)
            {
                TF_SELECTION sel = {};
                ULONG cFetched = 0;
                hr = _context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched);
                if (SUCCEEDED(hr) && cFetched == 1 && sel.range)
                {
                    positioned = position_from_range(sel.range);
                    sel.range->Release();
                }
            }

            pView->Release();
            if (!positioned)
            {
                positioned = fallback_position();
            }
            if (!positioned && _confirms_layout &&
                _service->m_compositionText == _composition)
            {
                // The Broker can still use its GUI-thread caret fallback.
                _service->m_brokerCandidateLayoutReady = true;
                _service->SyncBrokerCandidateState();
            }
            return S_OK;
        }

    private:
        LONG _ref;
        text_service* _service;
        ITfContext* _context;
        bool _confirms_layout;
        std::wstring _composition;
        bool _executed;
    };

    m_candidatePositionEditPending = true;
    CandidatePosEditSession* pEditSession = new CandidatePosEditSession(
        this, pContext, confirms_layout);
    if (pEditSession)
    {
        HRESULT hr = S_OK;
        const DWORD flags = (asynchronous ? TF_ES_ASYNC : TF_ES_SYNC) | TF_ES_READ;
        HRESULT hrRequest = pContext->RequestEditSession(
            m_tfClientId, pEditSession, flags, &hr);
        if (!asynchronous && (FAILED(hrRequest) || FAILED(hr)))
        {
            m_candidatePositionEditPending = true;
            hrRequest = pContext->RequestEditSession(
                m_tfClientId, pEditSession, TF_ES_ASYNC | TF_ES_READ, &hr);
        }
        ZIME_PERF_RECORD("tip.UpdateCandidatePosition.result",
                         hrRequest, hr, asynchronous ? 1 : 0);
        pEditSession->Release();
    }
    else
    {
        m_candidatePositionEditPending = false;
    }
}

void text_service::RememberCandidateAnchorRect(const RECT& rc)
{
    m_lastCandidateAnchorRect = rc;
    m_hasLastCandidateAnchorRect = true;
    m_lastCandidateAnchorTick = GetTickCount();
    m_lastCandidateAnchorOwner = get_foreground_ui_owner_window();
}

void text_service::ClearCandidateAnchorRect()
{
    SetRectEmpty(&m_lastCandidateAnchorRect);
    m_hasLastCandidateAnchorRect = false;
    m_lastCandidateAnchorTick = 0;
    m_lastCandidateAnchorOwner = nullptr;
    m_brokerCandidateLayoutReady = false;
}

bool text_service::TryGetCachedCandidateAnchorRect(RECT* rc_out) const
{
    if (!rc_out || !m_hasLastCandidateAnchorRect)
        return false;

    constexpr DWORD kAnchorCacheLifetimeMs = 5000;
    if (GetTickCount() - m_lastCandidateAnchorTick > kAnchorCacheLifetimeMs)
        return false;

    const HWND current_owner = get_foreground_ui_owner_window();
    if (m_lastCandidateAnchorOwner &&
        current_owner &&
        m_lastCandidateAnchorOwner != current_owner)
    {
        return false;
    }

    *rc_out = m_lastCandidateAnchorRect;
    return true;
}

void text_service::SetCandidateSelectionAbsolute(UINT index)
{
    const int candidate_count = m_candidateWindow.get_candidate_count();
    if (candidate_count <= 0)
        return;

    int clamped = static_cast<int>(index);
    if (clamped < 0)
        clamped = 0;
    if (clamped >= candidate_count)
        clamped = candidate_count - 1;

    const int page_size = max(1, m_candidateWindow.get_page_size());
    const int target_page = clamped / page_size;
    while (m_candidateWindow.get_current_page() < target_page)
        m_candidateWindow.page_down();
    while (m_candidateWindow.get_current_page() > target_page)
        m_candidateWindow.page_up();
    m_candidateWindow.set_selection(clamped % page_size);
}

int text_service::GetCandidateSelectionAbsolute() const
{
    const int page_size = max(1, m_candidateWindow.get_page_size());
    return m_candidateWindow.get_current_page() * page_size + m_candidateWindow.get_selection();
}

void text_service::FinalizeCurrentCandidateSelection()
{
    OnCandidateClicked(GetCandidateSelectionAbsolute());
}

void text_service::AbortCurrentCandidateUi()
{
    CancelCompositionInContext(nullptr);
    ClearComposition();
    HideCandidates();
}

BOOL text_service::HandleCandidateUiKeyDown(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    if (!m_bInComposition || m_compositionText.empty() || m_candidateWindow.get_candidate_count() <= 0)
        return FALSE;

    switch (wParam)
    {
    case VK_LEFT:
    case VK_UP:
    {
        const int current = GetCandidateSelectionAbsolute();
        if (current > 0)
            SetCandidateSelectionAbsolute(static_cast<UINT>(current - 1));
        return TRUE;
    }
    case VK_RIGHT:
    case VK_DOWN:
    {
        const int current = GetCandidateSelectionAbsolute();
        if (current + 1 < m_candidateWindow.get_candidate_count())
            SetCandidateSelectionAbsolute(static_cast<UINT>(current + 1));
        return TRUE;
    }
    case VK_PRIOR:
        m_candidateWindow.page_up();
        m_candidateWindow.set_selection(0);
        return TRUE;
    case VK_NEXT:
        m_candidateWindow.page_down();
        m_candidateWindow.set_selection(0);
        return TRUE;
    case VK_RETURN:
    case VK_SPACE:
        FinalizeCurrentCandidateSelection();
        return TRUE;
    case VK_ESCAPE:
        AbortCurrentCandidateUi();
        return TRUE;
    default:
        if (wParam >= '1' && wParam <= '9')
        {
            const int page_size = max(1, m_candidateWindow.get_page_size());
            const int actual = m_candidateWindow.get_current_page() * page_size + static_cast<int>(wParam - '1');
            if (actual >= 0 && actual < m_candidateWindow.get_candidate_count())
            {
                OnCandidateClicked(actual);
                return TRUE;
            }
        }
        break;
    }

    return FALSE;
}

void text_service::FinalizeExactCompositionStringFromUi()
{
    CommitCompositionCodeAndClear(nullptr);
}

void text_service::UpdateCandidateWindowPositionFromRect(const RECT& rc)
{
    SyncBrokerCandidateState(&rc);
    HWND hwndCand = m_candidateWindow.get_hwnd();
    if (!hwndCand)
        return;

    int winWidth = 0;
    int winHeight = 0;
    m_candidateWindow.get_window_size(winWidth, winHeight);

    // 按光标所在位置选择显示器，比按候选框旧位置更稳定。
    HMONITOR hMon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi))
        return;

    const RECT& wa = mi.rcWork; // 当前显示器的工作区域（排除任务栏）
    const int gap = 2;
    int x = rc.left;

    // X 方向矫正
    if (winWidth > (wa.right - wa.left))
    {
        // 如果窗口比工作区还宽，就直接贴到左边
        x = wa.left;
    }
    else
    {
        if (x < wa.left) x = wa.left;
        if (x + winWidth > wa.right) x = wa.right - winWidth;
    }

    // Y 方向优先策略：优先光标下方；下方不够时放到光标上方，避免底部区域抖动。
    const int yBelow = rc.bottom + gap;
    const int yAbove = rc.top - gap - winHeight;
    const int spaceBelow = wa.bottom - yBelow;
    const int spaceAbove = (rc.top - gap) - wa.top;

    int y = yBelow;
    if (winHeight <= max(0, spaceBelow))
    {
        y = yBelow;
    }
    else if (winHeight <= max(0, spaceAbove))
    {
        y = yAbove;
    }
    else if (spaceAbove > spaceBelow)
    {
        y = wa.top;
    }
    else
    {
        y = wa.bottom - winHeight;
    }

    // 最后再做一次通用矫正（覆盖极端尺寸情况）
    if (winHeight > (wa.bottom - wa.top))
    {
        y = wa.top;
    }
    else
    {
        if (y < wa.top) y = wa.top;
        if (y + winHeight > wa.bottom) y = wa.bottom - winHeight;
    }

    SetWindowPos(hwndCand, HWND_TOPMOST, x, y, winWidth, winHeight, SWP_NOACTIVATE);
}

std::filesystem::path text_service::GetConfigPath() const
{
    return tool::get_user_data_path() / CONFIG_FILE;
}

void text_service::LoadRuntimeConfig()
{
    std::filesystem::path cfg = GetConfigPath();
    std::error_code error;
    if (!std::filesystem::is_regular_file(cfg, error))
    {
        const std::filesystem::path legacy =
            tool::get_current_dll_path() / CONFIG_FILE;
        error.clear();
        if (std::filesystem::is_regular_file(legacy, error))
            cfg = legacy;
    }
    const wchar_t* path = cfg.c_str();

    m_autoCommitFourCodeUnique = GetPrivateProfileIntW(L"state", L"auto_commit_four_code_unique", m_autoCommitFourCodeUnique ? 1 : 0, path) != 0;
    m_commitFirstCandidateOnFifthCode = GetPrivateProfileIntW(L"state", L"commit_first_candidate_on_fifth_code", m_commitFirstCandidateOnFifthCode ? 1 : 0, path) != 0;
    m_showUncommonCandidates = GetPrivateProfileIntW(L"state", L"show_uncommon_candidates", m_showUncommonCandidates ? 1 : 0, path) != 0;
    m_replaceDotAfterDigit = GetPrivateProfileIntW(L"state", L"replace_dot_after_digit", m_replaceDotAfterDigit ? 1 : 0, path) != 0;
    m_useEnglishPunctuationInChineseMode = GetPrivateProfileIntW(
        L"state",
        L"use_english_punctuation_in_chinese_mode",
        m_useEnglishPunctuationInChineseMode ? 1 : 0,
        path) != 0;
    m_disableChineseDash = GetPrivateProfileIntW(
        L"state",
        L"disable_chinese_dash",
        m_disableChineseDash ? 1 : 0,
        path) != 0;
    SyncPunctuationModeWithLanguageMode();
    const int sort_mode = GetPrivateProfileIntW(
        L"state",
        L"candidate_sort_mode",
        static_cast<int>(m_candidateSortMode),
        path);
    m_candidateSortMode = candidate_sort_mode_from_int(sort_mode);
    m_uiFontPercent = GetPrivateProfileIntW(L"ui", L"font_percent", m_uiFontPercent, path);
    if (m_uiFontPercent < 80)
        m_uiFontPercent = 80;
    if (m_uiFontPercent > 250)
        m_uiFontPercent = 250;
    m_statusWindowPositionCustomized = GetPrivateProfileIntW(L"ui", L"status_position_customized", 0, path) != 0;
    m_statusWindowPosX = GetPrivateProfileIntW(L"ui", L"status_pos_x", 0, path);
    m_statusWindowPosY = GetPrivateProfileIntW(L"ui", L"status_pos_y", 0, path);
}

void text_service::SaveRuntimeConfig()
{
    using namespace zime::broker_protocol;
    if (m_brokerClient.HasStorageWriter())
        return;

    const std::filesystem::path cfg = GetConfigPath();
    std::error_code ec;
    if (!cfg.parent_path().empty())
    {
        std::filesystem::create_directories(cfg.parent_path(), ec);
    }

    const wchar_t* path = cfg.c_str();
    WritePrivateProfileStringW(L"state", L"auto_commit_four_code_unique", m_autoCommitFourCodeUnique ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"state", L"commit_first_candidate_on_fifth_code", m_commitFirstCandidateOnFifthCode ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"state", L"show_uncommon_candidates", m_showUncommonCandidates ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"state", L"replace_dot_after_digit", m_replaceDotAfterDigit ? L"1" : L"0", path);
    WritePrivateProfileStringW(
        L"state",
        L"use_english_punctuation_in_chinese_mode",
        m_useEnglishPunctuationInChineseMode ? L"1" : L"0",
        path);
    WritePrivateProfileStringW(
        L"state",
        L"disable_chinese_dash",
        m_disableChineseDash ? L"1" : L"0",
        path);
    wchar_t sort_mode_buf[16] = {};
    _itow_s(static_cast<int>(m_candidateSortMode), sort_mode_buf, 10);
    WritePrivateProfileStringW(L"state", L"candidate_sort_mode", sort_mode_buf, path);
    wchar_t font_percent_buf[16] = {};
    _itow_s(m_uiFontPercent, font_percent_buf, 10);
    WritePrivateProfileStringW(L"ui", L"font_percent", font_percent_buf, path);
    WritePrivateProfileStringW(L"ui", L"status_position_customized", m_statusWindowPositionCustomized ? L"1" : L"0", path);
    wchar_t posx[16] = {};
    wchar_t posy[16] = {};
    _itow_s(m_statusWindowPosX, posx, 10);
    _itow_s(m_statusWindowPosY, posy, 10);
    WritePrivateProfileStringW(L"ui", L"status_pos_x", posx, path);
    WritePrivateProfileStringW(L"ui", L"status_pos_y", posy, path);
}

void text_service::UpdateStatusWindow(bool sync_broker)
{
    const int candidate_font_percent = min(250, m_uiFontPercent + CANDIDATE_FONT_BOOST_PERCENT);

    // 同步状态窗口的各种模式
    m_statusWindow.set_chinese_mode(m_bChineseMode);
    m_statusWindow.set_full_width(m_bFullWidth);
    m_statusWindow.set_chinese_punctuation(m_bChinesePunctuation);
    m_statusWindow.set_auto_commit_four_code_unique(m_autoCommitFourCodeUnique);
    m_statusWindow.set_commit_first_candidate_on_fifth_code(m_commitFirstCandidateOnFifthCode);
    m_statusWindow.set_show_uncommon_candidates(m_showUncommonCandidates);
    m_statusWindow.set_replace_dot_after_digit(m_replaceDotAfterDigit);
    m_statusWindow.set_use_english_punctuation_in_chinese_mode(m_useEnglishPunctuationInChineseMode);
    m_statusWindow.set_disable_chinese_dash(m_disableChineseDash);
    m_statusWindow.set_candidate_sort_mode(to_status_sort_mode(m_candidateSortMode));
    m_statusWindow.set_ui_font_percent(m_uiFontPercent);
    m_candidateWindow.set_ui_font_percent(candidate_font_percent);
    if (sync_broker)
        SyncBrokerStatusState();
}

void text_service::SyncBrokerStatusState()
{
    using namespace zime::broker_protocol;
    std::uint32_t flags = 0;
    if (ShouldShowStatusWindow())
        flags |= status_custom_ui_allowed;
    if (m_bFullWidth)
        flags |= status_full_width;
    if (m_bChineseMode)
        flags |= status_chinese_mode;
    if (m_bChinesePunctuation)
        flags |= status_chinese_punctuation;

    HWND owner = m_brokerViewHwnd;
    if (owner && IsWindow(owner))
    {
        HWND root = GetAncestor(owner, GA_ROOT);
        if (root && IsWindow(root) &&
            window_process_id(root) == GetCurrentProcessId())
            owner = root;
        else if (root && window_process_id(root) != GetCurrentProcessId())
            owner = nullptr;
    }
    m_brokerClient.SendStatusState(
        owner,
        m_statusWindowDesiredVisible && ShouldShowStatusWindow(),
        flags,
        0,
        0,
        100,
        0);
}

void text_service::ShowStatusWindow()
{
    RefreshBrokerContextForCurrentFocus();
    if (!ShouldShowStatusWindow())
    {
        m_statusWindowDesiredVisible = false;
        if (m_statusWindow.get_hwnd())
            m_statusWindow.show(false);
        SyncBrokerStatusState();
        return;
    }

    // 在全屏前台窗口（典型游戏）下，不主动显示状态窗，避免打断全屏。
    if (is_likely_fullscreen_foreground_window())
    {
        ime_tracef(L"StatusWindow", L"suppressed reason=fullscreen");
        m_statusWindowDesiredVisible = false;
        if (m_statusWindow.get_hwnd())
            m_statusWindow.show(false);
        SyncBrokerStatusState();
        return;
    }

    m_statusWindowDesiredVisible = true;
    if (m_brokerClient.IsConnected())
    {
        ime_tracef(L"StatusWindow", L"route=broker visible=1");
        m_statusWindow.show(false);
        SyncBrokerStatusState();
        return;
    }

    if (!ShouldShowLocalStatusWindow())
    {
        ime_tracef(L"StatusWindow", L"route=none uielement_only=1");
        m_statusWindow.show(false);
        SyncBrokerStatusState();
        return;
    }

    HWND owner = get_foreground_ui_owner_window();
    // During language switching, the foreground window can belong to a system
    // UI thread. Match status_window::create's ownership rule before comparing
    // owners, otherwise every update would unnecessarily recreate the window.
    if (owner && GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId())
        owner = nullptr;
    ime_tracef(L"StatusWindow",
               L"route=local visible=1 owner=0x%p owner_pid=%lu",
               owner,
               window_process_id(owner));

    // 创建状态窗口（如果还没创建），或在 owner 变化时重建。
    HWND status_hwnd = m_statusWindow.get_hwnd();
    if (status_hwnd && GetWindow(status_hwnd, GW_OWNER) != owner)
    {
        m_statusWindow.destroy();
        status_hwnd = nullptr;
    }

    if (!status_hwnd)
    {
        m_statusWindow.create(owner);
        
        // 设置回调函数
        m_statusWindow.set_status_change_callback(
            [this](int status_type) { this->OnStatusChanged(status_type); }
        );
        m_statusWindow.set_menu_popup_state_callback(
            [this](bool showing) { this->m_inStatusMenuPopup = showing; }
        );
        m_statusWindow.set_position_changed_callback(
            [this](int x, int y) { this->OnStatusWindowMoved(x, y); }
        );
    }
    
    if (m_statusWindowPositionCustomized)
    {
        m_statusWindow.move(m_statusWindowPosX, m_statusWindowPosY);
    }
    else
    {
        RECT work_area = {};
        HMONITOR hMon = nullptr;
        const HWND fg = GetForegroundWindow();
        if (fg && IsWindow(fg))
            hMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        if (!hMon)
        {
            POINT pt = {};
            GetCursorPos(&pt);
            hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (hMon && GetMonitorInfoW(hMon, &mi))
        {
            work_area = mi.rcWork;
        }
        else
        {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
        }

        // 计算状态窗口的位置（屏幕右下角，任务栏上方）
        int window_width = 0;
        int window_height = 0;
        m_statusWindow.get_window_size(window_width, window_height);
        const int x = work_area.right - window_width - 10;
        const int y = work_area.bottom - window_height - 10;
        m_statusWindow.move(x, y);
    }
    
    // 显示窗口
    m_statusWindow.show(true);
    
    // 同步状态
    UpdateStatusWindow();
}

void text_service::OnStatusChanged(int status_type)
{
    switch (status_type)
    {
    case status_window::STATUS_FULL_WIDTH:
        m_bFullWidth = m_statusWindow.is_full_width();
        return;
    case status_window::STATUS_CHINESE_MODE:
    {
        const bool next_mode = m_statusWindow.is_chinese_mode();
        if (!next_mode)
            CommitCompositionCodeAndClear(nullptr);
        m_bChineseMode = next_mode;
        SyncPunctuationModeWithLanguageMode();
        UpdateStatusWindow(false);
        return;
    }
    case status_window::STATUS_PUNCTUATION:
        m_bChinesePunctuation = m_statusWindow.is_chinese_punctuation();
        return;
    default:
        break;
    }
    UpdateStatusWindow(false);
    MessageBoxW(m_statusWindow.get_hwnd(),
                L"后台服务尚未连接，功能菜单暂不可用。",
                L"提示",
                MB_OK | MB_ICONWARNING);
}

void text_service::OnStatusWindowMoved(int x, int y)
{
    m_statusWindowPositionCustomized = true;
    m_statusWindowPosX = x;
    m_statusWindowPosY = y;
    SaveRuntimeConfig();
    SyncBrokerStatusState();
}

LRESULT CALLBACK text_service::CreateWordWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    text_service* pThis = reinterpret_cast<text_service*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (uMsg)
    {
    case WM_CREATE:
        {
            auto pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
            pThis = static_cast<text_service*>(pcs->lpCreateParams);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));

            SetPropW(hWnd, CREATE_WORD_BG_BRUSH_PROP, CreateSolidBrush(RGB(248, 250, 253)));
            SetPropW(hWnd, CREATE_WORD_EDIT_BRUSH_PROP, CreateSolidBrush(RGB(255, 255, 255)));

            CreateWindowW(L"STATIC", L"新词:", WS_CHILD | WS_VISIBLE,
                          18, 20, 56, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEW_WORD_LABEL)), g_hInst, nullptr);
            HWND hWord = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                       78, 16, 266, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEW_WORD_EDIT)), g_hInst, nullptr);

            CreateWindowW(L"STATIC", L"编码:", WS_CHILD | WS_VISIBLE,
                          18, 62, 56, 24, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEW_CODE_LABEL)), g_hInst, nullptr);
            HWND hCode = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_LOWERCASE,
                                       78, 58, 266, 28, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEW_CODE_EDIT)), g_hInst, nullptr);
            SendMessageW(hCode, EM_SETLIMITTEXT, 32, 0);
            CreateWindowW(L"STATIC", L"仅允许 a-y 字母", WS_CHILD | WS_VISIBLE,
                          78, 90, 180, 18, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_NEW_CODE_HINT)), g_hInst, nullptr);

            CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          174, 114, 82, 30, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CREATE_OK)), g_hInst, nullptr);
            CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                          264, 114, 82, 30, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CREATE_CANCEL)), g_hInst, nullptr);

            ApplyCreateWordLayout(hWnd, pThis ? pThis->m_uiFontPercent : 100);
            if (hWord)
            {
                SetFocus(hWord);
            }
        }
        return 0;

    case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_CREATE_CANCEL)
            {
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == IDC_CREATE_OK)
            {
                if (pThis)
                    pThis->OnConfirmCreateWord(hWnd);
                return 0;
            }
            if (id == IDC_NEW_CODE_EDIT && code == EN_CHANGE)
            {
                static bool in_change = false;
                if (in_change)
                    return 0;

                HWND hEdit = reinterpret_cast<HWND>(lParam);
                wchar_t buf[128] = {};
                GetWindowTextW(hEdit, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
                std::wstring src(buf), dst;
                dst.reserve(src.size());
                for (wchar_t ch : src)
                {
                    wchar_t lower = static_cast<wchar_t>(towlower(ch));
                    if (lower >= L'a' && lower <= L'y')
                        dst.push_back(lower);
                }
                if (dst != src)
                {
                    in_change = true;
                    SetWindowTextW(hEdit, dst.c_str());
                    SendMessageW(hEdit, EM_SETSEL, static_cast<WPARAM>(dst.size()), static_cast<LPARAM>(dst.size()));
                    in_change = false;
                }
                return 0;
            }
        }
        break;

    case WM_DPICHANGED:
        {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested)
            {
                SetWindowPos(hWnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ApplyCreateWordLayout(hWnd, pThis ? pThis->m_uiFontPercent : 100);
        }
        return 0;

    case WM_CREATE_WORD_APPLY_LAYOUT:
        ApplyCreateWordLayout(hWnd, pThis ? pThis->m_uiFontPercent : 100);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HBRUSH bg = reinterpret_cast<HBRUSH>(GetPropW(hWnd, CREATE_WORD_BG_BRUSH_PROP));
            FillRect(hdc, &rc, bg ? bg : GetSysColorBrush(COLOR_WINDOW));
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(42, 56, 76));
            HBRUSH bg = reinterpret_cast<HBRUSH>(GetPropW(hWnd, CREATE_WORD_BG_BRUSH_PROP));
            return reinterpret_cast<INT_PTR>(bg ? bg : GetSysColorBrush(COLOR_WINDOW));
        }

    case WM_CTLCOLOREDIT:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(255, 255, 255));
            SetTextColor(hdc, RGB(30, 38, 50));
            HBRUSH edit_bg = reinterpret_cast<HBRUSH>(GetPropW(hWnd, CREATE_WORD_EDIT_BRUSH_PROP));
            return reinterpret_cast<INT_PTR>(edit_bg ? edit_bg : GetSysColorBrush(COLOR_WINDOW));
        }

    case WM_DESTROY:
        {
            HFONT hFont = reinterpret_cast<HFONT>(GetPropW(hWnd, CREATE_WORD_FONT_PROP));
            if (hFont)
            {
                DeleteObject(hFont);
                RemovePropW(hWnd, CREATE_WORD_FONT_PROP);
            }
            HBRUSH bg = reinterpret_cast<HBRUSH>(GetPropW(hWnd, CREATE_WORD_BG_BRUSH_PROP));
            if (bg)
            {
                DeleteObject(bg);
                RemovePropW(hWnd, CREATE_WORD_BG_BRUSH_PROP);
            }
            HBRUSH edit_bg = reinterpret_cast<HBRUSH>(GetPropW(hWnd, CREATE_WORD_EDIT_BRUSH_PROP));
            if (edit_bg)
            {
                DeleteObject(edit_bg);
                RemovePropW(hWnd, CREATE_WORD_EDIT_BRUSH_PROP);
            }
        }
        if (pThis)
        {
            pThis->m_hCreateWordWnd = nullptr;
            pThis->m_statusWindowHideSuppressedUntilTick = GetTickCount() + 1500;
            pThis->m_inStatusMenuPopup = false;
            if (pThis->ShouldShowStatusWindow())
            {
                pThis->m_statusWindow.restore_async();
            }
        }
        return 0;

    default:
        break;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void text_service::ShowCreateWordWindow()
{
    HWND owner = get_foreground_ui_owner_window();
    const HWND reference = owner ? owner : m_statusWindow.get_hwnd();
    const UINT dpi = resolve_window_dpi(reference);
    const int window_width = scale_ui_px(372, dpi, m_uiFontPercent);
    const int window_height = scale_ui_px(206, dpi, m_uiFontPercent);
    const RECT work_area = resolve_primary_work_area();
    const POINT centered = center_point_in_work_area(work_area, window_width, window_height);

    if (m_hCreateWordWnd && IsWindow(m_hCreateWordWnd))
    {
        m_inStatusMenuPopup = true;
        SetWindowPos(m_hCreateWordWnd,
                     HWND_TOPMOST,
                     centered.x,
                     centered.y,
                     window_width,
                     window_height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(m_hCreateWordWnd, SW_SHOWNORMAL);
        SetForegroundWindow(m_hCreateWordWnd);
        return;
    }

    static bool class_registered = false;
    if (!class_registered)
    {
        WNDCLASSEXW wcex = { 0 };
        wcex.cbSize = sizeof(wcex);
        wcex.lpfnWndProc = CreateWordWindowProc;
        wcex.hInstance = g_hInst;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wcex.lpszClassName = CREATE_WORD_WINDOW_CLASS;
        class_registered = RegisterClassExW(&wcex) != 0;
    }

    m_hCreateWordWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CREATE_WORD_WINDOW_CLASS,
        L"造词",
        WS_CAPTION | WS_SYSMENU,
        centered.x, centered.y, window_width, window_height,
        owner,
        nullptr,
        g_hInst,
        this);

    if (m_hCreateWordWnd)
    {
        m_inStatusMenuPopup = true;
        ShowWindow(m_hCreateWordWnd, SW_SHOWNORMAL);
        UpdateWindow(m_hCreateWordWnd);
    }
}

void text_service::OnConfirmCreateWord(HWND hWnd)
{
    if (m_pendingCreateWordRequest != 0)
        return;

    wchar_t word_buf[256] = {};
    wchar_t code_buf[128] = {};

    GetWindowTextW(GetDlgItem(hWnd, IDC_NEW_WORD_EDIT), word_buf, static_cast<int>(sizeof(word_buf) / sizeof(word_buf[0])));
    GetWindowTextW(GetDlgItem(hWnd, IDC_NEW_CODE_EDIT), code_buf, static_cast<int>(sizeof(code_buf) / sizeof(code_buf[0])));

    std::wstring word = trim_ws(word_buf);
    std::wstring code_ws = trim_ws(code_buf);
    std::transform(code_ws.begin(), code_ws.end(), code_ws.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

    if (word.empty() || code_ws.empty())
    {
        MessageBoxW(hWnd, L"新词和编码都不能为空。", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    for (wchar_t ch : code_ws)
    {
        if (ch < L'a' || ch > L'y')
        {
            MessageBoxW(hWnd, L"编码只允许 a-y 字母。", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (m_brokerClient.HasStorageWriter())
    {
        const std::uint64_t request_id = m_brokerClient.SendStorageMutation(
            zime::broker_protocol::storage_operation::add_custom_word,
            code_ws,
            word,
            true);
        if (request_id != 0)
        {
            m_pendingCreateWordRequest = request_id;
            EnableWindow(GetDlgItem(hWnd, IDC_CREATE_OK), FALSE);
            return;
        }
        MessageBoxW(hWnd,
                    L"后台写入队列繁忙，请稍后重试。",
                    L"提示",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    MessageBoxW(hWnd,
                L"后台服务尚未连接，暂时不能造词。",
                L"提示",
                MB_OK | MB_ICONWARNING);
}

void text_service::ExportRawDictionary()
{
    MessageBoxW(get_foreground_ui_owner_window(),
                L"后台服务尚未连接，暂时不能导出词库。",
                L"提示",
                MB_OK | MB_ICONWARNING);
}

std::wstring text_service::ResolveCandidateCodeForContextMenu(const std::wstring& code_snapshot,
                                                              const std::wstring& candidate_text,
                                                              const std::wstring& display_text) const
{
    if (code_snapshot.empty() || candidate_text.empty() || display_text.empty())
        return code_snapshot;
    if (display_text.size() <= candidate_text.size())
        return code_snapshot;

    if (display_text.compare(0, candidate_text.size(), candidate_text) != 0)
        return code_snapshot;

    const std::wstring suffix = display_text.substr(candidate_text.size());
    if (suffix.size() == 1)
    {
        const wchar_t ch = static_cast<wchar_t>(towlower(suffix[0]));
        if (ch >= L'a' && ch <= L'z')
            return code_snapshot + std::wstring(1, ch);
    }

    return code_snapshot;
}

void text_service::RecordCandidateSelection(const std::wstring& code_snapshot,
                                            const std::wstring& candidate_text,
                                            const std::wstring& display_text)
{
    if (code_snapshot.empty() || candidate_text.empty())
        return;
    const std::wstring effective_code = ResolveCandidateCodeForContextMenu(code_snapshot, candidate_text, display_text);
    if (m_brokerClient.HasStorageWriter())
    {
        m_brokerClient.SendStorageMutation(
            zime::broker_protocol::storage_operation::record_selection,
            effective_code,
            candidate_text,
            false);
        return;
    }
}

void text_service::OnCandidateContextMenu(int candidate_index, POINT screen_point)
{
    if (!m_bInComposition || m_compositionText.empty())
        return;
    if (candidate_index < 0 || candidate_index >= m_candidateWindow.get_candidate_count())
        return;

    const std::wstring candidate = m_candidateWindow.get_candidate(candidate_index);
    if (candidate.empty())
        return;

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;
    AppendMenuW(hMenu, MF_STRING, IDC_CAND_MENU_DELETE, L"删除");
    AppendMenuW(hMenu, MF_STRING, IDC_CAND_MENU_MARK_UNCOMMON, L"标记不常用");

    HWND owner = m_candidateWindow.get_hwnd();
    if (!owner)
    {
        DestroyMenu(hMenu);
        return;
    }

    m_inCandidateContextMenu = true;
    const UINT cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        screen_point.x, screen_point.y, 0, owner, nullptr);
    m_inCandidateContextMenu = false;
    DestroyMenu(hMenu);

    if (cmd != IDC_CAND_MENU_DELETE && cmd != IDC_CAND_MENU_MARK_UNCOMMON)
    {
        // 菜单取消后保持候选框可见和输入状态
        if (m_bInComposition && !m_compositionText.empty())
        {
            ApplyCandidateWindowVisibility();
        }
        return;
    }

    OnCandidateContextCommand(candidate_index, cmd == IDC_CAND_MENU_DELETE);
}

void text_service::OnCandidateContextCommand(int candidate_index, bool delete_candidate)
{
    if (!m_bInComposition || m_compositionText.empty())
        return;
    if (candidate_index < 0 ||
        candidate_index >= m_candidateWindow.get_candidate_count())
    {
        return;
    }

    const std::wstring code_snapshot = m_compositionText;
    const std::wstring candidate = m_candidateWindow.get_candidate(candidate_index);
    const std::wstring display_candidate =
        m_candidateWindow.get_display_candidate(candidate_index);
    if (candidate.empty())
        return;
    const std::wstring effective_code = ResolveCandidateCodeForContextMenu(
        code_snapshot, candidate, display_candidate);

    if (m_brokerClient.HasStorageWriter())
    {
        const auto operation = delete_candidate
            ? zime::broker_protocol::storage_operation::delete_candidate
            : zime::broker_protocol::storage_operation::mark_uncommon;
        const std::uint64_t request_id = m_brokerClient.SendStorageMutation(
            operation,
            effective_code,
            candidate,
            true);
        if (request_id != 0)
        {
            m_pendingCandidateStorageRequests.emplace(request_id, operation);
            return;
        }
        MessageBoxW(m_brokerViewHwnd,
                    L"后台写入队列繁忙，请稍后重试。",
                    L"提示",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    MessageBoxW(m_brokerViewHwnd,
                L"后台服务尚未连接，暂时不能修改词库。",
                L"提示",
                MB_OK | MB_ICONWARNING);
}

void text_service::OnCandidateClicked(int candidate_index)
{
    if (!m_bInComposition || m_compositionText.empty())
        return;
    if (candidate_index < 0 || candidate_index >= m_candidateWindow.get_candidate_count())
        return;

    const std::wstring code_snapshot = m_compositionText;
    const std::wstring candidate = m_candidateWindow.get_candidate(candidate_index);
    const std::wstring display_candidate = m_candidateWindow.get_display_candidate(candidate_index);
    if (candidate.empty())
        return;
    RecordCandidateSelection(code_snapshot, candidate, display_candidate);

    // 从当前焦点文档拿到 ITfContext，再执行上屏
    ITfDocumentMgr* pDocMgrFocus = nullptr;
    ITfContext* pContext = nullptr;
    if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
    {
        pDocMgrFocus->GetTop(&pContext);
        pDocMgrFocus->Release();
    }

    if (pContext)
    {
        InsertText(pContext, candidate);
        pContext->Release();
    }

    ClearComposition();
    HideCandidates();
}

