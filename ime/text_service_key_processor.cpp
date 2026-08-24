#include "text_service.h"

#include <Windows.h>
#include "perf_trace.h"
#include <map>
#include <cwctype>

namespace
{
bool is_shift_vk(WPARAM vk)
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

void reset_shift_track(bool& shift_pressed, bool& other_key_pressed)
{
    shift_pressed = FALSE;
    other_key_pressed = FALSE;
}

bool has_system_modifier(const BYTE key_state[256])
{
    return (key_state[VK_CONTROL] & 0x80) != 0 ||
           (key_state[VK_MENU] & 0x80) != 0 ||
           (key_state[VK_LWIN] & 0x80) != 0 ||
           (key_state[VK_RWIN] & 0x80) != 0;
}

void clear_shift_toggle_state(bool& shift_pressed, bool& other_key_pressed, bool& shift_pressed_with_modifier)
{
    reset_shift_track(shift_pressed, other_key_pressed);
    shift_pressed_with_modifier = false;
}

std::wstring resolve_raw_symbol_text(WPARAM wParam, bool shiftPressed)
{
    switch (wParam)
    {
    case VK_OEM_COMMA:  return shiftPressed ? L"<" : L",";
    case VK_OEM_PERIOD: return shiftPressed ? L">" : L".";
    case VK_OEM_1:      return shiftPressed ? L":" : L";";
    case VK_OEM_2:      return shiftPressed ? L"?" : L"/";
    case VK_OEM_3:      return shiftPressed ? L"~" : L"`";
    case VK_OEM_4:      return shiftPressed ? L"{" : L"[";
    case VK_OEM_5:      return shiftPressed ? L"|" : L"\\";
    case VK_OEM_6:      return shiftPressed ? L"}" : L"]";
    case VK_OEM_7:      return shiftPressed ? L"\"" : L"'";
    case VK_OEM_PLUS:   return shiftPressed ? L"+" : L"=";
    case VK_OEM_MINUS:  return shiftPressed ? L"_" : L"-";
    case '1':           return shiftPressed ? L"!" : L"";
    case '2':           return shiftPressed ? L"@" : L"";
    case '3':           return shiftPressed ? L"#" : L"";
    case '4':           return shiftPressed ? L"$" : L"";
    case '5':           return shiftPressed ? L"%" : L"";
    case '6':           return shiftPressed ? L"^" : L"";
    case '7':           return shiftPressed ? L"&" : L"";
    case '8':           return shiftPressed ? L"*" : L"";
    case '9':           return shiftPressed ? L"(" : L"";
    case '0':           return shiftPressed ? L")" : L"";
    default:
        return L"";
    }
}

bool is_composition_extension_symbol(WPARAM wParam, bool shiftPressed)
{
    return shiftPressed &&
           ((wParam >= '0' && wParam <= '9') ||
            wParam == VK_OEM_PLUS ||
            wParam == VK_OEM_MINUS ||
            wParam == VK_OEM_3);
}
}
STDAPI text_service::OnTestKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
    *pfEaten = FALSE;

    BYTE keyState[256] = {};
    GetKeyboardState(keyState);
    const bool systemModifierPressed = has_system_modifier(keyState);

    if (is_shift_vk(wParam))
    {
        m_bShiftPressed = TRUE;
        m_bOtherKeyPressed = FALSE;
        m_bShiftPressedWithModifier = systemModifierPressed;
        return S_OK;
    }

    if (m_bShiftPressed && !is_shift_vk(wParam))
    {
        m_bOtherKeyPressed = TRUE;
    }

    // 如果有系统修饰键按下，不拦截任何按键
    if (systemModifierPressed)
    {
        return S_OK;
    }
    
    // 在中文模式下，字母键总是拦截（用于输入）
    if (m_bChineseMode && wParam >= 'A' && wParam <= 'Z')
    {
        // 检查CapsLock状态
        bool capsLockOn = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (capsLockOn)
        {
            // CapsLock开启时，直接上屏，不拦截
            *pfEaten = FALSE;
            return S_OK;
        }
        else
        {
            // CapsLock关闭时，拦截用于组合输入
            *pfEaten = TRUE;
            return S_OK;
        }
    }
    
    // 在英文模式下，拦截字母；数字仅在未按 Shift 时拦截（Shift+数字应输出符号）
    if (!m_bChineseMode)
    {
        const bool shiftPressed = (keyState[VK_SHIFT] & 0x80) != 0;
        if ((wParam >= 'A' && wParam <= 'Z') ||
            (!shiftPressed && wParam >= '0' && wParam <= '9'))
        {
            *pfEaten = TRUE;
            return S_OK;
        }
    }
    
    // 标点符号拦截（如果开启了中文标点）
    if (m_bChinesePunctuation)
    {
        if (wParam == VK_OEM_COMMA ||   // , 键
            wParam == VK_OEM_PERIOD ||  // . 键
            wParam == VK_OEM_1 ||       // ; : 键
            wParam == VK_OEM_2 ||       // / ? 键
            wParam == VK_OEM_3 ||       // ` ~ 键
            wParam == VK_OEM_4 ||       // [ { 键
            wParam == VK_OEM_5 ||       // \ | 键
            wParam == VK_OEM_6 ||       // ] } 键
            wParam == VK_OEM_7 ||       // ' " 键
            wParam == VK_SPACE)         // 空格键
        {
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    // 以下按键只在有组合字符串时才拦截（中文模式）
    if (m_bChineseMode && m_bInComposition && !m_compositionText.empty())
    {
        if ((wParam >= '0' && wParam <= '9') ||
            wParam == VK_SPACE ||
            wParam == VK_BACK ||
            wParam == VK_ESCAPE ||
            wParam == VK_RETURN ||
            wParam == VK_OEM_PLUS ||   // = 键（也是 + 键）
            wParam == VK_OEM_MINUS ||  // - 键
            wParam == VK_OEM_COMMA ||
            wParam == VK_OEM_PERIOD ||
            wParam == VK_OEM_1 ||
            wParam == VK_OEM_2 ||
            wParam == VK_OEM_3 ||
            wParam == VK_OEM_4 ||
            wParam == VK_OEM_5 ||
            wParam == VK_OEM_6 ||
            wParam == VK_OEM_7 ||
            wParam == VK_LEFT ||       // 左方向键
            wParam == VK_RIGHT)        // 右方向键
        {
            *pfEaten = TRUE;
        }
    }

    return S_OK;
}

STDAPI text_service::OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
    ZIME_PERF_SCOPE("tip.OnKeyDown",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    m_bInComposition ? 1 : 0);
    *pfEaten = FALSE;

    // 检查修饰键状态，如果有Ctrl、Alt、Win键按下，不拦截（让系统处理）
    BYTE keyState[256] = {};
    GetKeyboardState(keyState);
    const bool systemModifierPressed = has_system_modifier(keyState);
    
    // 如果有系统修饰键按下，不拦截任何按键
    if (is_shift_vk(wParam))
    {
        m_bShiftPressed = TRUE;
        m_bOtherKeyPressed = FALSE;
        m_bShiftPressedWithModifier = systemModifierPressed;
        return S_OK;
    }

    if (systemModifierPressed)
    {
        if (m_bShiftPressed)
            m_bOtherKeyPressed = TRUE;
        return S_OK;
    }
    
    // 如果Shift被按下，标记有其他键被按下
    if (m_bShiftPressed && !is_shift_vk(wParam))
    {
        m_bOtherKeyPressed = TRUE;
    }

    const bool shiftPressed = (keyState[VK_SHIFT] & 0x80) != 0;
    const bool prevLastInputWasDigit = m_lastInputWasDigit;
    if (wParam == VK_BACK)
    {
        m_lastInputWasDigit = false;
    }
    else if (!shiftPressed && wParam >= '0' && wParam <= '9')
    {
        m_lastInputWasDigit = true;
    }
    else
    {
        m_lastInputWasDigit = false;
    }

    // 处理英文模式下的输入
    if (!m_bChineseMode)
    {
        if (wParam >= 'A' && wParam <= 'Z')
        {
            // 检查CapsLock状态
            bool capsLockOn = (GetKeyState(VK_CAPITAL) & 1) != 0;
            
            WCHAR wch;
            if (capsLockOn)
            {
                // CapsLock开启时，直接上屏大写字母
                wch = (WCHAR)wParam;
            }
            else if (shiftPressed)
            {
                // CapsLock关闭 + Shift按下时，上屏大写字母
                wch = (WCHAR)wParam;
            }
            else
            {
                // CapsLock关闭 + 无Shift时，上屏小写字母
                wch = (WCHAR)towlower((wint_t)wParam);
            }
            
            std::wstring text(1, wch);
            InsertText(pContext, text);
            *pfEaten = TRUE;
            return S_OK;
        }
        else if (wParam >= '0' && wParam <= '9')
        {
            if (shiftPressed)
            {
                return S_OK; // 不拦截，让宿主产生 !@#$%^&*()
            }
            WCHAR wch = (WCHAR)wParam;
            std::wstring text(1, wch);
            InsertText(pContext, text);
            *pfEaten = TRUE;
            return S_OK;
        }
    }
    
    // 处理标点符号（中文标点模式）
    if (m_bInComposition && !m_compositionText.empty())
    {
        if (is_composition_extension_symbol(wParam, shiftPressed))
        {
            const std::wstring composition_symbol = resolve_raw_symbol_text(wParam, true);
            if (!composition_symbol.empty())
            {
                HandleCharacter(pContext, composition_symbol[0]);
                *pfEaten = TRUE;
                return S_OK;
            }
        }
    }

    if (m_bChinesePunctuation)
    {
        std::wstring punctuation;
        
        switch (wParam)
        {
        case VK_OEM_3:      // ` ~
            punctuation = shiftPressed ? L"～" : L"·";
            break;
        case VK_OEM_COMMA:  // , <
            punctuation = shiftPressed ? L"《" : L"，";
            break;
        case VK_OEM_PERIOD: // . >
            punctuation = shiftPressed ? L"》" :
                         (m_replaceDotAfterDigit && prevLastInputWasDigit ? L"." : L"。");
            break;
        case VK_OEM_1:      // ; :
            punctuation = shiftPressed ? L"：" : L"；";
            break;
        case VK_OEM_2:      // / ?
            punctuation = shiftPressed ? L"？" : L"、";
            break;
        case VK_OEM_5:      // \ |
            punctuation = L"、";
            break;
        case VK_OEM_7:      // ' "
            punctuation = shiftPressed ? L"\u201C" : L"\u2018";  // 中文引号 " '
            break;
        case VK_OEM_4:      // [ {
            punctuation = shiftPressed ? L"{" : L"【";
            break;
        case VK_OEM_6:      // ] }
            punctuation = shiftPressed ? L"}" : L"】";
            break;
        case VK_SPACE:      // 空格
            if (!m_bInComposition)  // 只有在非组合状态下才直接处理空格
            {
                punctuation = m_bFullWidth ? L"\u3000" : L" ";
            }
            break;
        default:
            break;
        }
        if (punctuation.empty() && shiftPressed)
        {
            switch (wParam)
            {
            case '1': punctuation = L"！"; break;
            case '2': punctuation = L"@"; break;
            case '3': punctuation = L"#"; break;
            case '4': punctuation = L"￥"; break;
            case '5': punctuation = L"%"; break;
            case '6': punctuation = L"……"; break;
            case '7': punctuation = L"&"; break;
            case '8': punctuation = L"*"; break;
            case '9': punctuation = L"（"; break;
            case '0': punctuation = L"）"; break;
            case VK_OEM_MINUS: punctuation = m_disableChineseDash ? L"_" : L"——"; break; // _
            default: break;
            }
        }
        
        if (!punctuation.empty())
        {
            // 组合中输入非字母字符：先上屏第一个候选（或编码），再上屏该字符
            if (m_bInComposition && !m_compositionText.empty())
            {
                CommitFirstCandidateOrComposition(pContext);
            }

            // 直接插入标点，不经过转换（因为已经是中文标点了）
            InsertRawText(pContext, punctuation);
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    // 非中文标点模式下，组合中输入标点/符号时：
    // 先提交第一个候选（或编码），再把该按键交给宿主插入原字符。
    if (m_bInComposition && !m_compositionText.empty())
    {
        if (wParam == VK_OEM_COMMA ||
            wParam == VK_OEM_PERIOD ||
            wParam == VK_OEM_1 ||
            wParam == VK_OEM_2 ||
            wParam == VK_OEM_3 ||
            wParam == VK_OEM_4 ||
            wParam == VK_OEM_5 ||
            wParam == VK_OEM_6 ||
            wParam == VK_OEM_7)
        {
            const std::wstring raw_symbol = resolve_raw_symbol_text(wParam, shiftPressed);
            CommitFirstCandidateOrComposition(pContext);
            if (!raw_symbol.empty())
                InsertRawText(pContext, raw_symbol);
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    // 处理字母输入（中文模式）
    if (m_bChineseMode && wParam >= 'A' && wParam <= 'Z')
    {
        // 检查CapsLock状态
        bool capsLockOn = (GetKeyState(VK_CAPITAL) & 1) != 0;
        
        WCHAR wch;
        if (capsLockOn)
        {
            // CapsLock开启时，直接上屏大写字母
            wch = (WCHAR)wParam;
            InsertText(pContext, std::wstring(1, wch));
            *pfEaten = TRUE;
            return S_OK;
        }
        else
        {
            // CapsLock关闭时，检查Shift状态
            if (shiftPressed)
            {
                // Shift按下时，大写字母加入组合字符串
                wch = (WCHAR)wParam;
            }
            else
            {
                // 无Shift时，小写字母加入组合字符串
                wch = (WCHAR)towlower((wint_t)wParam);
            }
            HandleCharacter(pContext, wch);
            *pfEaten = TRUE;
            return S_OK;
        }
    }

    // 处理数字选择候选词（仅未按 Shift）
    if (wParam >= '1' && wParam <= '9')
    {
        if (shiftPressed)
            return S_OK;

        if (m_bInComposition && !m_compositionText.empty())
        {
            HandleNumber(pContext, (int)(wParam - '0'));
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // 处理空格 - 上屏当前选中的候选词
    if (wParam == VK_SPACE)
    {
        if (m_bInComposition && !m_compositionText.empty())
        {
            HandleSpace(pContext);
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // 处理退格
    if (wParam == VK_BACK)
    {
        if (m_bInComposition && !m_compositionText.empty())
        {
            HandleBackspace(pContext);
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // ESC取消输入
    if (wParam == VK_ESCAPE)
    {
        if (m_bInComposition)
        {
            CancelCompositionInContext(pContext);
            ClearComposition();
            HideCandidates();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // Enter键上屏组合字符串
    if (wParam == VK_RETURN)
    {
        if (m_bInComposition && !m_compositionText.empty())
        {
            // 直接上屏输入的编码
            InsertText(pContext, m_compositionText);
            ClearComposition();
            HideCandidates();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // = 键翻页（下一页）；Shift+=（+）按符号处理
    if (wParam == VK_OEM_PLUS)
    {
        if (m_bInComposition && !m_compositionText.empty())
        {
            if (shiftPressed)
            {
                return S_OK;
            }
            if (m_candidateWindow.get_candidate_count() > 0)
            {
                m_candidateWindow.page_down();
                m_candidateWindow.set_selection(0);  // 翻页后重置选择到第一项
                if (m_brokerClient.IsConnected() &&
                    ShouldShowBrokerCandidateWindow())
                    SyncBrokerCandidateState();
                else
                    UpdateCandidateWindowPosition(pContext);
                *pfEaten = TRUE;
            }
        }
        return S_OK;
    }

    // - 键翻页（上一页）；Shift+-（_）按符号处理
    if (wParam == VK_OEM_MINUS)
    {
        if (m_bInComposition && !m_compositionText.empty())
        {
            if (shiftPressed)
            {
                return S_OK;
            }
            if (m_candidateWindow.get_candidate_count() > 0)
            {
                m_candidateWindow.page_up();
                m_candidateWindow.set_selection(0);  // 翻页后重置选择到第一项
                if (m_brokerClient.IsConnected() &&
                    ShouldShowBrokerCandidateWindow())
                    SyncBrokerCandidateState();
                else
                    UpdateCandidateWindowPosition(pContext);
                *pfEaten = TRUE;
            }
        }
        return S_OK;
    }

    // 左方向键 - 选择上一个候选词
    if (wParam == VK_LEFT)
    {
        if (m_bInComposition && !m_compositionText.empty() && m_candidateWindow.get_candidate_count() > 0)
        {
            int currentSelection = m_candidateWindow.get_selection();
            int currentPage = m_candidateWindow.get_current_page();
            int pageSize = m_candidateWindow.get_page_size();
            
            if (currentSelection > 0)
            {
                // 在当前页内向左移动
                m_candidateWindow.set_selection(currentSelection - 1);
            }
            else if (currentPage > 0)
            {
                // 跳到上一页的最后一个候选词
                m_candidateWindow.page_up();
                int startIdx = currentPage - 1;
                int endIdx = min(startIdx * pageSize + pageSize, m_candidateWindow.get_candidate_count());
                int lastInPage = (endIdx - startIdx * pageSize) - 1;
                m_candidateWindow.set_selection(lastInPage);
                if (!m_brokerClient.IsConnected() ||
                    !ShouldShowBrokerCandidateWindow())
                    UpdateCandidateWindowPosition(pContext);
            }
            if (m_brokerClient.IsConnected() &&
                ShouldShowBrokerCandidateWindow())
                SyncBrokerCandidateState();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    // 右方向键 - 选择下一个候选词
    if (wParam == VK_RIGHT)
    {
        if (m_bInComposition && !m_compositionText.empty() && m_candidateWindow.get_candidate_count() > 0)
        {
            int currentSelection = m_candidateWindow.get_selection();
            int currentPage = m_candidateWindow.get_current_page();
            int pageSize = m_candidateWindow.get_page_size();
            int totalPages = m_candidateWindow.get_total_pages();
            
            // 计算当前页有多少个候选词
            int startIdx = currentPage * pageSize;
            int endIdx = min(startIdx + pageSize, m_candidateWindow.get_candidate_count());
            int itemsInPage = endIdx - startIdx;
            
            if (currentSelection < itemsInPage - 1)
            {
                // 在当前页内向右移动
                m_candidateWindow.set_selection(currentSelection + 1);
            }
            else if (currentPage < totalPages - 1)
            {
                // 跳到下一页的第一个候选词
                m_candidateWindow.page_down();
                m_candidateWindow.set_selection(0);
                if (!m_brokerClient.IsConnected() ||
                    !ShouldShowBrokerCandidateWindow())
                    UpdateCandidateWindowPosition(pContext);
            }
            if (m_brokerClient.IsConnected() &&
                ShouldShowBrokerCandidateWindow())
                SyncBrokerCandidateState();
            *pfEaten = TRUE;
        }
        return S_OK;
    }

    return S_OK;
}

STDAPI text_service::OnTestKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
    *pfEaten = FALSE;

    // 某些终端/宿主下可能不稳定触发 OnKeyUp，故在 OnTestKeyUp 提前执行一次切换。
    if (is_shift_vk(wParam) && m_bShiftPressed)
    {
        if (!m_bOtherKeyPressed && !m_bShiftPressedWithModifier)
        {
            const bool target_chinese_mode = !m_bChineseMode;
            if (!target_chinese_mode)
            {
                CommitCompositionCodeAndClear(pContext);
            }
            m_bChineseMode = !m_bChineseMode;
            SyncPunctuationModeWithLanguageMode();
            UpdateStatusWindow();
            *pfEaten = TRUE;
        }
        clear_shift_toggle_state(m_bShiftPressed, m_bOtherKeyPressed, m_bShiftPressedWithModifier);
    }
    return S_OK;
}

STDAPI text_service::OnKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten)
{
    *pfEaten = FALSE;
    
    // 处理Shift键释放事件
    if (is_shift_vk(wParam) && m_bShiftPressed)
    {
        // 只有在Shift单独按下（没有其他键）时才切换
        if (!m_bOtherKeyPressed && !m_bShiftPressedWithModifier)
        {
            const bool target_chinese_mode = !m_bChineseMode;
            if (!target_chinese_mode)
            {
                // 切换到英文前，直接上屏当前编码并清空候选
                CommitCompositionCodeAndClear(pContext);
            }

            m_bChineseMode = !m_bChineseMode;
            SyncPunctuationModeWithLanguageMode();
             
            UpdateStatusWindow();
            *pfEaten = TRUE;
        }
        
        clear_shift_toggle_state(m_bShiftPressed, m_bOtherKeyPressed, m_bShiftPressedWithModifier);
    }
    
    return S_OK;
}

STDAPI text_service::OnPreservedKey(ITfContext *pContext, REFGUID rguid, BOOL *pfEaten)
{
    *pfEaten = FALSE;
    return S_OK;
}

void text_service::HandleCharacter(ITfContext *pContext, WCHAR wch)
{
    ZIME_PERF_SCOPE("tip.HandleCharacter",
                    static_cast<std::int64_t>(m_compositionText.size()),
                    m_brokerClient.IsConnected() ? 1 : 0);
    const bool is_code_letter =
        (wch >= L'a' && wch <= L'z') ||
        (wch >= L'A' && wch <= L'Z');
    if (is_code_letter &&
        m_pendingCandidateAction != pending_candidate_action::none)
    {
        m_pendingCodeCharacters.push_back(wch);
        return;
    }
    if (is_code_letter &&
        m_brokerClient.IsConnected() &&
        (m_autoCommitFourCodeUnique || m_commitFirstCandidateOnFifthCode) &&
        m_bInComposition &&
        m_compositionText.length() == 4 &&
        m_brokerCandidateCode != m_compositionText)
    {
        if (!ResolveBrokerCandidatesSynchronously(true))
        {
            m_pendingCodeCharacters.push_back(wch);
            return;
        }
    }
    bool defer_context_sync_once = false;
    if (m_brokerCommitFirstOnNextCode &&
        is_code_letter &&
        m_bInComposition &&
        m_compositionText.length() == 4 &&
        m_brokerCandidateCode == m_compositionText &&
        m_candidateWindow.get_candidate_count() > 0)
    {
        // 第5码输入时先上屏首候选，再把当前按键作为下一轮的第1码处理。
        // 兼容部分宿主（如钉钉/QQ）在同一按键里 commit+start composition 时出现重复落字，
        // 这里将“新一轮组合写入上下文”延迟到下一次按键。
        CommitFirstCandidateOrComposition(pContext);
        defer_context_sync_once = true;
    }

    m_compositionText += wch;
    m_bInComposition = TRUE;
    m_brokerCandidateLayoutReady = false;
    
    // 更新组合字符串显示
    m_candidateWindow.set_composition_text(m_compositionText);
    if (!defer_context_sync_once)
        UpdateCompositionInContext(pContext);

    if (is_code_letter &&
        m_compositionText.length() == 4 &&
        m_brokerClient.IsConnected() &&
        (m_autoCommitFourCodeUnique || m_commitFirstCandidateOnFifthCode) &&
        ResolveBrokerCandidatesSynchronously(true))
    {
        return;
    }
    
    m_candidateWindow.set_candidates(std::vector<std::wstring>());
    ShowCandidates(pContext);
    RequestBrokerCandidates(true);
}

void text_service::HandleBackspace(ITfContext *pContext)
{
    m_pendingCandidateAction = pending_candidate_action::none;
    m_pendingCandidateActionCode.clear();
    m_pendingCandidateNumber = 0;
    if (!m_pendingCodeCharacters.empty())
    {
        m_pendingCodeCharacters.pop_back();
        return;
    }
    if (!m_compositionText.empty())
    {
        m_compositionText.pop_back();
        m_brokerCandidateLayoutReady = false;
        
        if (m_compositionText.empty())
        {
            CancelCompositionInContext(pContext);
            ClearComposition();
            HideCandidates();
        }
        else
        {
            // 更新组合字符串显示
            m_candidateWindow.set_composition_text(m_compositionText);
            UpdateCompositionInContext(pContext);
            
            m_candidateWindow.set_candidates(std::vector<std::wstring>());
            ShowCandidates(pContext);
            RequestBrokerCandidates();
        }
    }
}

void text_service::HandleSpace(ITfContext *pContext)
{
    if (m_bInComposition &&
        m_brokerClient.IsConnected() &&
        m_brokerCandidateCode != m_compositionText)
    {
        if (!ResolveBrokerCandidatesSynchronously(false))
        {
            m_pendingCandidateAction = pending_candidate_action::space;
            m_pendingCandidateActionCode = m_compositionText;
            m_pendingCandidateNumber = 0;
            return;
        }
        if (!m_bInComposition)
            return;
    }
    if (m_bInComposition && m_candidateWindow.get_candidate_count() > 0)
    {
        // 插入第一个候选词
        HandleNumber(pContext, 1);
    }
    else if (m_bInComposition)
    {
        // 如果没有候选词，插入原始输入
        InsertText(pContext, m_compositionText);
        ClearComposition();
        HideCandidates();
    }
}

void text_service::HandleNumber(ITfContext *pContext, int num)
{
    if (m_bInComposition &&
        num > 0 &&
        m_brokerClient.IsConnected() &&
        m_brokerCandidateCode != m_compositionText)
    {
        if (!ResolveBrokerCandidatesSynchronously(false))
        {
            m_pendingCandidateAction = pending_candidate_action::number;
            m_pendingCandidateActionCode = m_compositionText;
            m_pendingCandidateNumber = num;
            return;
        }
        if (!m_bInComposition)
            return;
    }
    if (m_bInComposition && num > 0 && num <= m_candidateWindow.get_candidate_count())
    {
        // 计算当前页中对应数字键的实际索引
        int currentPage = m_candidateWindow.get_current_page();
        int pageSize = m_candidateWindow.get_page_size();
        int actualIndex = currentPage * pageSize + (num - 1);
        const std::wstring code_snapshot = m_compositionText;
        
        // 直接从窗口获取原始候选词（不需要二次查询）
        std::wstring candidate = m_candidateWindow.get_candidate(actualIndex);
        std::wstring display_candidate = m_candidateWindow.get_display_candidate(actualIndex);
        
        if (!candidate.empty())
        {
            RecordCandidateSelection(code_snapshot, candidate, display_candidate);
            InsertText(pContext, candidate);
            ClearComposition();
            HideCandidates();
        }
    }
}

void text_service::CommitFirstCandidateOrComposition(ITfContext *pContext)
{
    if (!m_bInComposition || m_compositionText.empty())
        return;

    std::wstring commit_text;
    const std::wstring code_snapshot = m_compositionText;
    if (m_candidateWindow.get_candidate_count() > 0)
    {
        commit_text = m_candidateWindow.get_candidate(0);
        const std::wstring display_candidate = m_candidateWindow.get_display_candidate(0);
        if (!commit_text.empty())
            RecordCandidateSelection(code_snapshot, commit_text, display_candidate);
    }

    if (commit_text.empty())
    {
        commit_text = m_compositionText;
    }

    InsertText(pContext, commit_text);
    CancelCompositionInContext(pContext);
    ClearComposition();
    HideCandidates();
}

bool text_service::IsCurrentFirstCandidatePinyin()
{
    return m_bInComposition &&
        m_brokerCandidateCode == m_compositionText &&
        m_candidateWindow.get_candidate_count() > 0 &&
        m_firstCandidateIsPinyin;
}

std::wstring text_service::ConvertToFullWidth(const std::wstring& text)
{
    std::wstring result;
    for (wchar_t ch : text)
    {
        // 转换 ASCII 字符到全角（空格除外，特殊处理）
        if (ch == L' ')
        {
            result += L'\u3000';  // 全角空格
        }
        else if (ch >= L'!' && ch <= L'~')
        {
            // ASCII 可打印字符范围 (0x21-0x7E) 转换为全角 (0xFF01-0xFF5E)
            result += static_cast<wchar_t>(ch - L'!' + L'\uFF01');
        }
        else
        {
            result += ch;  // 其他字符保持不变
        }
    }
    return result;
}

std::wstring text_service::ConvertPunctuation(const std::wstring& text)
{
    // 英文标点 -> 中文标点映射表
    static const std::map<wchar_t, wchar_t> punctuation_map = {
        {L',', L'\uFF0C'},  // 逗号 , -> ，
        {L'.', L'\u3002'},  // 句号 . -> 。
        {L';', L'\uFF1B'},  // 分号 ; -> ；
        {L':', L'\uFF1A'},  // 冒号 : -> ：
        {L'?', L'\uFF1F'},  // 问号 ? -> ？
        {L'!', L'\uFF01'},  // 感叹号 ! -> ！
        {L'(', L'\uFF08'},  // 左括号 ( -> （
        {L')', L'\uFF09'},  // 右括号 ) -> ）
        {L'"', L'\u201C'},  // 双引号 " -> " (开)
        {L'\'', L'\u2018'}, // 单引号 ' -> ' (开)
        {L'[', L'\u3010'},  // 左方括号 [ -> 【
        {L']', L'\u3011'},  // 右方括号 ] -> 】
        {L'<', L'\u300A'},  // 左尖括号 < -> 《
        {L'>', L'\u300B'},  // 右尖括号 > -> 》
    };
    
    std::wstring result;
    for (wchar_t ch : text)
    {
        auto it = punctuation_map.find(ch);
        if (it != punctuation_map.end())
        {
            result += it->second;
        }
        else
        {
            result += ch;
        }
    }
    return result;
}

std::wstring text_service::ProcessTextBeforeInsert(const std::wstring& text)
{
    std::wstring result = text;
    
    // 1. 如果是中文标点模式，转换标点符号
    if (m_bChinesePunctuation)
    {
        result = ConvertPunctuation(result);
    }
    
    // 2. 如果是全角模式，转换为全角字符
    if (m_bFullWidth)
    {
        result = ConvertToFullWidth(result);
    }
    
    return result;
}

