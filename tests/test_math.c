// =============================================================================
//  tests/test_math.c —— 标量数学
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void radians_degrees(void)
{
    EXPECT_NEAR(math_radians(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_radians(180.0f), F32_PI, 1e-5f);
    EXPECT_NEAR(math_radians(90.0f), F32_HALF_PI, 1e-6f);
    EXPECT_NEAR(math_degrees(F32_PI), 180.0f, 1e-3f);
    EXPECT_NEAR(math_degrees(F32_HALF_PI), 90.0f, 1e-3f);
    // 往返转换
    EXPECT_NEAR(math_degrees(math_radians(37.5f)), 37.5f, 1e-3f);
}

static void clamp_and_lerp(void)
{
    EXPECT_NEAR(math_clamp(5.0f, 0.0f, 1.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_clamp(-5.0f, 0.0f, 1.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_clamp(0.5f, 0.0f, 1.0f), 0.5f, 0.0f);
    EXPECT_NEAR(math_saturate(2.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_saturate(-2.0f), 0.0f, 0.0f);

    EXPECT_NEAR(math_lerp(0.0f, 10.0f, 0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_lerp(0.0f, 10.0f, 1.0f), 10.0f, 0.0f);
    EXPECT_NEAR(math_lerp(0.0f, 10.0f, 0.25f), 2.5f, 1e-6f);
    EXPECT_NEAR(math_lerp(2.0f, 4.0f, 0.5f), 3.0f, 1e-6f);
    EXPECT_NEAR(math_lerp_clamped(0.0f, 10.0f, 2.0f), 10.0f, 0.0f);
}

static void smoothstep_endpoints(void)
{
    EXPECT_NEAR(math_smoothstep(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_smoothstep(1.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_smoothstep(0.5f), 0.5f, 1e-6f);
    EXPECT_NEAR(math_smoothstep(-1.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_smoothstep(2.0f), 1.0f, 0.0f);

    EXPECT_NEAR(math_smootherstep(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_smootherstep(1.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_smootherstep(0.5f), 0.5f, 1e-6f);

    // 单调性：中间值必须落在两端之间
    f32 prev = 0.0f;
    for (i32 i = 1; i <= 10; ++i) {
        f32 v = math_smoothstep((f32)i / 10.0f);
        EXPECT_TRUE(v >= prev);
        prev = v;
    }
}

static void sign_and_abs(void)
{
    EXPECT_NEAR(math_abs(-3.5f), 3.5f, 0.0f);
    EXPECT_NEAR(math_sign(-2.0f), -1.0f, 0.0f);
    EXPECT_NEAR(math_sign(2.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_sign(0.0f), 0.0f, 0.0f);
    EXPECT_NEAR(math_min(1.0f, -1.0f), -1.0f, 0.0f);
    EXPECT_NEAR(math_max(1.0f, -1.0f), 1.0f, 0.0f);
    EXPECT_NEAR(math_step(0.5f, 0.4f), 0.0f, 0.0f);
    EXPECT_NEAR(math_step(0.5f, 0.6f), 1.0f, 0.0f);
}

static void near_comparisons(void)
{
    EXPECT_TRUE(math_near(1.0f, 1.0f + 1e-7f, 1e-5f));
    EXPECT_FALSE(math_near(1.0f, 1.1f, 1e-5f));
    EXPECT_TRUE(math_near_zero(1e-8f, 1e-5f));
    EXPECT_FALSE(math_near_zero(1e-3f, 1e-5f));
}

static void wrap_and_angle_helpers(void)
{
    EXPECT_NEAR(math_wrap(370.0f, 0.0f, 360.0f), 10.0f, 1e-3f);
    EXPECT_NEAR(math_wrap(-10.0f, 0.0f, 360.0f), 350.0f, 1e-3f);
    EXPECT_NEAR(math_wrap(180.0f, 0.0f, 360.0f), 180.0f, 1e-3f);

    // wrap_angle 结果必须落在 (-PI, PI]
    const f32 inputs[] = {0.0f, 0.5f, F32_PI, -F32_PI, 3.0f * F32_PI, -3.0f * F32_PI, 10.0f, -10.0f};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(inputs); ++i) {
        f32 w = math_wrap_angle(inputs[i]);
        EXPECT_TRUE(w > -F32_PI - 1e-4f);
        EXPECT_TRUE(w <= F32_PI + 1e-4f);
    }

    // ±3π 恰好落在 ±π 这个边界上。float 无法精确表示 3π，所以这里验证
    // "绝对值等于 π"（+π 与 -π 表示同一个角度），而不是要求某个具体的代表值。
    EXPECT_NEAR(math_abs(math_wrap_angle(3.0f * F32_PI)), F32_PI, 1e-4f);
    EXPECT_NEAR(math_abs(math_wrap_angle(-3.0f * F32_PI)), F32_PI, 1e-4f);

    EXPECT_NEAR(math_wrap_angle(0.5f), 0.5f, 1e-6f);
    EXPECT_NEAR(math_wrap_angle(-0.5f), -0.5f, 1e-6f);
    EXPECT_NEAR(math_wrap_angle(F32_PI), F32_PI, 1e-6f);
    // 区间取 (-PI, PI]，所以 -PI 要归到 +PI
    EXPECT_NEAR(math_wrap_angle(-F32_PI), F32_PI, 1e-6f);
    EXPECT_NEAR(math_wrap_angle(F32_TAU + 0.5f), 0.5f, 1e-5f);
    EXPECT_NEAR(math_wrap_angle(F32_TAU - 0.5f), -0.5f, 1e-5f);

    // 最短角差：从 350 度到 10 度应走 +20 度，而不是 -340 度
    f32 delta = math_angle_delta(math_radians(350.0f), math_radians(10.0f));
    EXPECT_NEAR(delta, math_radians(20.0f), 1e-3f);

    delta = math_angle_delta(math_radians(10.0f), math_radians(350.0f));
    EXPECT_NEAR(delta, math_radians(-20.0f), 1e-3f);

    EXPECT_NEAR(math_move_towards(0.0f, 10.0f, 3.0f), 3.0f, 1e-6f);
    EXPECT_NEAR(math_move_towards(0.0f, 10.0f, 100.0f), 10.0f, 1e-6f);
    EXPECT_NEAR(math_move_towards(0.0f, -10.0f, 3.0f), -3.0f, 1e-6f);
    EXPECT_NEAR(math_move_towards(9.0f, 10.0f, 3.0f), 10.0f, 1e-6f);
}

static void basic_functions(void)
{
    EXPECT_NEAR(math_sqrt(16.0f), 4.0f, 1e-6f);
    EXPECT_NEAR(math_rsqrt(4.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(math_sin(0.0f), 0.0f, 1e-7f);
    EXPECT_NEAR(math_cos(0.0f), 1.0f, 1e-7f);
    EXPECT_NEAR(math_sin(F32_HALF_PI), 1.0f, 1e-6f);
    EXPECT_NEAR(math_atan2(1.0f, 0.0f), F32_HALF_PI, 1e-6f);
    EXPECT_NEAR(math_pow(2.0f, 10.0f), 1024.0f, 1e-2f);
    EXPECT_NEAR(math_floor(2.9f), 2.0f, 0.0f);
    EXPECT_NEAR(math_ceil(2.1f), 3.0f, 0.0f);
    EXPECT_NEAR(math_round(2.5f), 3.0f, 0.0f);
    EXPECT_NEAR(math_fmod(7.0f, 3.0f), 1.0f, 1e-6f);

    // acos/asin 对越界输入必须钳制而不是产生 NaN
    EXPECT_TRUE(!isnan((f64)math_acos(1.5f)));
    EXPECT_TRUE(!isnan((f64)math_asin(-1.5f)));
    EXPECT_NEAR(math_acos(1.5f), 0.0f, 1e-6f);
}

static void next_pow2(void)
{
    EXPECT_EQ_U64(math_next_pow2_u32(0), 1u);
    EXPECT_EQ_U64(math_next_pow2_u32(1), 1u);
    EXPECT_EQ_U64(math_next_pow2_u32(2), 2u);
    EXPECT_EQ_U64(math_next_pow2_u32(3), 4u);
    EXPECT_EQ_U64(math_next_pow2_u32(5), 8u);
    EXPECT_EQ_U64(math_next_pow2_u32(16), 16u);
    EXPECT_EQ_U64(math_next_pow2_u32(17), 32u);
    EXPECT_EQ_U64(math_next_pow2_u32(1000), 1024u);
    EXPECT_EQ_U64(math_next_pow2_u32(0x40000000u), 0x40000000u);
    EXPECT_EQ_U64(math_next_pow2_u32(0x80000000u), 0x80000000u);
    // 超出 u32 能表示的范围，返回 0 让调用方处理
    EXPECT_EQ_U64(math_next_pow2_u32(0x80000001u), 0u);
    EXPECT_EQ_U64(math_next_pow2_u32(0xFFFFFFFFu), 0u);

    // 结果必须总是 2 的幂
    for (u32 i = 1; i < 100000u; i = i * 3u + 1u) {
        u32 r = math_next_pow2_u32(i);
        EXPECT_TRUE(ENGINE_IS_POW2(r));
        EXPECT_GE_U64(r, i);
    }
}

static void flush_denormals_toggle(void)
{
    const bool original = math_get_flush_denormals();

    math_set_flush_denormals(true);
    EXPECT_TRUE(math_get_flush_denormals());

    math_set_flush_denormals(false);
    EXPECT_FALSE(math_get_flush_denormals());

    // 恢复现场，避免影响后续用例
    math_set_flush_denormals(original);
}

void test_math_register(void)
{
    test_register("math", "radians_degrees", radians_degrees);
    test_register("math", "clamp_and_lerp", clamp_and_lerp);
    test_register("math", "smoothstep_endpoints", smoothstep_endpoints);
    test_register("math", "sign_and_abs", sign_and_abs);
    test_register("math", "near_comparisons", near_comparisons);
    test_register("math", "wrap_and_angle_helpers", wrap_and_angle_helpers);
    test_register("math", "basic_functions", basic_functions);
    test_register("math", "next_pow2", next_pow2);
    test_register("math", "flush_denormals_toggle", flush_denormals_toggle);
}
