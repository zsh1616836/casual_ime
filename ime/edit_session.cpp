#include "edit_session.h"

edit_session::edit_session(ITfContext *context, const std::wstring& text)
{
    ref_ = 1;
    context_ = context;
    context_->AddRef();
    text_ = text;
}

edit_session::~edit_session()
{
    if (context_)
    {
        context_->Release();
    }
}

STDMETHODIMP edit_session::QueryInterface(REFIID riid, void **ppvObj)
{
    if (ppvObj == nullptr)
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

STDMETHODIMP_(ULONG) edit_session::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) edit_session::Release()
{
    const LONG cr = InterlockedDecrement(&ref_);
    if (cr == 0)
    {
        delete this;
    }
    return static_cast<ULONG>(cr);
}

STDMETHODIMP edit_session::DoEditSession(TfEditCookie ec)
{
    ITfInsertAtSelection *pInsertAtSelection = nullptr;
    ITfRange *pRange = nullptr;
    HRESULT hr = E_FAIL;

    if (SUCCEEDED(context_->QueryInterface(IID_ITfInsertAtSelection, (void **)&pInsertAtSelection)))
    {
        // 插入文本
        hr = pInsertAtSelection->InsertTextAtSelection(ec, 
                                                       0, 
                                                       text_.c_str(), 
                                                       (LONG)text_.length(), 
                                                       &pRange);
        if (SUCCEEDED(hr) && pRange)
        {
            // 将选择范围折叠到文本末尾（光标移到右侧）
            pRange->Collapse(ec, TF_ANCHOR_END);
            
            // 设置选择范围到插入文本的末尾
            TF_SELECTION tfSelection;
            tfSelection.range = pRange;
            tfSelection.style.ase = TF_AE_NONE;
            tfSelection.style.fInterimChar = FALSE;
            
            context_->SetSelection(ec, 1, &tfSelection);
            
            pRange->Release();
        }

        pInsertAtSelection->Release();
    }

    return hr;
}
