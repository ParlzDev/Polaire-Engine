// =============================================================================
//  engine/platform/window.h —— 窗口
//
//  上层（游戏逻辑）只持有 platform_window_t*，永远不接触 HWND。
//  唯一的例外是图形后端：它必须拿到原生句柄才能把渲染表面绑到窗口上，
//  所以提供了一个显式的 platform_window_native()，并在注释里写清"仅限后端使用"。
//
//  ---- 事件泵与输入的关系 ----
//
//  platform_window_poll_events() 一次做完三件事：
//    1. 交换输入状态（previous <- current）
//    2. 把消息队列里的消息全部派发出去（WndProc 往里写输入状态）
//    3. 返回是否已请求退出
//  游戏主循环每帧只调这一个函数，不需要知道消息循环的细节。
//
//  ---- 全屏的实现选择 ----
//
//  用"无边框窗口铺满显示器"而不是 ChangeDisplaySettingsEx 独占全屏：
//   * alt-tab 切换快且不会黑屏重排分辨率；
//   * 不改变桌面分辨率，调试时不会被切成 640x480 卡住；
//   * 多显示器下行为可预期。
//  代价是没有"独占全屏"那一点点延迟优势。对 M1 到 M14 都够用。
// =============================================================================
#ifndef ENGINE_PLATFORM_WINDOW_H
#define ENGINE_PLATFORM_WINDOW_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/platform/input.h"

typedef struct platform_window_t platform_window_t;

// 原生窗口句柄。**只有图形后端应该使用它。**
typedef struct platform_native_window_t {
    void *window;   // Win32: HWND
    void *display;  // X11: Display*（未使用）
    void *instance; // Win32: HINSTANCE
} platform_native_window_t;

typedef struct platform_window_desc_t {
    const char *title; // UTF-8
    u32         width;
    u32         height;
    bool        resizable;
    bool        fullscreen;
    bool        visible;
    bool        enable_high_dpi; // 进程级 DPI 感知；只有第一次创建窗口时生效
} platform_window_desc_t;

platform_window_desc_t platform_window_desc_default(void);

bool platform_window_create(allocator_t *alloc, const platform_window_desc_t *desc,
                            platform_window_t **out_window);
void platform_window_destroy(platform_window_t *window);

// 泵一次消息并更新输入状态。返回 false 表示已请求退出。
bool platform_window_poll_events(platform_window_t *window);

bool platform_window_should_close(const platform_window_t *window);
void platform_window_request_close(platform_window_t *window);

const input_state_t *platform_window_input(const platform_window_t *window);

// 客户区尺寸，单位物理像素（高 DPI 下与逻辑尺寸不同）。
void platform_window_get_size(const platform_window_t *window, u32 *out_width, u32 *out_height);

// title 为 UTF-8，内部转 UTF-16 后调用 CreateWindowExW / SetWindowTextW。
void platform_window_set_title(platform_window_t *window, const char *utf8_title);

void platform_window_set_fullscreen(platform_window_t *window, bool fullscreen);
bool platform_window_is_fullscreen(const platform_window_t *window);

// 锁定鼠标：隐藏光标 + 把光标限制在客户区内 + 每帧回中，
// 从而 mouse_dx/dy 变成不受窗口边界限制的无限位移（FPS 视角用）。
void platform_window_set_mouse_locked(platform_window_t *window, bool locked);
bool platform_window_is_mouse_locked(const platform_window_t *window);

void platform_window_set_cursor_visible(platform_window_t *window, bool visible);

// 每个物理像素对应的 DPI 缩放（96 DPI 时为 1.0）。
f64 platform_window_dpi_scale(const platform_window_t *window);

platform_native_window_t platform_window_native(const platform_window_t *window);

#endif // ENGINE_PLATFORM_WINDOW_H
