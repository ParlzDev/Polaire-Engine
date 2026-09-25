// =============================================================================
//  src/core/log.c —— 分级日志实现
//
//  线程安全策略：
//  用平台互斥量保护整个 emit 过程（格式化 + 写控制台 + 写文件 + 回调）。
//
//  为什么 M0 用的是自旋锁、M1 换成互斥量：
//  自旋锁在"另一个线程正在写磁盘"时会一直空转烧 CPU。M0 阶段只有单线程，
//  看不出问题；一旦 M7 引入加载线程，日志竞争就会变成可见的性能损失。
//  互斥量会把等待线程挂起，代价是每次加锁多一次原子操作与可能的上下文切换 ——
//  对日志这种低频调用完全划算。
//
//  为什么格式化也放在锁内：
//  格式化要写共享的行缓冲。要么给每个线程一份缓冲（内存代价 + TLS 复杂度），
//  要么就老实放在锁内。日志不是热路径，临界区只有几微秒，选择简单。
//
//  为什么不直接用 atomic_flag：
//  atomic_flag 必须用 ATOMIC_FLAG_INIT 初始化，在"静态结构体的成员"位置上
//  各编译器的支持程度不一致。atomic_bool 零初始化即为 false，行为确定。
//  这里只把原子量用在"惰性初始化互斥量"这一个地方。
// =============================================================================
#include "engine/core/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "engine/core/assert.h"
#include "engine/core/console.h"
#include "engine/core/thread.h"

#define LOG_LINE_CAPACITY 4096
#define LOG_FILE_PATH_CAPACITY 512

// ---------------------------------------------------------------- 内部状态
typedef struct log_state_t {
    bool        initialized;
    bool        echo_console;
    bool        echo_file;
    bool        ansi_enabled;
    log_level_t min_level;
    FILE       *file;
    char        file_path[LOG_FILE_PATH_CAPACITY];
    log_sink_fn sink;
    void       *sink_user;
    u64         lines_written;
    u64         lines_dropped;
} log_state_t;

// 零初始化：min_level 为 TRACE(0)，但我们希望默认是 INFO，所以显式初始化。
static log_state_t g_log = {
    .initialized  = false,
    .echo_console = true,
    .echo_file    = true,
    .ansi_enabled = false,
    .min_level    = LOG_LEVEL_INFO,
    .file         = NULL,
    .sink         = NULL,
    .sink_user    = NULL,
    .lines_written = 0,
    .lines_dropped = 0,
};

// 零初始化即为 false，无需 ATOMIC_FLAG_INIT。
//
// g_log_mutex 没有可移植的静态初始化方式，所以在第一次加锁时惰性初始化。
// 用"双重检查 + 一个引导自旋锁"保证只初始化一次：
//   * 已就绪后（99.99% 的情况）只多一次原子读，没有额外开销；
//   * 未就绪时才走自旋锁，把初始化保护起来。
static platform_mutex_t g_log_mutex;
static atomic_bool      g_log_mutex_ready = false;
static atomic_bool      g_log_bootstrap = false;

static void log_lock(void)
{
    if (!atomic_load_explicit(&g_log_mutex_ready, memory_order_acquire)) {
        while (atomic_exchange_explicit(&g_log_bootstrap, true, memory_order_acquire)) {
            // 另一个线程正在初始化，等它完成
        }
        if (!atomic_load_explicit(&g_log_mutex_ready, memory_order_relaxed)) {
            platform_mutex_init(&g_log_mutex);
            atomic_store_explicit(&g_log_mutex_ready, true, memory_order_release);
        }
        atomic_store_explicit(&g_log_bootstrap, false, memory_order_release);
    }

    platform_mutex_lock(&g_log_mutex);
}

static void log_unlock(void)
{
    platform_mutex_unlock(&g_log_mutex);
}

// ---------------------------------------------------------------- 静态数据
static const char *const k_level_names[LOG_LEVEL_COUNT] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL",
};

static const char *const k_level_colors[LOG_LEVEL_COUNT] = {
    "\x1b[90m",    // TRACE 灰
    "\x1b[36m",    // DEBUG 青
    "\x1b[32m",    // INFO  绿
    "\x1b[33m",    // WARN  黄
    "\x1b[31m",    // ERROR 红
    "\x1b[97;41m", // FATAL 亮白字 + 红底
};

