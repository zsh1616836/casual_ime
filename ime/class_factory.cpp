#include "class_factory.h"
#include "text_service.h"

class_factory::class_factory()
{
    dll_add_ref();
    m_cRef = 1;
}

class_factory::~class_factory()
{
    dll_release();
}

STDAPI class_factory::QueryInterface(REFIID riid, void **ppvObj)
{
    if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
    {
        *ppvObj = (IClassFactory *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDAPI_(ULONG) class_factory::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_cRef));
}

STDAPI_(ULONG) class_factory::Release()
{
    const LONG cr = InterlockedDecrement(&m_cRef);
    if (cr == 0)
    {
        delete this;
    }
    return static_cast<ULONG>(cr);
}

STDAPI class_factory::CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppvObj)
{
    if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (pUnkOuter != NULL)
        return CLASS_E_NOAGGREGATION;

    text_service *pTextService = new text_service();
    if (pTextService == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = pTextService->QueryInterface(riid, ppvObj);
    pTextService->Release();

    return hr;
}

STDAPI class_factory::LockServer(BOOL fLock)
{
    if (fLock)
    {
        dll_add_ref();
    }
    else
    {
        dll_release();
    }

    return S_OK;
}
