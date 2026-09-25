// =============================================================================
//  engine/platform/time.h —— 计时与帧率控制
//
//  分三层：
//    platform_time_*    系统时钟（Win32 下是 QueryPerformanceCounter）
//    frame_clock_t      帧时钟：算出 delta / 平滑 FPS / 帧时间统计
//    frame_limiter_t    帧率限制：把帧时间卡在目标值上
//
//  ---- 关于"帧率稳定 60"到底靠什么 ----
//
//  两条路，优先用第一条：
//   1. 垂直同步（wglSwapIntervalEXT(1)）。SwapBuffers 会阻塞到下一次刷新，
//      帧率天然锁在显示器刷新率上，且没有撕裂。这是首选。
//   2. 帧率限制器。vsync 关闭（例如测性能）时才用。
//
//  为什么帧率限制器不能简单 Sleep(剩余毫秒)：
//  Windows 默认的系统计时器精度是 **15.6ms**。Sleep(16) 可能睡 16ms，
//  也可能睡 31ms，帧率会剧烈抖动。解决办法：
//   * platform_time_init(true) 调 timeBeginPeriod(1) 把精度提到 1ms；
//   * 最后一段时间用忙等（spin）而不是 Sleep，因为 Sleep 无法精确到亚毫秒。
//  两者结合，帧率才能稳定。
// =============================================================================
#ifndef ENGINE_PLATFORM_TIME_H
#define ENGINE_PLATFORM_TIME_H

#include "engine/core/types.h"

typedef struct platform_time_point_t {
    u64 ticks;
} platform_time_point_t;

typedef struct platform_time_source_t {
    u64 frequency; // 每秒计数
} platform_time_source_t;

// 初始化计时系统。
// high_resolution_timer = true 时调用 timeBeginPeriod(1) 提升系统计时器精度。
// 代价：会增加系统级定时器中断频率（Win10 2004 起是进程级，影响已大幅降低），
// 对笔记本续航有一点影响。游戏用 true，工具/CI 用 false。
bool platform_time_init(bool high_resolution_timer);
void platform_time_shutdown(void);

// 高精度计时器是否已生效（即 timeBeginPeriod(1) 成功）。
// 帧率限制器靠它决定忙等窗口的大小，见 frame_limiter_init。
bool platform_time_is_high_resolution(void);

u64 platform_time_frequency(void);

platform_time_point_t platform_time_now(void);
f64 platform_time_to_seconds(platform_time_point_t point);
f64 platform_time_delta_seconds(platform_time_point_t from, platform_time_point_t to);

void platform_sleep_ms(u32 milliseconds);
void platform_sleep_seconds(f64 seconds);

// ---------------------------------------------------------------- 帧时钟
typedef struct frame_clock_t {
    u64 frequency;

    platform_time_point_t start;
    platform_time_point_t previous;
    platform_time_point_t current;

    f64 delta_seconds;   // 本帧耗时（原始值）
    f64 elapsed_seconds; // 自 start 起的总时长
    u64 frame_index;

    f64 delta_ms;     // 平滑后的帧时间
    f64 fps;          // 平滑后的帧率
    f64 raw_delta_ms; // 未平滑，用于发现尖刺
    f64 min_delta_ms; // 统计窗口内的极值
    f64 max_delta_ms;

    f64 smoothing; // 指数平滑系数，越小越平滑
} frame_clock_t;

void frame_clock_init(frame_clock_t *clock);
void frame_clock_reset(frame_clock_t *clock);

// 用当前系统时间推进一帧，返回 delta_seconds。
f64 frame_clock_tick(frame_clock_t *clock);

// 用**指定的**时间点推进一帧。单元测试用它构造确定的时间序列，
// 否则测试要么依赖真实时钟（慢且不稳定），要么没法验证 delta 计算。
f64 frame_clock_tick_at(frame_clock_t *clock, platform_time_point_t now);

// ---------------------------------------------------------------- 帧率限制
typedef struct frame_limiter_t {
    f64  target_fps;
    f64  budget_ms;      // 1000 / target_fps
    f64  spin_margin_ms; // 最后这段时间用忙等而不是 Sleep
    bool enabled;
} frame_limiter_t;

void frame_limiter_init(frame_limiter_t *limiter, f64 target_fps);

// 还需要等多少毫秒。<= 0 表示已经超预算，不该再等。
// 纯函数，不碰时钟 —— 这样它的边界条件可以被完整测试。
f64 frame_limiter_remaining_ms(const frame_limiter_t *limiter, f64 elapsed_ms);

// 等到本帧预算用完。frame_start 是本帧开始的时间点。
void frame_limiter_wait(const frame_limiter_t *limiter, platform_time_point_t frame_start);

#endif // ENGINE_PLATFORM_TIME_H