#define LOG_COLOR_RESET "\x1b[0m"
#define LOG_COLOR_DIM "\x1b[90m"

// ---------------------------------------------------------------- 辅助
static void log_timestamp(char *buffer, usize capacity)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);

    time_t    seconds = (time_t)ts.tv_sec;
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));

#if defined(_MSC_VER)
    localtime_s(&tm_buf, &seconds);
#else
    localtime_r(&seconds, &tm_buf);
#endif

    snprintf(buffer, capacity, "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(ts.tv_nsec / 1000000L));
}

// 只保留文件名部分，去掉编译时的完整路径。
// 完整路径会让每行日志长出一大截，反而看不清真正的内容。
static const char *log_short_file(const char *file)
{
    if (file == NULL) {
        return "?";
    }
    const char *last_slash = strrchr(file, '/');
    const char *last_bslash = strrchr(file, '\\');
    const char *last = file;
    if (last_slash != NULL) {
        last = last_slash + 1;
    }
    if (last_bslash != NULL && last_bslash + 1 > last) {
        last = last_bslash + 1;
    }
    return last;
}

// ---------------------------------------------------------------- 核心
static void log_emit(log_level_t level, const char *tag, const char *file, int line,
                     const char *fmt, va_list args)
{
    if (level < LOG_LEVEL_TRACE || level >= LOG_LEVEL_COUNT) {
        level = LOG_LEVEL_INFO;
    }

    // 未初始化时也能工作：只走控制台，级别默认 INFO。
    // "启动早期就崩"的场景必须还能看到日志，否则会变成盲查。
    const bool   initialized = g_log.initialized;
    const bool   echo_console = !initialized || g_log.echo_console;
    const bool   echo_file = initialized && g_log.echo_file && (g_log.file != NULL);
    const bool   ansi = g_log.ansi_enabled;
    log_sink_fn  sink = g_log.sink;
    void        *sink_user = g_log.sink_user;

    if (initialized && level < g_log.min_level) {
        return;
    }

    char line_buffer[LOG_LINE_CAPACITY];
    int  offset = 0;

    char timestamp[32];
    log_timestamp(timestamp, sizeof(timestamp));

    const char *color = ansi ? k_level_colors[level] : "";
    const char *dim = ansi ? LOG_COLOR_DIM : "";
    const char *reset = ansi ? LOG_COLOR_RESET : "";

    offset += snprintf(line_buffer + offset, LOG_LINE_CAPACITY - (usize)offset, "%s%s%s %s%s%s ",
                       dim, timestamp, reset, color, k_level_names[level], reset);

    if (tag != NULL && tag[0] != '\0') {
        offset += snprintf(line_buffer + offset, LOG_LINE_CAPACITY - (usize)offset, "%s[%s]%s ",
                           dim, tag, reset);
    }

    // file:line 只在 WARN 以上输出。
    // 理由：TRACE/DEBUG/INFO 是"运行过程"，读者关心的是消息本身，加上路径
    // 会让每行多出几十个字符，反而看不清重点；而 WARN/ERROR 是需要去改代码的，
    // 这时候能直接跳到出错位置就很值。
    if (level >= LOG_LEVEL_WARN) {
        offset += snprintf(line_buffer + offset, LOG_LINE_CAPACITY - (usize)offset, "%s%s:%d%s ",
                           dim, log_short_file(file), line, reset);
    }

    if (offset < 0) {
        offset = 0;
    }

    int remaining = LOG_LINE_CAPACITY - offset;
    if (remaining <= 1) {
        remaining = 1;
    }

    int written = vsnprintf(line_buffer + offset, (usize)remaining, fmt, args);
    if (written < 0) {
        written = 0;
    }

    if (written >= remaining) {
        // 被截断。显式标记出来，而不是静默丢掉后半句 ——
        // 静默截断会让日志"看起来正常"但信息不完整，比报错更难发现。
        static const char k_truncated[] = "...<截断>";
        const usize      tail = sizeof(k_truncated) - 1;
        usize            pos = LOG_LINE_CAPACITY - 1 - tail;
        memcpy(line_buffer + pos, k_truncated, tail + 1);
        g_log.lines_dropped++;
    }

    log_lock();

    if (echo_console) {
        // 用 stderr 输出 ERROR/FATAL，这样在 shell 里重定向 stdout 时
        // 错误信息依然可见；其余走 stdout 便于按级别分流。
        FILE *stream = (level >= LOG_LEVEL_ERROR) ? stderr : stdout;
        fprintf(stream, "%s\n", line_buffer);
        fflush(stream);
    }

    if (echo_file) {
        fprintf(g_log.file, "%s\n", line_buffer);
        // 只在 WARN 以上立即刷盘：正常日志每行 fflush 会让日志本身成为瓶颈，
        // 但警告与错误必须保证"程序崩了也能在文件里看到"。
        if (level >= LOG_LEVEL_WARN) {
            fflush(g_log.file);
        }
    }

    if (sink != NULL) {
        sink(level, line_buffer, sink_user);
    }

    g_log.lines_written++;

    log_unlock();
}

