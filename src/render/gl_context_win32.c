// =============================================================================
//  src/render/gl_context_win32.c —— WGL 上下文创建（Win32 实现）
//
//  两阶段流程的原因见 gl_context.h 文件头。这里只说实现细节上的坑：
//
//  * 引导窗口和正式窗口必须是**两个不同的窗口**。SetPixelFormat 对同一个
//    窗口只能成功调用一次，而两阶段需要的像素格式不同。
//  * 引导阶段必须在用完三个 WGL 扩展函数指针后立刻 wglMakeCurrent(NULL, NULL)
//    再销毁，否则销毁窗口时会连带失效当前的渲染上下文。
//  * wglChoosePixelFormatARB 在请求 MSAA 但驱动不支持时会返回 0 个可用格式，
//    必须降级重试，而不是直接失败。
//  * SetPixelFormat 需要一份 PIXELFORMATDESCRIPTOR，而 DescribePixelFormat
//    在某些驱动上会漏写 PFD_SUPPORT_OPENGL / PFD_DOUBLE_BUFFER，
//    直接用它去 SetPixelFormat 会失败。要手动补齐这几个标志。
// =============================================================================
#include "engine/render/gl_context.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/gl_api.h"
#include "engine/render/gl_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// =============================================================================
//  WGL 扩展函数
//
//  这几个函数不属于 opengl32.dll 的导出表，必须用 wglGetProcAddress 取，
//  而 wglGetProcAddress 又要求当前线程已经有一个 current 的上下文。
//  这就是必须走"引导上下文"的根本原因。
// =============================================================================
typedef struct wgl_api_t {
    BOOL(GLAPI *choose_pixel_format)(HDC hdc, const int *int_attribs, const FLOAT *float_attribs,
                                     UINT max_formats, int *formats, UINT *num_formats);
    HGLRC(GLAPI *create_context_attribs)(HDC hdc, HGLRC share_context, const int *attribs);
    BOOL(GLAPI *swap_interval)(int interval);
} wgl_api_t;

struct gl_context_t {
    allocator_t *alloc;

    HWND  hwnd;
    HDC   hdc;   // CS_OWNDC 窗口的 DC，与窗口同生命周期，不需要 ReleaseDC
    HGLRC hglrc;

    wgl_api_t wgl;

    bool vsync;
    bool debug_output;
    u32  msaa_samples;

    u32 major;
    u32 minor;

    // glGetString 返回的指针由驱动持有，在上下文存活期间有效。
    const char *version_string;
    const char *vendor_string;
    const char *renderer_string;
    const char *glsl_version_string;
};

// =============================================================================
//  调试消息回调
// =============================================================================
static void GLAPI win32_gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                          GLsizei length, const GLchar *message,
                                          const void *user_param)
{
    ENGINE_UNUSED(source);
    ENGINE_UNUSED(id);
    ENGINE_UNUSED(length);
    ENGINE_UNUSED(user_param);

    // NOTIFICATION 级别会把每一处 API 细节都吐出来（包括无害的冗余状态设置），
    // 开着它日志会被淹没，反而看不见真正的错误。
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }

    const char *type_name = "未知";
    switch (type) {
    case GL_DEBUG_TYPE_ERROR:
        type_name = "错误";
        break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        type_name = "使用了废弃功能";
        break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        type_name = "未定义行为";
        break;
    case GL_DEBUG_TYPE_PORTABILITY:
        type_name = "可移植性";
        break;
    case GL_DEBUG_TYPE_PERFORMANCE:
        type_name = "性能";
        break;
    case GL_DEBUG_TYPE_OTHER:
        type_name = "其他";
        break;
    default:
        break;
    }

    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        LOG_ERROR("[GL] %s: %s", type_name, (const char *)message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        LOG_WARN("[GL] %s: %s", type_name, (const char *)message);
    } else {
        LOG_INFO("[GL] %s: %s", type_name, (const char *)message);
    }
}

