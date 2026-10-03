// =============================================================================
//  src/platform/win32/win32_window.c —— 窗口与消息循环（Win32 实现）
//
//  本文件是 M1 最"平台"的地方，所有 windows.h 的复杂度都封在这里。
//  上层只看到 platform_window_t*。
// =============================================================================
#include "engine/platform/window.h"

#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

#define WIN32_WINDOW_CLASS_NAME L"PolaireWindowClass"
#define WIN32_TITLE_CAPACITY 512

// =============================================================================
//  内部结构
// =============================================================================
struct platform_window_t {
    allocator_t *alloc;
    HINSTANCE    instance;
    HWND         hwnd;

    u32 client_width;
    u32 client_height;

    bool should_close;
    bool visible;
    bool resizable;

    bool fullscreen;
    bool mouse_locked;
    bool cursor_visible;
    bool ignore_next_mouse_move;

    // 进入全屏前的窗口状态，用于还原
    WINDOWPLACEMENT saved_placement;
    LONG_PTR        saved_style;
    LONG_PTR        saved_ex_style;
    RECT            saved_rect;

    f64 dpi_scale;

    input_state_t input;
};

// 窗口类只需注册一次。用函数内静态变量而不是模块级全局：
// 让"是否已注册"这个状态只被它自己的初始化逻辑看到。
static bool win32_register_window_class(HINSTANCE instance);

// =============================================================================
//  辅助
// =============================================================================
static bool utf8_to_utf16(const char *utf8, wchar_t *out, usize out_capacity)
{
    if (out == NULL || out_capacity == 0) {
        return false;
    }
    if (utf8 == NULL) {
        out[0] = L'\0';
        return true;
    }

    const int written =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)out_capacity);
    if (written <= 0) {
        out[0] = L'\0';
        return false;
    }
    return true;
}

static u32 win32_dpi_of_window(HWND hwnd)
{
    // 直接调用 GetDpiForWindow（Win10 1607+）。本引擎的目标就是 Win10/11，
    // 不做动态解析 —— 少一层间接，出问题时调用栈也更干净。
    const UINT dpi = GetDpiForWindow(hwnd);
    return (dpi != 0) ? (u32)dpi : 96u;
}

