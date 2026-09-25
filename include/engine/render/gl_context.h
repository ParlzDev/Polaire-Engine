// =============================================================================
//  engine/render/gl_context.h —— OpenGL 上下文
//
//  职责边界：
//   * 上下文**不管窗口**。窗口生命周期由 platform_window_t 负责，
//     上下文只借用窗口的原生句柄来创建渲染表面。
//   * 上下文**不管渲染**。shader / buffer / draw 都是上层的事（M2 才做抽象）。
//     本模块只负责"拿到一个可用的 GL 上下文，并且能把它 make current / swap"。
//
//  ---- Win32 下创建 4.5 Core Context 的两阶段流程（本模块实现的核心）----
//
//  Windows 自带的 opengl32.dll 只导出 OpenGL 1.1。想拿 4.5 必须靠 WGL 扩展：
//    wglChoosePixelFormatARB / wglCreateContextAttribsARB / wglSwapIntervalEXT
//  而这些扩展函数指针本身**必须先有一个 current 的 GL 上下文**才能用
//  wglGetProcAddress 取到。于是必须两阶段：
//
//    阶段 1（引导）：建一个临时窗口 + 一个 1.1 的 legacy context，
//                   make current，取到上面三个函数指针。
//    阶段 2（正式）：销毁引导窗口，在真实窗口上用 ARB 接口挑像素格式、
//                   创建 4.5 core context，make current，加载函数表。
//
//  踩坑点：引导窗口和正式窗口必须是**不同的窗口**。因为 SetPixelFormat
//  对同一个窗口只能调用一次，而两阶段需要的像素格式不同。
// =============================================================================
#ifndef ENGINE_RENDER_GL_CONTEXT_H
#define ENGINE_RENDER_GL_CONTEXT_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/platform/window.h"

typedef struct gl_context_t gl_context_t;

typedef struct gl_context_desc_t {
    u32  major_version; // 默认 4
    u32  minor_version; // 默认 5
    bool core_profile;  // 默认 true（前向兼容，不含废弃功能）
    bool debug_context; // 默认：Debug 构建下 true，Release 下 false
    bool vsync;         // 默认 true
    u32  msaa_samples;  // 默认 4；0 表示关闭 MSAA
} gl_context_desc_t;

gl_context_desc_t gl_context_desc_default(void);

// 在指定窗口上创建上下文。alloc 用于分配上下文对象自身。
bool gl_context_create(allocator_t *alloc, const platform_window_t *window,
                       const gl_context_desc_t *desc, gl_context_t **out_context);

void gl_context_destroy(gl_context_t *context);

// 把上下文设为当前线程的 current。多线程渲染时每个渲染线程各调一次。
void gl_context_make_current(gl_context_t *context);

// 解绑当前线程的上下文（例如线程退出前）。
void gl_context_release_current(void);

// 交换前后缓冲。开启 vsync 时这个调用会阻塞到下一次屏幕刷新。
void gl_context_swap_buffers(gl_context_t *context);

bool gl_context_set_vsync(gl_context_t *context, bool enabled);
bool gl_context_get_vsync(const gl_context_t *context);

// 上下文实际拿到的版本（由驱动决定，可能高于请求值）。
u32 gl_context_major_version(const gl_context_t *context);
u32 gl_context_minor_version(const gl_context_t *context);

const char *gl_context_version_string(const gl_context_t *context);  // "4.6.0 NVIDIA 551.23"
const char *gl_context_vendor_string(const gl_context_t *context);
const char *gl_context_renderer_string(const gl_context_t *context);
const char *gl_context_glsl_version_string(const gl_context_t *context);

// 是否拿到了调试上下文（GL_KHR_debug 的调试消息回调是否可用）。
bool gl_context_has_debug_output(const gl_context_t *context);

u32 gl_context_msaa_samples(const gl_context_t *context);

#endif // ENGINE_RENDER_GL_CONTEXT_H
