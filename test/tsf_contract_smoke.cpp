#include <windows.h>

#include <ctffunc.h>
#include <msctf.h>

#include <iostream>

namespace
{
using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

constexpr CLSID kTextServiceClsid =
{0x8f8b8f8f, 0x8f8b, 0x8f8b, {0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x8b, 0x8f, 0x8b}};

int fail(const wchar_t* step, HRESULT hr)
{
    std::wcerr << step << L" failed: 0x" << std::hex << static_cast<unsigned long>(hr) << std::endl;
    return 1;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"usage: tsf_contract_smoke <casual_ime.dll>" << std::endl;
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return fail(L"CoInitializeEx", hr);

    HMODULE module = LoadLibraryW(argv[1]);
    if (!module)
    {
        CoUninitialize();
        return fail(L"LoadLibraryW", HRESULT_FROM_WIN32(GetLastError()));
    }

    auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(
        GetProcAddress(module, "DllGetClassObject"));
    if (!get_class_object)
    {
        FreeLibrary(module);
        CoUninitialize();
        return fail(L"GetProcAddress", HRESULT_FROM_WIN32(GetLastError()));
    }

    IClassFactory* factory = nullptr;
    hr = get_class_object(kTextServiceClsid,
                          IID_IClassFactory,
                          reinterpret_cast<void**>(&factory));
    if (FAILED(hr))
    {
        FreeLibrary(module);
        CoUninitialize();
        return fail(L"DllGetClassObject", hr);
    }

    ITfTextInputProcessorEx* tip = nullptr;
    hr = factory->CreateInstance(nullptr,
                                 IID_ITfTextInputProcessorEx,
                                 reinterpret_cast<void**>(&tip));
    factory->Release();
    if (FAILED(hr))
    {
        FreeLibrary(module);
        CoUninitialize();
        return fail(L"CreateInstance", hr);
    }

    ITfThreadMgrEx* thread_manager = nullptr;
    hr = CoCreateInstance(CLSID_TF_ThreadMgr,
                          nullptr,
                          CLSCTX_INPROC_SERVER,
                          IID_ITfThreadMgrEx,
                          reinterpret_cast<void**>(&thread_manager));
    if (FAILED(hr))
    {
        tip->Release();
        FreeLibrary(module);
        CoUninitialize();
        return fail(L"CoCreateInstance(CLSID_TF_ThreadMgr)", hr);
    }

    TfClientId client_id = TF_CLIENTID_NULL;
    hr = thread_manager->ActivateEx(&client_id, TF_TMAE_UIELEMENTENABLEDONLY);
    if (SUCCEEDED(hr))
        hr = tip->ActivateEx(thread_manager, client_id, TF_TMAE_UIELEMENTENABLEDONLY);
    if (FAILED(hr))
    {
        thread_manager->Deactivate();
        thread_manager->Release();
        tip->Release();
        FreeLibrary(module);
        CoUninitialize();
        return fail(L"ActivateEx", hr);
    }

    ITfFunctionProvider* function_provider = nullptr;
    hr = tip->QueryInterface(IID_ITfFunctionProvider,
                             reinterpret_cast<void**>(&function_provider));

    ITfFnSearchCandidateProvider* search_provider = nullptr;
    if (SUCCEEDED(hr))
    {
        IUnknown* function = nullptr;
        hr = function_provider->GetFunction(GUID_NULL,
                                            IID_ITfFnSearchCandidateProvider,
                                            &function);
        if (SUCCEEDED(hr))
        {
            hr = function->QueryInterface(IID_ITfFnSearchCandidateProvider,
                                          reinterpret_cast<void**>(&search_provider));
            function->Release();
        }
    }

    ITfCandidateList* list = nullptr;
    if (SUCCEEDED(hr))
    {
        BSTR query = SysAllocString(L"a");
        BSTR application_id = SysAllocString(L"ZIme.ContractSmoke");
        if (!query || !application_id)
            hr = E_OUTOFMEMORY;
        else
            hr = search_provider->GetSearchCandidates(query, application_id, &list);
        SysFreeString(application_id);
        SysFreeString(query);
    }

    ULONG candidate_count = 0;
    if (SUCCEEDED(hr))
        hr = list->GetCandidateNum(&candidate_count);

    if (list)
        list->Release();
    if (search_provider)
        search_provider->Release();
    if (function_provider)
        function_provider->Release();

    const HRESULT deactivate_hr = tip->Deactivate();
    thread_manager->Deactivate();
    thread_manager->Release();
    tip->Release();
    FreeLibrary(module);
    CoUninitialize();

    if (FAILED(hr))
        return fail(L"FunctionProvider/SearchCandidateProvider", hr);
    if (FAILED(deactivate_hr))
        return fail(L"TIP Deactivate", deactivate_hr);

    std::wcout << L"TSF contract smoke passed; candidates=" << candidate_count << std::endl;
    return 0;
}