static void win32_center_window_on_monitor(HWND hwnd, u32 width, u32 height)
{
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);

    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &info)) {
        return;
    }

    const u32 monitor_w = (u32)(info.rcWork.right - info.rcWork.left);
    const u32 monitor_h = (u32)(info.rcWork.bottom - info.rcWork.top);

    const i32 x = info.rcWork.left + (i32)((monitor_w - width) / 2u);
    const i32 y = info.rcWork.top + (i32)((monitor_h - height) / 2u);

    SetWindowPos(hwnd, NULL, x, y, (i32)width, (i32)height, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void win32_update_cursor_clip(platform_window_t *window)
{
    if (!window->mouse_locked) {
        ClipCursor(NULL);
        return;
    }

    RECT client;
    if (!GetClientRect(window->hwnd, &client)) {
        return;
    }

    // 客户区坐标 -> 屏幕坐标。ClipCursor 要的是屏幕坐标，
    // 直接传客户区矩形会把光标限制到错误的位置。
    POINT top_left = {client.left, client.top};
    POINT bottom_right = {client.right, client.bottom};
    ClientToScreen(window->hwnd, &top_left);
    ClientToScreen(window->hwnd, &bottom_right);

    RECT screen_rect;
    screen_rect.left = top_left.x;
    screen_rect.top = top_left.y;
    screen_rect.right = bottom_right.x;
    screen_rect.bottom = bottom_right.y;

    ClipCursor(&screen_rect);
}

static void win32_center_cursor(platform_window_t *window)
{
    POINT center;
    center.x = (LONG)(window->client_width / 2u);
    center.y = (LONG)(window->client_height / 2u);
    ClientToScreen(window->hwnd, &center);
    SetCursorPos(center.x, center.y);
}

// ---------------------------------------------------------------- 全屏
//
// 用"无边框窗口铺满显示器"而不是 ChangeDisplaySettingsEx 独占全屏。
// 理由见 window.h 文件头的说明。
static void win32_apply_fullscreen(platform_window_t *window, bool fullscreen)
{
    if (window->fullscreen == fullscreen) {
        return;
    }

    if (fullscreen) {
        // 先把当前状态完整存下来，退出全屏时才能原样还原
        window->saved_style = GetWindowLongPtrW(window->hwnd, GWL_STYLE);
        window->saved_ex_style = GetWindowLongPtrW(window->hwnd, GWL_EXSTYLE);
        GetWindowRect(window->hwnd, &window->saved_rect);
        window->saved_placement.length = sizeof(window->saved_placement);
        GetWindowPlacement(window->hwnd, &window->saved_placement);

        MONITORINFO info;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromWindow(window->hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
            return;
        }

        SetWindowLongPtrW(window->hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_CLIPSIBLINGS |
                                                              WS_CLIPCHILDREN | WS_VISIBLE));
        SetWindowLongPtrW(window->hwnd, GWL_EXSTYLE,
                          (LONG_PTR)(window->saved_ex_style & ~(WS_EX_DLGMODALFRAME |
                                                                WS_EX_WINDOWEDGE |
                                                                WS_EX_CLIENTEDGE |
                                                                WS_EX_STATICEDGE)));

        SetWindowPos(window->hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(window->hwnd, GWL_STYLE, window->saved_style);
        SetWindowLongPtrW(window->hwnd, GWL_EXSTYLE, window->saved_ex_style);

        SetWindowPos(window->hwnd, NULL, window->saved_rect.left, window->saved_rect.top,
                     window->saved_rect.right - window->saved_rect.left,
                     window->saved_rect.bottom - window->saved_rect.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetWindowPlacement(window->hwnd, &window->saved_placement);
    }

    window->fullscreen = fullscreen;

    // 全屏切换会改变客户区尺寸，光标裁剪区域要跟着更新
    win32_update_cursor_clip(window);
}

// =============================================================================
//  消息处理
// =============================================================================
static void win32_handle_key(platform_window_t *window, u32 vk, LPARAM lparam, bool down)
{
    const u32  scancode = (u32)((lparam >> 16) & 0xFF);
    const bool extended = (lparam & (1 << 24)) != 0;

    const key_t key = platform_input_key_from_native(vk, scancode, extended);
    if (key == KEY_UNKNOWN) {
        return;
    }
    input_push_key(&window->input, key, down);

    // Alt+Enter 切换全屏。
    // lParam 位 29 是"context code"，为 1 表示按下时有 Alt 参与。
    if (down && key == KEY_ENTER && (lparam & (1 << 29)) != 0) {
        platform_window_set_fullscreen(window, !window->fullscreen);
    }
}

static LRESULT CALLBACK win32_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    // WM_NCCREATE 是最早能拿到 lpCreateParams 的消息，
    // 在这里把 this 指针挂到窗口上，后续消息就都能取到。
    platform_window_t *window = (platform_window_t *)(void *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        window = (platform_window_t *)create->lpCreateParams;
        if (window != NULL) {
            window->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(void *)window);
        }
    }

    if (window == NULL) {
        // 窗口对象还没建好（或已销毁），交给系统默认处理。
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_CLOSE:
        // 只标记，不在这里 DestroyWindow。
        // 资源释放顺序由上层显式控制（先停渲染、再销毁上下文、最后销毁窗口）。
        // 如果在 WM_CLOSE 里直接 PostQuitMessage，上层的清理代码可能来不及跑。
        window->should_close = true;
        window->input.quit_requested = true;
        return 0;

    case WM_DESTROY:
        return 0;

    case WM_ERASEBKGND:
        // 返回 1 表示"我已经处理了背景擦除"。
        // 不这么做的话每次尺寸变化系统都会用背景刷涂一遍，
        // 在 OpenGL 窗口上会看到白色闪烁。
        return 1;

    case WM_SIZE: {
        window->client_width = (u32)LOWORD(lparam);
        window->client_height = (u32)HIWORD(lparam);
        if (window->mouse_locked) {
            win32_update_cursor_clip(window);
        }
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        win32_handle_key(window, (u32)wparam, lparam, true);
        // 必须返回 0 吞掉 WM_SYSKEYDOWN：交给 DefWindowProc 的话，
        // 系统会把它当成"激活系统菜单"的请求（F10、Alt 都会触发），
        // 表现为按一下 F10 输入焦点就被菜单抢走。
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        win32_handle_key(window, (u32)wparam, lparam, false);
        return 0;

    case WM_SYSCOMMAND:
        // 屏蔽 Alt 单独按下时弹出的系统菜单（SC_KEYMENU）。
        // 不处理的话，玩游戏时按 Alt 会"卡住"输入焦点。
        if ((wparam & 0xFFF0u) == (WPARAM)SC_KEYMENU) {
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        const f32 x = (f32)(i16)LOWORD(lparam);
        const f32 y = (f32)(i16)HIWORD(lparam);

        if (window->mouse_locked) {
            if (window->ignore_next_mouse_move) {
                // 刚锁定鼠标时，光标可能还在窗口另一头，
                // 那一次的位移是假的，丢掉。
                window->ignore_next_mouse_move = false;
                return 0;
            }

            const f32 center_x = (f32)(window->client_width / 2u);
            const f32 center_y = (f32)(window->client_height / 2u);

            // 光标每帧被回中，所以"当前位置 - 中心"就是本帧位移，
            // 不受窗口边界限制。
            input_push_mouse_delta(&window->input, x - center_x, y - center_y);
            input_push_mouse_move(&window->input, center_x, center_y);

            win32_center_cursor(window);
        } else {
            input_push_mouse_move(&window->input, x, y);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        const bool down = (message == WM_LBUTTONDOWN) || (message == WM_RBUTTONDOWN) ||
                          (message == WM_MBUTTONDOWN);
        mouse_button_t button = MOUSE_BUTTON_LEFT;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
            button = MOUSE_BUTTON_RIGHT;
        } else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) {
            button = MOUSE_BUTTON_MIDDLE;
        }
        input_push_mouse_button(&window->input, button, down);
        return 0;
    }

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        const u32 xbutton = GET_XBUTTON_WPARAM(wparam);
        const mouse_button_t button =
            (xbutton == XBUTTON1) ? MOUSE_BUTTON_X1 : MOUSE_BUTTON_X2;
        input_push_mouse_button(&window->input, button, message == WM_XBUTTONDOWN);
        return TRUE; // X 按钮必须返回 TRUE，否则系统会把它当浏览器导航键
    }

    case WM_MOUSEWHEEL: {
        const f32 notches = (f32)GET_WHEEL_DELTA_WPARAM(wparam) / (f32)WHEEL_DELTA;
        input_push_scroll(&window->input, 0.0f, notches);
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        const f32 notches = (f32)GET_WHEEL_DELTA_WPARAM(wparam) / (f32)WHEEL_DELTA;
        input_push_scroll(&window->input, notches, 0.0f);
        return 0;
    }

    case WM_KILLFOCUS:
        // 失焦后系统不会补发 KEYUP，不清空就会永久"卡键"。
        input_clear_all(&window->input);
        window->input.focus_lost = true;
        return 0;

    case WM_SETCURSOR:
        if (window->mouse_locked || !window->cursor_visible) {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_DPICHANGED: {
        const u32 new_dpi = (u32)HIWORD(wparam);
        window->dpi_scale = (f64)new_dpi / 96.0;

        // 系统给出了建议的新窗口矩形，照它调整能避免窗口被截断。
        const RECT *suggested = (const RECT *)lparam;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// =============================================================================
//  窗口类注册
// =============================================================================
static bool win32_register_window_class(HINSTANCE instance)
{
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    // CS_OWNDC：让窗口拥有一个固定的 DC。
    // OpenGL 要求 SetPixelFormat 与 wglCreateContext 使用同一个 DC，
    // 用 CS_OWNDC 可以保证这一点（否则每次 GetDC 可能拿到不同的 DC）。
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = win32_window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // 每帧自己清屏，不需要系统擦背景
    wc.lpszClassName = WIN32_WINDOW_CLASS_NAME;

    if (RegisterClassExW(&wc) == 0) {
        // 多个窗口对象共享同一个窗口类，第二个窗口注册时会撞名。
        // 这不是错误。
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    registered = true;
    return true;
}

// =============================================================================
//  公开 API
// =============================================================================
platform_window_desc_t platform_window_desc_default(void)
{
    platform_window_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.title = "Polaire Engine";
    desc.width = 1280;
    desc.height = 720;
    desc.resizable = true;
    desc.fullscreen = false;
    desc.visible = true;
    desc.enable_high_dpi = true;
    return desc;
}

static void win32_enable_dpi_awareness(void)
{
    // 必须在**创建任何窗口之前**调用。
    // 调用晚了的话，窗口会先以 96 DPI 的逻辑尺寸建好，
    // 之后系统做位图拉伸，表现为画面发虚、鼠标坐标与像素对不上。
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
#endif
    // 老系统回退：只做系统级 DPI 感知（多显示器不同 DPI 时不如上面精确）
    SetProcessDPIAware();
}

bool platform_window_create(allocator_t *alloc, const platform_window_desc_t *desc,
                            platform_window_t **out_window)
{
    ASSERT(alloc != NULL);
    ASSERT(desc != NULL);
    ASSERT(out_window != NULL);

    if (desc->enable_high_dpi) {
        win32_enable_dpi_awareness();
    }

    const HINSTANCE instance = GetModuleHandleW(NULL);
    if (!win32_register_window_class(instance)) {
        LOG_ERROR("窗口类注册失败 (GetLastError = %lu)", (unsigned long)GetLastError());
        return false;
    }

    platform_window_t *window =
        (platform_window_t *)alloc->alloc(alloc, sizeof(platform_window_t), 16);
    if (window == NULL) {
        LOG_ERROR("窗口对象分配失败（%llu 字节）",
                  (unsigned long long)sizeof(platform_window_t));
        return false;
    }
    memset(window, 0, sizeof(*window));

    window->alloc = alloc;
    window->instance = instance;
    window->client_width = desc->width;
    window->client_height = desc->height;
    window->resizable = desc->resizable;
    window->cursor_visible = true;
    window->dpi_scale = 1.0;

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (desc->resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    }

    // 期望的客户区尺寸 -> 外层窗口尺寸。
    // 不做这一步的话，给 1280x720 得到的是"含边框 1280x720"，
    // 客户区只有 1264x681，视口和画面尺寸对不上。
    RECT rect = {0, 0, (LONG)desc->width, (LONG)desc->height};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const i32 outer_width = rect.right - rect.left;
    const i32 outer_height = rect.bottom - rect.top;

    wchar_t title[WIN32_TITLE_CAPACITY];
    utf8_to_utf16(desc->title, title, ENGINE_ARRAY_COUNT(title));

    HWND hwnd = CreateWindowExW(0, WIN32_WINDOW_CLASS_NAME, title, style, CW_USEDEFAULT,
                                CW_USEDEFAULT, outer_width, outer_height, NULL, NULL, instance,
                                window);
    if (hwnd == NULL) {
        LOG_ERROR("CreateWindowExW 失败 (GetLastError = %lu)", (unsigned long)GetLastError());
        alloc->free(alloc, window, sizeof(platform_window_t));
        return false;
    }

    win32_center_window_on_monitor(hwnd, (u32)outer_width, (u32)outer_height);

    // 查询实际客户区尺寸（高 DPI 下可能与请求值不同）
    RECT client;
    GetClientRect(hwnd, &client);
    window->client_width = (u32)(client.right - client.left);
    window->client_height = (u32)(client.bottom - client.top);
    window->dpi_scale = (f64)win32_dpi_of_window(hwnd) / 96.0;

    if (desc->fullscreen) {
        win32_apply_fullscreen(window, true);
    }

    if (desc->visible) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
    window->visible = desc->visible;

    *out_window = window;
    return true;
}

void platform_window_destroy(platform_window_t *window)
{
    if (window == NULL) {
        return;
    }

    // 还原全屏状态，让窗口以正常尺寸销毁（否则某些驱动下会留下
    // 一个"屏幕被占满但窗口已消失"的残影）
    if (window->fullscreen) {
        win32_apply_fullscreen(window, false);
    }

    // 释放光标裁剪，否则整个系统的光标会被永久限制在已销毁窗口的矩形里
    ClipCursor(NULL);

    if (window->hwnd != NULL) {
        DestroyWindow(window->hwnd);
        window->hwnd = NULL;
    }

    allocator_t *alloc = window->alloc;
    if (alloc != NULL) {
        alloc->free(alloc, window, sizeof(platform_window_t));
    }
}

bool platform_window_poll_events(platform_window_t *window)
{
    ASSERT(window != NULL);

    input_begin_frame(&window->input);

    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        // 这里必须显式判断 WM_QUIT：
        // PeekMessage 的返回值 0 只表示"队列已空"，与"收到退出消息"是两回事。
        // 混用这两个语义会变成忙等（永远收不到退出）或提前退出（队列一空就退）。
        if (message.message == WM_QUIT) {
            window->should_close = true;
            window->input.quit_requested = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return !window->should_close;
}

bool platform_window_should_close(const platform_window_t *window)
{
    ASSERT(window != NULL);
    return window->should_close;
}

void platform_window_request_close(platform_window_t *window)
{
    ASSERT(window != NULL);
    window->should_close = true;
    window->input.quit_requested = true;
}

const input_state_t *platform_window_input(const platform_window_t *window)
{
    ASSERT(window != NULL);
    return &window->input;
}

void platform_window_get_size(const platform_window_t *window, u32 *out_width, u32 *out_height)
{
    ASSERT(window != NULL);

    if (out_width != NULL) {
        *out_width = window->client_width;
    }
    if (out_height != NULL) {
        *out_height = window->client_height;
    }
}

void platform_window_set_title(platform_window_t *window, const char *utf8_title)
{
    ASSERT(window != NULL);

    wchar_t title[WIN32_TITLE_CAPACITY];
    utf8_to_utf16(utf8_title, title, ENGINE_ARRAY_COUNT(title));
    SetWindowTextW(window->hwnd, title);
}

void platform_window_set_fullscreen(platform_window_t *window, bool fullscreen)
{
    ASSERT(window != NULL);
    win32_apply_fullscreen(window, fullscreen);
}

bool platform_window_is_fullscreen(const platform_window_t *window)
{
    ASSERT(window != NULL);
    return window->fullscreen;
}

void platform_window_set_mouse_locked(platform_window_t *window, bool locked)
{
    ASSERT(window != NULL);

    if (window->mouse_locked == locked) {
        return;
    }
    window->mouse_locked = locked;

    if (locked) {
        win32_update_cursor_clip(window);

        // 刚锁定时光标可能远离窗口中心，那一次的位移是假的。
        // 先回中，并让下一条 WM_MOUSEMOVE 被忽略。
        window->ignore_next_mouse_move = true;
        win32_center_cursor(window);

        SetCursor(NULL);
    } else {
        ClipCursor(NULL);
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    }
}

bool platform_window_is_mouse_locked(const platform_window_t *window)
{
    ASSERT(window != NULL);
    return window->mouse_locked;
}

void platform_window_set_cursor_visible(platform_window_t *window, bool visible)
{
    ASSERT(window != NULL);

    window->cursor_visible = visible;
    if (visible) {
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
    } else {
        SetCursor(NULL);
    }
}

f64 platform_window_dpi_scale(const platform_window_t *window)
{
    ASSERT(window != NULL);
    return window->dpi_scale;
}

platform_native_window_t platform_window_native(const platform_window_t *window)
{
    ASSERT(window != NULL);

    platform_native_window_t native;
    native.window = (void *)window->hwnd;
    native.display = NULL;
    native.instance = (void *)window->instance;
    return native;
}
