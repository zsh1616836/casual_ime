#pragma once

#include "ime_config.h"

#include <ctffunc.h>
#include <ctfutb.h>
#include <functional>

class language_bar_item final : public ITfLangBarItemButton,
                                public ITfSource
{
public:
    language_bar_item(std::function<void()> toggle_callback,
                      std::function<bool()> chinese_mode_callback);
    ~language_bar_item();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override;
    STDMETHODIMP GetStatus(DWORD* pdwStatus) override;
    STDMETHODIMP Show(BOOL fShow) override;
    STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override;

    STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) override;
    STDMETHODIMP InitMenu(ITfMenu* pMenu) override;
    STDMETHODIMP OnMenuSelect(UINT wID) override;
    STDMETHODIMP GetIcon(HICON* phIcon) override;
    STDMETHODIMP GetText(BSTR* pbstrText) override;

    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
    STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

    HRESULT AddToThreadManager(ITfThreadMgr* thread_manager);
    void RemoveFromThreadManager(ITfThreadMgr* thread_manager);
    void NotifyModeChanged();
    void SetDisabled(bool disabled);

private:
    static constexpr DWORD kSinkCookie = 1;

    LONG ref_;
    TF_LANGBARITEMINFO info_;
    ITfLangBarItemSink* sink_;
    DWORD status_;
    bool added_;
    std::function<void()> toggle_callback_;
    std::function<bool()> chinese_mode_callback_;
};