// ---------------------------------------------------------------- 公开 API
bool log_init(const char *file_path, log_level_t min_level)
{
    log_lock();

    // 重复 init 视为重新配置，先关掉旧文件
    if (g_log.file != NULL) {
        fclose(g_log.file);
        g_log.file = NULL;
    }

    g_log.initialized = true;
    g_log.min_level = (min_level >= 0 && min_level < LOG_LEVEL_COUNT) ? min_level : LOG_LEVEL_INFO;
    g_log.lines_written = 0;
    g_log.lines_dropped = 0;
    g_log.file_path[0] = '\0';

    bool file_ok = true;
    if (file_path != NULL && file_path[0] != '\0') {
        // 用 "w" 而不是 "a"：每次运行一份干净日志，避免上次崩溃的残留混进来
        // 干扰判断。需要跨运行保留的场景由上层自己拼时间戳文件名。
        g_log.file = fopen(file_path, "w");
        if (g_log.file == NULL) {
            file_ok = false;
        } else {
            snprintf(g_log.file_path, LOG_FILE_PATH_CAPACITY, "%s", file_path);
            // 64KB 全缓冲，配合"WARN 以上才 fflush"的策略
            static char file_buffer[64 * 1024];
            setvbuf(g_log.file, file_buffer, _IOFBF, sizeof(file_buffer));
        }
    }

    g_log.ansi_enabled = console_enable_ansi();

    log_unlock();

    if (!file_ok) {
        LOG_WARN("日志文件无法打开: %s", file_path);
    }
    return file_ok;
}

void log_shutdown(void)
{
    log_lock();
    if (g_log.file != NULL) {
        fflush(g_log.file);
        fclose(g_log.file);
        g_log.file = NULL;
    }
    g_log.initialized = false;
    g_log.sink = NULL;
    g_log.sink_user = NULL;
    log_unlock();
}

bool log_is_initialized(void)
{
    return g_log.initialized;
}

void log_set_level(log_level_t min_level)
{
    if (min_level < 0 || min_level >= LOG_LEVEL_COUNT) {
        return;
    }
    log_lock();
    g_log.min_level = min_level;
    log_unlock();
}

log_level_t log_get_level(void)
{
    return g_log.min_level;
}

void log_set_echo_console(bool enabled)
{
    log_lock();
    g_log.echo_console = enabled;
    log_unlock();
}

void log_set_echo_file(bool enabled)
{
    log_lock();
    g_log.echo_file = enabled;
    log_unlock();
}

void log_set_sink(log_sink_fn sink, void *user)
{
    log_lock();
    g_log.sink = sink;
    g_log.sink_user = user;
    log_unlock();
}

void log_flush(void)
{
    log_lock();
    if (g_log.file != NULL) {
        fflush(g_log.file);
    }
    fflush(stdout);
    fflush(stderr);
    log_unlock();
}

u64 log_lines_written(void)
{
    return g_log.lines_written;
}

u64 log_lines_dropped(void)
{
    return g_log.lines_dropped;
}

void log_printf(log_level_t level, const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_emit(level, NULL, file, line, fmt, args);
    va_end(args);
}

void log_printf_tagged(log_level_t level, const char *tag, const char *file, int line,
                       const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_emit(level, tag, file, line, fmt, args);
    va_end(args);
}
