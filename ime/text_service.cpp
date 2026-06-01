#include "text_service.h"
#include <olectl.h>
#include <map>
#include <Windows.h>
#include <filesystem>
#include <algorithm>
#include <commdlg.h>
#include <string>

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

class composition_edit_session : public ITfEditSession
{
public:
    enum class op_type
    {
        update,
        commit,
        clear
    };

    composition_edit_session(text_service* service, ITfContext* context, std::wstring text, op_type op)
        : ref_(1), service_(service), context_(context), text_(std::move(text)), op_(op)
    {
        if (context_)
            context_->AddRef();
        if (service_)
            service_->AddRef();
    }

    virtual ~composition_edit_session()
    {
        if (context_)
            context_->Release();
        if (service_)
            service_->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppvObj) override
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
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG ref = InterlockedDecrement(&ref_);
        if (ref == 0)
            delete this;
        return static_cast<ULONG>(ref);
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        if (!service_ || !context_)
            return E_FAIL;

        switch (op_)
        {
        case op_type::update:
            return DoUpdate(ec);
        case op_type::commit:
            return DoCommit(ec);
        case op_type::clear:
            return DoClear(ec);
        default:
            return E_FAIL;
        }
    }

private:
    HRESULT EnsureComposition(TfEditCookie ec)
    {
        if (service_->m_pComposition)
            return S_OK;

        // 某些宿主（如部分终端）没有可用的 TSF 文本视图，直接启动 composition
        // 会触发系统原生悬浮组合窗（常见在左上角）。这里先探测可布局能力，
        // 不满足时退化为“仅内部组合 + 提交时直接插入”。
        ITfContextView* pView = nullptr;
        HRESULT hr = context_->GetActiveView(&pView);
        if (FAILED(hr) || !pView)
            return FAILED(hr) ? hr : E_FAIL;

        ITfContextComposition* pContextComp = nullptr;
        if (FAILED(context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&pContextComp))))
        {
            pView->Release();
            return E_FAIL;
        }

        ITfInsertAtSelection* pInsert = nullptr;
        ITfRange* pRange = nullptr;
        hr = context_->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&pInsert));
        if (SUCCEEDED(hr))
        {
            hr = pInsert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &pRange);
        }

        if (SUCCEEDED(hr) && pRange)
        {
            RECT rc = { 0 };
            BOOL fClipped = FALSE;
            const HRESULT hrExt = pView->GetTextExt(ec, pRange, &rc, &fClipped);
            if (SUCCEEDED(hrExt) && is_valid_text_ext_rect(rc, fClipped))
                hr = pContextComp->StartComposition(ec, pRange, service_, &service_->m_pComposition);
            else
                hr = E_FAIL;
        }

        if (pRange)
            pRange->Release();
        if (pInsert)
            pInsert->Release();
        pContextComp->Release();
        pView->Release();

        return hr;
    }

    HRESULT SetSelectionToRangeEnd(TfEditCookie ec, ITfRange* range)
    {
        if (!range)
            return E_INVALIDARG;
        ITfRange* pCaretRange = nullptr;
        HRESULT hr = range->Clone(&pCaretRange);
        if (FAILED(hr) || !pCaretRange)
            return FAILED(hr) ? hr : E_FAIL;

        hr = pCaretRange->Collapse(ec, TF_ANCHOR_END);
        if (FAILED(hr))
        {
            pCaretRange->Release();
            return hr;
        }

        TF_SELECTION selection = {};
        selection.range = pCaretRange;
        selection.style.ase = TF_AE_NONE;
        selection.style.fInterimChar = FALSE;
        hr = context_->SetSelection(ec, 1, &selection);
        pCaretRange->Release();
        return hr;
    }

    HRESULT DoUpdate(TfEditCookie ec)
    {
        HRESULT hr = EnsureComposition(ec);
        if (FAILED(hr) || !service_->m_pComposition)
            return hr;

        ITfRange* pRange = nullptr;
        hr = service_->m_pComposition->GetRange(&pRange);
        if (SUCCEEDED(hr) && pRange)
        {
            hr = pRange->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.length()));
            if (SUCCEEDED(hr))
            {
                SetSelectionToRangeEnd(ec, pRange);
            }
            pRange->Release();
        }
        return hr;
    }

    HRESULT DoCommit(TfEditCookie ec)
    {
        if (service_->m_pComposition)
        {
            ITfRange* pRange = nullptr;
            HRESULT hr = service_->m_pComposition->GetRange(&pRange);
            if (SUCCEEDED(hr) && pRange)
            {
                hr = pRange->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.length()));
                if (SUCCEEDED(hr))
                {
                    SetSelectionToRangeEnd(ec, pRange);
                }
                pRange->Release();
            }
            service_->m_pComposition->EndComposition(ec);
            service_->m_pComposition->Release();
            service_->m_pComposition = nullptr;
            return hr;
        }

        ITfInsertAtSelection* pInsertAtSelection = nullptr;
        ITfRange* pRange = nullptr;
        HRESULT hr = context_->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&pInsertAtSelection));
        if (SUCCEEDED(hr))
        {
            hr = pInsertAtSelection->InsertTextAtSelection(ec,
                                                           0,
                                                           text_.c_str(),
                                                           static_cast<LONG>(text_.length()),
                                                           &pRange);
            if (SUCCEEDED(hr) && pRange)
            {
                SetSelectionToRangeEnd(ec, pRange);
                pRange->Release();
            }
            pInsertAtSelection->Release();
        }
        return hr;
    }

    HRESULT DoClear(TfEditCookie ec)
    {
        if (!service_->m_pComposition)
            return S_OK;

        ITfRange* pRange = nullptr;
        HRESULT hr = service_->m_pComposition->GetRange(&pRange);
        if (SUCCEEDED(hr) && pRange)
        {
            hr = pRange->SetText(ec, 0, L"", 0);
            if (SUCCEEDED(hr))
            {
                SetSelectionToRangeEnd(ec, pRange);
            }
            pRange->Release();
        }
        service_->m_pComposition->EndComposition(ec);
        service_->m_pComposition->Release();
        service_->m_pComposition = nullptr;
        return hr;
    }

    LONG ref_;
    text_service* service_;
    ITfContext* context_;
    std::wstring text_;
    op_type op_;
};

