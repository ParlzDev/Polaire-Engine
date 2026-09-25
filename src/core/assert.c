// =============================================================================
//  src/core/assert.c —— 断言失败处理
// =============================================================================
#include "engine/core/assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine/core/log.h"

#define ASSERT_MESSAGE_CAPACITY 1024

static void assert_report(const char *expr, const char *message, const char *file, int line,
                          const char *func)
{
    // 先落盘再中断：崩溃现场的日志如果还在缓冲区里，重启后就什么都没有了。
    if (message != NULL) {
        LOG_FATAL("断言失败: %s", message);
    }
    LOG_FATAL("  表达式: %s", expr);
    LOG_FATAL("  位置  : %s", func);
    LOG_FATAL("  文件  : %s:%d", file, line);

    log_flush();
    ENGINE_DEBUG_BREAK();

    // 调试器不存在或用户选择继续时，仍然要终止进程。
    // 断言失败意味着程序状态未知，继续跑下去只会制造更难查的二次故障。
    abort();
}

void engine_assert_fail(const char *expr, const char *msg, const char *file, int line,
                        const char *func)
{
    assert_report(expr, msg, file, line, func);
}

void engine_assert_fail_fmt(const char *expr, const char *file, int line, const char *func,
                            const char *fmt, ...)
{
    char message[ASSERT_MESSAGE_CAPACITY];

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    assert_report(expr, message, file, line, func);
}
