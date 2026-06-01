#include "candidate_form.h"
#include <windowsx.h>

constexpr auto CANDIDATEWINDOW_CLASS = L"SimpleTSFCandidateWindow";
constexpr int CANDIDATE_HEIGHT = 30;
constexpr int COMPOSITION_HEIGHT = 30;
constexpr int CANDIDATE_MIN_WIDTH = 200;
constexpr int CANDIDATE_TEXT_FONT = 16;
constexpr int COMPOSITION_TEXT_FONT = 18;
constexpr int PAGE_BUTTON_WIDTH = 20;
constexpr int PAGE_BUTTON_MARGIN = 6;
constexpr int PAGE_BUTTON_GAP = 4;
constexpr int PAGE_BUTTON_VPAD = 4;
constexpr int COMPOSITION_ROW_RATIO_PERCENT = 70;

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
}

bool candidate_form::class_registered = false;

candidate_form::candidate_form()
{
	m_hWnd = nullptr;
	selection_ = 0;
	current_page = 0;
	page_size = 9; // 每页显示9个候选词（对应数字键1-9）
	use_display_text_ = false;
	ui_font_percent_ = 100;
}

candidate_form::~candidate_form()
{
	destroy();
}

void candidate_form::register_window_class()
{
	if (class_registered)
		return;

	WNDCLASSEX wcex = {0};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW | CS_IME;
	wcex.lpfnWndProc = WindowProc;
	wcex.hInstance = g_hInst;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszClassName = CANDIDATEWINDOW_CLASS;

	if (RegisterClassEx(&wcex))
	{
		class_registered = true;
	}
}

BOOL candidate_form::create(HWND hWndParent)
{
	register_window_class();

	m_hWnd = CreateWindowEx(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		CANDIDATEWINDOW_CLASS,
		nullptr,
		WS_POPUP | WS_BORDER,
		-32000, -32000, get_window_width(), get_window_height(),
		hWndParent,
		nullptr,
		g_hInst,
		this);

	return (m_hWnd != nullptr);
}

