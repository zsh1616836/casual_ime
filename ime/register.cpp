#include "globals.h"
#include "../common/broker_protocol.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <Aclapi.h>
#include <sddl.h>
#include <filesystem>

#pragma comment(lib, "shlwapi.lib")

namespace
{
constexpr PCWSTR kAllApplicationPackagesSid = L"S-1-15-2-1";
constexpr PCWSTR kAllRestrictedApplicationPackagesSid = L"S-1-15-2-2";
constexpr PCWSTR kBrokerRegistryKey = L"Software\\ZIme\\Broker";
constexpr PCWSTR kBrokerRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr PCWSTR kBrokerRunValue = L"ZImeBroker";
#ifdef _WIN64
constexpr PCWSTR kBrokerRegistrationValue = L"Registration64";
constexpr PCWSTR kOtherBrokerRegistrationValue = L"Registration32";
#else
constexpr PCWSTR kBrokerRegistrationValue = L"Registration32";
constexpr PCWSTR kOtherBrokerRegistrationValue = L"Registration64";
#endif

class scoped_local_ptr
{
public:
    scoped_local_ptr() : ptr_(nullptr) {}
    ~scoped_local_ptr()
    {
        reset();
    }

    scoped_local_ptr(const scoped_local_ptr&) = delete;
    scoped_local_ptr& operator=(const scoped_local_ptr&) = delete;

    void** out()
    {
        reset();
        return &ptr_;
    }

    void* get() const
    {
        return ptr_;
    }

    void reset(void* value = nullptr)
    {
        if (ptr_)
            LocalFree(ptr_);
        ptr_ = value;
    }

private:
    void* ptr_;
};

DWORD add_appcontainer_read_access(const std::filesystem::path& path)
{
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    const DWORD get_acl_result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &old_dacl,
        nullptr,
        &security_descriptor);
    if (get_acl_result != ERROR_SUCCESS)
        return get_acl_result;

    scoped_local_ptr any_packages_sid;
    scoped_local_ptr restricted_packages_sid;
    if (!ConvertStringSidToSidW(kAllApplicationPackagesSid, reinterpret_cast<PSID*>(any_packages_sid.out())) ||
        !ConvertStringSidToSidW(kAllRestrictedApplicationPackagesSid, reinterpret_cast<PSID*>(restricted_packages_sid.out())))
    {
        const DWORD sid_error = GetLastError();
        if (security_descriptor)
            LocalFree(security_descriptor);
        return sid_error == ERROR_SUCCESS ? ERROR_INVALID_SID : sid_error;
    }

    EXPLICIT_ACCESSW access_entries[2] = {};
    PSID trustees[2] = {
        static_cast<PSID>(any_packages_sid.get()),
        static_cast<PSID>(restricted_packages_sid.get())
    };
    // A null DACL grants full access, so there is nothing to inspect or add.
    if (!old_dacl)
    {
        if (security_descriptor)
            LocalFree(security_descriptor);
        return ERROR_SUCCESS;
    }

