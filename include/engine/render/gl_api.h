// =============================================================================
//  engine/render/gl_api.h —— OpenGL 函数指针表（自写 loader）
//
//  ---- 为什么自己写 loader 而不用 glad ----
//
//  1. glad 是生成出来的几万行代码，出问题时只能对着生成器配置看，没法读。
//  2. 我们真正需要的函数是有限的（M1 阶段约 60 个）。用 X-macro 维护一份
//     清单，增删都是改一行，且"加载了哪些函数"一眼可见。
//  3. 加载失败的诊断可控：glad 只是把指针留成 NULL，用起来才发现段错误；
//     这里在加载时就逐个报告哪个函数没拿到。
//  4. glad 把函数指针放在全局变量里并靠宏重定义符号名（`#define glClear
//     glad_glClear`），这种"看不见的全局状态"在排查时很折磨人。
//
//  代价：新用到某个 GL 函数时必须手动加进清单。这是有意的取舍 ——
//  换来的是"引擎用到的 GL 表面"完全显式。
//
//  ---- X-macro 清单 ----
//
//  GL_FUNCTION_LIST(X) 里的每一项形如 X(返回类型, 函数名, (参数列表))。
//  同一份清单被用来生成：结构体成员、加载代码、缺失函数的日志。
//  加函数只需要在清单里加一行，三处自动同步。
// =============================================================================
#ifndef ENGINE_RENDER_GL_API_H
#define ENGINE_RENDER_GL_API_H

#include "engine/render/gl_types.h"

// 调试回调。GL 4.3 起是核心功能，KHR_debug 扩展提供同样的东西。
typedef void(GLAPI *GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                 GLsizei length, const GLchar *message, const void *user_param);

