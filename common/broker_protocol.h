#pragma once

#include <cstdint>

namespace zime::broker_protocol
{
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\LOCAL\\ZIme.Broker.v2";
constexpr wchar_t kAppContainerPipeName[] =
    L"\\\\.\\pipe\\ZIme.Broker.AppContainer.v2";
constexpr wchar_t kMutexName[] = L"Local\\ZIme.Broker.v2";
constexpr wchar_t kShutdownEventName[] = L"Local\\ZIme.Broker.Shutdown.v2";
constexpr std::uint32_t kMagic = 0x4b52425a; // ZBRK
constexpr std::uint16_t kMajorVersion = 2;
constexpr std::uint16_t kMinorVersion = 0;
constexpr std::uint32_t kMaxMessageSize = 64 * 1024;
constexpr std::uint32_t kHelloTimeoutMs = 2000;
constexpr std::uint32_t kIoTimeoutMs = 1000;

enum class message_type : std::uint16_t
{
    hello = 1,
    hello_result = 2,
    context_snapshot = 3,
    context_result = 4,
    candidate_state = 5,
    ui_action = 6,
    status_state = 7,
    ping = 8,
    pong = 9,
    storage_mutation = 10,
    storage_result = 11,
    config_state = 12,
    candidate_query = 13,
    candidate_result = 14,
    dictionary_state = 15,
};

enum class status_code : std::uint32_t
{
    ok = 0,
    malformed_message = 1,
    unsupported_version = 2,
    identity_mismatch = 3,
    session_mismatch = 4,
    access_denied = 5,
    invalid_window = 6,
    internal_error = 7,
};

enum capability : std::uint64_t
{
    capability_context_snapshot = 1ull << 0,
    capability_owned_windows = 1ull << 1,
    capability_candidate_window = 1ull << 2,
    capability_ui_actions = 1ull << 3,
    capability_status_window = 1ull << 4,
    capability_storage_writer = 1ull << 5,
};

enum candidate_state_flag : std::uint32_t
{
    candidate_visible = 1u << 0,
    candidate_custom_ui_allowed = 1u << 1,
    candidate_anchor_valid = 1u << 2,
    candidate_presentation_pending = 1u << 3,
};

enum class ui_action_type : std::uint32_t
{
    candidate_select = 1,
    candidate_page = 2,
    candidate_delete = 3,
    candidate_mark_uncommon = 4,
    status_change = 5,
    status_position = 6,
    status_menu_popup = 7,
};

enum session_status_flag : std::uint32_t
{
    status_visible = 1u << 0,
    status_custom_ui_allowed = 1u << 1,
    status_position_customized = 1u << 2,
    status_full_width = 1u << 3,
    status_chinese_mode = 1u << 4,
    status_chinese_punctuation = 1u << 5,
};

enum global_setting_flag : std::uint32_t
{
    setting_auto_commit_four_unique = 1u << 0,
    setting_commit_first_on_fifth = 1u << 1,
    setting_show_uncommon = 1u << 2,
    setting_replace_dot_after_digit = 1u << 3,
    setting_use_english_punctuation = 1u << 4,
    setting_disable_chinese_dash = 1u << 5,
    setting_status_position_customized = 1u << 6,
};

enum candidate_result_flag : std::uint32_t
{
    candidate_result_auto_commit_first = 1u << 0,
    candidate_result_commit_first_on_next_code = 1u << 1,
};

enum candidate_query_flag : std::uint32_t
{
    candidate_query_apply_auto_commit = 1u << 0,
};

enum candidate_record_flag : std::uint32_t
{
    candidate_record_pinyin = 1u << 0,
};

enum class storage_operation : std::uint32_t
{
    add_custom_word = 1,
    delete_candidate = 2,
    mark_uncommon = 3,
    record_selection = 4,
};

enum storage_mutation_flag : std::uint32_t
{
    storage_expects_result = 1u << 0,
};

#pragma pack(push, 1)
struct message_header
{
    std::uint32_t magic;
    std::uint16_t major_version;
    std::uint16_t minor_version;
    message_type type;
    std::uint16_t reserved;
    std::uint32_t size;
    std::uint64_t request_id;
    std::uint64_t connection_id;
};

struct hello
{
    message_header header;
    std::uint32_t reported_process_id;
    std::uint32_t reported_thread_id;
    std::uint32_t reported_session_id;
    std::uint32_t pointer_size;
    std::uint64_t client_nonce;
    std::uint64_t capabilities;
};

struct hello_result
{
    message_header header;
    status_code status;
    std::uint32_t actual_client_process_id;
    std::uint32_t actual_client_session_id;
    std::uint32_t server_process_id;
    std::uint32_t server_session_id;
    std::uint32_t reserved;
    std::uint64_t echoed_client_nonce;
    std::uint64_t server_nonce;
    std::uint64_t capabilities;
};

struct context_snapshot
{
    message_header header;
    std::uint32_t reported_process_id;
    std::uint32_t reported_thread_id;
    std::uint32_t reported_session_id;
    std::uint32_t reserved;
    std::uint64_t view_hwnd;
    std::uint64_t root_hwnd;
    std::uint64_t foreground_hwnd;
};

struct context_result
{
    message_header header;
    status_code status;
    std::uint32_t owner_process_id;
    std::uint32_t owner_window_created;
    std::uint32_t owner_relationship_matches;
    std::uint32_t owner_window_visible;
    std::uint32_t win32_error;
};

struct rect_i32
{
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;
    std::int32_t bottom;
};

struct candidate_state
{
    message_header header;
    std::uint64_t generation;
    std::uint64_t view_hwnd;
    std::uint64_t owner_hwnd;
    rect_i32 anchor;
    std::uint32_t flags;
    std::uint32_t selection_absolute;
    std::uint32_t page_size;
    std::uint32_t current_page;
    std::uint32_t ui_font_percent;
    std::uint32_t composition_chars;
    std::uint32_t candidate_count;
    std::uint32_t string_chars;
};

struct candidate_string_record
{
    std::uint32_t offset_chars;
    std::uint32_t length_chars;
};

struct ui_action
{
    message_header header;
    std::uint64_t generation;
    ui_action_type action;
    std::uint32_t value;
    std::int32_t screen_x;
    std::int32_t screen_y;
};

struct status_state
{
    message_header header;
    std::uint64_t generation;
    std::uint64_t owner_hwnd;
    std::uint32_t flags;
    std::int32_t position_x;
    std::int32_t position_y;
    std::uint32_t ui_font_percent;
    std::uint32_t candidate_sort_mode;
};

struct storage_mutation
{
    message_header header;
    storage_operation operation;
    std::uint32_t flags;
    std::uint32_t code_chars;
    std::uint32_t text_chars;
};

struct storage_result
{
    message_header header;
    status_code status;
    storage_operation operation;
    std::uint32_t error_chars;
    std::uint32_t reserved;
};

struct config_state
{
    message_header header;
    std::uint64_t revision;
    std::uint32_t flags;
    std::int32_t status_position_x;
    std::int32_t status_position_y;
    std::uint32_t ui_font_percent;
    std::uint32_t candidate_sort_mode;
};

struct candidate_query
{
    message_header header;
    std::uint64_t generation;
    std::uint32_t code_chars;
    std::uint32_t flags;
};

struct candidate_result
{
    message_header header;
    std::uint64_t generation;
    std::uint64_t config_revision;
    status_code status;
    std::uint32_t flags;
    std::uint32_t code_chars;
    std::uint32_t candidate_count;
    std::uint32_t string_chars;
    std::uint32_t reserved;
};

struct engine_candidate_record
{
    std::uint32_t text_offset_chars;
    std::uint32_t text_length_chars;
    std::uint32_t view_offset_chars;
    std::uint32_t view_length_chars;
    std::uint32_t flags;
};

struct dictionary_state
{
    message_header header;
    std::uint64_t revision;
};
#pragma pack(pop)

inline message_header make_header(message_type type,
                                  std::uint32_t size,
                                  std::uint64_t request_id = 0,
                                  std::uint64_t connection_id = 0)
{
    return {kMagic,
            kMajorVersion,
            kMinorVersion,
            type,
            0,
            size,
            request_id,
            connection_id};
}

inline bool has_valid_envelope(const message_header& header,
                               message_type expected_type,
                               std::uint32_t expected_size)
{
    return header.magic == kMagic &&
        header.major_version == kMajorVersion &&
        header.minor_version <= kMinorVersion &&
        header.type == expected_type &&
        header.size == expected_size;
}

static_assert(sizeof(message_header) == 32);
static_assert(sizeof(hello) == 64);
static_assert(sizeof(hello_result) == 80);
static_assert(sizeof(context_snapshot) == 72);
static_assert(sizeof(context_result) == 56);
static_assert(sizeof(rect_i32) == 16);
static_assert(sizeof(candidate_state) == 104);
static_assert(sizeof(candidate_string_record) == 8);
static_assert(sizeof(ui_action) == 56);
static_assert(sizeof(status_state) == 68);
static_assert(sizeof(storage_mutation) == 48);
static_assert(sizeof(storage_result) == 48);
static_assert(sizeof(config_state) == 60);
static_assert(sizeof(candidate_query) == 48);
static_assert(sizeof(candidate_result) == 72);
static_assert(sizeof(engine_candidate_record) == 20);
static_assert(sizeof(dictionary_state) == 40);
}
