#pragma once

#include <windows.h>

#include <string>

namespace zime::input_policy
{
inline bool is_temporary_raw_composition(const std::wstring& composition)
{
    return !composition.empty() &&
        composition.front() >= L'A' && composition.front() <= L'Z';
}

inline bool is_numpad_digit(WPARAM virtual_key)
{
    return virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9;
}

inline wchar_t numpad_digit(WPARAM virtual_key)
{
    return static_cast<wchar_t>(L'0' + (virtual_key - VK_NUMPAD0));
}

inline bool is_main_digit(WPARAM virtual_key)
{
    return virtual_key >= '0' && virtual_key <= '9';
}

inline bool should_append_unshifted_main_digit(
    const std::wstring& composition, WPARAM virtual_key, bool shift_pressed)
{
    return !shift_pressed && is_main_digit(virtual_key) &&
        is_temporary_raw_composition(composition);
}

inline bool should_append_number_row_symbol(
    const std::wstring& composition, WPARAM virtual_key, bool shift_pressed)
{
    const bool shared_composition_symbol = shift_pressed &&
        (is_main_digit(virtual_key) || virtual_key == VK_OEM_PLUS ||
         virtual_key == VK_OEM_MINUS || virtual_key == VK_OEM_3);
    if (shared_composition_symbol)
        return true;

    return is_temporary_raw_composition(composition) &&
        (virtual_key == VK_OEM_PLUS || virtual_key == VK_OEM_MINUS ||
         virtual_key == VK_OEM_3);
}
}
