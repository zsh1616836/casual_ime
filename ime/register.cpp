#include "globals.h"
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

DWORD add_appcontainer_read_access(const std::filesystem::path& path, DWORD inheritance_flags = NO_INHERITANCE)
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
    for (size_t i = 0; i < ARRAYSIZE(access_entries); ++i)
    {
        access_entries[i].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        access_entries[i].grfAccessMode = SET_ACCESS;
        access_entries[i].grfInheritance = inheritance_flags;
        BuildTrusteeWithSidW(&access_entries[i].Trustee, trustees[i]);
    }

    PACL new_dacl = nullptr;
    DWORD result = SetEntriesInAclW(static_cast<ULONG>(ARRAYSIZE(access_entries)),
                                    access_entries,
                                    old_dacl,
                                    &new_dacl);
    if (result == ERROR_SUCCESS)
    {
        result = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            new_dacl,
            nullptr);
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
        add_appcontainer_read_access(parent);
        if (parent == parent.root_path())
            break;
    }

    // Make the install root propagate the AppContainer ACEs so future DLL or
    // resource copies inherit them instead of regressing to Access Denied.
    add_appcontainer_read_access(install_dir, SUB_CONTAINERS_AND_OBJECTS_INHERIT);

    DWORD result = add_appcontainer_read_access(module_path);
    if (result != ERROR_SUCCESS)
        return result;

    for (std::filesystem::recursive_directory_iterator it(
             install_dir,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        const auto status = it->symlink_status(ec);
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status))
            continue;

        result = std::filesystem::is_directory(status)
            ? add_appcontainer_read_access(it->path(), SUB_CONTAINERS_AND_OBJECTS_INHERIT)
            : add_appcontainer_read_access(it->path());
        (void)result;
    }

    return ERROR_SUCCESS;
}
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
    const DWORD acl_result = grant_appcontainer_access_to_install_tree(std::filesystem::path(szModule));
    if (acl_result != ERROR_SUCCESS)
        return FALSE;

    if (StringFromGUID2(c_clsidTextService, szCLSID, ARRAYSIZE(szCLSID)) <= 0)
        return FALSE;

    // 注册CLSID
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
        // 注册为TIP
        hr = pCategoryMgr->RegisterCategory(c_clsidTextService, GUID_TFCAT_TIP_KEYBOARD, c_clsidTextService);
        
        // 声明支持沉浸式文本框（Search/设置等现代宿主）
        if (SUCCEEDED(hr))
        {
            hr = pCategoryMgr->RegisterCategory(c_clsidTextService, 
                                               GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, 
                                               c_clsidTextService);
        }

        // 声明支持 UIElement 候选列表，让宿主按接口决定是否接管候选 UI。
        if (SUCCEEDED(hr))
        {
            hr = pCategoryMgr->RegisterCategory(c_clsidTextService,
                                               GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
                                               c_clsidTextService);
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
#ifdef GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, c_clsidTextService);
#endif
#ifdef GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, c_clsidTextService);
#endif
#ifdef GUID_TFCAT_TIPCAP_SECUREMODE
        pCategoryMgr->UnregisterCategory(c_clsidTextService, GUID_TFCAT_TIPCAP_SECUREMODE, c_clsidTextService);
#endif
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
