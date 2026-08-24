#include "broker_storage.h"

#include "../common/default_settings.h"
#include "perf_trace.h"
#include "tool.h"
#include "3rd/sqlite3.h"

#include <windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>

namespace
{
constexpr PCWSTR kAllApplicationPackagesSid = L"S-1-15-2-1";
constexpr PCWSTR kAllRestrictedApplicationPackagesSid = L"S-1-15-2-2";

class local_memory
{
public:
    ~local_memory() { if (value_) LocalFree(value_); }
    void** out() { return &value_; }
    void* get() const { return value_; }

private:
    void* value_ = nullptr;
};

DWORD grant_package_read_access(const std::filesystem::path& path,
                                DWORD inheritance)
{
    PACL old_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &old_acl,
        nullptr,
        &descriptor);
    if (result != ERROR_SUCCESS)
        return result;

    local_memory all_packages;
    local_memory restricted_packages;
    if (!ConvertStringSidToSidW(
            kAllApplicationPackagesSid,
            reinterpret_cast<PSID*>(all_packages.out())) ||
        !ConvertStringSidToSidW(
            kAllRestrictedApplicationPackagesSid,
            reinterpret_cast<PSID*>(restricted_packages.out())))
    {
        result = GetLastError();
        LocalFree(descriptor);
        return result;
    }

    EXPLICIT_ACCESSW entries[2] = {};
    PSID sids[2] = {
        static_cast<PSID>(all_packages.get()),
        static_cast<PSID>(restricted_packages.get())};
    for (std::size_t index = 0; index < std::size(entries); ++index)
    {
        entries[index].grfAccessPermissions =
            FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        entries[index].grfAccessMode = SET_ACCESS;
        entries[index].grfInheritance = inheritance;
        BuildTrusteeWithSidW(&entries[index].Trustee, sids[index]);
    }

    PACL new_acl = nullptr;
    result = SetEntriesInAclW(
        static_cast<ULONG>(std::size(entries)), entries, old_acl, &new_acl);
    if (result == ERROR_SUCCESS)
    {
        result = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            new_acl,
            nullptr);
    }
    if (new_acl)
        LocalFree(new_acl);
    LocalFree(descriptor);
    return result;
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

bool migrate_sqlite_database(const std::filesystem::path& source,
                             const std::filesystem::path& destination,
                             std::wstring* error)
{
    std::filesystem::path temporary = destination;
    temporary += L".migrating";
    DeleteFileW(temporary.c_str());
    sqlite3* source_db = nullptr;
    sqlite3* destination_db = nullptr;
    const std::string source_utf8 = path_to_utf8(source);
    const std::string destination_utf8 = path_to_utf8(temporary);
    bool success = !source_utf8.empty() && !destination_utf8.empty() &&
        sqlite3_open_v2(source_utf8.c_str(),
                        &source_db,
                        SQLITE_OPEN_READONLY,
                        nullptr) == SQLITE_OK &&
        sqlite3_open_v2(destination_utf8.c_str(),
                        &destination_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) == SQLITE_OK;
    sqlite3_backup* backup = nullptr;
    if (success)
    {
        backup = sqlite3_backup_init(
            destination_db, "main", source_db, "main");
        success = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
    }
    if (backup)
        success = sqlite3_backup_finish(backup) == SQLITE_OK && success;
    if (destination_db)
        success = sqlite3_close(destination_db) == SQLITE_OK && success;
    if (source_db)
        sqlite3_close(source_db);

    if (success)
    {
        success = MoveFileExW(temporary.c_str(),
                              destination.c_str(),
                              MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!success)
    {
        DeleteFileW(temporary.c_str());
        if (error)
            *error = L"Cannot migrate legacy user_dict.db";
    }
    return success;
}

std::wstring widen_error(const std::string& error)
{
    if (error.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8,
                                         0,
                                         error.data(),
                                         static_cast<int>(error.size()),
                                         nullptr,
                                         0);
    if (size <= 0)
        return std::wstring(error.begin(), error.end());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        error.data(),
                        static_cast<int>(error.size()),
                        result.data(),
                        size);
    return result;
}

void append_bool(std::wstring* output,
                 const wchar_t* key,
                 bool value)
{
    output->append(key).append(L"=").append(value ? L"1\r\n" : L"0\r\n");
}

void append_int(std::wstring* output,
                const wchar_t* key,
                int value)
{
    output->append(key)
        .append(L"=")
        .append(std::to_wstring(value))
        .append(L"\r\n");
}
}