// =============================================================================
//  阶段 1：引导
// =============================================================================
static bool win32_bootstrap_wgl(allocator_t *alloc, wgl_api_t *out_wgl)
{
    platform_window_desc_t desc = platform_window_desc_default();
    desc.title = "Polaire Engine WGL bootstrap";
    desc.width = 8;
    desc.height = 8;
    desc.resizable = false;
    desc.visible = false;
    desc.fullscreen = false;
    desc.enable_high_dpi = false; // 正式窗口创建时已经设置过

    platform_window_t *dummy = NULL;
    if (!platform_window_create(alloc, &desc, &dummy)) {
        LOG_ERROR("WGL 引导窗口创建失败");
        return false;
    }

    const platform_native_window_t native = platform_window_native(dummy);
    HWND hwnd = (HWND)native.window;
    HDC  hdc = GetDC(hwnd);

    bool ok = false;
    HGLRC rc = NULL;

    if (hdc == NULL) {
        LOG_ERROR("引导窗口取不到设备上下文");
        goto cleanup;
    }

    {
        PIXELFORMATDESCRIPTOR pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;

        const int format = ChoosePixelFormat(hdc, &pfd);
        if (format == 0) {
            LOG_ERROR("引导窗口 ChoosePixelFormat 失败");
            goto cleanup;
        }
        if (!SetPixelFormat(hdc, format, &pfd)) {
            LOG_ERROR("引导窗口 SetPixelFormat 失败 (GetLastError = %lu)",
                      (unsigned long)GetLastError());
            goto cleanup;
        }
    }

    rc = wglCreateContext(hdc);
    if (rc == NULL) {
        LOG_ERROR("引导上下文 wglCreateContext 失败");
        goto cleanup;
    }
    if (!wglMakeCurrent(hdc, rc)) {
        LOG_ERROR("引导上下文 wglMakeCurrent 失败");
        goto cleanup;
    }

    // 到了这里才有资格取 WGL 扩展函数
    out_wgl->choose_pixel_format =
        (BOOL(GLAPI *)(HDC, const int *, const FLOAT *, UINT, int *, UINT *))wglGetProcAddress(
            "wglChoosePixelFormatARB");
    out_wgl->create_context_attribs =
        (HGLRC(GLAPI *)(HDC, HGLRC, const int *))wglGetProcAddress("wglCreateContextAttribsARB");
    out_wgl->swap_interval = (BOOL(GLAPI *)(int))wglGetProcAddress("wglSwapIntervalEXT");

    ok = (out_wgl->choose_pixel_format != NULL) && (out_wgl->create_context_attribs != NULL);

    if (!ok) {
        LOG_ERROR("驱动不支持 WGL_ARB_pixel_format / WGL_ARB_create_context，"
                  "无法创建 OpenGL 3.2+ 上下文");
    }
    if (out_wgl->swap_interval == NULL) {
        LOG_WARN("驱动不支持 WGL_EXT_swap_control，垂直同步将不可用");
    }

cleanup:
    // 先解绑再删上下文，最后销毁窗口。
    // 顺序反了会让"销毁窗口"顺手失效掉当前上下文，产生难查的副作用。
    wglMakeCurrent(NULL, NULL);
    if (rc != NULL) {
        wglDeleteContext(rc);
    }
    platform_window_destroy(dummy);

    return ok;
}

