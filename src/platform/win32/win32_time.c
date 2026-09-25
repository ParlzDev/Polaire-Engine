// =============================================================================
//  src/platform/win32/win32_time.c —— 计时与帧率控制（Win32 实现）
// =============================================================================
#include "engine/platform/time.h"

#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// timeBeginPeriod / timeEndPeriod 在 winmm 里。
// 为了这两个函数把整个 mmsystem.h 拖进来不划算，这里直接声明原型。
// WINMMAPI / MMRESULT / UINT 在 windows.h 里已经有了，用 #ifndef 兜底。
#ifndef MMRESULT
typedef unsigned int MMRESULT;
#endif
#ifndef WINMMAPI
#define WINMMAPI __declspec(dllimport)
#endif
#define MYENGINE_TIMERR_NOERROR 0

WINMMAPI MMRESULT __stdcall timeBeginPeriod(UINT uPeriod);
WINMMAPI MMRESULT __stdcall timeEndPeriod(UINT uPeriod);

#include "engine/core/assert.h"

// ---------------------------------------------------------------- 内部状态
typedef struct win32_time_state_t {
    LARGE_INTEGER frequency;
    bool initialized;
    bool period_raised;
} win32_time_state_t;

static win32_time_state_t g_time = {0};

// ---------------------------------------------------------------- 初始化
bool platform_time_init(bool high_resolution_timer)
{
    if (!g_time.initialized) {
        // QueryPerformanceFrequency 的结果在进程生命周期内恒定，
        // 每次取时间都调用它属于纯浪费。缓存一次。
        if (!QueryPerformanceFrequency(&g_time.frequency) || g_time.frequency.QuadPart <= 0) {
            return false;
        }
        g_time.initialized = true;
    }

    if (high_resolution_timer && !g_time.period_raised) {
        // 把系统计时器分辨率从默认的 ~15.6ms 提到 1ms。
        // 不做这一步，Sleep(1) 可能睡 15.6ms，帧率限制器会剧烈抖动。
        // 代价：定时器中断变频繁，略微影响功耗。Win10 2004 起该设置是
        // 进程级的，不再拖累整个系统。
        if (timeBeginPeriod(1) == MYENGINE_TIMERR_NOERROR) {
            g_time.period_raised = true;
        }
    }

    return true;
}

void platform_time_shutdown(void)
{
    if (g_time.period_raised) {
        timeEndPeriod(1);
        g_time.period_raised = false;
    }
    g_time.initialized = false;
}

bool platform_time_is_high_resolution(void)
{
    return g_time.period_raised;
}

// ---------------------------------------------------------------- 基本查询
u64 platform_time_frequency(void)
{
    if (!g_time.initialized) {
        // 没初始化过就顺手初始化一次，避免调用方必须记住调用顺序。
        platform_time_init(false);
    }
    return (u64)g_time.frequency.QuadPart;
}

platform_time_point_t platform_time_now(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    platform_time_point_t point;
    point.ticks = (u64)counter.QuadPart;
    return point;
}

f64 platform_time_to_seconds(platform_time_point_t point)
{
    const u64 frequency = platform_time_frequency();
    if (frequency == 0) {
        return 0.0;
    }
    return (f64)point.ticks / (f64)frequency;
}

f64 platform_time_delta_seconds(platform_time_point_t from, platform_time_point_t to)
{
    if (to.ticks < from.ticks) {
        return 0.0; // 计数器异常回退，当作 0 处理而不是返回负数
    }
    const u64 frequency = platform_time_frequency();
    if (frequency == 0) {
        return 0.0;
    }
    return (f64)(to.ticks - from.ticks) / (f64)frequency;
}

void platform_sleep_ms(u32 milliseconds)
{
    if (milliseconds == 0) {
        return;
    }
    Sleep((DWORD)milliseconds);
}

void platform_sleep_seconds(f64 seconds)
{
    if (seconds <= 0.0) {
        return;
    }
    f64 ms = seconds * 1000.0;
    if (ms > 4294967295.0) {
        ms = 4294967295.0;
    }
    // 向下取整：宁可少睡一点让外层继续循环，也不要睡过头错过帧时间。
    Sleep((DWORD)ms);
}

// =============================================================================
//  帧时钟
// =============================================================================
#define FRAME_CLOCK_DEFAULT_SMOOTHING 0.1

void frame_clock_init(frame_clock_t *clock)
{
    ASSERT(clock != NULL);

    memset(clock, 0, sizeof(*clock));
    clock->frequency = platform_time_frequency();
    clock->smoothing = FRAME_CLOCK_DEFAULT_SMOOTHING;
    clock->start = platform_time_now();
    clock->previous = clock->start;
    clock->current = clock->start;
    clock->min_delta_ms = 1.0e9;
}