    bool already_granted = true;
    for (size_t i = 0; i < ARRAYSIZE(trustees); ++i)
    {
        TRUSTEEW trustee = {};
        BuildTrusteeWithSidW(&trustee, trustees[i]);
        ACCESS_MASK effective_rights = 0;
        if (GetEffectiveRightsFromAclW(
                old_dacl, &trustee, &effective_rights) != ERROR_SUCCESS ||
            (effective_rights &
             (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) !=
                (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE))
        {
            already_granted = false;
            break;
        }
    }
    if (already_granted)
    {
        if (security_descriptor)
            LocalFree(security_descriptor);
        return ERROR_SUCCESS;
    }

    for (size_t i = 0; i < ARRAYSIZE(access_entries); ++i)
    {
        access_entries[i].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        access_entries[i].grfAccessMode = SET_ACCESS;
        access_entries[i].grfInheritance = NO_INHERITANCE;
        BuildTrusteeWithSidW(&access_entries[i].Trustee, trustees[i]);
    }

    PACL new_dacl = nullptr;
    DWORD result = SetEntriesInAclW(static_cast<ULONG>(ARRAYSIZE(access_entries)),
                                    access_entries,
                                    old_dacl,
                                    &new_dacl);
    if (result == ERROR_SUCCESS)
    {
        SECURITY_DESCRIPTOR updated_descriptor = {};
        if (!InitializeSecurityDescriptor(
                &updated_descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(
                &updated_descriptor, TRUE, new_dacl, FALSE) ||
            !SetFileSecurityW(path.c_str(),
                              DACL_SECURITY_INFORMATION,
                              &updated_descriptor))
        {
            result = GetLastError();
        }
    }

    if (new_dacl)
        LocalFree(new_dacl);
    if (security_descriptor)
        LocalFree(security_descriptor);
    return result;
}

DWORD grant_appcontainer_access_to_install_tree(const std::filesystem::path& module_path)
{
    std::error_code ec;
    const std::filesystem::path install_dir = module_path.parent_path();
    if (install_dir.empty() || !std::filesystem::exists(install_dir, ec))
        return ERROR_PATH_NOT_FOUND;

    // 包应用加载 DLL 时，路径上的父目录也需要最基本的遍历权限。
    for (std::filesystem::path parent = install_dir.parent_path();
         !parent.empty();
         parent = parent.parent_path())
    {
        const std::wstring begin_stage =
            L"appcontainer_acl.path.begin path=" + parent.wstring();
        registration_trace(begin_stage.c_str(), S_OK);
        const DWORD parent_result = add_appcontainer_read_access(parent);
        const std::wstring complete_stage =
            L"appcontainer_acl.path.complete path=" + parent.wstring();
        registration_trace(
            complete_stage.c_str(), HRESULT_FROM_WIN32(parent_result));
        if (parent_result != ERROR_SUCCESS)
            return parent_result;
        if (parent == parent.root_path())
            break;
    }

    // SetFileSecurity applies these non-inheritable ACEs only to the named
    // object. SetNamedSecurityInfo on D:\ or Program Files can trigger an ACL
    // propagation pass across their entire trees and stall Setup for minutes.
    std::wstring begin_stage =
        L"appcontainer_acl.path.begin path=" + install_dir.wstring();
    registration_trace(begin_stage.c_str(), S_OK);
    DWORD result = add_appcontainer_read_access(install_dir);
    std::wstring complete_stage =
        L"appcontainer_acl.path.complete path=" + install_dir.wstring();
    registration_trace(complete_stage.c_str(), HRESULT_FROM_WIN32(result));
    if (result != ERROR_SUCCESS)
        return result;

    const std::filesystem::path critical_files[] = {
        module_path,
        install_dir / L"casual_ime.dll",
        install_dir / L"casual_ime32.dll",
        install_dir / L"zime_broker.exe",
        install_dir / L"dict.idx",
    };
    for (const auto& path : critical_files)
    {
        if (!std::filesystem::is_regular_file(path, ec) || ec)
        {
            ec.clear();
            continue;
        }
        begin_stage = L"appcontainer_acl.path.begin path=" + path.wstring();
        registration_trace(begin_stage.c_str(), S_OK);
        result = add_appcontainer_read_access(path);
        complete_stage =
            L"appcontainer_acl.path.complete path=" + path.wstring();
        registration_trace(complete_stage.c_str(), HRESULT_FROM_WIN32(result));
        if (result != ERROR_SUCCESS)
            return result;
    }

    return ERROR_SUCCESS;
}

bool current_broker_path(std::filesystem::path* broker_path)
{
    if (!broker_path)
        return false;
    WCHAR module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(g_hInst, module, ARRAYSIZE(module));
    if (length == 0 || length >= ARRAYSIZE(module))
        return false;
    *broker_path = std::filesystem::path(module).parent_path() /
        L"zime_broker.exe";
    std::error_code error;
    return std::filesystem::is_regular_file(*broker_path, error) && !error;
}

LONG set_registry_string(HKEY key,
                         PCWSTR value_name,
                         const std::wstring& value)
{
    return RegSetValueExW(
        key,
        value_name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void signal_broker_shutdown()
{
    HANDLE event = OpenEventW(
        EVENT_MODIFY_STATE, FALSE, zime::broker_protocol::kShutdownEventName);
    if (!event)
        return;
    SetEvent(event);
    CloseHandle(event);
}
}

void registration_trace(const wchar_t* stage, HRESULT result)
{
    WCHAR module[MAX_PATH] = {};
    const DWORD module_length = GetModuleFileNameW(
        g_hInst, module, ARRAYSIZE(module));
    if (module_length == 0 || module_length >= ARRAYSIZE(module))
        return;

    const std::filesystem::path log_path =
        std::filesystem::path(module).parent_path() / L"zime_install.log";
    HANDLE log_file = CreateFileW(log_path.c_str(),
                                  FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (log_file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    WCHAR line[1024] = {};
    const wchar_t* architecture = sizeof(void*) == 8 ? L"x64" : L"x86";
    const HRESULT format_result = StringCchPrintfW(
        line,
        ARRAYSIZE(line),
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu arch=%s stage=%s result=0x%08lx\r\n",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        GetCurrentProcessId(),
        architecture,
        stage ? stage : L"(null)",
        static_cast<unsigned long>(result));
    if (SUCCEEDED(format_result))
    {
        const int utf8_size = WideCharToMultiByte(
            CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (utf8_size > 1)
        {
            std::vector<char> utf8(static_cast<std::size_t>(utf8_size));
            WideCharToMultiByte(CP_UTF8,
                                0,
                                line,
                                -1,
                                utf8.data(),
                                utf8_size,
                                nullptr,
                                nullptr);
            DWORD written = 0;
            WriteFile(log_file,
                      utf8.data(),
                      static_cast<DWORD>(utf8.size() - 1),
                      &written,
                      nullptr);
        }
        OutputDebugStringW(line);
    }
    CloseHandle(log_file);
}

BOOL register_broker_installation()
{
    std::filesystem::path broker_path;
    if (!current_broker_path(&broker_path))
        return FALSE;

    HKEY broker_key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER,
                                  kBrokerRegistryKey,
                                  0,
                                  nullptr,
                                  REG_OPTION_NON_VOLATILE,
                                  KEY_SET_VALUE | KEY_QUERY_VALUE |
                                      KEY_WOW64_64KEY,
                                  nullptr,
                                  &broker_key,
                                  nullptr);
    if (result != ERROR_SUCCESS)
        return FALSE;
    result = set_registry_string(
        broker_key, kBrokerRegistrationValue, broker_path.wstring());
    RegCloseKey(broker_key);
    if (result != ERROR_SUCCESS)
        return FALSE;

    // Older builds launched the Broker at logon. The Broker is now started
    // only after a TIP client cannot connect to the per-session pipe.
    HKEY run_key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      kBrokerRunKey,
                      0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY,
                      &run_key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(run_key, kBrokerRunValue);
        RegCloseKey(run_key);
    }
    return TRUE;
}

BOOL unregister_broker_installation()
{
    std::wstring remaining_broker_path;
    HKEY broker_key = nullptr;
    LONG result = RegOpenKeyExW(HKEY_CURRENT_USER,
                                kBrokerRegistryKey,
                                0,
                                KEY_SET_VALUE | KEY_QUERY_VALUE |
                                    KEY_WOW64_64KEY,
                                &broker_key);
    if (result == ERROR_SUCCESS)
    {
        RegDeleteValueW(broker_key, kBrokerRegistrationValue);
        wchar_t path[MAX_PATH * 4] = {};
        DWORD type = 0;
        DWORD bytes = sizeof(path);
        if (RegQueryValueExW(broker_key,
                             kOtherBrokerRegistrationValue,
                             nullptr,
                             &type,
                             reinterpret_cast<BYTE*>(path),
                             &bytes) == ERROR_SUCCESS &&
            type == REG_SZ && path[0] != L'\0')
        {
            remaining_broker_path = path;
        }
        RegCloseKey(broker_key);
    }

    HKEY run_key = nullptr;
    result = RegOpenKeyExW(HKEY_CURRENT_USER,
                           kBrokerRunKey,
                           0,
                           KEY_SET_VALUE | KEY_WOW64_64KEY,
                           &run_key);
    if (result == ERROR_SUCCESS)
    {
        RegDeleteValueW(run_key, kBrokerRunValue);
        RegCloseKey(run_key);
    }

    if (remaining_broker_path.empty())
    {
        signal_broker_shutdown();
        RegDeleteKeyExW(HKEY_CURRENT_USER,
                        kBrokerRegistryKey,
                        KEY_WOW64_64KEY,
                        0);
    }
    return TRUE;
}

// 创建注册表键
static LONG recurse_delete_key(HKEY hKeyParent, LPCTSTR lpszKey)
{
    HKEY hKey;
    LONG lRes = RegOpenKeyEx(hKeyParent, lpszKey, 0, KEY_READ | KEY_WRITE, &hKey);
    if (lRes != ERROR_SUCCESS)
        return lRes;

    FILETIME time;
    TCHAR szBuffer[256];
    DWORD dwSize = ARRAYSIZE(szBuffer);
    
    while (RegEnumKeyEx(hKey, 0, szBuffer, &dwSize, nullptr, nullptr, nullptr, &time) == ERROR_SUCCESS)
    {
        lRes = recurse_delete_key(hKey, szBuffer);
        if (lRes != ERROR_SUCCESS)
            break;
        dwSize = ARRAYSIZE(szBuffer);
    }

    RegCloseKey(hKey);
    return RegDeleteKey(hKeyParent, lpszKey);
}

// 注册COM服务器
BOOL register_server()
{
    WCHAR szModule[MAX_PATH] = {};
    WCHAR szCLSID[CLSID_STRLEN] = {};
    WCHAR szKey[MAX_PATH] = {};
    HKEY hKey = nullptr;
    HKEY hSubKey = nullptr;
    LONG result = ERROR_SUCCESS;

    const DWORD module_len = GetModuleFileNameW(g_hInst, szModule, ARRAYSIZE(szModule));
    if (module_len == 0 || module_len >= ARRAYSIZE(szModule))
        return FALSE;

    // Sticky Notes / SearchHost 等包应用运行在 AppContainer 中时，
    // 当前 TIP DLL 必须具备包应用读取/执行权限，否则 COM 激活前就会 Access Denied。
    registration_trace(L"appcontainer_acl.begin", S_OK);
    const DWORD acl_result = grant_appcontainer_access_to_install_tree(std::filesystem::path(szModule));
    if (acl_result != ERROR_SUCCESS)
    {
        registration_trace(
            L"appcontainer_acl.failed", HRESULT_FROM_WIN32(acl_result));
        return FALSE;
    }
    registration_trace(L"appcontainer_acl.complete", S_OK);

    if (StringFromGUID2(c_clsidTextService, szCLSID, ARRAYSIZE(szCLSID)) <= 0)
        return FALSE;

    // 注册CLSID
    registration_trace(L"com_registry.begin", S_OK);
    if (FAILED(StringCchPrintf(szKey, ARRAYSIZE(szKey), L"CLSID\\%s", szCLSID)))
        return FALSE;
    result = RegCreateKeyExW(HKEY_CLASSES_ROOT, szKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &hKey, nullptr);
    if (result != ERROR_SUCCESS)
        return FALSE;

    result = RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(TEXTSERVICE_NAME),
                            static_cast<DWORD>((wcslen(TEXTSERVICE_NAME) + 1) * sizeof(WCHAR)));
    if (result != ERROR_SUCCESS)
        goto cleanup;

    // InprocServer32
    result = RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_WRITE, nullptr, &hSubKey, nullptr);
    if (result != ERROR_SUCCESS)
        goto cleanup;

    result = RegSetValueExW(hSubKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(szModule),
                            static_cast<DWORD>((wcslen(szModule) + 1) * sizeof(WCHAR)));
    if (result != ERROR_SUCCESS)
        goto cleanup;

    result = RegSetValueExW(hSubKey, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(TEXTSERVICE_MODEL),
                            static_cast<DWORD>((wcslen(TEXTSERVICE_MODEL) + 1) * sizeof(WCHAR)));

cleanup:
    if (hSubKey)
        RegCloseKey(hSubKey);
    if (hKey)
        RegCloseKey(hKey);
    if (result != ERROR_SUCCESS)
        recurse_delete_key(HKEY_CLASSES_ROOT, szKey);

    registration_trace(result == ERROR_SUCCESS
                           ? L"com_registry.complete"
                           : L"com_registry.failed",
                       HRESULT_FROM_WIN32(result));

    return result == ERROR_SUCCESS;
}

// 卸载COM服务器
BOOL unregister_server()
{
    WCHAR szCLSID[CLSID_STRLEN];
    WCHAR szKey[MAX_PATH];

    StringFromGUID2(c_clsidTextService, szCLSID, ARRAYSIZE(szCLSID));
    StringCchPrintf(szKey, ARRAYSIZE(szKey), L"CLSID\\%s", szCLSID);

    recurse_delete_key(HKEY_CLASSES_ROOT, szKey);

    return TRUE;
}

// 注册类别
BOOL register_categories()
{
    ITfCategoryMgr *pCategoryMgr = nullptr;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                         IID_ITfCategoryMgr, (void **)&pCategoryMgr);

    if (SUCCEEDED(hr))
    {
        const GUID categories[] = {
            GUID_TFCAT_TIP_KEYBOARD,
            GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
            GUID_TFCAT_TIPCAP_COMLESS,
            GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
            GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
            GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
        };

        size_t registered_count = 0;
        for (; registered_count < ARRAYSIZE(categories); ++registered_count)
        {
            hr = pCategoryMgr->RegisterCategory(c_clsidTextService,
                                                categories[registered_count],
                                                c_clsidTextService);
            if (FAILED(hr))
                break;
        }

        if (FAILED(hr))
        {
            while (registered_count > 0)
            {
                --registered_count;
                pCategoryMgr->UnregisterCategory(c_clsidTextService,
                                                 categories[registered_count],
                                                 c_clsidTextService);
            }
        }

        pCategoryMgr->Release();
    }

    return SUCCEEDED(hr);
}

// 卸载类别
BOOL unregister_categories()
{
    ITfCategoryMgr *pCategoryMgr = nullptr;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                         IID_ITfCategoryMgr, (void **)&pCategoryMgr);

