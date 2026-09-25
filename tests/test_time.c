// =============================================================================
//  tests/test_time.c —— 帧时钟与帧率限制器
//
//  frame_clock 的关键设计是提供了 frame_clock_tick_at(clock, 时间点) 这个
//  入口。有了它，测试可以喂合成时间序列，验证 delta / FPS / 极值统计的
//  边界条件，而不用真的等 1 秒钟 —— 否则测试要么慢，要么不稳定。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// 1/60 秒对应的计数值。用整数除法会有一点截断误差，
// 所以所有断言都用容差比较而不是精确相等。
static u64 ticks_per_frame(u64 frequency, u32 fps)
{
    return frequency / (u64)fps;
}

static void frame_clock_computes_delta_and_elapsed(void)
{
    frame_clock_t clock;
    frame_clock_init(&clock);
    frame_clock_reset(&clock);

    const u64 frequency = clock.frequency;
    EXPECT_GT_U64(frequency, 0u);

    const u64 step = ticks_per_frame(frequency, 60);
    platform_time_point_t now = clock.current;

    for (u32 i = 0; i < 60; ++i) {
        now.ticks += step;
        frame_clock_tick_at(&clock, now);
    }

    EXPECT_EQ_U64(clock.frame_index, 60u);
    EXPECT_NEAR(clock.delta_seconds, 1.0 / 60.0, 1.0e-3);
    EXPECT_NEAR(clock.raw_delta_ms, 1000.0 / 60.0, 1.0);
    EXPECT_NEAR(clock.elapsed_seconds, 1.0, 1.0e-2);

    // EMA 平滑后应该收敛到 60 FPS
    EXPECT_NEAR(clock.fps, 60.0, 0.5);
    EXPECT_NEAR(clock.delta_ms, 1000.0 / 60.0, 0.5);
}

static void frame_clock_tracks_min_and_max(void)
{
    frame_clock_t clock;
    frame_clock_init(&clock);
    frame_clock_reset(&clock);

    const u64 frequency = clock.frequency;
    const u64 normal_step = ticks_per_frame(frequency, 60);
    platform_time_point_t now = clock.current;

    for (u32 i = 0; i < 10; ++i) {
        now.ticks += normal_step;
        frame_clock_tick_at(&clock, now);
    }

    // 制造一次 100ms 的尖刺（例如加载资源导致的卡顿）
    now.ticks += frequency / 10u;
    frame_clock_tick_at(&clock, now);

    EXPECT_NEAR(clock.max_delta_ms, 100.0, 1.0);
    // 最小值应该是普通帧，而不是那次尖刺
    EXPECT_NEAR(clock.min_delta_ms, 1000.0 / 60.0, 1.0);
    // 平滑值被尖刺拉高，但不会直接跳到 100
    EXPECT_TRUE(clock.delta_ms > 1000.0 / 60.0);
    EXPECT_TRUE(clock.delta_ms < 100.0);
}

static void frame_clock_handles_zero_delta(void)
{
    // 同一时间点连续 tick（例如 SwapBuffers 立即返回且时钟分辨率不够）
    // 不能让 delta 变成负数或 NaN。
    frame_clock_t clock;
    frame_clock_init(&clock);
    frame_clock_reset(&clock);

    const platform_time_point_t now = clock.current;
    for (u32 i = 0; i < 5; ++i) {
        const f64 delta = frame_clock_tick_at(&clock, now);
        EXPECT_NEAR(delta, 0.0, 0.0);
        EXPECT_NEAR(clock.delta_seconds, 0.0, 0.0);
    }

    EXPECT_TRUE(!isnan((f64)clock.fps));
    EXPECT_EQ_U64(clock.frame_index, 5u);
}

static void frame_clock_reset_zeroes_statistics(void)
{
    frame_clock_t clock;
    frame_clock_init(&clock);
    frame_clock_reset(&clock);

    platform_time_point_t now = clock.current;
    now.ticks += ticks_per_frame(clock.frequency, 60);
    frame_clock_tick_at(&clock, now);
    EXPECT_EQ_U64(clock.frame_index, 1u);

    frame_clock_reset(&clock);
    EXPECT_EQ_U64(clock.frame_index, 0u);
    EXPECT_NEAR(clock.delta_seconds, 0.0, 0.0);
    EXPECT_NEAR(clock.elapsed_seconds, 0.0, 0.0);
    EXPECT_NEAR(clock.delta_ms, 0.0, 0.0);
    EXPECT_NEAR(clock.max_delta_ms, 0.0, 0.0);
    // reset 后 min 要回到"无穷大"，否则第一帧的极值统计会带着上一轮的数据
    EXPECT_GT_U64((u64)clock.min_delta_ms, 1000000u);
}

static void frame_limiter_budget_math(void)
{
    frame_limiter_t limiter;
    frame_limiter_init(&limiter, 60.0);

    EXPECT_TRUE(limiter.enabled);
    EXPECT_NEAR(limiter.budget_ms, 1000.0 / 60.0, 1.0e-6);
    EXPECT_TRUE(limiter.spin_margin_ms > 0.0);

    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 0.0), 16.6667, 1.0e-3);
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 10.0), 6.6667, 1.0e-3);
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 16.6667), 0.0, 1.0e-3);

    // 超预算时必须返回 0 而不是负数 ——
    // 返回负数会让调用方去 Sleep 一个负值（转成 DWORD 就是天文数字）。
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 100.0), 0.0, 0.0);
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 1.0e9), 0.0, 0.0);
}