text_service::text_service()
{
    dll_add_ref();
    
    m_cRef = 1;
    m_pThreadMgr = NULL;
    m_tfClientId = TF_CLIENTID_NULL;
    m_dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    m_dwKeyEventSinkCookie = TF_INVALID_COOKIE;
    m_dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
    m_pTextLayoutSinkContext = nullptr;
    m_candidateUIElement = nullptr;
    m_candidateUIElementId = TF_INVALID_UIELEMENTID;
    m_hostWantsCandidateWindow = true;
    m_uiElementOnlyMode = false;
    m_immersiveMode = false;
    SetRectEmpty(&m_lastCandidateAnchorRect);
    m_hasLastCandidateAnchorRect = false;
    m_lastCandidateAnchorTick = 0;
    m_lastCandidateAnchorOwner = nullptr;
    m_hCreateWordWnd = NULL;
    m_bInComposition = FALSE;
    m_pComposition = nullptr;
    m_bChineseMode = TRUE; // 默认中文模式
    m_bFullWidth = FALSE;  // 默认半角
    m_bChinesePunctuation = FALSE;  // 默认英文标点
    m_autoCommitFourCodeUnique = true; // 默认开启：四码唯一时直接上屏
    m_commitFirstCandidateOnFifthCode = true; // 默认开启：第5码时上屏首个候选词
    m_showUncommonCandidates = false; // 默认不显示不常用词
    m_replaceDotAfterDigit = true; // 默认开启：数字后"。"替换为"."
    m_useEnglishPunctuationInChineseMode = true; // 默认开启
    m_disableChineseDash = true; // 默认开启
    m_candidateSortMode = ime_dict::candidate_sort_mode::frequency; // 默认按词频排序
    m_uiFontPercent = 100; // 状态栏默认 100%
    m_statusWindowPositionCustomized = false;
    m_statusWindowPosX = 0;
    m_statusWindowPosY = 0;
    LoadRuntimeConfig();
    SyncPunctuationModeWithLanguageMode();
    
    // Shift键状态初始化
    m_bShiftPressed = FALSE;
    m_bOtherKeyPressed = FALSE;
    m_bShiftPressedWithModifier = false;
    m_lastInputWasDigit = false;
    m_inCandidateContextMenu = false;
    m_inStatusMenuPopup = false;
    m_statusWindowHideSuppressedUntilTick = 0;
    
    // 初始化词库
    m_dictionary_ready = m_dictionary.init();
    if (!m_dictionary_ready)
    {
        OutputDebugStringA("ERROR: dictionary init failed, IME activation will fail\n");
    }
    m_dictionary.set_show_uncommon_candidates(m_showUncommonCandidates);
    m_dictionary.set_candidate_sort_mode(m_candidateSortMode);
}

