#include "candidate_ui_element.h"

#include <algorithm>

candidate_ui_element::candidate_ui_element()
    : ref_(1),
      shown_(TRUE),
      updated_flags_(0),
      current_page_(0),
      selection_(0),
      document_mgr_(nullptr),
      integration_style_(GUID_NULL)
{
}

candidate_ui_element::~candidate_ui_element()
{
    if (document_mgr_)
    {
        document_mgr_->Release();
        document_mgr_ = nullptr;
    }
}

STDMETHODIMP candidate_ui_element::QueryInterface(REFIID riid, void **ppvObj)
{
    if (ppvObj == nullptr)
        return E_INVALIDARG;

    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior))
    {
        *ppvObj = static_cast<ITfCandidateListUIElementBehavior *>(this);
        AddRef();
        return S_OK;
    }

    if (IsEqualIID(riid, IID_ITfIntegratableCandidateListUIElement))
    {
        *ppvObj = static_cast<ITfIntegratableCandidateListUIElement *>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) candidate_ui_element::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) candidate_ui_element::Release()
{
    const LONG ref = InterlockedDecrement(&ref_);
    if (ref == 0)
        delete this;
    return static_cast<ULONG>(ref);
}

STDMETHODIMP candidate_ui_element::GetDescription(BSTR *pbstrDescription)
{
    if (pbstrDescription == nullptr)
        return E_INVALIDARG;
    *pbstrDescription = SysAllocString(L"casual_ime candidate list");
    return (*pbstrDescription != nullptr) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP candidate_ui_element::GetGUID(GUID *pguid)
{
    if (pguid == nullptr)
        return E_INVALIDARG;
    *pguid = c_clsidTextService;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::Show(BOOL bShow)
{
    shown_ = bShow ? TRUE : FALSE;
    if (show_callback_)
    {
        try
        {
            show_callback_(shown_);
        }
        catch (...)
        {
            // Never let callback exceptions cross COM boundaries.
        }
    }
    return S_OK;
}

STDMETHODIMP candidate_ui_element::IsShown(BOOL *pbShow)
{
    if (pbShow == nullptr)
        return E_INVALIDARG;
    *pbShow = shown_;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetUpdatedFlags(DWORD *pdwFlags)
{
    if (pdwFlags == nullptr)
        return E_INVALIDARG;
    *pdwFlags = updated_flags_;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetDocumentMgr(ITfDocumentMgr **ppdim)
{
    if (ppdim == nullptr)
        return E_INVALIDARG;
    *ppdim = document_mgr_;
    if (*ppdim)
        (*ppdim)->AddRef();
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetCount(UINT *puCount)
{
    if (puCount == nullptr)
        return E_INVALIDARG;
    *puCount = static_cast<UINT>(candidates_.size());
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetSelection(UINT *puIndex)
{
    if (puIndex == nullptr)
        return E_INVALIDARG;
    *puIndex = selection_;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetString(UINT uIndex, BSTR *pstr)
{
    if (pstr == nullptr)
        return E_INVALIDARG;
    if (uIndex >= candidates_.size())
        return E_INVALIDARG;
    *pstr = SysAllocString(candidates_[uIndex].c_str());
    return (*pstr != nullptr) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP candidate_ui_element::GetPageIndex(UINT *pIndex, UINT uSize, UINT *puPageCnt)
{
    if (puPageCnt == nullptr)
        return E_INVALIDARG;

    const UINT page_count = static_cast<UINT>(page_index_.size());
    *puPageCnt = page_count;

    if (page_count == 0)
        return S_OK;

    if (pIndex == nullptr)
        return (uSize == 0) ? S_OK : E_INVALIDARG;

    if (uSize < page_count)
        return E_NOT_SUFFICIENT_BUFFER;

    for (UINT i = 0; i < page_count; ++i)
        pIndex[i] = page_index_[i];

    return S_OK;
}

STDMETHODIMP candidate_ui_element::SetPageIndex(UINT *pIndex, UINT uPageCnt)
{
    if (uPageCnt > 0 && pIndex == nullptr)
        return E_INVALIDARG;
    page_index_.assign(pIndex, pIndex + uPageCnt);
    updated_flags_ |= TF_CLUIE_PAGEINDEX;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetCurrentPage(UINT *puPage)
{
    if (puPage == nullptr)
        return E_INVALIDARG;
    *puPage = current_page_;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::SetSelection(UINT nIndex)
{
    selection_ = nIndex;
    updated_flags_ |= TF_CLUIE_SELECTION;
    if (selection_callback_)
        selection_callback_(selection_);
    return S_OK;
}

STDMETHODIMP candidate_ui_element::Finalize()
{
    if (finalize_callback_)
        finalize_callback_();
    return S_OK;
}

STDMETHODIMP candidate_ui_element::Abort()
{
    if (abort_callback_)
        abort_callback_();
    return S_OK;
}

STDMETHODIMP candidate_ui_element::SetIntegrationStyle(GUID guidIntegrationStyle)
{
    integration_style_ = guidIntegrationStyle;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::GetSelectionStyle(TfIntegratableCandidateListSelectionStyle *ptfSelectionStyle)
{
    if (ptfSelectionStyle == nullptr)
        return E_INVALIDARG;
    *ptfSelectionStyle = STYLE_ACTIVE_SELECTION;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::OnKeyDown(WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
    if (pfEaten == nullptr)
        return E_INVALIDARG;
    *pfEaten = FALSE;
    if (key_down_callback_)
        *pfEaten = key_down_callback_(wParam, lParam) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::ShowCandidateNumbers(BOOL *pfShow)
{
    if (pfShow == nullptr)
        return E_INVALIDARG;
    *pfShow = TRUE;
    return S_OK;
}

STDMETHODIMP candidate_ui_element::FinalizeExactCompositionString()
{
    if (finalize_exact_callback_)
        finalize_exact_callback_();
    return S_OK;
}

void candidate_ui_element::set_show_callback(std::function<void(BOOL)> callback)
{
    show_callback_ = std::move(callback);
}

void candidate_ui_element::set_selection_callback(std::function<void(UINT)> callback)
{
    selection_callback_ = std::move(callback);
}

void candidate_ui_element::set_finalize_callback(std::function<void()> callback)
{
    finalize_callback_ = std::move(callback);
}

void candidate_ui_element::set_abort_callback(std::function<void()> callback)
{
    abort_callback_ = std::move(callback);
}

void candidate_ui_element::set_key_down_callback(std::function<bool(WPARAM, LPARAM)> callback)
{
    key_down_callback_ = std::move(callback);
}

void candidate_ui_element::set_finalize_exact_callback(std::function<void()> callback)
{
    finalize_exact_callback_ = std::move(callback);
}

void candidate_ui_element::update_state(const std::vector<std::wstring> &candidates,
                                        const std::vector<UINT> &page_index,
                                        UINT current_page,
                                        UINT selection,
                                        ITfDocumentMgr *pDocMgr,
                                        DWORD updated_flags)
{
    candidates_ = candidates;
    page_index_ = page_index;
    current_page_ = current_page;
    selection_ = selection;
    updated_flags_ = updated_flags;

    if (document_mgr_)
    {
        document_mgr_->Release();
        document_mgr_ = nullptr;
    }
    document_mgr_ = pDocMgr;
    if (document_mgr_)
        document_mgr_->AddRef();
}
