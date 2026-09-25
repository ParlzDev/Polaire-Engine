// =============================================================================
//  engine/core/math.h —— 标量数学工具
//
//  这个文件只放"标量"层面的东西。向量/矩阵/四元数在 vec.h / mat.h / quat.h。
//
//  为什么全部用 float 而不是 double：
//   * GPU 只吃 float（除少数科学计算管线），double 在 CPU 算完还要截断回 float，
//     等于白算一遍。
//   * 64 位下 double 运算不慢，但 double 让 vec3/mat4 体积翻倍，缓存命中率下降，
//     对"10 万精灵 60FPS"这种目标不利。
//   * 代价是精度：float 只有约 7 位有效十进制数字。世界坐标超过约 1e5 单位后
//     抖动会肉眼可见。这是所有大世界引擎的共同问题，M6 之后用"相机相对坐标"
//     或"分区原点"解决，而不是靠换成 double。
//
//  为什么数学库是 header-only（static inline）而非 .h/.c 分离：
//   * 向量运算每个只占几条指令，函数调用开销占比极高。只有放在头文件里、
//     让编译器看到调用点上下文，才能真正内联并向量化。
//   * 分离到 .c 会强制走 ABI，在 Win64 上 16 字节结构体还会走隐藏指针返回，
//     单次 vec3_add 的代价可能比实际运算高一个数量级。
//   * 这是有意偏离"接口与实现分离"的约定，理由与代价记在 docs/M0-设计决策.md。
// =============================================================================
#ifndef ENGINE_CORE_MATH_H
#define ENGINE_CORE_MATH_H

#include "engine/core/assert.h"
#include "engine/core/types.h"

#include <math.h>

// ---------------------------------------------------------------- 常量
#define F32_PI 3.14159265358979323846f
#define F32_TAU 6.28318530717958647692f
#define F32_HALF_PI 1.57079632679489661923f
#define F32_QUARTER_PI 0.78539816339744830961f
#define F32_INV_PI 0.31830988618379067154f

#define F32_DEG2RAD 0.01745329251994329577f
#define F32_RAD2DEG 57.29577951308232087680f

#define F32_EPSILON 1.19209290e-07f
#define F32_MAX 3.402823466e+38f
#define F32_MIN 1.175494351e-38f

// 用于"两个量是否足够接近"的默认容差。
// 为什么不用 F32_EPSILON：它约 1e-7，是"float 在 1.0 附近的最小间隔"，
// 直接拿它比较两个经过若干次运算的量几乎必然失败。1e-5 才是工程上好用的阈值。
#define F32_TOLERANCE 1.0e-5f

// ---------------------------------------------------------------- 角度
static inline f32 math_radians(f32 degrees)
{
    return degrees * F32_DEG2RAD;
}

static inline f32 math_degrees(f32 radians)
{
    return radians * F32_RAD2DEG;
}