void frame_clock_reset(frame_clock_t *clock)
{
    ASSERT(clock != NULL);

    const f64 smoothing = (clock->smoothing > 0.0) ? clock->smoothing : FRAME_CLOCK_DEFAULT_SMOOTHING;

    clock->start = platform_time_now();
    clock->previous = clock->start;
    clock->current = clock->start;
    clock->delta_seconds = 0.0;
    clock->elapsed_seconds = 0.0;
    clock->frame_index = 0;
    clock->delta_ms = 0.0;
    clock->fps = 0.0;
    clock->raw_delta_ms = 0.0;
    clock->min_delta_ms = 1.0e9;
    clock->max_delta_ms = 0.0;
    clock->smoothing = smoothing;
}

f64 frame_clock_tick_at(frame_clock_t *clock, platform_time_point_t now)
{
    ASSERT(clock != NULL);

    clock->previous = clock->current;
    clock->current = now;

    const f64 previous_seconds = platform_time_to_seconds(clock->previous);
    const f64 current_seconds = platform_time_to_seconds(clock->current);

    f64 delta = current_seconds - previous_seconds;
    if (delta < 0.0) {
        delta = 0.0;
    }

    clock->delta_seconds = delta;
    clock->elapsed_seconds = current_seconds - platform_time_to_seconds(clock->start);
    clock->raw_delta_ms = delta * 1000.0;
    clock->frame_index++;

    // 指数滑动平均。用 EMA 而不是"最近 N 帧取平均"：
    // EMA 不需要存历史数组，且对突变反应更快。
    clock->delta_ms += (clock->raw_delta_ms - clock->delta_ms) * clock->smoothing;
    if (clock->delta_ms > 0.0) {
        clock->fps = 1000.0 / clock->delta_ms;
    }

    if (clock->frame_index == 1) {
        clock->min_delta_ms = clock->raw_delta_ms;
        clock->max_delta_ms = clock->raw_delta_ms;
    } else {
        if (clock->raw_delta_ms < clock->min_delta_ms) {
            clock->min_delta_ms = clock->raw_delta_ms;
        }
        if (clock->raw_delta_ms > clock->max_delta_ms) {
            clock->max_delta_ms = clock->raw_delta_ms;
        }
    }

    return delta;
}

f64 frame_clock_tick(frame_clock_t *clock)
{
    return frame_clock_tick_at(clock, platform_time_now());
}

// =============================================================================
//  帧率限制器
// =============================================================================
void frame_limiter_init(frame_limiter_t *limiter, f64 target_fps)
{
    ASSERT(limiter != NULL);

    memset(limiter, 0, sizeof(*limiter));

    if (target_fps > 0.0) {
        limiter->target_fps = target_fps;
        limiter->budget_ms = 1000.0 / target_fps;
        limiter->enabled = true;
    }

    // 忙等窗口的大小取决于系统计时器精度：
    //  * 已经调过 timeBeginPeriod(1)：Sleep 精度约 1ms，留 1.5ms 忙等足够。
    //  * 没调过（默认 15.6ms 粒度）：Sleep 会严重超时，必须把忙等窗口
    //    放大到接近一个 tick，否则帧率会来回抖。代价是这段时间烧 CPU，
    //    所以这种模式下更推荐直接开 vsync。
    limiter->spin_margin_ms = platform_time_is_high_resolution() ? 1.5 : 16.0;
}

f64 frame_limiter_remaining_ms(const frame_limiter_t *limiter, f64 elapsed_ms)
{
    ASSERT(limiter != NULL);

    if (!limiter->enabled) {
        return 0.0;
    }

    const f64 remaining = limiter->budget_ms - elapsed_ms;
    return (remaining > 0.0) ? remaining : 0.0;
}

void frame_limiter_wait(const frame_limiter_t *limiter, platform_time_point_t frame_start)
{
    ASSERT(limiter != NULL);

    if (!limiter->enabled) {
        return;
    }

    for (;;) {
        const f64 elapsed_ms =
            platform_time_delta_seconds(frame_start, platform_time_now()) * 1000.0;
        const f64 remaining = frame_limiter_remaining_ms(limiter, elapsed_ms);

        if (remaining <= 0.0) {
            return;
        }

        if (remaining > limiter->spin_margin_ms) {
            // 还有充裕时间，粗略睡一觉。只睡到"剩余时间减去忙等窗口"，
            // 这样即使 Sleep 超时也不会睡过头。
            platform_sleep_seconds((remaining - limiter->spin_margin_ms) / 1000.0);
        } else {
            // 进入忙等窗口。Sleep 无法精确到亚毫秒，只能自旋。
            // YieldProcessor 只是提示 CPU"这里是自旋等待"，
            // 让超线程上的另一个逻辑核拿到执行单元，不会真正让出时间片，
            // 所以等待仍然是精确的。
            YieldProcessor();
        }
    }
}
