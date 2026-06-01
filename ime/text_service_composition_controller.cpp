#include "text_service.h"

#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace
{
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

void text_service::CommitCompositionCodeAndClear(ITfContext* pContext)
{
    if (!m_bInComposition || m_compositionText.empty())
        return;

    ITfContext* commit_context = pContext;
    if (commit_context)
        commit_context->AddRef();

    if (!commit_context && m_pThreadMgr)
    {
        ITfDocumentMgr* pDocMgrFocus = nullptr;
        if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            pDocMgrFocus->GetTop(&commit_context);
            pDocMgrFocus->Release();
        }
    }

    if (commit_context)
    {
        InsertRawText(commit_context, m_compositionText);
        commit_context->Release();
    }

    CancelCompositionInContext(pContext);
    ClearComposition();
    HideCandidates();
}

void text_service::InsertRawText(ITfContext *pContext, const std::wstring& text)
{
    if (!pContext || text.empty())
        return;

    composition_edit_session *pEditSession = new composition_edit_session(
        this, pContext, text, composition_edit_session::op_type::commit);
    if (pEditSession)
    {
        HRESULT hr;
        pContext->RequestEditSession(m_tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hr);
        pEditSession->Release();
    }
}

void text_service::InsertText(ITfContext *pContext, const std::wstring& text)
{
    if (!pContext || text.empty())
        return;

    // 根据当前状态处理文本（全角/半角、标点转换）
    std::wstring processed_text = ProcessTextBeforeInsert(text);

    // 创建编辑会话来插入文本（若有组合，使用组合范围提交）
    composition_edit_session *pEditSession = new composition_edit_session(
        this, pContext, processed_text, composition_edit_session::op_type::commit);
    if (pEditSession)
    {
        HRESULT hr;
        // 请求编辑会话
        pContext->RequestEditSession(m_tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hr);
        pEditSession->Release();
    }
}

void text_service::UpdateCompositionInContext(ITfContext* pContext)
{
    if (!pContext || m_compositionText.empty())
        return;

    composition_edit_session *pEditSession = new composition_edit_session(
        this, pContext, m_compositionText, composition_edit_session::op_type::update);
    if (pEditSession)
    {
        HRESULT hr;
        pContext->RequestEditSession(m_tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hr);
        pEditSession->Release();
    }
}

void text_service::CancelCompositionInContext(ITfContext* pContext)
{
    if (!m_pComposition)
        return;

    ITfContext* context = pContext;
    if (context)
    {
        context->AddRef();
    }
    else if (m_pThreadMgr)
    {
        ITfDocumentMgr* pDocMgrFocus = nullptr;
        if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            pDocMgrFocus->GetTop(&context);
            pDocMgrFocus->Release();
        }
    }

    if (!context)
    {
        m_pComposition->Release();
        m_pComposition = nullptr;
        return;
    }

    composition_edit_session *pEditSession = new composition_edit_session(
        this, context, L"", composition_edit_session::op_type::clear);
    if (pEditSession)
    {
        HRESULT hr;
        context->RequestEditSession(m_tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hr);
        pEditSession->Release();
    }
    context->Release();
}

void text_service::ClearComposition()
{
    CancelCompositionInContext(nullptr);
    m_compositionText.clear();
    m_bInComposition = FALSE;
    ClearCandidateAnchorRect();
    m_candidateWindow.set_composition_text(L"");
    m_candidateWindow.set_candidates(std::vector<std::wstring>());
}