// =============================================================================
//  阶段 2：正式上下文
// =============================================================================
static bool win32_choose_pixel_format(const wgl_api_t *wgl, HDC hdc, u32 msaa_samples,
                                      int *out_format)
{
    // 先按请求的 MSAA 试一次，失败就去掉 MSAA 重试。
    // wglChoosePixelFormatARB 在不支持时会返回 0 个格式，
    // 而不是返回一个"不带 MSAA 的格式"，所以必须自己降级。
    const u32 attempts[2] = {msaa_samples, 0u};

    for (u32 i = 0; i < 2u; ++i) {
        const u32 samples = attempts[i];
        if (i > 0 && msaa_samples == 0) {
            break; // 本来就没要 MSAA，不用重试
        }

        int attribs[32];
        u32 n = 0;
        attribs[n++] = WGL_DRAW_TO_WINDOW_ARB;
        attribs[n++] = GL_TRUE;
        attribs[n++] = WGL_SUPPORT_OPENGL_ARB;
        attribs[n++] = GL_TRUE;
        attribs[n++] = WGL_DOUBLE_BUFFER_ARB;
        attribs[n++] = GL_TRUE;
        attribs[n++] = WGL_ACCELERATION_ARB;
        attribs[n++] = WGL_FULL_ACCELERATION_ARB;
        attribs[n++] = WGL_PIXEL_TYPE_ARB;
        attribs[n++] = WGL_TYPE_RGBA_ARB;
        attribs[n++] = WGL_COLOR_BITS_ARB;
        attribs[n++] = 32;
        attribs[n++] = WGL_ALPHA_BITS_ARB;
        attribs[n++] = 8;
        attribs[n++] = WGL_DEPTH_BITS_ARB;
        attribs[n++] = 24;
        attribs[n++] = WGL_STENCIL_BITS_ARB;
        attribs[n++] = 8;
        attribs[n++] = WGL_SAMPLE_BUFFERS_ARB;
        attribs[n++] = (samples > 0u) ? GL_TRUE : GL_FALSE;
        attribs[n++] = WGL_SAMPLES_ARB;
        attribs[n++] = (int)samples;
        attribs[n++] = 0;

        int  format = 0;
        UINT num_formats = 0;
        const BOOL result =
            wgl->choose_pixel_format(hdc, attribs, NULL, 1, &format, &num_formats);

        if (result && num_formats > 0 && format != 0) {
            if (samples != msaa_samples) {
                LOG_WARN("请求 %u 倍 MSAA 不可用，已降级为关闭 MSAA", msaa_samples);
            }
            *out_format = format;
            return true;
        }
    }

    return false;
}

static HGLRC win32_create_context_with_fallback(const wgl_api_t *wgl, HDC hdc, u32 want_major,
                                                u32 want_minor, bool core_profile,
                                                bool debug_context, u32 *out_major,
                                                u32 *out_minor)
{
    // 逐级降级：驱动可能不支持请求的版本。直接失败对使用者毫无帮助，
    // 报告"实际拿到了什么"才有用。
    typedef struct version_t {
        u32 major;
        u32 minor;
    } version_t;

    version_t ladder[4];
    u32       ladder_count = 0;

    ladder[ladder_count].major = want_major;
    ladder[ladder_count].minor = want_minor;
    ladder_count++;

    const version_t known[] = {{4u, 6u}, {4u, 5u}, {4u, 3u}, {3u, 3u}, {3u, 2u}};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(known); ++i) {
        const bool lower = (known[i].major < want_major) ||
                           (known[i].major == want_major && known[i].minor < want_minor);
        if (lower && ladder_count < ENGINE_ARRAY_COUNT(ladder)) {
            ladder[ladder_count++] = known[i];
        }
    }

    for (u32 i = 0; i < ladder_count; ++i) {
        int attribs[16];
        u32 n = 0;

        attribs[n++] = WGL_CONTEXT_MAJOR_VERSION_ARB;
        attribs[n++] = (int)ladder[i].major;
        attribs[n++] = WGL_CONTEXT_MINOR_VERSION_ARB;
        attribs[n++] = (int)ladder[i].minor;
        attribs[n++] = WGL_CONTEXT_PROFILE_MASK_ARB;
        attribs[n++] = core_profile ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                                    : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        if (debug_context) {
            attribs[n++] = WGL_CONTEXT_FLAGS_ARB;
            attribs[n++] = WGL_CONTEXT_DEBUG_BIT_ARB;
        }
        attribs[n++] = 0;

        HGLRC rc = wgl->create_context_attribs(hdc, NULL, attribs);
        if (rc != NULL) {
            if (ladder[i].major != want_major || ladder[i].minor != want_minor) {
                LOG_WARN("请求 OpenGL %u.%u 未成功，已降级到 %u.%u", want_major, want_minor,
                         ladder[i].major, ladder[i].minor);
            }
            *out_major = ladder[i].major;
            *out_minor = ladder[i].minor;
            return rc;
        }
    }

    return NULL;
}

