#pragma once

#include "globals.h"
#include <functional>
#include <memory>

struct status_window_graphics;

class status_window
{
public:
    enum class CandidateSortMode {
        FixedOrder = 0,
        Recent = 1,
        Frequency = 2
    };

    status_window();
    ~status_window();
    
    // 设置状态改变回调函数
    typedef std::function<void(int)> StatusChangeCallback;
    void set_status_change_callback(StatusChangeCallback callback) { status_change_callback_ = callback; }
    using MenuPopupStateCallback = std::function<void(bool)>;
    void set_menu_popup_state_callback(MenuPopupStateCallback callback) { menu_popup_state_callback_ = callback; }
    using PositionChangedCallback = std::function<void(int, int)>;
    void set_position_changed_callback(PositionChangedCallback callback) { position_changed_callback_ = callback; }

    // Foreign-thread owners are ignored to keep input queues independent.
    BOOL create(HWND hWndParent);
    void destroy();
    void show(bool bShow) const;
    void restore_async() const;
    void move(int x, int y) const;
    
    // 状态切换
    void toggle_full_width();
    void toggle_chinese_mode();
    void toggle_punctuation();
    void on_settings_click();
    
    // 状态获取
    [[nodiscard]] bool is_full_width() const { return is_full_width_; }
    [[nodiscard]] bool is_chinese_mode() const { return is_chinese_mode_; }
    [[nodiscard]] bool is_chinese_punctuation() const { return is_chinese_punctuation_; }
    [[nodiscard]] bool is_auto_commit_four_code_unique() const { return is_auto_commit_four_code_unique_; }
    [[nodiscard]] bool is_commit_first_candidate_on_fifth_code() const { return is_commit_first_candidate_on_fifth_code_; }
    [[nodiscard]] bool is_show_uncommon_candidates() const { return is_show_uncommon_candidates_; }
    [[nodiscard]] bool is_replace_dot_after_digit() const { return is_replace_dot_after_digit_; }
    [[nodiscard]] bool is_use_english_punctuation_in_chinese_mode() const { return is_use_english_punctuation_in_chinese_mode_; }
    [[nodiscard]] bool is_disable_chinese_dash() const { return is_disable_chinese_dash_; }
    [[nodiscard]] CandidateSortMode get_candidate_sort_mode() const { return candidate_sort_mode_; }
    
    // 状态设置
    void set_full_width(bool full_width);
    void set_chinese_mode(bool chinese_mode);
    void set_chinese_punctuation(bool chinese_punctuation);
    void set_auto_commit_four_code_unique(bool enabled);
    void set_commit_first_candidate_on_fifth_code(bool enabled);
    void set_show_uncommon_candidates(bool enabled);
    void set_replace_dot_after_digit(bool enabled);
    void set_use_english_punctuation_in_chinese_mode(bool enabled);
    void set_disable_chinese_dash(bool enabled);
    void set_candidate_sort_mode(CandidateSortMode mode);
    void set_ui_font_percent(int percent);
    [[nodiscard]] int get_ui_font_percent() const { return ui_font_percent_; }
    void get_window_size(int& width, int& height) const;
    
    HWND get_hwnd() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void register_window_class();
    void on_paint(HDC hdc) const;
    void on_lbutton_down(int x, int y);
    
    // 获取按钮区域
    RECT get_button_rect(int button_index) const;
    int hit_test(int x, int y) const;
    
    HWND m_hWnd;
    
    // 状态标志
    bool is_full_width_;           // 全角/半角
    bool is_chinese_mode_;         // 中文/英文
    bool is_chinese_punctuation_;  // 中英文标点
    bool is_auto_commit_four_code_unique_; // 四码唯一时直接上屏
    bool is_commit_first_candidate_on_fifth_code_; // 第5码时上屏首个候选词
    bool is_show_uncommon_candidates_; // 候选不常用字词
    bool is_replace_dot_after_digit_; // 数字后"。"替换为"."
    bool is_use_english_punctuation_in_chinese_mode_; // 中文模式时默认使用英文标点
    bool is_disable_chinese_dash_; // Shift+- 不使用中文破折号
    CandidateSortMode candidate_sort_mode_;
    
    // 按钮定义
    static constexpr int BUTTON_WIDTH = 35;
    static constexpr int BUTTON_HEIGHT = 30;
    static constexpr int BUTTON_SPACING = 2;
    static constexpr int BUTTON_COUNT = 4;
    static constexpr int MAIN_ICON_SLOT_WIDTH = 40;
    
    // 鼠标悬停状态
    int hover_button_;
    
    // 状态改变回调
    StatusChangeCallback status_change_callback_;
    MenuPopupStateCallback menu_popup_state_callback_;
    PositionChangedCallback position_changed_callback_;
    int ui_font_percent_;
    bool is_dragging_;
    POINT drag_start_cursor_;
    POINT drag_start_window_;
    HWND m_hFontSliderWnd;
    HWND m_hFontSliderTrack;
    std::unique_ptr<status_window_graphics> graphics_;
    
    static bool class_registered;
    [[nodiscard]] int scale_px(int base_px) const;
    [[nodiscard]] UINT get_dpi() const;
    void recalc_window_size(int& width, int& height) const;
    void show_font_slider_window();
    void sync_font_slider_pos() const;
    static LRESULT CALLBACK FontSliderWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

public:
    // 回调类型定义
    enum StatusChangeType {
        STATUS_FULL_WIDTH = 0,
        STATUS_CHINESE_MODE = 1,
        STATUS_PUNCTUATION = 2,
        STATUS_SETTINGS = 3,
        STATUS_AUTO_COMMIT_FOUR_UNIQUE = 4,
        STATUS_COMMIT_FIRST_CANDIDATE_ON_FIFTH_CODE = 5,
        STATUS_SHOW_UNCOMMON_CANDIDATES = 6,
        STATUS_REPLACE_DOT_AFTER_DIGIT = 7,
        STATUS_USE_ENGLISH_PUNCTUATION_IN_CHINESE_MODE = 8,
        STATUS_DISABLE_CHINESE_DASH = 9,
        STATUS_EXPORT_RAW_DICT = 10,
        STATUS_UI_FONT_CHANGED = 11,
        STATUS_CANDIDATE_SORT_MODE_CHANGED = 12
    };
};
