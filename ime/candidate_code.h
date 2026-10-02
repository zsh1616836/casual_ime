#pragma once

#include <cwctype>
#include <string>

namespace zime
{
inline std::wstring resolve_candidate_code(
    const std::wstring& typed_code,
    const std::wstring& candidate,
    const std::wstring& display)
{
    if (typed_code.empty() || candidate.empty() || display.empty() ||
        display.size() <= candidate.size() ||
        display.compare(0, candidate.size(), candidate) != 0)
    {
        return typed_code;
    }

    const std::wstring suffix = display.substr(candidate.size());
    if (suffix.size() == 1)
    {
        const wchar_t extension = static_cast<wchar_t>(towlower(suffix[0]));
        if (extension >= L'a' && extension <= L'z')
            return typed_code + std::wstring(1, extension);
    }

    // Pinyin hits are displayed as "candidate(wubi-code)". Mutations must
    // target that exact wubi code instead of the pinyin query.
    if (suffix.size() >= 3 && suffix.front() == L'(' && suffix.back() == L')')
    {
        std::wstring exact = suffix.substr(1, suffix.size() - 2);
        if (exact.size() <= 64)
        {
            bool valid = true;
            for (wchar_t& character : exact)
            {
                character = static_cast<wchar_t>(towlower(character));
                if (character < L'a' || character > L'z')
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
                return exact;
        }
    }
    return typed_code;
}
}
