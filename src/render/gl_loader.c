// =============================================================================
//  src/render/gl_loader.c —— OpenGL 函数指针加载
//
//  Win32 上的加载规则（每一句都是踩过的坑）：
//
//  1. Windows 自带的 opengl32.dll 只导出 **OpenGL 1.1**。1.2 以上的函数
//     必须用 wglGetProcAddress 逐个取。
//  2. wglGetProcAddress **只对扩展/1.2+ 函数有效**，对 1.1 函数返回 NULL。
//     所以取不到时必须回退到 GetProcAddress(opengl32.dll)。
//  3. 某些驱动在函数不存在时返回 1 / 2 / 3 / -1 这些"假指针"而不是 NULL。
//     不判这几个值，会在调用时跳到非法地址直接崩。
//  4. wglGetProcAddress 需要一个 current 的上下文，否则一律返回 NULL。
//     因此加载必须发生在 gl_context_create 之后。
// =============================================================================
#include "engine/render/gl_api.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// =============================================================================
//  进程级函数表
//
//  OpenGL 本身是进程级的（只有一个 opengl32.dll、一组函数指针），
//  强行做成多实例只会制造假的隔离感。这与日志模块同理，属于
//  docs/M0-设计决策.md 里记录的"有意为之的例外"。
// =============================================================================
static gl_api_t s_gl_api;
static bool     s_loaded = false;

#if defined(_WIN32)

// 用平台原生的函数指针类型做中转。
// 不从 void* 走：把函数指针转成对象指针在 C 里是实现定义行为，
// -Wpedantic 会报；而函数指针之间互相转换是允许的。
typedef PROC gl_proc_t;

static HMODULE s_opengl32 = NULL;

static gl_proc_t win32_get_gl_proc(const char *name)
{
    PROC proc = wglGetProcAddress(name);

    // 坑 3：假指针
    if (proc == NULL || proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 ||
        proc == (PROC)-1) {
        // 坑 2：GL 1.1 的函数不在扩展机制里，得去 opengl32.dll 的导出表找
        if (s_opengl32 == NULL) {
            s_opengl32 = LoadLibraryW(L"opengl32.dll");
        }
        proc = (s_opengl32 != NULL) ? GetProcAddress(s_opengl32, name) : NULL;
    }

    return proc;
}

#else
typedef void (*gl_proc_t)(void);
#endif

// 可选函数：驱动/上下文版本较低时缺失是正常的，不算加载失败。
static bool gl_function_is_optional(const char *name)
{
    return strcmp(name, "glDebugMessageCallback") == 0 ||
           strcmp(name, "glDebugMessageControl") == 0 ||
           strcmp(name, "glDebugMessageInsert") == 0 ||
           strcmp(name, "glGetStringi") == 0 ||   // GL 3.0+
           strcmp(name, "glGenerateMipmap") == 0; // GL 3.0+
}

const gl_api_t *gl_api_load(void)
{
    if (s_loaded) {
        return &s_gl_api;
    }

    memset(&s_gl_api, 0, sizeof(s_gl_api));

#if !defined(_WIN32)
    LOG_ERROR("gl_api_load 目前只有 Win32(WGL) 实现");
    return NULL;
#else
    u32 missing_required = 0;
    u32 missing_optional = 0;

    // 函数指针之间的转换（PROC -> 具体签名）在 C 里是合法的，
    // 但 clang 的 -Wcast-function-type 会对"签名不兼容"报警。
    // 这里正是靠运行期保证签名匹配的场合，显式关掉。
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type"
#pragma clang diagnostic ignored "-Wcast-function-type-strict"
#endif

#define X(ret, name, params)                                                                       \
    s_gl_api.name = (ret(GLAPI *) params)win32_get_gl_proc(#name);                                 \
    if (s_gl_api.name == NULL) {                                                                   \
        if (gl_function_is_optional(#name)) {                                                      \
            ++missing_optional;                                                                    \
            LOG_DEBUG("OpenGL 可选函数不可用: %s", #name);                                          \
        } else {                                                                                   \
            ++missing_required;                                                                    \
            LOG_ERROR("OpenGL 必需函数加载失败: %s", #name);                                        \
        }                                                                                          \
    }
    GL_FUNCTION_LIST(X)
#undef X

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    s_gl_api.missing_functions = missing_required + missing_optional;

    if (missing_required > 0) {
        // 关键：**失败结果不缓存**。
        //
        // wglGetProcAddress 需要当前线程有 current 上下文，否则一律返回 NULL。
        // 如果在这里把"加载失败"也标记成已加载，那么"有人在创建上下文之前
        // 先调了一次 gl_api_load"就会把整张残缺的表永久缓存下来 ——
        // 之后即使上下文就绪，也再拿不到正确的函数指针。
        // 症状是"必需的 GL 函数全部加载失败"，看起来像驱动问题，
        // 实际上是调用顺序问题。
        LOG_ERROR("OpenGL 函数表加载不完整：缺 %u 个必需函数（可选缺失 %u 个）。"
                  "常见原因：当前线程没有 current 的 GL 上下文，或环境没有可用的 OpenGL 驱动。",
                  missing_required, missing_optional);
        return NULL;
    }

    s_loaded = true;
    LOG_DEBUG("OpenGL 函数表加载完成（可选缺失 %u 个）", missing_optional);
    return &s_gl_api;
#endif // _WIN32
}

bool gl_api_is_loaded(void)
{
    return s_loaded;
}

const char *gl_error_string(GLenum error)
{
    switch (error) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM（枚举值非法）";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE（参数数值越界）";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION（当前状态不允许该操作）";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION（帧缓冲不完整）";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY（显存不足）";
    default:
        return "未知的 OpenGL 错误码";
    }
}

bool gl_check_error(const char *file, int line)
{
    if (!s_loaded || s_gl_api.glGetError == NULL) {
        return true;
    }

    bool ok = true;

    // 必须循环取空整个错误队列：glGetError 一次只弹出队首的一个错误。
    // 上限 16 次是为了防止驱动异常时死循环。
    for (u32 i = 0; i < 16u; ++i) {
        const GLenum error = s_gl_api.glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        LOG_ERROR("%s:%d OpenGL 错误: %s (0x%04X)", file, line, gl_error_string(error),
                  (unsigned)error);
        ok = false;
    }

    return ok;
}