bool broker_storage::Prepare(std::wstring* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Finish the immutable dictionary load before clients can issue their
    // first query. Cold disk I/O must never run on the first typing request.
    return EnsureStorageReady(error) && EnsureDictionary(error);
}

bool broker_storage::ApplyMutation(
    std::uint64_t client_nonce,
    std::uint64_t request_id,
    zime::broker_protocol::storage_operation operation,
    const std::wstring& code,
    const std::wstring& text,
    std::wstring* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (error)
        error->clear();
    const auto cache_key = std::make_pair(client_nonce, request_id);
    const auto cached = mutation_cache_.find(cache_key);
    if (cached != mutation_cache_.end())
    {
        if (error)
            *error = cached->second.error;
        return cached->second.success;
    }

    bool success = false;
    std::wstring result_error;
    if (!EnsureStorageReady(error))
    {
        result_error = error ? *error : L"Cannot prepare storage";
    }
    else if (!EnsureDictionary(error))
    {
        result_error = error ? *error : L"Cannot initialize dictionary";
    }
    else
    {
        std::string operation_error;
        switch (operation)
        {
        case zime::broker_protocol::storage_operation::add_custom_word:
        {
            std::string narrow_code;
            narrow_code.reserve(code.size());
            bool valid_code = true;
            for (const wchar_t character : code)
            {
                if (character < L'a' || character > L'y')
                {
                    valid_code = false;
                    break;
                }
                narrow_code.push_back(static_cast<char>(character));
            }
            if (!valid_code)
                result_error = L"Invalid custom word code";
            else
                success = dictionary_.add_custom_word(
                    text, narrow_code, operation_error);
            break;
        }
        case zime::broker_protocol::storage_operation::delete_candidate:
            success = dictionary_.delete_candidate(
                code, text, operation_error);
            break;
        case zime::broker_protocol::storage_operation::mark_uncommon:
            success = dictionary_.mark_candidate_uncommon(
                code, text, operation_error);
            break;
        case zime::broker_protocol::storage_operation::record_selection:
            success = dictionary_.record_candidate_selected(
                code, text, &operation_error);
            break;
        default:
            result_error = L"Unsupported storage operation";
            break;
        }
        if (!success && result_error.empty())
            result_error = widen_error(operation_error);
    }

    constexpr std::size_t kMutationCacheLimit = 4096;
    if (mutation_cache_order_.size() >= kMutationCacheLimit)
    {
        mutation_cache_.erase(mutation_cache_order_.front());
        mutation_cache_order_.pop_front();
    }
    mutation_cache_order_.push_back(cache_key);
    mutation_cache_.emplace(
        cache_key, mutation_cache_entry{success, result_error});
    if (success)
        ++dictionary_revision_;
    if (error)
        *error = std::move(result_error);
    return success;
}

bool broker_storage::SaveConfig(
    const zime::broker_protocol::config_state& state,
    zime::broker_protocol::config_state* committed_state,
    std::wstring* error)
{
    using namespace zime::broker_protocol;
    std::lock_guard<std::mutex> lock(mutex_);
    if (error)
        error->clear();
    if (!EnsureStorageReady(error))
        return false;

    std::wstring contents;
    contents.reserve(768);
    contents.append(L"\ufeff[state]\r\n");
    append_bool(&contents,
                L"auto_commit_four_code_unique",
                (state.flags & setting_auto_commit_four_unique) != 0);
    append_bool(&contents,
                L"commit_first_candidate_on_fifth_code",
                (state.flags & setting_commit_first_on_fifth) != 0);
    append_bool(&contents,
                L"show_uncommon_candidates",
                (state.flags & setting_show_uncommon) != 0);
    append_bool(&contents,
                L"replace_dot_after_digit",
                (state.flags & setting_replace_dot_after_digit) != 0);
    append_bool(&contents,
                L"use_english_punctuation_in_chinese_mode",
                (state.flags & setting_use_english_punctuation) != 0);
    append_bool(&contents,
                L"disable_chinese_dash",
                (state.flags & setting_disable_chinese_dash) != 0);
    append_int(&contents,
               L"candidate_sort_mode",
               static_cast<int>((std::min)(state.candidate_sort_mode, 2u)));
    contents.append(L"\r\n[ui]\r\n");
    append_int(&contents,
               L"font_percent",
               static_cast<int>(std::clamp<std::uint32_t>(
                   state.ui_font_percent, 80, 250)));
    append_bool(&contents,
                L"status_position_customized",
                (state.flags & setting_status_position_customized) != 0);
    append_int(&contents, L"status_pos_x", state.status_position_x);
    append_int(&contents, L"status_pos_y", state.status_position_y);

    const std::filesystem::path config_path =
        tool::get_user_data_path() / L"config.ini";
    std::filesystem::path temporary_path = config_path;
    temporary_path += L".tmp";
    HANDLE file = CreateFileW(temporary_path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        if (error)
            *error = L"Cannot create config temporary file";
        return false;
    }

    DWORD written = 0;
    const DWORD byte_count = static_cast<DWORD>(
        contents.size() * sizeof(wchar_t));
    const bool wrote = WriteFile(file,
                                 contents.data(),
                                 byte_count,
                                 &written,
                                 nullptr) &&
        written == byte_count &&
        FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote ||
        !MoveFileExW(temporary_path.c_str(),
                     config_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary_path.c_str());
        if (error)
            *error = L"Cannot replace config file";
        return false;
    }
    ++config_revision_;
    if (committed_state)
    {
        *committed_state = state;
        committed_state->revision = config_revision_;
        current_config_ = *committed_state;
    }
    else
    {
        current_config_ = state;
        current_config_.revision = config_revision_;
    }
    return true;
}

