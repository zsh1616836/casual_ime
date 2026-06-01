#pragma once

#include "ime_config.h"
#include <msctf.h>
#include <ctffunc.h>
#include <functional>
#include <string>
#include <vector>

class candidate_ui_element : public ITfCandidateListUIElementBehavior,
                             public ITfIntegratableCandidateListUIElement
{
public:
    candidate_ui_element();
    virtual ~candidate_ui_element();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetDescription(BSTR *pbstrDescription) override;
    STDMETHODIMP GetGUID(GUID *pguid) override;
    STDMETHODIMP Show(BOOL bShow) override;
    STDMETHODIMP IsShown(BOOL *pbShow) override;

    STDMETHODIMP GetUpdatedFlags(DWORD *pdwFlags) override;
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **ppdim) override;
    STDMETHODIMP GetCount(UINT *puCount) override;
    STDMETHODIMP GetSelection(UINT *puIndex) override;
    STDMETHODIMP GetString(UINT uIndex, BSTR *pstr) override;
    STDMETHODIMP GetPageIndex(UINT *pIndex, UINT uSize, UINT *puPageCnt) override;
    STDMETHODIMP SetPageIndex(UINT *pIndex, UINT uPageCnt) override;
    STDMETHODIMP GetCurrentPage(UINT *puPage) override;
    STDMETHODIMP SetSelection(UINT nIndex) override;
    STDMETHODIMP Finalize() override;
    STDMETHODIMP Abort() override;

    STDMETHODIMP SetIntegrationStyle(GUID guidIntegrationStyle) override;
    STDMETHODIMP GetSelectionStyle(TfIntegratableCandidateListSelectionStyle *ptfSelectionStyle) override;
    STDMETHODIMP OnKeyDown(WPARAM wParam, LPARAM lParam, BOOL *pfEaten) override;
    STDMETHODIMP ShowCandidateNumbers(BOOL *pfShow) override;
    STDMETHODIMP FinalizeExactCompositionString() override;

    void set_show_callback(std::function<void(BOOL)> callback);
    void set_selection_callback(std::function<void(UINT)> callback);
    void set_finalize_callback(std::function<void()> callback);
    void set_abort_callback(std::function<void()> callback);
    void set_key_down_callback(std::function<bool(WPARAM, LPARAM)> callback);
    void set_finalize_exact_callback(std::function<void()> callback);
    void update_state(const std::vector<std::wstring> &candidates,
                      const std::vector<UINT> &page_index,
                      UINT current_page,
                      UINT selection,
                      ITfDocumentMgr *pDocMgr,
                      DWORD updated_flags);

private:
    LONG ref_;
    BOOL shown_;
    DWORD updated_flags_;
    std::vector<std::wstring> candidates_;
    std::vector<UINT> page_index_;
    UINT current_page_;
    UINT selection_;
    ITfDocumentMgr *document_mgr_;
    GUID integration_style_;
    std::function<void(BOOL)> show_callback_;
    std::function<void(UINT)> selection_callback_;
    std::function<void()> finalize_callback_;
    std::function<void()> abort_callback_;
    std::function<bool(WPARAM, LPARAM)> key_down_callback_;
    std::function<void()> finalize_exact_callback_;
};