text_service::~text_service()
{
    dll_release();
}

STDAPI text_service::QueryInterface(REFIID riid, void **ppvObj)
{
    if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor))
    {
        *ppvObj = (ITfTextInputProcessor *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    {
        *ppvObj = (ITfTextInputProcessorEx *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    {
        *ppvObj = (ITfThreadMgrEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
    {
        *ppvObj = (ITfCompositionSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    {
        *ppvObj = static_cast<ITfTextLayoutSink *>(this);
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDAPI_(ULONG) text_service::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
}

STDAPI_(ULONG) text_service::Release()
{
    const LONG cr = InterlockedDecrement(&m_cRef);
    if (cr == 0)
    {
        delete this;
    }
    return static_cast<ULONG>(cr);
}

STDAPI text_service::Activate(ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    return ActivateInternal(pThreadMgr, tfClientId, 0);
}

STDAPI text_service::ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags)
{
    return ActivateInternal(pThreadMgr, tfClientId, dwFlags);
}

HRESULT text_service::ActivateInternal(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags)
{
    // 某些宿主（尤其系统壳层/沉浸式文本框）可能无法访问外部词库文件。
    // 不应因此导致 TIP 激活失败；缺词库时退化为“仅组合/直出”。
    if (!m_dictionary_ready)
    {
        m_dictionary_ready = m_dictionary.init();
        if (m_dictionary_ready)
        {
            m_dictionary.set_show_uncommon_candidates(m_showUncommonCandidates);
            m_dictionary.set_candidate_sort_mode(m_candidateSortMode);
        }
    }

    m_pThreadMgr = pThreadMgr;
    m_pThreadMgr->AddRef();
    m_tfClientId = tfClientId;
    m_hostWantsCandidateWindow = true;
    m_uiElementOnlyMode = (dwFlags & TF_TMAE_UIELEMENTENABLEDONLY) != 0;
    m_immersiveMode = (dwFlags & TF_TMF_IMMERSIVEMODE) != 0;
    reset_shift_track(m_bShiftPressed, m_bOtherKeyPressed);
    m_bShiftPressedWithModifier = false;
    SyncPunctuationModeWithLanguageMode();

    InitThreadMgrEventSink();
    RefreshTextLayoutSinkForCurrentFocus();

    // 始终尝试挂接按键 sink。部分系统文本框会返回失败，但仍可完成 profile 激活；
    // 这里不再因为挂接失败而中断 Activate，避免出现“无法切换到该输入法”。
    const bool keySinkReady = !!InitKeyEventSink();
    (void)keySinkReady;

    if (ShouldShowStatusWindow())
        ShowStatusWindow();

    return S_OK;
}

STDAPI text_service::Deactivate()
{
    UnadviseTextLayoutSink();

    // 清理输入状态和候选窗口
    CancelCompositionInContext(nullptr);
    ClearComposition();
    ClearCandidateAnchorRect();
    HideCandidates();
    EndCandidateUIElement();

    // 隐藏和销毁状态窗口
    m_statusWindow.show(false);
    m_statusWindow.destroy();

    if (m_hCreateWordWnd)
    {
        DestroyWindow(m_hCreateWordWnd);
        m_hCreateWordWnd = nullptr;
    }

    // 销毁候选窗口
    m_candidateWindow.destroy();

    if (m_candidateUIElement)
    {
        m_candidateUIElement->set_show_callback(nullptr);
        m_candidateUIElement->Release();
        m_candidateUIElement = nullptr;
    }
    m_candidateUIElementId = TF_INVALID_UIELEMENTID;
    
    UninitKeyEventSink();
    UninitThreadMgrEventSink();

    if (m_pThreadMgr)
    {
        m_pThreadMgr->Release();
        m_pThreadMgr = NULL;
    }

    m_tfClientId = TF_CLIENTID_NULL;

    return S_OK;
}

STDAPI text_service::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition *pComposition)
{
    if (pComposition && m_pComposition == pComposition)
    {
        m_pComposition->Release();
        m_pComposition = nullptr;
    }
    return S_OK;
}

BOOL text_service::InitThreadMgrEventSink()
{
    ITfSource *pSource = NULL;
    HRESULT hr = m_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource);
    
    if (SUCCEEDED(hr))
    {
        hr = pSource->AdviseSink(IID_ITfThreadMgrEventSink, 
                                 (ITfThreadMgrEventSink *)this, 
                                 &m_dwThreadMgrEventSinkCookie);
        pSource->Release();
    }

    return SUCCEEDED(hr);
}

void text_service::UninitThreadMgrEventSink()
{
    if (m_dwThreadMgrEventSinkCookie != TF_INVALID_COOKIE)
    {
        ITfSource *pSource = NULL;
        if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource)))
        {
            pSource->UnadviseSink(m_dwThreadMgrEventSinkCookie);
            pSource->Release();
        }
        m_dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    }
}

BOOL text_service::InitKeyEventSink()
{
    ITfKeystrokeMgr *pKeystrokeMgr = NULL;
    HRESULT hr = m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr);
    
    if (SUCCEEDED(hr))
    {
        hr = pKeystrokeMgr->AdviseKeyEventSink(m_tfClientId,
                                               (ITfKeyEventSink *)this,
                                               TRUE);
        if (FAILED(hr))
        {
            // 部分宿主不接受 foreground-only 订阅，回退到非 foreground 模式重试。
            hr = pKeystrokeMgr->AdviseKeyEventSink(m_tfClientId,
                                                   (ITfKeyEventSink*)this,
                                                   FALSE);
        }
        pKeystrokeMgr->Release();
    }

    return SUCCEEDED(hr);
}

void text_service::UninitKeyEventSink()
{
    ITfKeystrokeMgr *pKeystrokeMgr = NULL;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr)))
    {
        pKeystrokeMgr->UnadviseKeyEventSink(m_tfClientId);
        pKeystrokeMgr->Release();
    }
}