    if (SUCCEEDED(hr))
    {
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIP_KEYBOARD, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_COMLESS, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_UIELEMENTENABLED, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, c_clsidTextService);
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, c_clsidTextService);
        pCategoryMgr->Release();
    }

    return SUCCEEDED(hr);
}

// 注册配置文件
BOOL register_profile()
{
    ITfInputProcessorProfiles *pProfiles = nullptr;
    ITfInputProcessorProfileMgr *pProfileMgr = nullptr;
    WCHAR szModule[MAX_PATH] = {};
    const DWORD module_len = GetModuleFileNameW(g_hInst, szModule, ARRAYSIZE(szModule));
    if (module_len == 0 || module_len >= ARRAYSIZE(szModule))
        return FALSE;

    bool registered = false;

    HRESULT hrProfiles = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_ITfInputProcessorProfiles, reinterpret_cast<void **>(&pProfiles));

    if (SUCCEEDED(hrProfiles) && pProfiles)
    {
        HRESULT hr = pProfiles->Register(c_clsidTextService);

        if (SUCCEEDED(hr))
        {
            hr = pProfiles->AddLanguageProfile(
                c_clsidTextService,
                LANGUAGE_ID,
                c_guidProfile,
                TEXTSERVICE_DESC,
                (ULONG)wcslen(TEXTSERVICE_DESC),
                szModule,
                module_len,
                ICON_INDEX);
        }

        if (SUCCEEDED(hr))
        {
            pProfiles->EnableLanguageProfile(c_clsidTextService, LANGUAGE_ID, c_guidProfile, TRUE);
            pProfiles->EnableLanguageProfileByDefault(c_clsidTextService, LANGUAGE_ID, c_guidProfile, TRUE);
            registered = true;
        }

        pProfiles->Release();
    }

    HRESULT hrProfileMgr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&pProfileMgr));
    if (SUCCEEDED(hrProfileMgr) && pProfileMgr)
    {
        const HRESULT hrRegisterProfile = pProfileMgr->RegisterProfile(
            c_clsidTextService,
            LANGUAGE_ID,
            c_guidProfile,
            TEXTSERVICE_DESC,
            static_cast<ULONG>(wcslen(TEXTSERVICE_DESC)),
            szModule,
            module_len,
            ICON_INDEX,
            nullptr,
            0,
            TRUE,
            0);

        if (SUCCEEDED(hrRegisterProfile))
            registered = true;
        pProfileMgr->Release();
    }

    return registered ? TRUE : FALSE;
}

// 卸载配置文件
BOOL unregister_profile()
{
    ITfInputProcessorProfiles *pProfiles = nullptr;
    ITfInputProcessorProfileMgr *pProfileMgr = nullptr;
    HRESULT hr;

    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                         IID_ITfInputProcessorProfiles, (void **)&pProfiles);

    if (SUCCEEDED(hr))
    {
        pProfiles->EnableLanguageProfile(c_clsidTextService, LANGUAGE_ID, c_guidProfile, FALSE);
        hr = pProfiles->Unregister(c_clsidTextService);
        pProfiles->Release();
    }

    HRESULT hrProfileMgr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_ITfInputProcessorProfileMgr, reinterpret_cast<void **>(&pProfileMgr));
    if (SUCCEEDED(hrProfileMgr) && pProfileMgr)
    {
        pProfileMgr->UnregisterProfile(c_clsidTextService, LANGUAGE_ID, c_guidProfile, 0);
        pProfileMgr->Release();
    }

    return SUCCEEDED(hr);
}