// ---------------------------------------------------------------- 插值与钳制
static inline f32 math_clamp(f32 v, f32 lo, f32 hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static inline f32 math_saturate(f32 v)
{
    return math_clamp(v, 0.0f, 1.0f);
}

static inline f32 math_lerp(f32 a, f32 b, f32 t)
{
    // 写成 a + (b-a)*t 而不是 a*(1-t) + b*t：
    // 前者在 t 精确等于 1 时能保证结果精确等于 b，后者有浮点误差。
    return a + (b - a) * t;
}

static inline f32 math_lerp_clamped(f32 a, f32 b, f32 t)
{
    return math_lerp(a, b, math_saturate(t));
}

// 经典 smoothstep：3t^2 - 2t^3，在 0 和 1 处一阶导为 0，用于缓入缓出。
static inline f32 math_smoothstep(f32 t)
{
    t = math_saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// 更平滑的 6t^5 - 15t^4 + 10t^3，二阶导也为 0。
static inline f32 math_smootherstep(f32 t)
{
    t = math_saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ---------------------------------------------------------------- 符号与比较
static inline f32 math_abs(f32 v)
{
    return fabsf(v);
}

static inline f32 math_sign(f32 v)
{
    return (v > 0.0f) ? 1.0f : ((v < 0.0f) ? -1.0f : 0.0f);
}

static inline f32 math_min(f32 a, f32 b)
{
    return (a < b) ? a : b;
}

static inline f32 math_max(f32 a, f32 b)
{
    return (a > b) ? a : b;
}

static inline f32 math_step(f32 edge, f32 v)
{
    return (v < edge) ? 0.0f : 1.0f;
}

// |a - b| <= tolerance
static inline bool math_near(f32 a, f32 b, f32 tolerance)
{
    return fabsf(a - b) <= tolerance;
}

static inline bool math_near_zero(f32 v, f32 tolerance)
{
    return fabsf(v) <= tolerance;
}

// ---------------------------------------------------------------- 基础函数
// 全部显式使用 f 后缀版本。这不是洁癖：写成 sqrt(x) 会让 float 被提升为
// double 运算再截回 float，慢一倍以上，而且 -Wdouble-promotion 会报出来。
static inline f32 math_sqrt(f32 v)
{
    return sqrtf(v);
}

static inline f32 math_rsqrt(f32 v)
{
    ASSERT(v > 0.0f);
    return 1.0f / sqrtf(v);
}

static inline f32 math_sin(f32 v)
{
    return sinf(v);
}

static inline f32 math_cos(f32 v)
{
    return cosf(v);
}

static inline f32 math_tan(f32 v)
{
    return tanf(v);
}

static inline f32 math_asin(f32 v)
{
    return asinf(math_clamp(v, -1.0f, 1.0f));
}

static inline f32 math_acos(f32 v)
{
    return acosf(math_clamp(v, -1.0f, 1.0f));
}

static inline f32 math_atan(f32 v)
{
    return atanf(v);
}

static inline f32 math_atan2(f32 y, f32 x)
{
    return atan2f(y, x);
}

static inline f32 math_pow(f32 base, f32 exponent)
{
    return powf(base, exponent);
}

static inline f32 math_exp(f32 v)
{
    return expf(v);
}

static inline f32 math_log(f32 v)
{
    ASSERT(v > 0.0f);
    return logf(v);
}

static inline f32 math_floor(f32 v)
{
    return floorf(v);
}

static inline f32 math_ceil(f32 v)
{
    return ceilf(v);
}

static inline f32 math_round(f32 v)
{
    return roundf(v);
}

static inline f32 math_trunc(f32 v)
{
    return truncf(v);
}

static inline f32 math_fmod(f32 a, f32 b)
{
    return fmodf(a, b);
}

// ---------------------------------------------------------------- 区间工具
// 把 v 环绕到 [lo, hi) 区间。用于角度归一化、UV 循环。
static inline f32 math_wrap(f32 v, f32 lo, f32 hi)
{
    ASSERT(hi > lo);
    f32 range = hi - lo;
    f32 t     = fmodf(v - lo, range);
    if (t < 0.0f) {
        t += range;
    }
    return t + lo;
}

// 把弧度归一化到 (-PI, PI]。
//
// 为什么不用 math_wrap(v, -PI, PI) + 精确比较：
// 后者依赖"结果恰好等于 -PI"这个条件，而 -3*F32_PI 这类输入在 float 下
// 无法精确表示，fmodf 会返回一个与 -PI 相差 1e-7 的值，精确比较就失效了，
// 结果会随机地停在 -PI 侧或 +PI 侧。这里改成"先折到 [0, TAU) 再折到 (-PI, PI]"，
// 全程只有一次比较，边界行为确定。
static inline f32 math_wrap_angle(f32 radians)
{
    f32 r = fmodf(radians, F32_TAU);
    if (r < 0.0f) {
        r += F32_TAU;
    }
    // r ∈ [0, TAU)
    if (r > F32_PI) {
        r -= F32_TAU;
    }
    // r ∈ (-PI, PI]
    return r;
}

// 求两角之间的最短差值，结果落在 (-PI, PI]。
static inline f32 math_angle_delta(f32 from, f32 to)
{
    return math_wrap_angle(to - from);
}

static inline f32 math_move_towards(f32 current, f32 target, f32 max_delta)
{
    f32 diff = target - current;
    if (fabsf(diff) <= max_delta) {
        return target;
    }
    return current + math_sign(diff) * max_delta;
}

// ---------------------------------------------------------------- 整数工具
// 返回 >= v 的最小 2 的幂。v == 0 时返回 1。
u32 math_next_pow2_u32(u32 v);

// ---------------------------------------------------------------- 浮点环境
// 打开/关闭 FTZ（flush-to-zero）与 DAZ（denormals-are-zero）。
//
// 为什么值得单独一个开关：非规格化数的运算在 x86 上要走微码，慢几十倍。
// 游戏中出现的非规格化数几乎都是"数值已经衰减到没意义"的残留（比如衰减到
// 极小的音量、速度），直接清零不会影响观感，却能避免随机的性能尖刺。
//
// 为什么不放在 math_init 里自动开：它会改变浮点运算结果。单元测试里如果
// 拿极小值做近似比较，会因为结果被清零而失败。所以做成显式开关，由引擎
// 初始化时按构建类型决定是否打开。
void math_set_flush_denormals(bool enabled);

// 查询当前状态，主要供测试使用。
bool math_get_flush_denormals(void);

#endif // ENGINE_CORE_MATH_H