// 清单。注意每项的第三个参数必须带外层括号，否则参数里的逗号会被当成宏分隔符。
#define GL_FUNCTION_LIST(X)                                                                        \
    /* ---- 错误与信息查询 ---- */                                                                 \
    X(GLenum, glGetError, (void))                                                                  \
    X(const GLubyte *, glGetString, (GLenum name))                                                 \
    X(const GLubyte *, glGetStringi, (GLenum name, GLuint index))                                  \
    X(void, glGetIntegerv, (GLenum pname, GLint *data))                                            \
    X(void, glGetFloatv, (GLenum pname, GLfloat *data))                                            \
    /* ---- 视口与清屏 ---- */                                                                     \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))                         \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height))                          \
    X(void, glClear, (GLbitfield mask))                                                            \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                            \
    X(void, glClearDepth, (GLdouble depth))                                                        \
    /* ---- 状态开关 ---- */                                                                       \
    X(void, glEnable, (GLenum cap))                                                                \
    X(void, glDisable, (GLenum cap))                                                               \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))                                         \
    X(void, glBlendFuncSeparate, (GLenum src_rgb, GLenum dst_rgb, GLenum src_a, GLenum dst_a))     \
    X(void, glBlendEquation, (GLenum mode))                                                        \
    X(void, glBlendEquationSeparate, (GLenum mode_rgb, GLenum mode_alpha))                         \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                     \
    X(void, glDepthFunc, (GLenum func))                                                            \
    X(void, glDepthMask, (GLboolean flag))                                                         \
    X(void, glClearStencil, (GLint s))                                                             \
    X(void, glCullFace, (GLenum mode))                                                             \
    X(void, glFrontFace, (GLenum mode))                                                            \
    X(void, glPolygonMode, (GLenum face, GLenum mode))                                             \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units))                                      \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                            \
    X(void, glFinish, (void))                                                                      \
    X(void, glFlush, (void))                                                                       \
    /* ---- 着色器 ---- */                                                                         \
    X(GLuint, glCreateShader, (GLenum type))                                                       \
    X(void, glShaderSource,                                                                        \
      (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))            \
    X(void, glCompileShader, (GLuint shader))                                                      \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params))                           \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei buf_size, GLsizei *length, GLchar *info_log)) \
    X(void, glDeleteShader, (GLuint shader))                                                       \
    X(GLuint, glCreateProgram, (void))                                                             \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                       \
    X(void, glDetachShader, (GLuint program, GLuint shader))                                       \
    X(void, glLinkProgram, (GLuint program))                                                       \
    X(void, glValidateProgram, (GLuint program))                                                   \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params))                         \
    X(void, glGetProgramInfoLog,                                                                   \
      (GLuint program, GLsizei buf_size, GLsizei *length, GLchar *info_log))                       \
    X(void, glUseProgram, (GLuint program))                                                        \
    X(void, glDeleteProgram, (GLuint program))                                                     \
    /* ---- uniform ---- */                                                                        \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))                           \
    X(void, glUniform1i, (GLint location, GLint v0))                                               \
    X(void, glUniform1f, (GLint location, GLfloat v0))                                             \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                                 \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))                     \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))         \
    X(void, glUniformMatrix4fv,                                                                    \
      (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))                  \
    /* ---- 缓冲与顶点数组 ---- */                                                                 \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers))                                            \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                          \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage))        \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data))  \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers))                                   \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays))                                        \
    X(void, glBindVertexArray, (GLuint array))                                                     \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays))                               \
    X(void, glEnableVertexAttribArray, (GLuint index))                                             \
    X(void, glDisableVertexAttribArray, (GLuint index))                                            \
    X(void, glVertexAttribPointer,                                                                 \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,                \
       const void *pointer))                                                                       \
    X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                                 \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                               \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void *indices))        \
    X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances))   \
    X(void, glDrawElementsInstanced,                                                               \
      (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances))           \
    /* ---- uniform buffer ---- */                                                                 \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))                        \
    X(void, glBindBufferRange,                                                                     \
      (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))              \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar *name))                        \
    X(void, glUniformBlockBinding, (GLuint program, GLuint block_index, GLuint binding))           \
    X(void, glGetActiveUniformBlockiv,                                                             \
      (GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params))                     \
    /* ---- 采样器对象 ---- */                                                                     \
    X(void, glGenSamplers, (GLsizei count, GLuint *samplers))                                      \
    X(void, glDeleteSamplers, (GLsizei count, const GLuint *samplers))                             \
    X(void, glBindSampler, (GLuint unit, GLuint sampler))                                          \
    X(void, glSamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                      \
    X(void, glSamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))                    \
    /* ---- 帧缓冲对象（离屏渲染）---- */                                                           \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers))                                  \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))                         \
    X(void, glFramebufferTexture2D,                                                                \
      (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))           \
    X(void, glFramebufferRenderbuffer,                                                             \
      (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer))          \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                           \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers))                                \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                              \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers))                       \
    X(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width,           \
                                    GLsizei height))                                               \
    X(void, glRenderbufferStorageMultisample,                                                      \
      (GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height))      \
    X(void, glBlitFramebuffer,                                                                     \
      (GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1, GLint dst_x0, GLint dst_y0,         \
       GLint dst_x1, GLint dst_y1, GLbitfield mask, GLenum filter))                                \
    X(void, glDrawBuffer, (GLenum buf))                                                            \
    X(void, glReadBuffer, (GLenum src))                                                            \
    X(void, glReadPixels,                                                                          \
      (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)) \
    /* ---- 纹理 ---- */                                                                           \
    X(void, glGenTextures, (GLsizei n, GLuint *textures))                                          \
    X(void, glBindTexture, (GLenum target, GLuint texture))                                        \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures))                                 \
    X(void, glActiveTexture, (GLenum texture))                                                     \
    X(void, glTexImage2D,                                                                          \
      (GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height,           \
       GLint border, GLenum format, GLenum type, const void *pixels))                              \
    X(void, glTexSubImage2D,                                                                       \
      (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,    \
       GLenum format, GLenum type, const void *pixels))                                            \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                           \
    X(void, glGenerateMipmap, (GLenum target))                                                     \
    /* ---- 调试输出（4.3+，属于可选功能）---- */                                                   \
    X(void, glDebugMessageCallback, (GLDEBUGPROC callback, const void *user_param))                \
    X(void, glDebugMessageControl,                                                                 \
      (GLenum source, GLenum type, GLenum severity, GLsizei count, const GLuint *ids,              \
       GLboolean enabled))                                                                         \
    X(void, glDebugMessageInsert,                                                                  \
      (GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,                     \
       const GLchar *message))

// ---------------------------------------------------------------- 函数表
typedef struct gl_api_t {
#define X(ret, name, params) ret(GLAPI *name) params;
    GL_FUNCTION_LIST(X)
#undef X

    // 加载时没拿到的函数数量。非 0 说明有功能不可用，具体是哪些看日志。
    u32 missing_functions;
} gl_api_t;

// 加载全部函数指针。
//
// 成功返回指向进程级函数表的指针（该表生命周期 = 进程）。
// 返回 NULL 表示有**必需**函数没加载到 —— 这通常意味着当前环境根本没有
// 可用的 OpenGL 驱动（例如跑在纯软件、或者没有显卡驱动的机器上）。
//
// 关于"全局状态"：OpenGL 本身就是进程级的（只有一个 opengl32.dll，
// 一组函数指针）。强行做成多实例只会制造假的隔离感。这与日志模块同理，
// 属于 docs/M0-设计决策.md 里记录的"有意为之的例外"。
const gl_api_t *gl_api_load(void);

// 是否已经加载过。
bool gl_api_is_loaded(void);

// 把 GL 错误码翻译成可读字符串。
const char *gl_error_string(GLenum error);

// 检查并清空 GL 错误队列。发现错误时记日志（带 file/line）并返回 false。
// 只在 Debug 构建下做实际检查，Release 下直接返回 true（避免每帧多一次
// glGetError 往返，它会强制刷新命令队列）。
bool gl_check_error(const char *file, int line);

#if ENGINE_DEBUG
#define GL_CHECK() gl_check_error(__FILE__, __LINE__)
#else
#define GL_CHECK() ((void)0)
#endif

#endif // ENGINE_RENDER_GL_API_H
