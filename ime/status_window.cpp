#include "status_window.h"
#include <windowsx.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <memory>
#include <filesystem>
#include <array>

#include "tool.h"
#include "../common/default_settings.h"

constexpr auto STATUSWINDOW_CLASS = L"SimpleTSFStatusWindow";
constexpr auto STATUS_FONT_SLIDER_WINDOW_CLASS = L"SimpleTSFFontSliderWindow";
constexpr int IDC_FONT_SLIDER_TRACK = 2601;
constexpr UINT WM_STATUSWINDOW_RESTORE_VISIBILITY = WM_APP + 201;

namespace
{
UINT resolve_window_dpi(HWND hwnd)
{
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (pGetDpiForWindow && hwnd)
        return pGetDpiForWindow(hwnd);
    return 96;
}

std::filesystem::path locate_icon_path(std::initializer_list<const wchar_t*> names)
{
    const std::filesystem::path dll_dir = tool::get_current_dll_path();
    const std::filesystem::path cwd = std::filesystem::current_path();
    std::array<std::filesystem::path, 5> roots = {
        dll_dir,
        dll_dir.parent_path(),
        dll_dir.parent_path().parent_path(),
        dll_dir.parent_path().parent_path().parent_path(),
        cwd
    };
    const std::array<std::filesystem::path, 2> icon_directories = {
        std::filesystem::path(L"ico"),
        std::filesystem::path(L"assets") / L"status-icons"
    };
    for (const auto& name : names)
    {
        for (const auto& root : roots)
        {
            for (const auto& icon_directory : icon_directories)
            {
                std::error_code ec;
                const auto p = root / icon_directory / name;
                if (std::filesystem::exists(p, ec))
                    return p;
            }
        }
    }
    return {};
}

enum class StatusIconId : size_t
{
    Main = 0,
    Full,
    Half,
    Chinese,
    English,
    ChinesePunctuation,
    EnglishPunctuation,
    Settings,
    Count
};

}

struct status_window_graphics
{
    ~status_window_graphics()
    {
        for (auto& icon : icons)
            icon.reset();
        if (token != 0)
            Gdiplus::GdiplusShutdown(token);
    }

    bool ensure_started()
    {
        if (token != 0)
            return true;
        Gdiplus::GdiplusStartupInput input;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }

    ULONG_PTR token = 0;
    std::array<std::unique_ptr<Gdiplus::Image>,
               static_cast<size_t>(StatusIconId::Count)> icons;
    std::array<bool, static_cast<size_t>(StatusIconId::Count)> tried = {};
};

namespace
{

Gdiplus::Image* load_status_icon(status_window_graphics* graphics,
                                 StatusIconId id)
{
    if (!graphics)
        return nullptr;
    const size_t idx = static_cast<size_t>(id);
    if (graphics->tried[idx])
        return graphics->icons[idx].get();
    graphics->tried[idx] = true;

    std::filesystem::path path;
    switch (id)
    {
    case StatusIconId::Main:
        path = locate_icon_path({ L"main.png" });
        break;
    case StatusIconId::Full:
        path = locate_icon_path({ L"full_width.png", L"full_wdith.png" });
        break;
    case StatusIconId::Half:
        path = locate_icon_path({ L"half_width.png" });
        break;
    case StatusIconId::Chinese:
        path = locate_icon_path({ L"cn.png", L"ch.png" });
        break;
    case StatusIconId::English:
        path = locate_icon_path({ L"en.png", L"en.eng" });
        break;
    case StatusIconId::ChinesePunctuation:
        path = locate_icon_path({ L"cn_pun.png" });
        break;
    case StatusIconId::EnglishPunctuation:
        path = locate_icon_path({ L"en_pun.png" });
        break;
    case StatusIconId::Settings:
        path = locate_icon_path({ L"settings.png" });
        break;
    default:
        break;
    }
    if (path.empty())
        return nullptr;
    if (!graphics->ensure_started())
        return nullptr;

    auto image = std::make_unique<Gdiplus::Image>(path.c_str());
    if (image->GetLastStatus() != Gdiplus::Ok)
        return nullptr;

    graphics->icons[idx] = std::move(image);
    return graphics->icons[idx].get();
}

void draw_icon_aspect_fit(Gdiplus::Graphics& g, Gdiplus::Image* icon, const RECT& slot, int pad_px)
{
    if (!icon)
        return;

    const int slot_w = slot.right - slot.left;
    const int slot_h = slot.bottom - slot.top;
    const int max_w = slot_w - pad_px * 2;
    const int max_h = slot_h - pad_px * 2;
    if (max_w <= 0 || max_h <= 0)
        return;

    const UINT src_w = icon->GetWidth();
    const UINT src_h = icon->GetHeight();
    if (src_w == 0 || src_h == 0)
        return;

    const double scale_w = static_cast<double>(max_w) / static_cast<double>(src_w);
    const double scale_h = static_cast<double>(max_h) / static_cast<double>(src_h);
    const double scale = min(scale_w, scale_h);
    if (scale <= 0.0)
        return;

    int draw_w = static_cast<int>(static_cast<double>(src_w) * scale + 0.5);
    int draw_h = static_cast<int>(static_cast<double>(src_h) * scale + 0.5);
    if (draw_w < 1)
        draw_w = 1;
    if (draw_h < 1)
        draw_h = 1;

    const int x = slot.left + (slot_w - draw_w) / 2;
    const int y = slot.top + (slot_h - draw_h) / 2;
    g.DrawImage(icon, x, y, draw_w, draw_h);
}
}