bool text_service::AdviseTextLayoutSink(ITfContext* pContext)
{
    if (!pContext)
        return false;

    if (m_pTextLayoutSinkContext == pContext &&
        m_dwTextLayoutSinkCookie != TF_INVALID_COOKIE)
    {
        return true;
    }

    UnadviseTextLayoutSink();

    ITfSource* pSource = nullptr;
    HRESULT hr = pContext->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource));
    if (FAILED(hr) || !pSource)
        return false;

    DWORD cookie = TF_INVALID_COOKIE;
    hr = pSource->AdviseSink(
        IID_ITfTextLayoutSink,
        static_cast<ITfTextLayoutSink*>(this),
        &cookie);
    pSource->Release();

    if (FAILED(hr))
        return false;

    m_pTextLayoutSinkContext = pContext;
    m_pTextLayoutSinkContext->AddRef();
    m_dwTextLayoutSinkCookie = cookie;
    return true;
}

void text_service::UnadviseTextLayoutSink()
{
    if (m_pTextLayoutSinkContext && m_dwTextLayoutSinkCookie != TF_INVALID_COOKIE)
    {
        ITfSource* pSource = nullptr;
        if (SUCCEEDED(m_pTextLayoutSinkContext->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))) && pSource)
        {
            pSource->UnadviseSink(m_dwTextLayoutSinkCookie);
            pSource->Release();
        }
    }

    m_dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
    if (m_pTextLayoutSinkContext)
    {
        m_pTextLayoutSinkContext->Release();
        m_pTextLayoutSinkContext = nullptr;
    }
}

void text_service::RefreshTextLayoutSink(ITfDocumentMgr* pDocMgrFocus)
{
    ITfContext* pContext = nullptr;
    if (pDocMgrFocus)
        pDocMgrFocus->GetTop(&pContext);

    if (!pContext)
    {
        UnadviseTextLayoutSink();
        return;
    }

    AdviseTextLayoutSink(pContext);
    pContext->Release();
}

void text_service::RefreshTextLayoutSinkForCurrentFocus()
{
    if (!m_pThreadMgr)
    {
        UnadviseTextLayoutSink();
        return;
    }

    ITfDocumentMgr* pDocMgrFocus = nullptr;
    if (FAILED(m_pThreadMgr->GetFocus(&pDocMgrFocus)) || !pDocMgrFocus)
    {
        UnadviseTextLayoutSink();
        return;
    }

    RefreshTextLayoutSink(pDocMgrFocus);
    pDocMgrFocus->Release();
}

STDAPI text_service::OnInitDocumentMgr(ITfDocumentMgr *pDocMgr)
{
    return S_OK;
}