void candidate_form::destroy()
{
	if (m_hWnd)
	{
		DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
}

void candidate_form::show(bool bShow) const
{
	if (m_hWnd)
	{
		ShowWindow(m_hWnd, bShow ? SW_SHOWNOACTIVATE : SW_HIDE);
	}
}

void candidate_form::move(int x, int y) const
{
	if (m_hWnd)
	{
		int width = 0;
		int height = 0;
		calc_window_size(width, height);

		SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
	}
}

void candidate_form::get_window_size(int& width, int& height) const
{
	calc_window_size(width, height);
}

void candidate_form::set_candidates(const std::vector<std::wstring>& candidates)
{
	candidates_ = candidates;
	original_candidates_.clear();
	display_candidates_.clear();
	use_display_text_ = false;
	selection_ = 0;
	current_page = 0; // 重置到第一页

	if (m_hWnd)
	{
		int width = 0;
		int height = 0;
		calc_window_size(width, height);
		SetWindowPos(m_hWnd, nullptr, 0, 0, width, height,
		             SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
		InvalidateRect(m_hWnd, nullptr, TRUE);
	}
}

void candidate_form::set_candidates(const std::vector<std::wstring>& original, const std::vector<std::wstring>& display)
{
	original_candidates_ = original;
	display_candidates_ = display;
	if (display_candidates_.size() > original_candidates_.size())
	{
		display_candidates_.resize(original_candidates_.size());
	}
	candidates_.clear();
	use_display_text_ = true;
	selection_ = 0;
	current_page = 0;

	if (m_hWnd)
	{
		int width = 0;
		int height = 0;
		calc_window_size(width, height);
		SetWindowPos(m_hWnd, nullptr, 0, 0, width, height,
		             SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
		InvalidateRect(m_hWnd, nullptr, TRUE);
	}
}

std::wstring candidate_form::get_candidate(int index) const
{
	if (use_display_text_)
	{
		if (index >= 0 && index < static_cast<int>(original_candidates_.size()))
			return original_candidates_[index];
	}
	else
	{
		if (index >= 0 && index < static_cast<int>(candidates_.size()))
			return candidates_[index];
	}
	return L"";
}

std::wstring candidate_form::get_display_candidate(int index) const
{
	if (use_display_text_)
	{
		if (index >= 0 && index < static_cast<int>(display_candidates_.size()))
			return display_candidates_[index];
		if (index >= 0 && index < static_cast<int>(original_candidates_.size()))
			return original_candidates_[index];
	}
	else
	{
		if (index >= 0 && index < static_cast<int>(candidates_.size()))
			return candidates_[index];
	}
	return L"";
}

void candidate_form::set_composition_text(const std::wstring& text)
{
	composition_text_ = text;

	if (m_hWnd)
	{
		int width = 0;
		int height = 0;
		calc_window_size(width, height);
		SetWindowPos(m_hWnd, nullptr, 0, 0, width, height,
		             SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
		InvalidateRect(m_hWnd, nullptr, TRUE);
	}
}

void candidate_form::set_selection(int nSelection)
{
	// 计算当前页的候选词数量
	const int start_index = current_page * page_size;
	const int candidate_count = get_candidate_count();
	const int end_index = min(start_index + page_size, candidate_count);
	const int items_in_page = end_index - start_index;

	// 确保选择在当前页范围内
	if (nSelection >= 0 && nSelection < items_in_page)
	{
		selection_ = nSelection;
		if (m_hWnd)
		{
			InvalidateRect(m_hWnd, nullptr, TRUE);
		}
	}
}

int candidate_form::get_candidate_count() const
{
	if (use_display_text_)
		return static_cast<int>(original_candidates_.size());
	else
		return static_cast<int>(candidates_.size());
}

// 翻页功能实现
void candidate_form::page_up()
{
	if (current_page > 0)
	{
		current_page--;
		if (m_hWnd)
		{
			RECT rc = {};
			GetWindowRect(m_hWnd, &rc);
			move(rc.left, rc.top);
			InvalidateRect(m_hWnd, nullptr, TRUE);
		}
	}
}

void candidate_form::page_down()
{
	const int total_pages = get_total_pages();
	if (current_page < total_pages - 1)
	{
		current_page++;
		if (m_hWnd)
		{
			RECT rc = {};
			GetWindowRect(m_hWnd, &rc);
			move(rc.left, rc.top);
			InvalidateRect(m_hWnd, nullptr, TRUE);
		}
	}
}

int candidate_form::get_current_page() const
{
	return current_page;
}

int candidate_form::get_total_pages() const
{
	const int count = get_candidate_count();
	if (count == 0)
		return 0;
	return (count + page_size - 1) / page_size;
}

int candidate_form::get_page_size() const
{
	return page_size;
}

LRESULT CALLBACK candidate_form::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	candidate_form* pThis;

	if (uMsg == WM_CREATE)
	{
		auto pcs = (LPCREATESTRUCT)lParam;
		pThis = static_cast<candidate_form*>(pcs->lpCreateParams);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
	}
	else
	{
		pThis = (candidate_form*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	}

	switch (uMsg)
	{
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		if (pThis)
		{
			pThis->on_paint(hdc);
		}
		EndPaint(hWnd, &ps);
	}
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_DPICHANGED:
	{
		if (pThis)
		{
			const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
			int width = 0;
			int height = 0;
			pThis->calc_window_size(width, height);
			SetWindowPos(hWnd, nullptr,
				suggested ? suggested->left : 0,
				suggested ? suggested->top : 0,
				width,
				height,
				SWP_NOZORDER | SWP_NOACTIVATE);
			InvalidateRect(hWnd, nullptr, TRUE);
			return 0;
		}
	}
	break;

	case WM_LBUTTONUP:
	{
		if (pThis)
		{
			const int x = GET_X_LPARAM(lParam);
			const int y = GET_Y_LPARAM(lParam);
			pThis->on_lbutton_up(x, y);
		}
	}
		return 0;

	case WM_RBUTTONUP:
	{
		if (pThis)
		{
			const int x = GET_X_LPARAM(lParam);
			const int y = GET_Y_LPARAM(lParam);
			pThis->on_rbutton_up(x, y);
		}
	}
		return 0;
	default: ;
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void candidate_form::on_paint(HDC hdc) const
{
	RECT rc;
	GetClientRect(m_hWnd, &rc);
	const int candidate_height = scale_px(CANDIDATE_HEIGHT);
	const int candidate_font = min(candidate_height - scale_px(6), scale_px(CANDIDATE_TEXT_FONT) + scale_px(2));
	const int composition_height = max(scale_px(14), (candidate_height * COMPOSITION_ROW_RATIO_PERCENT) / 100);
	const int composition_font = max(scale_px(13), (candidate_font * 92) / 100);
	const int radius = scale_px(8);

	// 背景与外框（统一白底）
	const HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(hdc, &rc, brush);
	DeleteObject(brush);
	const HPEN frame_pen = CreatePen(PS_SOLID, 1, RGB(220, 226, 232));
	const auto old_frame_pen = static_cast<HPEN>(SelectObject(hdc, frame_pen));
	const HBRUSH old_frame_brush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
	Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
	SelectObject(hdc, old_frame_brush);
	SelectObject(hdc, old_frame_pen);
	DeleteObject(frame_pen);

	// 设置文本格式
	SetBkMode(hdc, TRANSPARENT);

	int y = 0;

	// 绘制组合字符串（上栏）
	if (!composition_text_.empty())
	{
		RECT rc_composition = {0, y, rc.right, y + composition_height};
		RECT rcPrev = {};
		RECT rcNext = {};
		const bool hasButtons = get_page_button_rects(rcPrev, rcNext);
		RECT rcPageTop = {};
		bool hasPageTop = false;

		// 组合字符串区域（第一行改为白底）
		const HBRUSH comp_brush = CreateSolidBrush(RGB(255, 255, 255));
		FillRect(hdc, &rc_composition, comp_brush);
		DeleteObject(comp_brush);

		// 绘制分隔线
		const HPEN pen = CreatePen(PS_SOLID, 1, RGB(228, 232, 236));
		const HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
		MoveToEx(hdc, 0, y + composition_height - 1, nullptr);
		LineTo(hdc, rc.right, y + composition_height - 1);
		SelectObject(hdc, old_pen);
		DeleteObject(pen);

		// 预留右侧页码+翻页按钮区域
		if (hasButtons)
		{
			const int total_pages = get_total_pages();
			if (total_pages > 1)
			{
				wchar_t szPageInfo[64];
				(void)swprintf_s(szPageInfo, L"[%d/%d]", current_page + 1, total_pages);
				SIZE pageInfoSize = {};
				GetTextExtentPoint32(hdc, szPageInfo, static_cast<int>(wcslen(szPageInfo)), &pageInfoSize);
				const int page_right = rcPrev.left - scale_px(PAGE_BUTTON_GAP);
				rcPageTop = { page_right - pageInfoSize.cx, y, page_right, y + composition_height };
				hasPageTop = true;
			}
		}

		// 绘制组合字符串文本
		SetTextColor(hdc, RGB(144, 86, 116));
		const HFONT font = CreateFont(composition_font, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		const auto old_font = static_cast<HFONT>(SelectObject(hdc, font));

		rc_composition.left += scale_px(8);
		if (hasButtons)
			rc_composition.right = min(rc_composition.right, rcPrev.left - scale_px(PAGE_BUTTON_GAP));
		if (hasPageTop)
			rc_composition.right = min(rc_composition.right, rcPageTop.left - scale_px(PAGE_BUTTON_GAP));
		DrawText(hdc, composition_text_.c_str(), -1, &rc_composition,
		         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

		// 在页码/翻页按钮前增加低对比度品牌字样（偏背景感）
		{
			RECT rcBrand = { 0, y, rc.right, y + composition_height };
			rcBrand.left = rc.left + scale_px(8);
			rcBrand.right = rc.right - scale_px(8);
			if (hasButtons)
				rcBrand.right = min(rcBrand.right, rcPrev.left - scale_px(PAGE_BUTTON_GAP));
			if (hasPageTop)
				rcBrand.right = min(rcBrand.right, rcPageTop.left - scale_px(PAGE_BUTTON_GAP));
			rcBrand.left = max(rcBrand.left, rcBrand.right - scale_px(108));

			SetTextColor(hdc, RGB(148, 202, 170)); // 浅绿，略提高可见度
			const HFONT brand_font = CreateFont(max(scale_px(13), composition_font), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
			                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
			const auto old_brand_font = static_cast<HFONT>(SelectObject(hdc, brand_font));
			DrawText(hdc, L"随意五笔", -1, &rcBrand, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
			SelectObject(hdc, old_brand_font);
			DeleteObject(brand_font);
		}

		SelectObject(hdc, old_font);
		DeleteObject(font);

		if (hasPageTop)
		{
			wchar_t szPageInfo[64];
			(void)swprintf_s(szPageInfo, L"[%d/%d]", current_page + 1, get_total_pages());
			SetTextColor(hdc, RGB(124, 135, 129));
			DrawText(hdc, szPageInfo, -1, &rcPageTop, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		}

		if (hasButtons)
		{
			auto draw_button = [&](const RECT& r, wchar_t ch, bool enabled) {
				const HBRUSH btn_brush = CreateSolidBrush(enabled ? RGB(237, 250, 243) : RGB(246, 247, 246));
				const HPEN btn_pen = CreatePen(PS_SOLID, 1, enabled ? RGB(157, 199, 178) : RGB(216, 220, 216));
				const auto old_pen2 = static_cast<HPEN>(SelectObject(hdc, btn_pen));
				const auto old_brush2 = static_cast<HBRUSH>(SelectObject(hdc, btn_brush));
				RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
				SelectObject(hdc, old_brush2);
				SelectObject(hdc, old_pen2);
				DeleteObject(btn_brush);
				DeleteObject(btn_pen);

				SetTextColor(hdc, enabled ? RGB(82, 109, 95) : RGB(151, 157, 153));
				wchar_t text[2] = { ch, 0 };
				RECT rt = r;
				DrawText(hdc, text, -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			};

			draw_button(rcPrev, L'<', current_page > 0);
			draw_button(rcNext, L'>', current_page < get_total_pages() - 1);
		}

		y += composition_height;
	}

	// 绘制候选词（横向排列，支持翻页）
	const int candidate_count = get_candidate_count();
	if (candidate_count > 0)
	{
		HFONT hCandidateFont = CreateFont(candidate_font, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
		auto hOldFont = static_cast<HFONT>(SelectObject(hdc, hCandidateFont));

		SetTextColor(hdc, RGB(60, 68, 63));

		// 计算当前页的候选词范围
		const int start_index = current_page * page_size;
		const int end_index = min(start_index + page_size, candidate_count);

		int x = scale_px(5); // 左边距

			for (int i = start_index; i < end_index; i++)
			{
				// 格式化候选词文本（使用显示文本或原始文本）
				wchar_t szText[256];
				const std::wstring* text_to_display = nullptr;
				if (use_display_text_)
				{
					if (i < static_cast<int>(display_candidates_.size()))
						text_to_display = &display_candidates_[i];
					else if (i < static_cast<int>(original_candidates_.size()))
						text_to_display = &original_candidates_[i];
				}
				else if (i < static_cast<int>(candidates_.size()))
				{
					text_to_display = &candidates_[i];
				}
				if (!text_to_display)
					continue;
				(void)swprintf_s(szText, L"%d.%s", (i - start_index + 1), text_to_display->c_str());

			// 测量文本宽度（自适应）
			SIZE textSize;
			GetTextExtentPoint32(hdc, szText, static_cast<int>(wcslen(szText)), &textSize);
			const int item_width = textSize.cx + scale_px(15); // 添加左右边距

			RECT rcItem = {x, y, x + item_width, y + candidate_height};

			// 高亮选中项（selection_ 是相对于当前页的索引）
			const int relative_index = i - start_index;
			if (relative_index == selection_)
			{
				const HBRUSH sel_brush = CreateSolidBrush(RGB(254, 241, 248));
				const HPEN sel_pen = CreatePen(PS_SOLID, 1, RGB(156, 201, 179));
				const auto old_sel_pen = static_cast<HPEN>(SelectObject(hdc, sel_pen));
				const auto old_sel_brush = static_cast<HBRUSH>(SelectObject(hdc, sel_brush));
				RoundRect(hdc, rcItem.left, rcItem.top + scale_px(2), rcItem.right, rcItem.bottom - scale_px(2), radius, radius);
				SelectObject(hdc, old_sel_brush);
				SelectObject(hdc, old_sel_pen);
				DeleteObject(sel_brush);
				DeleteObject(sel_pen);
			}

			// 绘制候选词文本
			rcItem.left += scale_px(5);
			rcItem.right -= scale_px(5);
			DrawText(hdc, szText, -1, &rcItem, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

			x += item_width;
		}

		// 如果有多页，页码已绘制在第一行右上角（组合行）；无组合行时保留第二行页码显示。
		const int total_pages = get_total_pages();
		if (total_pages > 1 && composition_text_.empty())
		{
			RECT rcPrev = {};
			RECT rcNext = {};
			const bool hasButtons = get_page_button_rects(rcPrev, rcNext);
			wchar_t szPageInfo[64];
			(void)swprintf_s(szPageInfo, L" [%d/%d]", current_page + 1, total_pages);

			SIZE pageInfoSize;
			GetTextExtentPoint32(hdc, szPageInfo, static_cast<int>(wcslen(szPageInfo)), &pageInfoSize);

			int pageInfoRight = rc.right - scale_px(PAGE_BUTTON_MARGIN);
			if (hasButtons)
				pageInfoRight = rcPrev.left - scale_px(PAGE_BUTTON_GAP);
			RECT rcPageInfo = {pageInfoRight - pageInfoSize.cx, y, pageInfoRight, y + candidate_height};
			SetTextColor(hdc, RGB(124, 135, 129));
			DrawText(hdc, szPageInfo, -1, &rcPageInfo, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

			// 翻页按钮已绘制到第一行右上角；此处保留页码文字。
		}

		SelectObject(hdc, hOldFont);
		DeleteObject(hCandidateFont);
	}
}

void candidate_form::set_ui_font_percent(int percent)
{
	if (percent < 80)
		percent = 80;
	if (percent > 250)
		percent = 250;
	ui_font_percent_ = percent;
	if (m_hWnd)
	{
		RECT rc = {};
		GetWindowRect(m_hWnd, &rc);
		int width = 0;
		int height = 0;
		calc_window_size(width, height);
		SetWindowPos(m_hWnd, nullptr, rc.left, rc.top, width, height,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
	}
}

int candidate_form::get_window_width() const
{
	return scale_px(CANDIDATE_MIN_WIDTH);
}

int candidate_form::get_window_height() const
{
	const int base_candidate_height = scale_px(CANDIDATE_HEIGHT);
	const int compact_composition_height = max(scale_px(14), (base_candidate_height * COMPOSITION_ROW_RATIO_PERCENT) / 100);
	const int composition_height = composition_text_.empty() ? 0 : compact_composition_height;
	const int candidate_height = get_candidate_count() > 0 ? base_candidate_height : 0;
	const int fallback = scale_px(COMPOSITION_HEIGHT);
	return composition_height + candidate_height + (composition_height + candidate_height == 0 ? fallback : 0);
}

void candidate_form::calc_window_size(int& width, int& height) const
{
	width = get_window_width();
	height = 0;

	const int candidate_height = scale_px(CANDIDATE_HEIGHT);
	const int composition_height = max(scale_px(14), (candidate_height * COMPOSITION_ROW_RATIO_PERCENT) / 100);
	const int candidate_font = min(candidate_height - scale_px(6), scale_px(CANDIDATE_TEXT_FONT) + scale_px(2));

	if (!composition_text_.empty())
	{
		height += composition_height;
	}

	const int candidate_count = get_candidate_count();
	if (candidate_count > 0)
	{
		height += candidate_height;

		if (m_hWnd)
		{
			if (const HDC hdc = GetDC(m_hWnd))
			{
				const HFONT font = CreateFont(candidate_font, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
				const auto old_font = static_cast<HFONT>(SelectObject(hdc, font));

				const int start_index = current_page * page_size;
				const int end_index = min(start_index + page_size, candidate_count);

				int totalWidth = scale_px(10);
					for (int i = start_index; i < end_index; ++i)
					{
						wchar_t szText[256];
						const std::wstring* text_to_measure = nullptr;
						if (use_display_text_)
						{
							if (i < static_cast<int>(display_candidates_.size()))
								text_to_measure = &display_candidates_[i];
							else if (i < static_cast<int>(original_candidates_.size()))
								text_to_measure = &original_candidates_[i];
						}
						else if (i < static_cast<int>(candidates_.size()))
						{
							text_to_measure = &candidates_[i];
						}
						if (!text_to_measure)
							continue;
						(void)swprintf_s(szText, L"%d.%s", (i - start_index + 1), text_to_measure->c_str());

					SIZE textSize;
					GetTextExtentPoint32(hdc, szText, static_cast<int>(wcslen(szText)), &textSize);
					totalWidth += textSize.cx + scale_px(15);
				}

				const int total_pages = get_total_pages();
				if (total_pages > 1 && composition_text_.empty())
				{
					wchar_t szPageInfo[64];
					(void)swprintf_s(szPageInfo, L" [%d/%d]", current_page + 1, total_pages);
					SIZE pageInfoSize;
					GetTextExtentPoint32(hdc, szPageInfo, static_cast<int>(wcslen(szPageInfo)), &pageInfoSize);
					totalWidth += pageInfoSize.cx + scale_px(10);
					totalWidth += scale_px(PAGE_BUTTON_MARGIN * 2 + PAGE_BUTTON_GAP + PAGE_BUTTON_WIDTH * 2);
				}

				SelectObject(hdc, old_font);
				DeleteObject(font);
				ReleaseDC(m_hWnd, hdc);

				if (totalWidth > width)
					width = totalWidth;
			}
		}
	}

	if (height == 0)
		height = composition_height;
}

int candidate_form::scale_px(int base_px) const
{
	const UINT dpi = get_dpi();
	return MulDiv(base_px * ui_font_percent_, static_cast<int>(dpi), 96 * 100);
}

UINT candidate_form::get_dpi() const
{
	return resolve_window_dpi(m_hWnd);
}

void candidate_form::on_lbutton_up(int x, int y)
{
	const int page_button = hit_test_page_button(x, y);
	if (page_button == 0)
	{
		page_up();
		set_selection(0);
		return;
	}
	if (page_button == 1)
	{
		page_down();
		set_selection(0);
		return;
	}

	if (!click_callback_)
		return;
	const int idx = hit_test_candidate(x, y);
	if (idx >= 0)
	{
		click_callback_(idx);
	}
}

int candidate_form::hit_test_page_button(int x, int y) const
{
	RECT rcPrev = {};
	RECT rcNext = {};
	if (!get_page_button_rects(rcPrev, rcNext))
		return -1;

	if (x >= rcPrev.left && x < rcPrev.right && y >= rcPrev.top && y < rcPrev.bottom)
		return 0;
	if (x >= rcNext.left && x < rcNext.right && y >= rcNext.top && y < rcNext.bottom)
		return 1;
	return -1;
}

int candidate_form::hit_test_candidate(int x, int y) const
{
	const int candidate_count = get_candidate_count();
	if (candidate_count <= 0)
		return -1;

	const int candidate_height = scale_px(CANDIDATE_HEIGHT);
	const int composition_height = max(scale_px(14), (candidate_height * COMPOSITION_ROW_RATIO_PERCENT) / 100);
	const int candidate_font = min(candidate_height - scale_px(6), scale_px(CANDIDATE_TEXT_FONT) + scale_px(2));
	int y_top = composition_text_.empty() ? 0 : composition_height;
	if (y < y_top || y >= y_top + candidate_height)
		return -1;

	HDC hdc = GetDC(m_hWnd);
	if (!hdc)
		return -1;

	HFONT hFont = CreateFont(candidate_font, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
	const auto hOldFont = static_cast<HFONT>(SelectObject(hdc, hFont));

	const int start_index = current_page * page_size;
	const int end_index = min(start_index + page_size, candidate_count);
	int x_left = scale_px(5);
	int result = -1;

	for (int i = start_index; i < end_index; ++i)
	{
		wchar_t szText[256];
		const std::wstring* text_to_display = nullptr;
		if (use_display_text_)
		{
			if (i < static_cast<int>(display_candidates_.size()))
				text_to_display = &display_candidates_[i];
			else if (i < static_cast<int>(original_candidates_.size()))
				text_to_display = &original_candidates_[i];
		}
		else if (i < static_cast<int>(candidates_.size()))
		{
			text_to_display = &candidates_[i];
		}
		if (!text_to_display)
			continue;
		(void)swprintf_s(szText, L"%d.%s", (i - start_index + 1), text_to_display->c_str());

		SIZE textSize = {};
		GetTextExtentPoint32(hdc, szText, static_cast<int>(wcslen(szText)), &textSize);
		const int item_width = textSize.cx + scale_px(15);
		const RECT rcItem = { x_left, y_top, x_left + item_width, y_top + candidate_height };

		if (x >= rcItem.left && x < rcItem.right && y >= rcItem.top && y < rcItem.bottom)
		{
			result = i; // 返回全局候选索引
			break;
		}
		x_left += item_width;
	}

	SelectObject(hdc, hOldFont);
	DeleteObject(hFont);
	ReleaseDC(m_hWnd, hdc);
	return result;
}

bool candidate_form::get_page_button_rects(RECT& prev_rect, RECT& next_rect) const
{
	const int candidate_count = get_candidate_count();
	const int total_pages = get_total_pages();
	if (candidate_count <= 0 || total_pages <= 1 || !m_hWnd)
		return false;

	RECT rc = {};
	GetClientRect(m_hWnd, &rc);
	const int candidate_height = scale_px(CANDIDATE_HEIGHT);
	const int composition_height = max(scale_px(14), (candidate_height * COMPOSITION_ROW_RATIO_PERCENT) / 100);
	const int y_top = 0; // 按钮优先放在第一行；无组合行时第一行即候选行
	const int row_height = composition_text_.empty() ? candidate_height : composition_height;
	const int btn_w = scale_px(PAGE_BUTTON_WIDTH);
	const int btn_margin = scale_px(PAGE_BUTTON_MARGIN);
	const int btn_gap = scale_px(PAGE_BUTTON_GAP);
	const int btn_vpad = scale_px(PAGE_BUTTON_VPAD);
	const int top = y_top + btn_vpad;
	const int bottom = y_top + row_height - btn_vpad;

	next_rect = { rc.right - btn_margin - btn_w, top, rc.right - btn_margin, bottom };
	prev_rect = { next_rect.left - btn_gap - btn_w, top, next_rect.left - btn_gap, bottom };
	return true;
}

void candidate_form::on_rbutton_up(int x, int y)
{
	if (!context_menu_callback_)
		return;

	const int idx = hit_test_candidate(x, y);
	if (idx < 0)
		return;

	POINT pt = { x, y };
	ClientToScreen(m_hWnd, &pt);
	context_menu_callback_(idx, pt);
}
