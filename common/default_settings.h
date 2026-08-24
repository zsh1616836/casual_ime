#pragma once

#include <cstdint>

namespace zime::default_settings
{
inline constexpr bool chinese_mode = true;
inline constexpr bool full_width = false;
inline constexpr bool chinese_punctuation = true;

inline constexpr bool auto_commit_four_code_unique = true;
inline constexpr bool commit_first_candidate_on_fifth_code = true;
inline constexpr bool show_uncommon_candidates = false;
inline constexpr bool replace_dot_after_digit = true;
inline constexpr bool use_english_punctuation_in_chinese_mode = false;
inline constexpr bool disable_chinese_dash = false;

inline constexpr std::uint32_t candidate_sort_mode = 2;
inline constexpr std::uint32_t ui_font_percent = 100;
}
