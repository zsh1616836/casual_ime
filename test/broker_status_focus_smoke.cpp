#include "broker_ui.h"

#include <future>
#include <iostream>
#include <thread>

namespace
{
constexpr wchar_t kHostClass[] = L"ZIme.StatusFocusSmoke.Host";

LRESULT CALLBACK host_window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// Hidden windows on independent GUI threads model foreign host input queues.
// No real keystrokes, foreground changes, named pipes or user data are needed.
class host_window
{
public:
    host_window()
    {
        std::promise<HWND> ready;
        auto future = ready.get_future();
        thread_ = std::thread([ready = std::move(ready)]() mutable {
            HWND hwnd = CreateWindowExW(0, kHostClass, L"", WS_OVERLAPPEDWINDOW,
                0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ready.set_value(hwnd);
            if (!hwnd)
                return;
            MSG message = {};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        });
        hwnd_ = future.get();
    }

    ~host_window()
    {
        if (hwnd_)
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        thread_.join();
    }

    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    std::thread thread_;
};

void pump_messages()
{
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND find_status_window()
{
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM context) -> BOOL {
        wchar_t name[128] = {};
        GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
        if (wcscmp(name, L"SimpleTSFStatusWindow") == 0)
        {
            *reinterpret_cast<HWND*>(context) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool send_status(broker_ui_controller& controller, HWND host,
                 std::uint64_t connection, bool visible)
{
    using namespace zime::broker_protocol;
    auto update = std::make_unique<broker_status_update>();
    update->connection_id = connection;
    update->generation = 1;
    update->owner_hwnd = host;
    update->flags = status_custom_ui_allowed | status_chinese_mode |
        (visible ? status_visible : 0);
    if (!broker_ui_controller::PostStatusUpdate(controller.hwnd(), std::move(update)))
        return false;
    pump_messages();
    return true;
}

int fail(const wchar_t* reason)
{
    std::wcerr << reason << std::endl;
    return 1;
}
}

int wmain()
{
    g_hInst = GetModuleHandleW(nullptr);
    WNDCLASSW window_class = {};
    window_class.hInstance = g_hInst;
    window_class.lpfnWndProc = host_window_proc;
    window_class.lpszClassName = kHostClass;
    if (!RegisterClassW(&window_class))
        return fail(L"register hidden host class");

    host_window first;
    host_window second;
    if (!first.hwnd() || !second.hwnd())
        return fail(L"create hidden host windows");

    // The shared class must also protect the TIP's local fallback path.
    {
        status_window local;
        if (!local.create(first.hwnd()) || GetWindow(local.get_hwnd(), GW_OWNER))
            return fail(L"local fallback must reject a foreign-thread owner");
    }

    broker_ui_controller controller;
    if (!controller.Initialize(g_hInst, nullptr, {}, {}))
        return fail(L"initialize UI controller without storage");

    const HWND foreground = GetForegroundWindow();
    if (!send_status(controller, first.hwnd(), 1, true))
        return fail(L"post initial status");
    const HWND status = find_status_window();
    if (!status || !IsWindowVisible(status))
        return fail(L"initial status must be visible");
    if (GetWindow(status, GW_OWNER) != nullptr)
        return fail(L"status attaches its input queue to a foreign host owner");
    const auto style = GetWindowLongPtrW(status, GWL_EXSTYLE);
    if (!(style & WS_EX_NOACTIVATE) || !(style & WS_EX_TOOLWINDOW))
        return fail(L"status must not activate or appear on the taskbar");
    if (foreground != GetForegroundWindow())
        return fail(L"showing status changed foreground focus");

    // A different client takes over the reusable window, without owner churn.
    if (!send_status(controller, second.hwnd(), 2, true) ||
        find_status_window() != status || !IsWindowVisible(status))
        return fail(L"switching host must reuse the status window");
    if (!send_status(controller, first.hwnd(), 1, false) || !IsWindowVisible(status))
        return fail(L"old host hide must not hide the current host status");
    broker_ui_controller::PostConnectionClosed(controller.hwnd(), 1);
    pump_messages();
    if (!IsWindowVisible(status))
        return fail(L"old connection close must not hide the current status");

    if (!send_status(controller, second.hwnd(), 2, false) || IsWindowVisible(status))
        return fail(L"focus loss must hide status without a Win32 owner");
    if (!send_status(controller, second.hwnd(), 2, true) || !IsWindowVisible(status))
        return fail(L"focus return must show the same status window");
    broker_ui_controller::PostConnectionClosed(controller.hwnd(), 2);
    pump_messages();
    if (IsWindowVisible(status))
        return fail(L"current connection close must hide status");

    std::wcout << L"broker status focus smoke passed" << std::endl;
    return 0;
}