static void frame_limiter_can_be_disabled(void)
{
    frame_limiter_t limiter;
    frame_limiter_init(&limiter, 0.0);

    EXPECT_FALSE(limiter.enabled);
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 0.0), 0.0, 0.0);
    EXPECT_NEAR(frame_limiter_remaining_ms(&limiter, 5.0), 0.0, 0.0);

    // 关闭状态下 wait 应该立刻返回（不阻塞）
    const platform_time_point_t start = platform_time_now();
    frame_limiter_wait(&limiter, start);
    const f64 elapsed = platform_time_delta_seconds(start, platform_time_now());
    EXPECT_TRUE(elapsed < 0.05);
}

static void frame_limiter_high_refresh_rates(void)
{
    frame_limiter_t limiter;

    frame_limiter_init(&limiter, 120.0);
    EXPECT_NEAR(limiter.budget_ms, 8.3333, 1.0e-3);

    frame_limiter_init(&limiter, 144.0);
    EXPECT_NEAR(limiter.budget_ms, 6.9444, 1.0e-3);

    frame_limiter_init(&limiter, 30.0);
    EXPECT_NEAR(limiter.budget_ms, 33.3333, 1.0e-3);
}

// ---------------------------------------------------------------- 系统时钟
static void platform_time_frequency_is_sane(void)
{
    const u64 frequency = platform_time_frequency();
    EXPECT_GT_U64(frequency, 0u);
    // QueryPerformanceFrequency 在 Windows 上通常是 10MHz，
    // 但也可能是 CPU 主频。给一个宽但仍有意义的范围。
    EXPECT_GE_U64(frequency, 1000u);
}

static void platform_time_is_monotonic(void)
{
    const platform_time_point_t first = platform_time_now();

    // 空转一小会儿，确保计数器确实在前进
    volatile u32 sink = 0;
    for (u32 i = 0; i < 200000u; ++i) {
        sink += i;
    }
    ENGINE_UNUSED(sink);

    const platform_time_point_t second = platform_time_now();
    EXPECT_GE_U64(second.ticks, first.ticks);

    const f64 delta = platform_time_delta_seconds(first, second);
    EXPECT_TRUE(delta >= 0.0);

    // 反向的时间点必须返回 0 而不是负数（时钟异常时的保护）
    EXPECT_NEAR(platform_time_delta_seconds(second, first), 0.0, 0.0);
}

static void platform_sleep_actually_waits(void)
{
    platform_time_init(false); // 用默认精度，不干扰系统设置

    const platform_time_point_t start = platform_time_now();
    platform_sleep_ms(5);
    const f64 elapsed = platform_time_delta_seconds(start, platform_time_now());

    // Windows 的 Sleep 精度受系统计时器分辨率限制（默认 15.6ms），
    // 所以只能断言"至少等够了"，不能断言上界。
    EXPECT_GE_U64((u64)(elapsed * 1000.0), 4u);

    // 0 毫秒应该立刻返回
    const platform_time_point_t zero_start = platform_time_now();
    platform_sleep_ms(0);
    const f64 zero_elapsed = platform_time_delta_seconds(zero_start, platform_time_now());
    EXPECT_TRUE(zero_elapsed < 0.05);

    // 负数/零秒也必须是安全的空操作
    platform_sleep_seconds(0.0);
    platform_sleep_seconds(-1.0);
}

static void platform_time_init_is_idempotent(void)
{
    // 重复初始化不能出错，也不能把频率改掉
    const u64 before = platform_time_frequency();

    EXPECT_TRUE(platform_time_init(false));
    EXPECT_TRUE(platform_time_init(false));
    EXPECT_TRUE(platform_time_init(true));
    EXPECT_TRUE(platform_time_init(true));

    EXPECT_EQ_U64(platform_time_frequency(), before);
}

void test_time_register(void)
{
    test_register("time", "frame_clock_computes_delta_and_elapsed",
                  frame_clock_computes_delta_and_elapsed);
    test_register("time", "frame_clock_tracks_min_and_max", frame_clock_tracks_min_and_max);
    test_register("time", "frame_clock_handles_zero_delta", frame_clock_handles_zero_delta);
    test_register("time", "frame_clock_reset_zeroes_statistics", frame_clock_reset_zeroes_statistics);

    test_register("time", "frame_limiter_budget_math", frame_limiter_budget_math);
    test_register("time", "frame_limiter_can_be_disabled", frame_limiter_can_be_disabled);
    test_register("time", "frame_limiter_high_refresh_rates", frame_limiter_high_refresh_rates);

    test_register("time", "platform_time_frequency_is_sane", platform_time_frequency_is_sane);
    test_register("time", "platform_time_is_monotonic", platform_time_is_monotonic);
    test_register("time", "platform_sleep_actually_waits", platform_sleep_actually_waits);
    test_register("time", "platform_time_init_is_idempotent", platform_time_init_is_idempotent);
}
