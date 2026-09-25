// =============================================================================
//  engine/core/log.h —— 分级日志
//
//  设计要点：
//   * 双通道：控制台（人看）+ 文件（机器/回溯看），可独立开关。
//   * 线程安全。实现用 C11 原子自旋锁，因此本模块不依赖任何平台头。
//   * 支持外部 sink 回调，为 M11 的编辑器内控制台面板预留接口。
//   * 未调用 log_init 也能用：默认只输出到控制台，级别 INFO。
//     这样"启动早期就崩了"的场景仍然有日志可看。
//
//  关于"无全局状态"约束：
//  日志在语义上就是进程级设施，强行把它实例化并逐层传递，只会污染每一个
//  函数签名。这里选择"线程安全单例"，并在 docs/M0-设计决策.md 中记录为
//  有意为之的例外。断言同理。
// =============================================================================
#ifndef ENGINE_CORE_LOG_H
#define ENGINE_CORE_LOG_H

#include "engine/core/types.h"

typedef enum log_level_t {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
    LOG_LEVEL_COUNT
} log_level_t;

// 外部日志接收器。level 为级别，line 为已格式化好的整行（不含换行），
// user 是注册时传入的上下文指针。
typedef void (*log_sink_fn)(log_level_t level, const char *line, void *user);

// 初始化日志系统。file_path 为 NULL 时只输出控制台。
// 返回 false 表示日志文件打开失败（控制台通道仍然可用）。
bool log_init(const char *file_path, log_level_t min_level);

// 关闭并 flush。可重复调用。
void log_shutdown(void);

bool log_is_initialized(void);

void        log_set_level(log_level_t min_level);
log_level_t log_get_level(void);

void log_set_echo_console(bool enabled);
void log_set_echo_file(bool enabled);

// 注册（或清除，传 NULL）外部接收器。同一时刻只支持一个。
void log_set_sink(log_sink_fn sink, void *user);

// 立即 flush 文件缓冲。崩溃处理器里应该先调它。
void log_flush(void);

// 统计信息，供测试与性能面板使用。
u64 log_lines_written(void);
u64 log_lines_dropped(void); // 因为超出单行缓冲上限而被截断的行数

// 底层入口。业务代码请用下面的宏。
void log_printf(log_level_t level, const char *file, int line, const char *fmt, ...)
    ENGINE_PRINTF_FMT(4, 5);

// 带标签的版本，标签用于区分子系统，例如 LOGT_WARN("render", "着色器编译失败: %s", err)。
void log_printf_tagged(log_level_t level, const char *tag, const char *file, int line,
                       const char *fmt, ...) ENGINE_PRINTF_FMT(5, 6);

#define LOG_TRACE(...) log_printf(LOG_LEVEL_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) log_printf(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) log_printf(LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) log_printf(LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_printf(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) log_printf(LOG_LEVEL_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#define LOGT_TRACE(tag, ...) log_printf_tagged(LOG_LEVEL_TRACE, (tag), __FILE__, __LINE__, __VA_ARGS__)
#define LOGT_DEBUG(tag, ...) log_printf_tagged(LOG_LEVEL_DEBUG, (tag), __FILE__, __LINE__, __VA_ARGS__)
#define LOGT_INFO(tag, ...) log_printf_tagged(LOG_LEVEL_INFO, (tag), __FILE__, __LINE__, __VA_ARGS__)
#define LOGT_WARN(tag, ...) log_printf_tagged(LOG_LEVEL_WARN, (tag), __FILE__, __LINE__, __VA_ARGS__)
#define LOGT_ERROR(tag, ...) log_printf_tagged(LOG_LEVEL_ERROR, (tag), __FILE__, __LINE__, __VA_ARGS__)
#define LOGT_FATAL(tag, ...) log_printf_tagged(LOG_LEVEL_FATAL, (tag), __FILE__, __LINE__, __VA_ARGS__)

#endif // ENGINE_CORE_LOG_H