// =============================================================================
//  公开 API
// =============================================================================
gl_context_desc_t gl_context_desc_default(void)
{
    gl_context_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.major_version = 4;
    desc.minor_version = 5;
    desc.core_profile = true;
    desc.debug_context = ENGINE_DEBUG ? true : false;
    desc.vsync = true;
    desc.msaa_samples = 4;
    return desc;
}

bool gl_context_create(allocator_t *alloc, const platform_window_t *window,
                       const gl_context_desc_t *desc, gl_context_t **out_context)
{
    ASSERT(alloc != NULL);
    ASSERT(window != NULL);
    ASSERT(desc != NULL);
    ASSERT(out_context != NULL);

    const platform_native_window_t native = platform_window_native(window);
    HWND hwnd = (HWND)native.window;
    if (hwnd == NULL) {
        LOG_ERROR("窗口没有有效的原生句柄");
        return false;
    }

    gl_context_t *context = (gl_context_t *)alloc->alloc(alloc, sizeof(gl_context_t), 16);
    if (context == NULL) {
        LOG_ERROR("上下文对象分配失败");
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->alloc = alloc;
    context->hwnd = hwnd;
    context->vsync = desc->vsync;
    context->msaa_samples = desc->msaa_samples;

    if (!win32_bootstrap_wgl(alloc, &context->wgl)) {
        alloc->free(alloc, context, sizeof(gl_context_t));
        return false;
    }

    // CS_OWNDC 的窗口，GetDC 每次返回同一个 DC，且不需要 ReleaseDC。
    context->hdc = GetDC(hwnd);
    if (context->hdc == NULL) {
        LOG_ERROR("正式窗口取不到设备上下文");
        alloc->free(alloc, context, sizeof(gl_context_t));
        return false;
    }

    int pixel_format = 0;
    if (!win32_choose_pixel_format(&context->wgl, context->hdc, desc->msaa_samples,
                                   &pixel_format)) {
        LOG_ERROR("找不到任何可用的像素格式（连无 MSAA 的全硬件加速格式都没有）");
        goto fail;
    }

    {
        PIXELFORMATDESCRIPTOR pfd;
        memset(&pfd, 0, sizeof(pfd));
        DescribePixelFormat(context->hdc, pixel_format, sizeof(pfd), &pfd);

        // 某些驱动的 DescribePixelFormat 会漏写这几个标志，
        // 直接拿去 SetPixelFormat 会失败。补齐它们。
        pfd.dwFlags |= PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;

        if (!SetPixelFormat(context->hdc, pixel_format, &pfd)) {
            LOG_ERROR("SetPixelFormat 失败 (GetLastError = %lu)", (unsigned long)GetLastError());
            goto fail;
        }
        context->msaa_samples =
            ((pfd.dwFlags & PFD_SUPPORT_GDI) != 0) ? 0u : desc->msaa_samples;
    }

    context->hglrc = win32_create_context_with_fallback(
        &context->wgl, context->hdc, desc->major_version, desc->minor_version, desc->core_profile,
        desc->debug_context, &context->major, &context->minor);
    if (context->hglrc == NULL) {
        LOG_ERROR("OpenGL %u.%u 上下文创建失败。可能原因：显卡驱动过旧、"
                  "远程桌面会话、或运行在没有硬件加速的环境里。",
                  desc->major_version, desc->minor_version);
        goto fail;
    }

    if (!wglMakeCurrent(context->hdc, context->hglrc)) {
        LOG_ERROR("wglMakeCurrent 失败");
        goto fail;
    }

    // 加载函数表。必须在 make current 之后，否则 wglGetProcAddress 一律返回 NULL。
    const gl_api_t *api = gl_api_load();
    if (api == NULL) {
        LOG_ERROR("OpenGL 函数表加载失败");
        goto fail;
    }

    context->version_string = (const char *)api->glGetString(GL_VERSION);
    context->vendor_string = (const char *)api->glGetString(GL_VENDOR);
    context->renderer_string = (const char *)api->glGetString(GL_RENDERER);
    context->glsl_version_string = (const char *)api->glGetString(GL_SHADING_LANGUAGE_VERSION);

    if (context->version_string == NULL) {
        LOG_ERROR("glGetString(GL_VERSION) 返回空，上下文状态异常");
        goto fail;
    }

    // 用驱动报告的版本校正我们记录的版本号（可能比请求值高）
    {
        GLint major = 0;
        GLint minor = 0;
        api->glGetIntegerv(GL_MAJOR_VERSION, &major);
        api->glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major > 0) {
            context->major = (u32)major;
            context->minor = (u32)minor;
        }
    }

    // 垂直同步
    if (context->wgl.swap_interval != NULL) {
        context->wgl.swap_interval(desc->vsync ? 1 : 0);
    } else {
        context->vsync = false;
    }

    // 调试输出
    if (desc->debug_context && api->glDebugMessageCallback != NULL) {
        api->glEnable(GL_DEBUG_OUTPUT);
        api->glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        api->glDebugMessageCallback(win32_gl_debug_callback, NULL);
        context->debug_output = true;
    }

    *out_context = context;
    return true;

fail:
    // 失败路径要完整回滚，否则会留下一个"半初始化"的上下文对象，
    // 而且 GL 上下文泄漏会一直占着显存。
    if (context->hglrc != NULL) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context->hglrc);
    }
    alloc->free(alloc, context, sizeof(gl_context_t));
    return false;
}

