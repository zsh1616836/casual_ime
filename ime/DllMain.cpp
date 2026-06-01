#include "globals.h"
#include "class_factory.h"

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID pvReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hInst = hInstance;
        DisableThreadLibraryCalls(hInstance);
        break;

    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return (g_cRefDll == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppvObj)
{
    if (ppvObj == NULL)
        return E_INVALIDARG;

    *ppvObj = NULL;

    if (IsEqualCLSID(rclsid, c_clsidTextService))
    {
        class_factory *pClassFactory = new class_factory();
        if (pClassFactory == NULL)
            return E_OUTOFMEMORY;

        HRESULT hr = pClassFactory->QueryInterface(riid, ppvObj);
        pClassFactory->Release();

        return hr;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllRegisterServer()
{
    if (!register_server())
        return E_FAIL;

    if (!register_categories())
    {
        unregister_server();
        return E_FAIL;
    }

    if (!register_profile())
    {
        unregister_categories();
        unregister_server();
        return E_FAIL;
    }

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    unregister_profile();
    unregister_categories();
    unregister_server();

    return S_OK;
}
