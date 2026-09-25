// =============================================================================
//  engine/core/assert.h —— 断言与调试断点
//
//  三档语义，不要混用：
//
//    ASSERT(expr)        Debug 下检查并在失败时中断；Release 下整个表达式被
//                        编译掉（expr 不求值）。用于"纯 Debug 自检"，例如
//                        检查内部不变量、数组下标、指针非空。
//
//    VERIFY(expr)        Debug 下与 ASSERT 相同；Release 下仍求值表达式，只是
//                        不检查结果。用于"表达式本身有副作用且必须执行"的场合，
//                        例如 `VERIFY(pool_free(&p, h))`。
//
//    ENGINE_UNREACHABLE() 标记逻辑上不可达的分支。Release 下依然会触发断点，
//                        因为走到这里说明程序已经处于未知状态。
//
//  为什么不用 <assert.h>：
//   * 标准 assert 的开关由 NDEBUG 控制，而 NDEBUG 会被第三方库的构建脚本
//     意外定义/取消定义，导致"Debug 构建里断言被关掉"这种极难排查的问题。
//     这里用 ENGINE_DEBUG，由 CMake 显式按配置注入。
//   * 失败时需要先写日志再中断，标准 assert 做不到。
// =============================================================================
#ifndef ENGINE_CORE_ASSERT_H
#define ENGINE_CORE_ASSERT_H

#include "engine/core/types.h"

// 由 CMake 按构建类型注入；单独编译某个 .c 时的兜底推导。
#ifndef ENGINE_DEBUG
#if defined(_DEBUG) || !defined(NDEBUG)
#define ENGINE_DEBUG 1
#else
#define ENGINE_DEBUG 0
#endif
#endif

// ---------------------------------------------------------------- 调试断点
#if defined(_MSC_VER)
#include <intrin.h>
#define ENGINE_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_DEBUG_BREAK() __builtin_trap()
#else
#include <stdlib.h>
#define ENGINE_DEBUG_BREAK() abort()
#endif

// 由 assert.c 实现。打印日志后中断，不返回。
// 参数 msg 允许为 NULL。
void engine_assert_fail(const char *expr, const char *msg, const char *file, int line,
                        const char *func);

// 带 printf 格式化的版本。之所以单独一个函数而不是让 ASSERT_MSG 自己拼字符串：
// 宏里没法把 __VA_ARGS__ 拆成"格式串 + 参数"两段，只有交给变参函数才行。
void engine_assert_fail_fmt(const char *expr, const char *file, int line, const char *func,
                            const char *fmt, ...) ENGINE_PRINTF_FMT(5, 6);

#if ENGINE_DEBUG

#define ASSERT(expr)                                                                               \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            engine_assert_fail(#expr, NULL, __FILE__, __LINE__, __func__);                         \
        }                                                                                          \
    } while (0)

// 用法：ASSERT_MSG(ptr != NULL, "资源 %s 未加载", name);
#define ASSERT_MSG(expr, ...)                                                                      \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            engine_assert_fail_fmt(#expr, __FILE__, __LINE__, __func__, __VA_ARGS__);              \
        }                                                                                          \
    } while (0)

#define VERIFY(expr) ASSERT(expr)

#else // ENGINE_DEBUG == 0

// Release：表达式整体消失。注意 expr 不被求值，副作用也不会发生。
#define ASSERT(expr) ((void)0)
#define ASSERT_MSG(expr, ...) ((void)0)

// Release：仍然求值，只是不检查。用于必须执行且有副作用的表达式。
#define VERIFY(expr) ((void)(expr))

#endif // ENGINE_DEBUG

// 走到这里 = 程序已处于未知状态，Release 下也必须停。
#define ENGINE_UNREACHABLE()                                                                       \
    do {                                                                                           \
        engine_assert_fail("ENGINE_UNREACHABLE()", "逻辑上不可达的分支被触达", __FILE__, __LINE__, \
                           __func__);                                                              \
        ENGINE_DEBUG_BREAK();                                                                      \
    } while (0)

#endif // ENGINE_CORE_ASSERT_H