void gl_context_destroy(gl_context_t *context)
{
    if (context == NULL) {
        return;
    }

    // 先关掉调试输出：否则销毁过程中触发的 GL 消息会在日志系统已经
    // 关闭之后才回调进来。
    if (context->debug_output) {
        if (gl_api_is_loaded()) {
            const gl_api_t *api = gl_api_load();
            api->glDisable(GL_DEBUG_OUTPUT);
            api->glDebugMessageCallback(NULL, NULL);
        }
        context->debug_output = false;
    }

    if (context->hglrc != NULL) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(context->hglrc);
        context->hglrc = NULL;
    }

    allocator_t *alloc = context->alloc;
    if (alloc != NULL) {
        alloc->free(alloc, context, sizeof(gl_context_t));
    }
}

void gl_context_make_current(gl_context_t *context)
{
    ASSERT(context != NULL);
    wglMakeCurrent(context->hdc, context->hglrc);
}

void gl_context_release_current(void)
{
    wglMakeCurrent(NULL, NULL);
}

void gl_context_swap_buffers(gl_context_t *context)
{
    ASSERT(context != NULL);
    SwapBuffers(context->hdc);
}

bool gl_context_set_vsync(gl_context_t *context, bool enabled)
{
    ASSERT(context != NULL);

    if (context->wgl.swap_interval == NULL) {
        context->vsync = false;
        return false;
    }
    context->wgl.swap_interval(enabled ? 1 : 0);
    context->vsync = enabled;
    return true;
}

bool gl_context_get_vsync(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return context->vsync;
}

u32 gl_context_major_version(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return context->major;
}

u32 gl_context_minor_version(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return context->minor;
}

const char *gl_context_version_string(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return (context->version_string != NULL) ? context->version_string : "unknown";
}

const char *gl_context_vendor_string(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return (context->vendor_string != NULL) ? context->vendor_string : "unknown";
}

const char *gl_context_renderer_string(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return (context->renderer_string != NULL) ? context->renderer_string : "unknown";
}

const char *gl_context_glsl_version_string(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return (context->glsl_version_string != NULL) ? context->glsl_version_string : "unknown";
}

bool gl_context_has_debug_output(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return context->debug_output;
}

u32 gl_context_msaa_samples(const gl_context_t *context)
{
    ASSERT(context != NULL);
    return context->msaa_samples;
}
