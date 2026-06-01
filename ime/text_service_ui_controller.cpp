#include "text_service.h"

#include <Windows.h>
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
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd))
        return false;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0)
        return false;

    RECT wr = {};
    if (!GetWindowRect(hwnd, &wr))
        return false;

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return false;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi))
        return false;

    const RECT mr = mi.rcMonitor;
    constexpr int tol = 2;
    const bool covers_monitor =
        wr.left <= mr.left + tol &&
        wr.top <= mr.top + tol &&
        wr.right >= mr.right - tol &&
        wr.bottom >= mr.bottom - tol;
    if (!covers_monitor)
        return false;

    const bool borderless = (style & WS_CAPTION) == 0 && (style & WS_THICKFRAME) == 0;
    return borderless;
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
    const HWND hwnd_before_show = m_candidateWindow.get_hwnd();
    const bool was_visible = hwnd_before_show && IsWindowVisible(hwnd_before_show);

    UpdateCandidateUIElement(pContext);
    if (ShouldShowOwnCandidateWindow())
    {
        EnsureCandidateWindow();
        if (m_candidateWindow.get_hwnd())
            UpdateCandidateWindowPosition(pContext);
    }
    ApplyCandidateWindowVisibility();

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

bool text_service::ShouldShowStatusWindow() const
{
    return !m_uiElementOnlyMode;
}

void text_service::ApplyCandidateWindowVisibility()
{
    const bool has_content =
        m_bInComposition &&
        (!m_compositionText.empty() || m_candidateWindow.get_candidate_count() > 0);
    const bool should_show = has_content && ShouldShowOwnCandidateWindow();
    m_candidateWindow.show(should_show ? TRUE : FALSE);
}

void text_service::UpdateCandidateUIElement(ITfContext *pContext)
{
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
        if (FAILED(hrBegin))
        {
            pUIMgr->Release();
            return;
        }
        else
        {
            m_hostWantsCandidateWindow = (bShow != FALSE);
        }
    }
    else if (candidate_count <= 0)
    {
        const HRESULT hrUpdateEmpty = pUIMgr->UpdateUIElement(m_candidateUIElementId);
        (void)hrUpdateEmpty;
        pUIMgr->Release();
        return;
    }

    const HRESULT hrUpdate = pUIMgr->UpdateUIElement(m_candidateUIElementId);
    (void)hrUpdate;
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
        (void)hrEnd;
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

    m_candidateWindow.create(owner);
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