bool status_window::class_registered = false;

status_window::status_window()
{
    m_hWnd = nullptr;
    is_full_width_ = zime::default_settings::full_width;
    is_chinese_mode_ = zime::default_settings::chinese_mode;
    is_chinese_punctuation_ = zime::default_settings::chinese_punctuation;
    is_auto_commit_four_code_unique_ =
        zime::default_settings::auto_commit_four_code_unique;
    is_commit_first_candidate_on_fifth_code_ =
        zime::default_settings::commit_first_candidate_on_fifth_code;
    is_show_uncommon_candidates_ =
        zime::default_settings::show_uncommon_candidates;
    is_replace_dot_after_digit_ =
        zime::default_settings::replace_dot_after_digit;
    is_use_english_punctuation_in_chinese_mode_ =
        zime::default_settings::use_english_punctuation_in_chinese_mode;
    is_disable_chinese_dash_ = zime::default_settings::disable_chinese_dash;
    candidate_sort_mode_ = static_cast<CandidateSortMode>(
        zime::default_settings::candidate_sort_mode);
    hover_button_ = -1;
    status_change_callback_ = nullptr;
    ui_font_percent_ = static_cast<int>(
        zime::default_settings::ui_font_percent);
    position_changed_callback_ = nullptr;
    is_dragging_ = false;
    drag_start_cursor_ = { 0, 0 };
    drag_start_window_ = { 0, 0 };
    m_hFontSliderWnd = nullptr;
    m_hFontSliderTrack = nullptr;
}

status_window::~status_window()
{
    destroy();
}

void status_window::register_window_class()
{
    if (class_registered)
        return;

    WNDCLASSEX wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WindowProc;
    wcex.hInstance = g_hInst;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = STATUSWINDOW_CLASS;

    if (RegisterClassEx(&wcex))
    {
        class_registered = true;
    }
}

BOOL status_window::create(HWND hWndParent)
{
    if (!graphics_)
        graphics_ = std::make_unique<status_window_graphics>();
    register_window_class();
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    int window_width = 0;
    int window_height = 0;
    recalc_window_size(window_width, window_height);
    
    m_hWnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        STATUSWINDOW_CLASS,
        nullptr,
        WS_POPUP | WS_BORDER,
        0, 0, window_width, window_height,
        hWndParent,
        nullptr,
        g_hInst,
        this);

    if (!m_hWnd)
        graphics_.reset();
    return (m_hWnd != nullptr);
}

void status_window::destroy()
{
    if (m_hFontSliderWnd && IsWindow(m_hFontSliderWnd))
    {
        DestroyWindow(m_hFontSliderWnd);
    }
    m_hFontSliderWnd = nullptr;
    m_hFontSliderTrack = nullptr;
    if (m_hWnd && IsWindow(m_hWnd))
    {
        DestroyWindow(m_hWnd);
    }
    m_hWnd = nullptr;
    graphics_.reset();
}

