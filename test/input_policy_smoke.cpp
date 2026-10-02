#include "broker_protocol.h"
#include "candidate_code.h"
#include "input_policy.h"

#include <iostream>

namespace
{
int fail(const wchar_t* reason)
{
    std::wcerr << reason << std::endl;
    return 1;
}
}

int wmain()
{
    using namespace zime::input_policy;
    if (!is_temporary_raw_composition(L"Q") ||
        !is_temporary_raw_composition(L"Qabc") ||
        is_temporary_raw_composition(L"q") ||
        is_temporary_raw_composition(L""))
    {
        return fail(L"temporary raw mode must depend on the first uppercase letter");
    }

    for (WPARAM key = '0'; key <= '9'; ++key)
    {
        if (!should_append_unshifted_main_digit(L"Qabc", key, false) ||
            should_append_unshifted_main_digit(L"qabc", key, false) ||
            should_append_unshifted_main_digit(L"Qabc", key, true))
        {
            return fail(L"main digits must append only in unshifted temporary raw mode");
        }
    }
    for (WPARAM key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key)
    {
        if (!is_numpad_digit(key) ||
            numpad_digit(key) != L'0' + (key - VK_NUMPAD0))
        {
            return fail(L"numpad digit mapping is incorrect");
        }
    }
    if (!should_append_number_row_symbol(L"q", '1', true) ||
        !should_append_number_row_symbol(L"Q", VK_OEM_MINUS, false) ||
        !should_append_number_row_symbol(L"Q", VK_OEM_PLUS, false) ||
        !should_append_number_row_symbol(L"Q", VK_OEM_3, false) ||
        should_append_number_row_symbol(L"q", VK_OEM_MINUS, false) ||
        should_append_number_row_symbol(L"Q", VK_OEM_PERIOD, false))
    {
        return fail(L"number-row symbol policy is incorrect");
    }

    if (zime::resolve_candidate_code(L"q", L"\u6211", L"\u6211a") != L"qa" ||
        zime::resolve_candidate_code(L"wo", L"\u6211", L"\u6211(q)") != L"q" ||
        zime::resolve_candidate_code(L"wo", L"\u8bcd", L"\u8bcd(ABCD)") != L"abcd" ||
        zime::resolve_candidate_code(L"wo", L"\u8bcd", L"\u8bcd(abc1)") != L"wo" ||
        zime::resolve_candidate_code(L"wo", L"\u8bcd", L"\u522b\u7684\u8bcd(q)") != L"wo")
    {
        return fail(L"candidate mutation code resolution is incorrect");
    }

    using zime::broker_protocol::is_candidate_ui_action;
    using zime::broker_protocol::ui_action_type;
    if (!is_candidate_ui_action(ui_action_type::candidate_menu_popup) ||
        !is_candidate_ui_action(ui_action_type::candidate_delete) ||
        is_candidate_ui_action(ui_action_type::status_menu_popup))
    {
        return fail(L"candidate action generation routing is incorrect");
    }

    std::wcout << L"input policy smoke passed" << std::endl;
    return 0;
}
