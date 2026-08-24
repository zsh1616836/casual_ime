#include "language_bar_item.h"

#include "ime_trace.h"

#include <olectl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace
{
HICON create_mode_icon(bool chinese_mode)
{
    constexpr int size = 16;
    BITMAPINFO bitmap_info = {};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = size;
    bitmap_info.bmiHeader.biHeight = -size;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !pixels)
    {
        if (color)
            DeleteObject(color);
        return nullptr;
    }

    auto* rgba = static_cast<DWORD*>(pixels);
    std::fill(rgba, rgba + size * size, 0xffffffffu);

    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bitmap = SelectObject(dc, color);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HFONT font = CreateFontW(chinese_mode ? -13 : -15,
                            0,
                            0,
                            0,
                            FW_BOLD,
                            FALSE,
                            FALSE,
                            FALSE,
                            DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY,
                            DEFAULT_PITCH,
                            L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    RECT rect = {0, 0, size, size};
    const wchar_t* glyph = chinese_mode ? L"\u4e2d" : L"A";
    DrawTextW(dc, glyph, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(font);
    DeleteDC(dc);

    for (int i = 0; i < size * size; ++i)
        rgba[i] |= 0xff000000u;

    std::array<BYTE, size * size / 8> mask_bits = {};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
    if (!mask)
    {
        DeleteObject(color);
        return nullptr;
    }

    ICONINFO icon_info = {};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}
}

language_bar_item::language_bar_item(std::function<void()> toggle_callback,
                                     std::function<bool()> chinese_mode_callback)
    : ref_(1),
      sink_(nullptr),
      status_(0),
      added_(false),
      toggle_callback_(std::move(toggle_callback)),
      chinese_mode_callback_(std::move(chinese_mode_callback))
{
    ZeroMemory(&info_, sizeof(info_));
    info_.clsidService = c_clsidTextService;
    info_.guidItem = GUID_LBI_INPUTMODE;
    info_.dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    info_.ulSort = 0;
    wcsncpy_s(info_.szDescription, IME_DESCRIPTION, _TRUNCATE);
}

language_bar_item::~language_bar_item()
{
    if (sink_)
        sink_->Release();
}

STDMETHODIMP language_bar_item::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfLangBarItem) ||
        IsEqualIID(riid, IID_ITfLangBarItemButton))
    {
        *ppvObj = static_cast<ITfLangBarItemButton*>(this);
    }
    else if (IsEqualIID(riid, IID_ITfSource))
    {
        *ppvObj = static_cast<ITfSource*>(this);
    }

    if (!*ppvObj)
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) language_bar_item::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) language_bar_item::Release()
{
    const LONG ref = InterlockedDecrement(&ref_);
    if (ref == 0)
        delete this;
    return static_cast<ULONG>(ref);
}

STDMETHODIMP language_bar_item::GetInfo(TF_LANGBARITEMINFO* pInfo)
{
    if (!pInfo)
        return E_INVALIDARG;
    *pInfo = info_;
    return S_OK;
}

STDMETHODIMP language_bar_item::GetStatus(DWORD* pdwStatus)
{
    if (!pdwStatus)
        return E_INVALIDARG;
    *pdwStatus = status_;
    return S_OK;
}

STDMETHODIMP language_bar_item::Show(BOOL fShow)
{
    const DWORD old_status = status_;
    if (fShow)
        status_ &= ~TF_LBI_STATUS_HIDDEN;
    else
        status_ |= TF_LBI_STATUS_HIDDEN;
    if (old_status != status_ && sink_)
        sink_->OnUpdate(TF_LBI_STATUS);
    return S_OK;
}