zime::broker_protocol::config_state broker_storage::LoadConfig()
{
    using namespace zime::broker_protocol;
    std::lock_guard<std::mutex> lock(mutex_);
    config_state state = {};
    if (config_revision_ == 0)
        config_revision_ = 1;
    state.revision = config_revision_;
    std::wstring prepare_error;
    EnsureStorageReady(&prepare_error);
    const std::filesystem::path config_path =
        tool::get_user_data_path() / L"config.ini";
    const wchar_t* path = config_path.c_str();

    if (GetPrivateProfileIntW(L"ui", L"status_position_customized", 0, path))
        state.flags |= setting_status_position_customized;
    if (GetPrivateProfileIntW(
            L"state",
            L"auto_commit_four_code_unique",
            zime::default_settings::auto_commit_four_code_unique ? 1 : 0,
            path))
    {
        state.flags |= setting_auto_commit_four_unique;
    }
    if (GetPrivateProfileIntW(
            L"state",
            L"commit_first_candidate_on_fifth_code",
            zime::default_settings::commit_first_candidate_on_fifth_code ? 1 : 0,
            path))
    {
        state.flags |= setting_commit_first_on_fifth;
    }
    if (GetPrivateProfileIntW(
            L"state",
            L"show_uncommon_candidates",
            zime::default_settings::show_uncommon_candidates ? 1 : 0,
            path))
    {
        state.flags |= setting_show_uncommon;
    }
    if (GetPrivateProfileIntW(
            L"state",
            L"replace_dot_after_digit",
            zime::default_settings::replace_dot_after_digit ? 1 : 0,
            path))
    {
        state.flags |= setting_replace_dot_after_digit;
    }
    if (GetPrivateProfileIntW(
            L"state",
            L"use_english_punctuation_in_chinese_mode",
            zime::default_settings::use_english_punctuation_in_chinese_mode
                ? 1
                : 0,
            path))
    {
        state.flags |= setting_use_english_punctuation;
    }
    if (GetPrivateProfileIntW(
            L"state",
            L"disable_chinese_dash",
            zime::default_settings::disable_chinese_dash ? 1 : 0,
            path))
    {
        state.flags |= setting_disable_chinese_dash;
    }

    state.status_position_x =
        GetPrivateProfileIntW(L"ui", L"status_pos_x", 0, path);
    state.status_position_y =
        GetPrivateProfileIntW(L"ui", L"status_pos_y", 0, path);
    state.ui_font_percent = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"ui",
            L"font_percent",
            static_cast<int>(zime::default_settings::ui_font_percent),
            path)),
        80,
        250));
    state.candidate_sort_mode = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(GetPrivateProfileIntW(
            L"state",
            L"candidate_sort_mode",
            static_cast<int>(zime::default_settings::candidate_sort_mode),
            path)),
        0,
        2));
    current_config_ = state;
    return state;
}