void text_service::UpdateCandidateWindowPosition(ITfContext* pContext)
{
    if (!pContext)
        return;

    // 为了遵循 TSF 规范，在 EditSession 中通过 ITfContextView::GetTextExt
    // 获取插入符号的屏幕坐标，然后再移动候选窗口。
    class CandidatePosEditSession : public ITfEditSession
    {
    public:
        CandidatePosEditSession(text_service* service, ITfContext* context)
            : _ref(1), _service(service), _context(context)
        {
            if (_context)
                _context->AddRef();
        }

        virtual ~CandidatePosEditSession()
        {
            if (_context)
                _context->Release();
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
            if (!_service || !_context)
                return E_FAIL;

            ITfContextView* pView = nullptr;
            bool positioned = false;

            const auto fallback_position = [&]() -> bool
            {
                RECT fallback = {};
                if (try_get_caret_fallback_rect(&fallback))
                {
                    _service->RememberCandidateAnchorRect(fallback);
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (_service->TryGetCachedCandidateAnchorRect(&fallback))
                {
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_focus_window_fallback_rect(&fallback))
                {
                    _service->RememberCandidateAnchorRect(fallback);
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_view_fallback_rect(pView, &fallback))
                {
                    _service->RememberCandidateAnchorRect(fallback);
                    _service->UpdateCandidateWindowPositionFromRect(fallback);
                    return true;
                }
                if (try_get_owner_window_fallback_rect(&fallback))
                {
                    _service->RememberCandidateAnchorRect(fallback);
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
                if (SUCCEEDED(hrExt) && is_valid_text_ext_rect(rc, fClipped))
                {
                    _service->RememberCandidateAnchorRect(rc);
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
                fallback_position();
            }
            return S_OK;
        }

    private:
        LONG _ref;
        text_service* _service;
        ITfContext* _context;
    };

    CandidatePosEditSession* pEditSession = new CandidatePosEditSession(this, pContext);
    if (pEditSession)
    {
        HRESULT hr = S_OK;
        const HRESULT hrRequest = pContext->RequestEditSession(m_tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READ, &hr);
        if (FAILED(hrRequest) || FAILED(hr))
            pContext->RequestEditSession(m_tfClientId, pEditSession, TF_ES_ASYNC | TF_ES_READ, &hr);
        pEditSession->Release();
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
}

bool text_service::TryGetCachedCandidateAnchorRect(RECT* rc_out) const
{
    if (!rc_out || !m_hasLastCandidateAnchorRect)
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
    std::filesystem::path dll_dir = tool::get_current_dll_path();
    return dll_dir / CONFIG_FILE;
}

void text_service::LoadRuntimeConfig()
{
    const std::filesystem::path cfg = GetConfigPath();
    const wchar_t* path = cfg.c_str();

    m_bFullWidth = GetPrivateProfileIntW(L"state", L"full_width", m_bFullWidth ? 1 : 0, path) != 0;
    m_bChinesePunctuation = GetPrivateProfileIntW(L"state", L"chinese_punctuation", m_bChinesePunctuation ? 1 : 0, path) != 0;
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

void text_service::SaveRuntimeConfig() const
{
    const std::filesystem::path cfg = GetConfigPath();
    std::error_code ec;
    if (!cfg.parent_path().empty())
    {
        std::filesystem::create_directories(cfg.parent_path(), ec);
    }

    const wchar_t* path = cfg.c_str();
    WritePrivateProfileStringW(L"state", L"full_width", m_bFullWidth ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"state", L"chinese_punctuation", m_bChinesePunctuation ? L"1" : L"0", path);
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

void text_service::UpdateStatusWindow()
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
    m_dictionary.set_show_uncommon_candidates(m_showUncommonCandidates);
    m_dictionary.set_candidate_sort_mode(m_candidateSortMode);
}

void text_service::ShowStatusWindow()
{
    if (!ShouldShowStatusWindow())
    {
        if (m_statusWindow.get_hwnd())
            m_statusWindow.show(false);
        return;
    }

    // 在全屏前台窗口（典型游戏）下，不主动显示状态窗，避免打断全屏。
    if (is_likely_fullscreen_foreground_window())
    {
        if (m_statusWindow.get_hwnd())
            m_statusWindow.show(false);
        return;
    }

    const HWND owner = get_foreground_ui_owner_window();

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
    bool need_save = false;
    bool need_refresh_candidates = false;
    bool avoid_reposition_after_refresh = false;

    // 根据状态类型更新输入法状态
    switch (status_type)
    {
    case status_window::STATUS_FULL_WIDTH:
        m_bFullWidth = m_statusWindow.is_full_width();
        need_save = true;
        break;
    case status_window::STATUS_CHINESE_MODE:
    {
        const bool next_mode = m_statusWindow.is_chinese_mode();
        if (!next_mode)
        {
            CommitCompositionCodeAndClear(nullptr);
        }
        m_bChineseMode = m_statusWindow.is_chinese_mode();
        SyncPunctuationModeWithLanguageMode();
        
        // 同步标点状态到状态窗口
        UpdateStatusWindow();
        need_save = true;
        break;
    }
    case status_window::STATUS_PUNCTUATION:
        m_bChinesePunctuation = m_statusWindow.is_chinese_punctuation();
        need_save = true;
        break;
    case status_window::STATUS_SETTINGS:
        ShowCreateWordWindow();
        break;
    case status_window::STATUS_AUTO_COMMIT_FOUR_UNIQUE:
        m_autoCommitFourCodeUnique = m_statusWindow.is_auto_commit_four_code_unique();
        need_save = true;
        break;
    case status_window::STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE:
        m_commitFirstCandidateOnFifthCode = m_statusWindow.is_commit_first_candidate_on_fifth_code();
        need_save = true;
        break;
    case status_window::STATUS_SHOW_UNCOMMON_CANDIDATES:
        m_showUncommonCandidates = m_statusWindow.is_show_uncommon_candidates();
        m_dictionary.set_show_uncommon_candidates(m_showUncommonCandidates);
        need_save = true;
        need_refresh_candidates = true;
        avoid_reposition_after_refresh = true;
        break;
    case status_window::STATUS_REPLACE_DOT_AFTER_DIGIT:
        m_replaceDotAfterDigit = m_statusWindow.is_replace_dot_after_digit();
        need_save = true;
        break;
    case status_window::STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE:
        m_useEnglishPunctuationInChineseMode = m_statusWindow.is_use_english_punctuation_in_chinese_mode();
        if (m_bChineseMode)
        {
            SyncPunctuationModeWithLanguageMode();
            UpdateStatusWindow();
        }
        need_save = true;
        break;
    case status_window::STATUS_DISABLE_CHINESE_DASH:
        m_disableChineseDash = m_statusWindow.is_disable_chinese_dash();
        need_save = true;
        break;
    case status_window::STATUS_CANDIDATE_SORT_MODE_CHANGED:
        m_candidateSortMode = from_status_sort_mode(m_statusWindow.get_candidate_sort_mode());
        m_dictionary.set_candidate_sort_mode(m_candidateSortMode);
        need_save = true;
        need_refresh_candidates = true;
        avoid_reposition_after_refresh = true;
        break;
    case status_window::STATUS_EXPORT_RAW_DICT:
        ExportRawDictionary();
        break;
    case status_window::STATUS_UI_FONT_CHANGED:
        m_uiFontPercent = m_statusWindow.get_ui_font_percent();
        m_statusWindow.set_ui_font_percent(m_uiFontPercent);
        m_candidateWindow.set_ui_font_percent(min(250, m_uiFontPercent + CANDIDATE_FONT_BOOST_PERCENT));
        if (m_hCreateWordWnd && IsWindow(m_hCreateWordWnd))
        {
            PostMessageW(m_hCreateWordWnd, WM_CREATE_WORD_APPLY_LAYOUT, 0, 0);
        }
        need_save = true;
        need_refresh_candidates = true;
        break;
    default:
        break;
    }

    if (need_refresh_candidates && m_bInComposition && !m_compositionText.empty())
    {
        EnsureCandidateWindow();
        std::vector<std::wstring> candidates;
        std::vector<std::wstring> view_texts;
        if (m_dictionary.get_candidates(m_compositionText, candidates, view_texts))
        {
            m_candidateWindow.set_composition_text(m_compositionText);
            m_candidateWindow.set_candidates(candidates, view_texts);
            ITfDocumentMgr* pDocMgrFocus = nullptr;
            ITfContext* pContext = nullptr;
            if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
            {
                pDocMgrFocus->GetTop(&pContext);
                pDocMgrFocus->Release();
            }
            if (pContext)
            {
                if (!avoid_reposition_after_refresh)
                    UpdateCandidateWindowPosition(pContext);
                pContext->Release();
            }
            ApplyCandidateWindowVisibility();
        }
        else
        {
            m_candidateWindow.set_composition_text(m_compositionText);
            m_candidateWindow.set_candidates(std::vector<std::wstring>());
            HideCandidates();
        }
    }

    if (need_save)
        SaveRuntimeConfig();
}

void text_service::OnStatusWindowMoved(int x, int y)
{
    m_statusWindowPositionCustomized = true;
    m_statusWindowPosX = x;
    m_statusWindowPosY = y;
    SaveRuntimeConfig();
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

    std::string code;
    code.reserve(code_ws.size());
    for (wchar_t ch : code_ws)
    {
        code.push_back(static_cast<char>(ch));
    }

    std::string error;
    if (!m_dictionary.add_custom_word(word, code, error))
    {
        std::wstring werr(error.begin(), error.end());
        MessageBoxW(hWnd, (L"写入词库失败:\n" + werr).c_str(), L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    MessageBoxW(hWnd, L"造词成功，已写入用户词库并即时生效。", L"提示", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(hWnd);
}

void text_service::ExportRawDictionary()
{
    wchar_t file_path[MAX_PATH] = {};
    HWND owner = nullptr;
    if (m_hCreateWordWnd && IsWindow(m_hCreateWordWnd))
        owner = m_hCreateWordWnd;
    if (!owner)
        owner = get_foreground_ui_owner_window();
    if (!owner)
        owner = m_statusWindow.get_hwnd();

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Dictionary Files (*.dic)\0*.dic\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"dic";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn))
        return;

    std::string error;
    bool exported = false;
    try
    {
        exported = m_dictionary.export_raw_dictionary(file_path, error);
    }
    catch (const std::exception& ex)
    {
        error = "Unhandled exception while exporting dictionary: ";
        error += ex.what();
    }
    catch (...)
    {
        error = "Unhandled non-standard exception while exporting dictionary";
    }

    if (!exported)
    {
        std::wstring werr(error.begin(), error.end());
        MessageBoxW(owner,
                    (L"导出失败:\n" + werr).c_str(),
                    L"错误",
                    MB_OK | MB_ICONERROR);
        return;
    }

    MessageBoxW(owner,
                L"导出完成（.dic + .idx）。",
                L"提示",
                MB_OK | MB_ICONINFORMATION);
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
    m_dictionary.record_candidate_selected(effective_code, candidate_text);
}

void text_service::OnCandidateContextMenu(int candidate_index, POINT screen_point)
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
    const std::wstring effective_code = ResolveCandidateCodeForContextMenu(code_snapshot, candidate, display_candidate);

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

    std::string error;
    if (cmd == IDC_CAND_MENU_DELETE)
    {
        if (!m_dictionary.delete_candidate(effective_code, candidate, error))
        {
            const std::wstring werr(error.begin(), error.end());
            MessageBoxW(owner, (L"删除失败:\n" + werr).c_str(), L"错误", MB_OK | MB_ICONERROR);
            return;
        }
    }
    else if (cmd == IDC_CAND_MENU_MARK_UNCOMMON)
    {
        if (!m_dictionary.mark_candidate_uncommon(effective_code, candidate, error))
        {
            const std::wstring werr(error.begin(), error.end());
            MessageBoxW(owner, (L"标记失败:\n" + werr).c_str(), L"错误", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // 删除后刷新当前候选列表
    std::vector<std::wstring> candidates;
    std::vector<std::wstring> view_texts;
    if (!m_dictionary.get_candidates(code_snapshot, candidates, view_texts))
    {
        ClearComposition();
        HideCandidates();
        return;
    }
    m_compositionText = code_snapshot;
    m_bInComposition = TRUE;
    m_candidateWindow.set_composition_text(m_compositionText);
    m_candidateWindow.set_candidates(candidates, view_texts);
    ApplyCandidateWindowVisibility();
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

bool text_service::RebuildDictionaryIndex(std::wstring& error_msg)
{
    std::filesystem::path dir = tool::get_current_dll_path();
    std::vector<std::filesystem::path> compiler_candidates = {
        dir / "dict_compiler.exe",
        dir.parent_path() / "dic" / "Release" / "dict_compiler.exe",
        dir.parent_path() / "Release" / "dict_compiler.exe"
    };
    std::filesystem::path compiler;
    for (const auto& p : compiler_candidates)
    {
        if (std::filesystem::exists(p))
        {
            compiler = p;
            break;
        }
    }
    std::filesystem::path dic = dir / "dict.dic";
    std::filesystem::path idx = dir / "dict.idx";

    if (compiler.empty())
    {
        error_msg = L"未找到 dict_compiler.exe（已检查 DLL 目录及常见构建目录）";
        return false;
    }
    if (!std::filesystem::exists(dic))
    {
        error_msg = L"未找到 dict.dic";
        return false;
    }

    std::wstring cmd = L"\"" + compiler.wstring() + L"\" \"" + dic.wstring() + L"\" \"" + idx.wstring() + L"\"";

    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);

    std::wstring work_dir = dir.wstring();
    BOOL ok = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        work_dir.c_str(),
        &si,
        &pi);

    if (!ok)
    {
        error_msg = L"无法启动 dict_compiler.exe";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0)
    {
        error_msg = L"dict_compiler.exe 返回非零退出码";
        return false;
    }

    return true;
}