STDMETHODIMP language_bar_item::GetTooltipString(BSTR* pbstrToolTip)
{
    if (!pbstrToolTip)
        return E_INVALIDARG;
    const bool chinese = chinese_mode_callback_ && chinese_mode_callback_();
    *pbstrToolTip = SysAllocString(chinese
        ? L"\u968f\u610f\u4e94\u7b14\uff1a\u4e2d\u6587\u6a21\u5f0f"
        : L"\u968f\u610f\u4e94\u7b14\uff1a\u82f1\u6587\u6a21\u5f0f");
    return *pbstrToolTip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP language_bar_item::OnClick(TfLBIClick click, POINT pt, const RECT* prcArea)
{
    UNREFERENCED_PARAMETER(pt);
    UNREFERENCED_PARAMETER(prcArea);
    if (click == TF_LBI_CLK_LEFT && toggle_callback_ && !(status_ & TF_LBI_STATUS_DISABLED))
    {
        toggle_callback_();
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP language_bar_item::InitMenu(ITfMenu* pMenu)
{
    UNREFERENCED_PARAMETER(pMenu);
    return S_OK;
}

STDMETHODIMP language_bar_item::OnMenuSelect(UINT wID)
{
    UNREFERENCED_PARAMETER(wID);
    return E_NOTIMPL;
}

STDMETHODIMP language_bar_item::GetIcon(HICON* phIcon)
{
    if (!phIcon)
        return E_INVALIDARG;
    const bool chinese = chinese_mode_callback_ && chinese_mode_callback_();
    *phIcon = create_mode_icon(chinese);
    return *phIcon ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP language_bar_item::GetText(BSTR* pbstrText)
{
    if (!pbstrText)
        return E_INVALIDARG;
    const bool chinese = chinese_mode_callback_ && chinese_mode_callback_();
    *pbstrText = SysAllocString(chinese ? L"\u4e2d" : L"\u82f1");
    return *pbstrText ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP language_bar_item::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie)
{
    if (!pdwCookie || !punk)
        return E_INVALIDARG;
    if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
        return CONNECT_E_CANNOTCONNECT;
    if (sink_)
        return CONNECT_E_ADVISELIMIT;

    const HRESULT hr = punk->QueryInterface(IID_ITfLangBarItemSink, reinterpret_cast<void**>(&sink_));
    if (FAILED(hr))
    {
        sink_ = nullptr;
        return hr;
    }
    *pdwCookie = kSinkCookie;
    return S_OK;
}

STDMETHODIMP language_bar_item::UnadviseSink(DWORD dwCookie)
{
    if (dwCookie != kSinkCookie || !sink_)
        return CONNECT_E_NOCONNECTION;
    sink_->Release();
    sink_ = nullptr;
    return S_OK;
}

HRESULT language_bar_item::AddToThreadManager(ITfThreadMgr* thread_manager)
{
    if (!thread_manager)
        return E_INVALIDARG;
    if (added_)
        return S_OK;

    ITfLangBarItemMgr* manager = nullptr;
    const HRESULT query_hr = thread_manager->QueryInterface(IID_ITfLangBarItemMgr,
                                                             reinterpret_cast<void**>(&manager));
    if (FAILED(query_hr))
        return query_hr;
    const HRESULT hr = manager->AddItem(this);
    manager->Release();
    if (SUCCEEDED(hr))
        added_ = true;
    ime_tracef(L"LangBarAdd", L"hr=0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

void language_bar_item::RemoveFromThreadManager(ITfThreadMgr* thread_manager)
{
    if (!added_ || !thread_manager)
        return;
    ITfLangBarItemMgr* manager = nullptr;
    if (SUCCEEDED(thread_manager->QueryInterface(IID_ITfLangBarItemMgr,
                                                 reinterpret_cast<void**>(&manager))))
    {
        const HRESULT hr = manager->RemoveItem(this);
        ime_tracef(L"LangBarRemove", L"hr=0x%08lx", static_cast<unsigned long>(hr));
        manager->Release();
    }
    added_ = false;
}

void language_bar_item::NotifyModeChanged()
{
    if (sink_)
        sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP | TF_LBI_STATUS);
}

void language_bar_item::SetDisabled(bool disabled)
{
    const DWORD old_status = status_;
    if (disabled)
        status_ |= TF_LBI_STATUS_DISABLED;
    else
        status_ &= ~TF_LBI_STATUS_DISABLED;
    if (old_status != status_ && sink_)
        sink_->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
}