bool broker_storage::GetCandidates(
    const std::wstring& code,
    bool apply_auto_commit,
    std::vector<std::wstring>* candidates,
    std::vector<std::wstring>* view_texts,
    std::vector<bool>* pinyin_flags,
    std::uint32_t* result_flags,
    std::uint64_t* config_revision,
    std::wstring* error)
{
    ZIME_PERF_SCOPE("broker.GetCandidates",
                    static_cast<std::int64_t>(code.size()),
                    apply_auto_commit ? 1 : 0);
    using namespace zime::broker_protocol;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!candidates || !view_texts || !pinyin_flags || !result_flags)
        return false;
    candidates->clear();
    view_texts->clear();
    pinyin_flags->clear();
    *result_flags = 0;
    if (!EnsureStorageReady(error) || !EnsureDictionary(error))
        return false;

    dictionary_.set_show_uncommon_candidates(
        (current_config_.flags & setting_show_uncommon) != 0);
    dictionary_.set_candidate_sort_mode(static_cast<ime_dict::candidate_sort_mode>(
        (std::min)(current_config_.candidate_sort_mode, 2u)));
    const bool found = dictionary_.get_candidates(
        code, *candidates, *view_texts, pinyin_flags);
    if (config_revision)
        *config_revision = current_config_.revision;
    if (!found)
        return true;

    const bool first_is_wubi = !pinyin_flags->empty() && !(*pinyin_flags)[0];
    if (apply_auto_commit && code.size() == 4 &&
        candidates->size() == 1 && first_is_wubi &&
        (current_config_.flags & setting_auto_commit_four_unique) != 0)
    {
        *result_flags |= candidate_result_auto_commit_first;
    }
    if (code.size() == 4 && !candidates->empty() && first_is_wubi &&
        (current_config_.flags & setting_commit_first_on_fifth) != 0)
    {
        *result_flags |= candidate_result_commit_first_on_next_code;
    }
    return true;
}

bool broker_storage::ExportRawDictionary(
    const std::filesystem::path& output_path,
    std::wstring* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureStorageReady(error) || !EnsureDictionary(error))
        return false;
    std::string narrow_error;
    const bool success = dictionary_.export_raw_dictionary(
        output_path, narrow_error);
    if (!success && error)
        *error = widen_error(narrow_error);
    return success;
}

std::uint64_t broker_storage::DictionaryRevision()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dictionary_revision_;
}

bool broker_storage::EnsureStorageReady(std::wstring* error)
{
    if (storage_prepared_)
        return true;
    if (error)
        error->clear();

    const std::filesystem::path data_dir = tool::get_user_data_path();
    std::error_code filesystem_error;
    std::filesystem::create_directories(data_dir, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(data_dir))
    {
        if (error)
            *error = L"Cannot create per-user data directory";
        return false;
    }

    const std::filesystem::path legacy_dir = tool::get_current_dll_path();
    const std::filesystem::path config_path = data_dir / L"config.ini";
    const std::filesystem::path legacy_config = legacy_dir / L"config.ini";
    const std::filesystem::path default_config =
        legacy_dir / L"default-config.ini";
    if (!std::filesystem::exists(config_path, filesystem_error))
    {
        filesystem_error.clear();
        const std::filesystem::path* seed_config = nullptr;
        if (std::filesystem::is_regular_file(legacy_config, filesystem_error))
            seed_config = &legacy_config;
        else
        {
            filesystem_error.clear();
            if (std::filesystem::is_regular_file(default_config, filesystem_error))
                seed_config = &default_config;
        }
        if (seed_config &&
            !CopyFileW(seed_config->c_str(), config_path.c_str(), TRUE) &&
            GetLastError() != ERROR_FILE_EXISTS)
        {
            if (error)
                *error = L"Cannot seed config.ini";
            return false;
        }
    }

    const std::filesystem::path database_path = data_dir / L"user_dict.db";
    const std::filesystem::path legacy_database =
        legacy_dir / L"user_dict.db";
    filesystem_error.clear();
    if (!std::filesystem::exists(database_path, filesystem_error))
    {
        filesystem_error.clear();
        if (std::filesystem::is_regular_file(legacy_database, filesystem_error) &&
            !migrate_sqlite_database(
                legacy_database, database_path, error))
        {
            return false;
        }
    }

    DWORD acl_result = grant_package_read_access(
        data_dir, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    if (acl_result != ERROR_SUCCESS)
    {
        if (error)
            *error = L"Cannot grant AppContainer access to user data";
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(
             data_dir,
             std::filesystem::directory_options::skip_permission_denied,
             filesystem_error))
    {
        if (filesystem_error)
            break;
        grant_package_read_access(
            entry.path(),
            entry.is_directory()
                ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
                : NO_INHERITANCE);
    }

    storage_prepared_ = true;
    return true;
}

bool broker_storage::EnsureDictionary(std::wstring* error)
{
    if (dictionary_initialized_)
        return true;
    dictionary_initialized_ = dictionary_.init();
    if (!dictionary_initialized_ && error)
        *error = L"Cannot initialize broker dictionary";
    return dictionary_initialized_;
}
