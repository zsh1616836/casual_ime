#pragma once

#include "broker_protocol.h"
#include "ime_dictionary.h"

#include <mutex>
#include <deque>
#include <map>
#include <string>
#include <utility>

class broker_storage
{
public:
    bool Prepare(std::wstring* error);
    bool ApplyMutation(std::uint64_t client_nonce,
                       std::uint64_t request_id,
                       zime::broker_protocol::storage_operation operation,
                       const std::wstring& code,
                       const std::wstring& text,
                       std::wstring* error);
    bool SaveConfig(const zime::broker_protocol::config_state& state,
                    zime::broker_protocol::config_state* committed_state,
                    std::wstring* error);
    zime::broker_protocol::config_state LoadConfig();
    bool GetCandidates(const std::wstring& code,
                       bool apply_auto_commit,
                       std::vector<std::wstring>* candidates,
                       std::vector<std::wstring>* view_texts,
                       std::vector<bool>* pinyin_flags,
                       std::uint32_t* result_flags,
                       std::uint64_t* config_revision,
                       std::wstring* error);
    bool ExportRawDictionary(const std::filesystem::path& output_path,
                             std::wstring* error);
    std::uint64_t DictionaryRevision();

private:
    bool EnsureStorageReady(std::wstring* error);
    bool EnsureDictionary(std::wstring* error);

    std::mutex mutex_;
    ime_dict dictionary_;
    bool dictionary_initialized_ = false;
    bool storage_prepared_ = false;
    std::uint64_t config_revision_ = 0;
    std::uint64_t dictionary_revision_ = 1;
    zime::broker_protocol::config_state current_config_ = {};
    struct mutation_cache_entry
    {
        bool success;
        std::wstring error;
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, mutation_cache_entry>
        mutation_cache_;
    std::deque<std::pair<std::uint64_t, std::uint64_t>>
        mutation_cache_order_;
};