void status_window::show(bool bShow) const
{
    if (m_hWnd)
    {
        ShowWindow(m_hWnd, bShow ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void status_window::restore_async() const
{
    if (m_hWnd && IsWindow(m_hWnd))
    {
        PostMessageW(m_hWnd, WM_STATUSWINDOW_RESTORE_VISIBILITY, 0, 0);
    }
}

void status_window::move(int x, int y) const
{
    if (m_hWnd)
    {
        int window_width = 0;
        int window_height = 0;
        recalc_window_size(window_width, window_height);

        RECT proposed = {x, y, x + window_width, y + window_height};
        HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL);
        if (!monitor)
        {
            const HWND owner = GetWindow(m_hWnd, GW_OWNER);
            monitor = MonitorFromWindow(owner ? owner : m_hWnd,
                                        MONITOR_DEFAULTTONEAREST);
        }

        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info))
        {
            const RECT& work = info.rcWork;
            if (window_width >= work.right - work.left)
                x = work.left;
            else
                x = max(work.left, min(x, work.right - window_width));
            if (window_height >= work.bottom - work.top)
                y = work.top;
            else
                y = max(work.top, min(y, work.bottom - window_height));
        }

        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, window_width, window_height, SWP_NOACTIVATE);
    }
}

void status_window::toggle_full_width()
{
    is_full_width_ = !is_full_width_;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::toggle_chinese_mode()
{
    is_chinese_mode_ = !is_chinese_mode_;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::toggle_punctuation()
{
    is_chinese_punctuation_ = !is_chinese_punctuation_;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::on_settings_click()
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    constexpr UINT ID_MENU_AUTO_COMMIT_FOUR_UNIQUE = 1001;
    constexpr UINT ID_MENU_CREATE_WORD = 1002;
    constexpr UINT ID_MENU_SHOW_UNCOMMON = 1003;
    constexpr UINT ID_MENU_EXPORT_RAW_DICT = 1004;
    constexpr UINT ID_MENU_UI_FONT_SLIDER = 1005;
    constexpr UINT ID_MENU_REPLACE_DOT_AFTER_DIGIT = 1006;
    constexpr UINT ID_MENU_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE = 1007;
    constexpr UINT ID_MENU_SORT_FIXED_ORDER = 1008;
    constexpr UINT ID_MENU_SORT_RECENT = 1009;
    constexpr UINT ID_MENU_SORT_FREQUENCY = 1010;
    constexpr UINT ID_MENU_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE = 1011;
    constexpr UINT ID_MENU_DISABLE_CHINESE_DASH = 1012;

    HMENU hSortMenu = CreatePopupMenu();
    if (hSortMenu)
    {
        AppendMenuW(hSortMenu, MF_STRING, ID_MENU_SORT_FIXED_ORDER, L"固定词序");
        AppendMenuW(hSortMenu, MF_STRING, ID_MENU_SORT_RECENT, L"按最近输入排序");
        AppendMenuW(hSortMenu, MF_STRING, ID_MENU_SORT_FREQUENCY, L"按输入次数排序");
        UINT checked_id = ID_MENU_SORT_FREQUENCY;
        if (candidate_sort_mode_ == CandidateSortMode::FixedOrder)
            checked_id = ID_MENU_SORT_FIXED_ORDER;
        else if (candidate_sort_mode_ == CandidateSortMode::Recent)
            checked_id = ID_MENU_SORT_RECENT;
        CheckMenuRadioItem(hSortMenu,
                           ID_MENU_SORT_FIXED_ORDER,
                           ID_MENU_SORT_FREQUENCY,
                           checked_id,
                           MF_BYCOMMAND);
    }

    AppendMenuW(hMenu, MF_STRING | (is_auto_commit_four_code_unique_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_AUTO_COMMIT_FOUR_UNIQUE, L"4码唯一时直接上屏");
    AppendMenuW(hMenu, MF_STRING | (is_commit_first_candidate_on_fifth_code_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE, L"第5码时上屏首个候选词");
    AppendMenuW(hMenu, MF_STRING | (is_show_uncommon_candidates_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_SHOW_UNCOMMON, L"候选不常用字词");
    AppendMenuW(hMenu, MF_STRING | (is_replace_dot_after_digit_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_REPLACE_DOT_AFTER_DIGIT, L"数字后\"。\"替换为\".\"");
    AppendMenuW(hMenu, MF_STRING | (is_use_english_punctuation_in_chinese_mode_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE, L"中文模式下使用英文标点");
    AppendMenuW(hMenu, MF_STRING | (is_disable_chinese_dash_ ? MF_CHECKED : MF_UNCHECKED),
               ID_MENU_DISABLE_CHINESE_DASH, L"不使用中文破折号");
    if (hSortMenu)
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSortMenu), L"候选词排序");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_MENU_UI_FONT_SLIDER, L"字体大小...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, ID_MENU_CREATE_WORD, L"造词");
    AppendMenu(hMenu, MF_STRING, ID_MENU_EXPORT_RAW_DICT, L"导出字典库");

    RECT rc = get_button_rect(3);
    // 让菜单在“功能”按钮上方弹出，增强关联感
    POINT pt = { rc.left, rc.top };
    ClientToScreen(m_hWnd, &pt);

    if (menu_popup_state_callback_)
        menu_popup_state_callback_(true);

    UINT cmd = TrackPopupMenu(hMenu,
                              TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                              pt.x, pt.y, 0, m_hWnd, nullptr);
    PostMessageW(m_hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (menu_popup_state_callback_)
        menu_popup_state_callback_(false);

    // 菜单关闭后确保状态栏仍可见（不激活窗口）
    ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (cmd == ID_MENU_AUTO_COMMIT_FOUR_UNIQUE)
    {
        is_auto_commit_four_code_unique_ = !is_auto_commit_four_code_unique_;
        if (status_change_callback_)
            status_change_callback_(STATUS_AUTO_COMMIT_FOUR_UNIQUE);
    }
    else if (cmd == ID_MENU_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE)
    {
        is_commit_first_candidate_on_fifth_code_ = !is_commit_first_candidate_on_fifth_code_;
        if (status_change_callback_)
            status_change_callback_(STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE);
    }
    else if (cmd == ID_MENU_SHOW_UNCOMMON)
    {
        is_show_uncommon_candidates_ = !is_show_uncommon_candidates_;
        if (status_change_callback_)
            status_change_callback_(STATUS_SHOW_UNCOMMON_CANDIDATES);
    }
    else if (cmd == ID_MENU_REPLACE_DOT_AFTER_DIGIT)
    {
        is_replace_dot_after_digit_ = !is_replace_dot_after_digit_;
        if (status_change_callback_)
            status_change_callback_(STATUS_REPLACE_DOT_AFTER_DIGIT);
    }
    else if (cmd == ID_MENU_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE)
    {
        is_use_english_punctuation_in_chinese_mode_ = !is_use_english_punctuation_in_chinese_mode_;
        if (status_change_callback_)
            status_change_callback_(STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE);
    }
    else if (cmd == ID_MENU_DISABLE_CHINESE_DASH)
    {
        is_disable_chinese_dash_ = !is_disable_chinese_dash_;
        if (status_change_callback_)
            status_change_callback_(STATUS_DISABLE_CHINESE_DASH);
    }
    else if (cmd == ID_MENU_SORT_FIXED_ORDER ||
             cmd == ID_MENU_SORT_RECENT ||
             cmd == ID_MENU_SORT_FREQUENCY)
    {
        CandidateSortMode next_mode = CandidateSortMode::Frequency;
        if (cmd == ID_MENU_SORT_FIXED_ORDER)
            next_mode = CandidateSortMode::FixedOrder;
        else if (cmd == ID_MENU_SORT_RECENT)
            next_mode = CandidateSortMode::Recent;

        if (candidate_sort_mode_ != next_mode)
        {
            candidate_sort_mode_ = next_mode;
            if (status_change_callback_)
                status_change_callback_(STATUS_CANDIDATE_SORT_MODE_CHANGED);
        }
    }
    else if (cmd == ID_MENU_CREATE_WORD)
    {
        if (status_change_callback_)
            status_change_callback_(STATUS_SETTINGS);
    }
    else if (cmd == ID_MENU_EXPORT_RAW_DICT)
    {
        if (status_change_callback_)
            status_change_callback_(STATUS_EXPORT_RAW_DICT);
    }
    else if (cmd == ID_MENU_UI_FONT_SLIDER)
    {
        show_font_slider_window();
    }
}

void status_window::set_full_width(bool full_width)
{
    is_full_width_ = full_width;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::set_chinese_mode(bool chinese_mode)
{
    is_chinese_mode_ = chinese_mode;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::set_chinese_punctuation(bool chinese_punctuation)
{
    is_chinese_punctuation_ = chinese_punctuation;
    if (m_hWnd)
    {
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
}

void status_window::set_auto_commit_four_code_unique(bool enabled)
{
    is_auto_commit_four_code_unique_ = enabled;
}

void status_window::set_commit_first_candidate_on_fifth_code(bool enabled)
{
    is_commit_first_candidate_on_fifth_code_ = enabled;
}

void status_window::set_show_uncommon_candidates(bool enabled)
{
    is_show_uncommon_candidates_ = enabled;
}

void status_window::set_replace_dot_after_digit(bool enabled)
{
    is_replace_dot_after_digit_ = enabled;
}

void status_window::set_use_english_punctuation_in_chinese_mode(bool enabled)
{
    is_use_english_punctuation_in_chinese_mode_ = enabled;
}

void status_window::set_disable_chinese_dash(bool enabled)
{
    is_disable_chinese_dash_ = enabled;
}

void status_window::set_candidate_sort_mode(CandidateSortMode mode)
{
    candidate_sort_mode_ = mode;
}

void status_window::set_ui_font_percent(int percent)
{
    if (percent < 80)
        percent = 80;
    if (percent > 250)
        percent = 250;
    ui_font_percent_ = percent;
    if (m_hWnd)
    {
        int width = 0;
        int height = 0;
        recalc_window_size(width, height);
        SetWindowPos(m_hWnd, nullptr, 0, 0, width, height,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }
    sync_font_slider_pos();
}

void status_window::get_window_size(int& width, int& height) const
{
    recalc_window_size(width, height);
}

int status_window::scale_px(int base_px) const
{
    const UINT dpi = get_dpi();
    // 状态栏大小固定，不受“界面字体大小”滑块影响；滑块仅用于候选窗口字体。
    return MulDiv(base_px * 100, static_cast<int>(dpi), 96 * 100);
}

UINT status_window::get_dpi() const
{
    return resolve_window_dpi(m_hWnd);
}

void status_window::recalc_window_size(int& width, int& height) const
{
    width = scale_px(MAIN_ICON_SLOT_WIDTH) + scale_px(2) +
            scale_px(BUTTON_WIDTH) * BUTTON_COUNT +
            scale_px(BUTTON_SPACING) * (BUTTON_COUNT - 1) + scale_px(4);
    height = scale_px(BUTTON_HEIGHT) + scale_px(4);
}

RECT status_window::get_button_rect(int button_index) const
{
    RECT rc;
    const int margin = scale_px(2);
    const int button_width = scale_px(BUTTON_WIDTH);
    const int button_height = scale_px(BUTTON_HEIGHT);
    const int spacing = scale_px(BUTTON_SPACING);
    const int icon_slot = scale_px(MAIN_ICON_SLOT_WIDTH);
    rc.left = margin + icon_slot + scale_px(2) + button_index * (button_width + spacing);
    rc.top = margin;
    rc.right = rc.left + button_width;
    rc.bottom = rc.top + button_height;
    return rc;
}

int status_window::hit_test(int x, int y) const
{
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        RECT rc = get_button_rect(i);
        if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom)
        {
            return i;
        }
    }
    return -1;
}

void status_window::on_lbutton_down(int x, int y)
{
    const int button = hit_test(x, y);
    if (button < 0)
    {
        is_dragging_ = true;
        drag_start_cursor_.x = x;
        drag_start_cursor_.y = y;
        ClientToScreen(m_hWnd, &drag_start_cursor_);
        RECT rc = {};
        GetWindowRect(m_hWnd, &rc);
        drag_start_window_.x = rc.left;
        drag_start_window_.y = rc.top;
        SetCapture(m_hWnd);
        return;
    }
    
    switch (button)
    {
    case 0: // 中/英文
        toggle_chinese_mode();
        if (status_change_callback_)
            status_change_callback_(STATUS_CHINESE_MODE);
        break;
    case 1: // 中/英文标点
        toggle_punctuation();
        if (status_change_callback_)
            status_change_callback_(STATUS_PUNCTUATION);
        break;
    case 2: // 全/半角
        toggle_full_width();
        if (status_change_callback_)
            status_change_callback_(STATUS_FULL_WIDTH);
        break;
    case 3: // 功能按钮
        on_settings_click();
        break;
    default:
        break;
    }
}

void status_window::show_font_slider_window()
{
    if (m_hFontSliderWnd && IsWindow(m_hFontSliderWnd))
    {
        sync_font_slider_pos();
        ShowWindow(m_hFontSliderWnd, SW_SHOWNORMAL);
        SetWindowPos(m_hFontSliderWnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(m_hFontSliderWnd);
        return;
    }

    static bool slider_class_registered = false;
    if (!slider_class_registered)
    {
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(wcex);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = FontSliderWindowProc;
        wcex.hInstance = g_hInst;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wcex.lpszClassName = STATUS_FONT_SLIDER_WINDOW_CLASS;
        slider_class_registered = RegisterClassExW(&wcex) != 0;
    }

    RECT rc = {};
    GetWindowRect(m_hWnd, &rc);
    const int width = scale_px(260);
    const int height = scale_px(90);
    m_hFontSliderWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        STATUS_FONT_SLIDER_WINDOW_CLASS,
        L"界面字体大小",
        WS_POPUP | WS_BORDER | WS_CAPTION,
        rc.left, rc.top - height - scale_px(6), width, height,
        m_hWnd,
        nullptr,
        g_hInst,
        this);

    if (m_hFontSliderWnd)
    {
        ShowWindow(m_hFontSliderWnd, SW_SHOWNORMAL);
        UpdateWindow(m_hFontSliderWnd);
        SetForegroundWindow(m_hFontSliderWnd);
    }
}

void status_window::sync_font_slider_pos() const
{
    if (m_hFontSliderTrack && IsWindow(m_hFontSliderTrack))
    {
        SendMessageW(m_hFontSliderTrack, TBM_SETPOS, TRUE, static_cast<LPARAM>(ui_font_percent_));
        InvalidateRect(m_hFontSliderWnd, nullptr, TRUE);
    }
}

LRESULT CALLBACK status_window::FontSliderWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    status_window* pThis = reinterpret_cast<status_window*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (uMsg)
    {
    case WM_CREATE:
    {
        auto pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        pThis = static_cast<status_window*>(pcs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        if (!pThis)
            return -1;

        const int margin = pThis->scale_px(12);
        const int track_y = pThis->scale_px(30);
        const int track_w = pThis->scale_px(230);
        const int track_h = pThis->scale_px(26);
        pThis->m_hFontSliderTrack = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            margin, track_y, track_w, track_h,
            hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_SLIDER_TRACK)),
            g_hInst,
            nullptr);
        SendMessageW(pThis->m_hFontSliderTrack, TBM_SETRANGEMIN, TRUE, 80);
        SendMessageW(pThis->m_hFontSliderTrack, TBM_SETRANGEMAX, TRUE, 250);
        SendMessageW(pThis->m_hFontSliderTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(pThis->m_hFontSliderTrack, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(pThis->m_hFontSliderTrack, TBM_SETPOS, TRUE, pThis->ui_font_percent_);
        return 0;
    }
    case WM_HSCROLL:
        if (pThis && reinterpret_cast<HWND>(lParam) == pThis->m_hFontSliderTrack)
        {
            const int pos = static_cast<int>(SendMessageW(pThis->m_hFontSliderTrack, TBM_GETPOS, 0, 0));
            pThis->set_ui_font_percent(pos);
            if (pThis->status_change_callback_)
                pThis->status_change_callback_(STATUS_UI_FONT_CHANGED);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_PAINT:
        if (pThis)
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc = {};
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(60, 72, 88));
            HFONT font = CreateFontW(pThis->scale_px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
            RECT label = { pThis->scale_px(12), pThis->scale_px(8), rc.right - pThis->scale_px(12), pThis->scale_px(24) };
            wchar_t text[64] = {};
            swprintf_s(text, L"字号缩放: %d%%", pThis->ui_font_percent_);
            DrawTextW(hdc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            DeleteObject(font);
            EndPaint(hWnd, &ps);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (pThis)
        {
            pThis->m_hFontSliderTrack = nullptr;
            pThis->m_hFontSliderWnd = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK status_window::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    status_window* pThis;

    if (uMsg == WM_CREATE)
    {
        auto pcs = (LPCREATESTRUCT)lParam;
        pThis = static_cast<status_window*>(pcs->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    }
    else
    {
        pThis = (status_window*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }

    switch (uMsg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_STATUSWINDOW_RESTORE_VISIBILITY:
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            if (pThis)
            {
                RECT client = {};
                GetClientRect(hWnd, &client);
                const int w = client.right - client.left;
                const int h = client.bottom - client.top;
                HDC memdc = CreateCompatibleDC(hdc);
                HBITMAP membmp = (w > 0 && h > 0) ? CreateCompatibleBitmap(hdc, w, h) : nullptr;
                HGDIOBJ oldbmp = (memdc && membmp) ? SelectObject(memdc, membmp) : nullptr;
                if (memdc && membmp && oldbmp)
                {
                    pThis->on_paint(memdc);
                    BitBlt(hdc,
                           ps.rcPaint.left, ps.rcPaint.top,
                           ps.rcPaint.right - ps.rcPaint.left,
                           ps.rcPaint.bottom - ps.rcPaint.top,
                           memdc,
                           ps.rcPaint.left, ps.rcPaint.top,
                           SRCCOPY);
                    SelectObject(memdc, oldbmp);
                    DeleteObject(membmp);
                    DeleteDC(memdc);
                }
                else
                {
                    if (oldbmp)
                        SelectObject(memdc, oldbmp);
                    if (membmp)
                        DeleteObject(membmp);
                    if (memdc)
                        DeleteDC(memdc);
                    pThis->on_paint(hdc);
                }
            }
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_LBUTTONDOWN:
        {
            if (pThis)
            {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                pThis->on_lbutton_down(x, y);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        {
            if (pThis)
            {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                if (pThis->is_dragging_)
                {
                    POINT pt = { x, y };
                    ClientToScreen(hWnd, &pt);
                    const int dx = pt.x - pThis->drag_start_cursor_.x;
                    const int dy = pt.y - pThis->drag_start_cursor_.y;
                    const int new_x = pThis->drag_start_window_.x + dx;
                    const int new_y = pThis->drag_start_window_.y + dy;
                    SetWindowPos(hWnd, HWND_TOPMOST, new_x, new_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                    if (pThis->position_changed_callback_)
                        pThis->position_changed_callback_(new_x, new_y);
                    return 0;
                }

                int button = pThis->hit_test(x, y);
                SetCursor(LoadCursor(nullptr, button >= 0 ? IDC_HAND : IDC_ARROW));
                if (button != pThis->hover_button_)
                {
                    RECT dirty = { 0, 0, 0, 0 };
                    bool has_dirty = false;
                    const int old_button = pThis->hover_button_;
                    pThis->hover_button_ = button;
                    if (old_button >= 0)
                    {
                        const RECT old_rc = pThis->get_button_rect(old_button);
                        dirty = old_rc;
                        has_dirty = true;
                    }
                    if (button >= 0)
                    {
                        const RECT new_rc = pThis->get_button_rect(button);
                        if (has_dirty)
                        {
                            RECT merged = {};
                            UnionRect(&merged, &dirty, &new_rc);
                            dirty = merged;
                        }
                        else
                        {
                            dirty = new_rc;
                            has_dirty = true;
                        }
                    }
                    if (has_dirty)
                        InvalidateRect(hWnd, &dirty, FALSE);
                }
                
                // 设置鼠标跟踪，以便能够接收 WM_MOUSELEAVE
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hWnd;
                TrackMouseEvent(&tme);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (pThis && pThis->is_dragging_)
        {
            pThis->is_dragging_ = false;
            ReleaseCapture();
            return 0;
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (pThis)
            pThis->is_dragging_ = false;
        return 0;

    case WM_SETCURSOR:
        if (pThis)
        {
            if (LOWORD(lParam) != HTCLIENT)
                break;
            POINT pt = {};
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            const int button = pThis->hit_test(pt.x, pt.y);
            SetCursor(LoadCursor(nullptr, button >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_MOUSELEAVE:
        {
            if (pThis && pThis->hover_button_ != -1)
            {
                const RECT old_rc = pThis->get_button_rect(pThis->hover_button_);
                pThis->hover_button_ = -1;
                InvalidateRect(hWnd, &old_rc, FALSE);
            }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        if (pThis)
        {
            int width = 0;
            int height = 0;
            pThis->recalc_window_size(width, height);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hWnd, nullptr,
                suggested ? suggested->left : 0,
                suggested ? suggested->top : 0,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hWnd, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (pThis)
        {
            SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
            if (pThis->m_hWnd == hWnd)
            {
                pThis->m_hWnd = nullptr;
                pThis->graphics_.reset();
            }
        }
        break;
    default:
        break;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void status_window::on_paint(HDC hdc) const
{
    RECT rc;
    GetClientRect(m_hWnd, &rc);
    const int font_size = scale_px(15);
    const int round_radius = scale_px(8);

    // 背景
    const HBRUSH bg_brush = CreateSolidBrush(RGB(245, 248, 252));
    FillRect(hdc, &rc, bg_brush);
    DeleteObject(bg_brush);

    // 设置文本格式
    SetBkMode(hdc, TRANSPARENT);
    
    // 创建字体
    const HFONT font = CreateFont(font_size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    const auto old_font = static_cast<HFONT>(SelectObject(hdc, font));

    // 左侧主图标槽
    const int margin = scale_px(2);
    const int icon_slot = scale_px(MAIN_ICON_SLOT_WIDTH);
    RECT rcMain = { margin, margin, margin + icon_slot, rc.bottom - margin };
    const HBRUSH main_bg = CreateSolidBrush(RGB(255, 255, 255));
    const auto old_main_pen = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
    const auto old_main_brush = static_cast<HBRUSH>(SelectObject(hdc, main_bg));
    RoundRect(hdc, rcMain.left, rcMain.top, rcMain.right, rcMain.bottom, round_radius, round_radius);
    SelectObject(hdc, old_main_brush);
    SelectObject(hdc, old_main_pen);
    DeleteObject(main_bg);

    if (Gdiplus::Image* main_icon =
            load_status_icon(graphics_.get(), StatusIconId::Main))
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        draw_icon_aspect_fit(g, main_icon, rcMain, 0);
    }

    // 绘制按钮
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        RECT button_rc = get_button_rect(i);
        
        bool active = false;
        if (i == 0) active = is_chinese_mode_;
        if (i == 1) active = is_chinese_punctuation_;
        if (i == 2) active = is_full_width_;

        COLORREF bg_color = RGB(255, 255, 255);
        COLORREF text_color = RGB(40, 56, 75);
        if (hover_button_ == i)
        {
            bg_color = RGB(241, 246, 252);
        }

        const HBRUSH button_brush = CreateSolidBrush(bg_color);
        const auto old_pen = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
        const auto old_brush = static_cast<HBRUSH>(SelectObject(hdc, button_brush));
        RoundRect(hdc, button_rc.left, button_rc.top, button_rc.right, button_rc.bottom, round_radius, round_radius);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(button_brush);
        
        StatusIconId icon_id = StatusIconId::Settings;
        if (i == 0)
            icon_id = is_chinese_mode_ ? StatusIconId::Chinese : StatusIconId::English;
        else if (i == 1)
            icon_id = is_chinese_punctuation_ ? StatusIconId::ChinesePunctuation : StatusIconId::EnglishPunctuation;
        else if (i == 2)
            icon_id = is_full_width_ ? StatusIconId::Full : StatusIconId::Half;

        if (Gdiplus::Image* icon =
                load_status_icon(graphics_.get(), icon_id))
        {
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            draw_icon_aspect_fit(g, icon, button_rc, scale_px(2));
        }
        else
        {
            const wchar_t* fallback = L"";
            if (i == 0) fallback = is_chinese_mode_ ? L"中" : L"英";
            if (i == 1) fallback = is_chinese_punctuation_ ? L"，。" : L",.";
            if (i == 2) fallback = is_full_width_ ? L"全" : L"半";
            if (i == 3) fallback = L"设";
            SetTextColor(hdc, text_color);
            DrawText(hdc, fallback, -1, &button_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, old_font);
    DeleteObject(font);
}
