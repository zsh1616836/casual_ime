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
    registration_trace(L"DllRegisterServer.begin", S_OK);
    if (!register_server())
    {
        registration_trace(L"register_server.failed", E_FAIL);
        return E_FAIL;
    }
    registration_trace(L"register_server.complete", S_OK);

    registration_trace(L"register_categories.begin", S_OK);
    if (!register_categories())
    {
        registration_trace(L"register_categories.failed", E_FAIL);
        unregister_server();
        return E_FAIL;
    }
    registration_trace(L"register_categories.complete", S_OK);

    registration_trace(L"register_profile.begin", S_OK);
    if (!register_profile())
    {
        registration_trace(L"register_profile.failed", E_FAIL);
        unregister_categories();
        unregister_server();
        return E_FAIL;
    }
    registration_trace(L"register_profile.complete", S_OK);

    registration_trace(L"register_broker_installation.begin", S_OK);
    if (!register_broker_installation())
    {
        registration_trace(L"register_broker_installation.failed", E_FAIL);
        unregister_profile();
        unregister_categories();
        unregister_server();
        return E_FAIL;
    }

    registration_trace(L"DllRegisterServer.complete", S_OK);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    registration_trace(L"DllUnregisterServer.begin", S_OK);
    unregister_broker_installation();
    unregister_profile();
    unregister_categories();
    unregister_server();
    registration_trace(L"DllUnregisterServer.complete", S_OK);

    return S_OK;
}