STDAPI text_service::OnUninitDocumentMgr(ITfDocumentMgr *pDocMgr)
{
    return S_OK;
}

STDAPI text_service::OnSetFocus(ITfDocumentMgr *pDocMgrFocus, ITfDocumentMgr *pDocMgrPrevFocus)
{
    if (m_inCandidateContextMenu || m_inStatusMenuPopup)
    {
        return S_OK;
    }

    // 文档焦点变化：
    // - 失去文本焦点（pDocMgrFocus == nullptr）：清理输入状态并隐藏候选/状态窗口
    // - 获得文本焦点：确保状态窗口可见（候选框仍按组合状态决定是否显示）
    if (!pDocMgrFocus)
    {
        UnadviseTextLayoutSink();
        reset_shift_track(m_bShiftPressed, m_bOtherKeyPressed);
        m_bShiftPressedWithModifier = false;
        CancelCompositionInContext(nullptr);
        ClearComposition();
        ClearCandidateAnchorRect();
        HideCandidates();
        if (GetTickCount() >= m_statusWindowHideSuppressedUntilTick)
            m_statusWindow.show(false);
    }
    else
    {
        RefreshTextLayoutSink(pDocMgrFocus);
        // 每次切回本输入法时默认恢复中文模式（不持久化中英文状态）
        m_bChineseMode = TRUE;
        SyncPunctuationModeWithLanguageMode();
        reset_shift_track(m_bShiftPressed, m_bOtherKeyPressed);
        m_bShiftPressedWithModifier = false;
        UpdateStatusWindow();
        if (ShouldShowStatusWindow())
            ShowStatusWindow();
        m_statusWindowHideSuppressedUntilTick = 0;
    }

    return S_OK;
}

STDAPI text_service::OnPushContext(ITfContext *pContext)
{
    RefreshTextLayoutSinkForCurrentFocus();
    return S_OK;
}

STDAPI text_service::OnPopContext(ITfContext *pContext)
{
    RefreshTextLayoutSinkForCurrentFocus();
    return S_OK;
}

STDAPI text_service::OnSetFocus(BOOL fForeground)
{
    if (m_inCandidateContextMenu || m_inStatusMenuPopup)
    {
        return S_OK;
    }

    // 输入法获得/失去键盘焦点（例如切换到其他输入法）：
    // - 失去焦点：清理组合并隐藏候选/状态窗口
    // - 获得焦点：恢复状态窗口（候选框仍由组合状态控制）
    if (!fForeground)
    {
        UnadviseTextLayoutSink();
        reset_shift_track(m_bShiftPressed, m_bOtherKeyPressed);
        m_bShiftPressedWithModifier = false;
        CancelCompositionInContext(nullptr);
        ClearComposition();
        ClearCandidateAnchorRect();
        HideCandidates();
        if (GetTickCount() >= m_statusWindowHideSuppressedUntilTick)
            m_statusWindow.show(false);
    }
    else
    {
        RefreshTextLayoutSinkForCurrentFocus();
        // 切换到本输入法时，始终恢复为中文模式（含从其他输入法切换过来的场景）。
        m_bChineseMode = TRUE;
        SyncPunctuationModeWithLanguageMode();
        reset_shift_track(m_bShiftPressed, m_bOtherKeyPressed);
        m_bShiftPressedWithModifier = false;
        UpdateStatusWindow();
        if (ShouldShowStatusWindow())
            ShowStatusWindow();
        m_statusWindowHideSuppressedUntilTick = 0;
    }

    return S_OK;
}

STDAPI text_service::OnLayoutChange(ITfContext* pContext, TfLayoutCode lcode, ITfContextView* pView)
{
    UNREFERENCED_PARAMETER(pView);

    if (!pContext)
        return E_INVALIDARG;

    if (lcode != TF_LC_CHANGE)
        return S_OK;

    if (!m_bInComposition || m_compositionText.empty())
        return S_OK;

    if (!ShouldShowOwnCandidateWindow() || !m_candidateWindow.get_hwnd())
        return S_OK;

    UpdateCandidateWindowPosition(pContext);
    ApplyCandidateWindowVisibility();
    return S_OK;
}

void text_service::SyncPunctuationModeWithLanguageMode()
{
    m_bChinesePunctuation = m_bChineseMode ? !m_useEnglishPunctuationInChineseMode : FALSE;
}


