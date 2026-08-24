#pragma once

#include "globals.h"
#include <vector>
#include <functional>

class candidate_form
{
public:
    candidate_form();

    ~candidate_form();

    BOOL create(HWND hWndParent);

    void destroy();
    
    void show(bool bShow) const;

    void move(int x, int y) const;

    void set_candidates(const std::vector<std::wstring>& candidates);
    
    // 设置候选词（原始文本和显示文本）
    void set_candidates(const std::vector<std::wstring>& original, const std::vector<std::wstring>& display);
    
    // 获取指定索引的原始候选词（用于上屏）
    std::wstring get_candidate(int index) const;
    std::wstring get_display_candidate(int index) const;

    void set_composition_text(const std::wstring& text);

    [[nodiscard]] int get_selection() const { return selection_; }

    void set_selection(int nSelection);

    [[nodiscard]] int get_candidate_count() const;

    HWND get_hwnd() const { return m_hWnd; }
    
    /* page flipping func */
    void page_up();

    void page_down();

    /* get the current page index */
    [[nodiscard]] int get_current_page() const;

    /* get total num of pages */
    [[nodiscard]] int get_total_pages() const;

    /* get candidates count of every page */
    [[nodiscard]] int get_page_size() const;

    using CandidateContextMenuCallback = std::function<void(int, POINT)>;
    void set_context_menu_callback(CandidateContextMenuCallback callback) { context_menu_callback_ = std::move(callback); }
    using CandidateClickCallback = std::function<void(int)>;
    void set_click_callback(CandidateClickCallback callback) { click_callback_ = std::move(callback); }
    using PageChangeCallback = std::function<void(int)>;
    void set_page_change_callback(PageChangeCallback callback) { page_change_callback_ = std::move(callback); }
    void set_ui_font_percent(int percent);
    void get_window_size(int& width, int& height) const;
    [[nodiscard]] int get_window_width() const;
    [[nodiscard]] int get_window_height() const;

private:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static void register_window_class();

    HWND m_hWnd;

    /* list of candidates */
    std::vector<std::wstring> candidates_;
    
    /* list of original candidates (for committing) */
    std::vector<std::wstring> original_candidates_;
    
    /* list of display candidates (for showing with code hints) */
    std::vector<std::wstring> display_candidates_;
    
    /* flag to indicate if using separate display text */
    bool use_display_text_;

    /* composition text */
    std::wstring composition_text_;

    /* current selection */
    int selection_;

    /* current page index */
    int current_page;

    /* candidates count of every page */
    int page_size;

    /* already registered or not*/
    static bool class_registered;

    /*  */
    void on_paint(HDC hdc) const;
    int hit_test_candidate(int x, int y) const;
    int hit_test_page_button(int x, int y) const;
    void on_lbutton_up(int x, int y);
    void on_rbutton_up(int x, int y);
    bool get_page_button_rects(RECT& prev_rect, RECT& next_rect) const;
    CandidateContextMenuCallback context_menu_callback_;
    CandidateClickCallback click_callback_;
    PageChangeCallback page_change_callback_;
    int ui_font_percent_;
    void calc_window_size(int& width, int& height) const;

    [[nodiscard]] int scale_px(int base_px) const;
    [[nodiscard]] UINT get_dpi() const;
};
