// =============================================================================
//  engine/core/vec.h —— 向量
//
//  ---- 关于内存布局（重要，先读完再写代码） ----
//
//    vec2_t  8 字节,  4 字节对齐
//    vec3_t 16 字节, 16 字节对齐   <-- x,y,z 之后有一个 pad 字段
//    vec4_t 16 字节, 16 字节对齐
//
//  vec3_t 为什么是 16 字节而不是 12：
//   * 16 字节对齐后可以整块装进 SSE 寄存器（movaps 要求 16 字节对齐），
//     未来做 SIMD 优化时不需要改数据布局，这是改布局最贵的部分。
//   * 代价是 vec3 数组浪费 25% 内存，且 pad 字段容易让人困惑。
//   * 顶点缓冲不要用 vec3_t。GPU 侧请用紧凑的 12 字节布局（M2 会引入
//     专门的 vertex 类型），CPU 侧数学运算才用 vec3_t。
//   * pad 字段命名为 pad 而不是 w，是为了防止有人"顺手"把它当齐次坐标用。
//     需要齐次坐标请显式用 vec4_t。
//
//  ---- 关于 _Alignas 的坑（踩过一次，记下来） ----
//  下面这种写法是错的：
//      typedef struct { _Alignas(16) float x, y, z, pad; } vec3_t;   // sizeof == 64 !
//  因为 _Alignas 作用于该声明里的**每一个** declarator，等于给 x/y/z/pad
//  各自加了 16 字节对齐，结构体直接膨胀到 64 字节。
//  正确写法是只让第一个成员带 _Alignas，其余成员另起一条声明。
//  所有布局都用 _Static_assert 钉死，改错会立刻编译失败。
// =============================================================================
#ifndef ENGINE_CORE_VEC_H
#define ENGINE_CORE_VEC_H

#include "engine/core/math.h"
#include "engine/core/types.h"

// =============================================================================
//  vec2_t
// =============================================================================
typedef struct vec2_t {
    f32 x;
    f32 y;
} vec2_t;

_Static_assert(sizeof(vec2_t) == 8, "vec2_t 必须是 8 字节");
_Static_assert(alignof(vec2_t) == 4, "vec2_t 必须是 4 字节对齐");

static inline vec2_t vec2_make(f32 x, f32 y)
{
    vec2_t v;
    v.x = x;
    v.y = y;
    return v;
}

static inline vec2_t vec2_splat(f32 s)
{
    return vec2_make(s, s);
}

static inline vec2_t vec2_zero(void)
{
    return vec2_make(0.0f, 0.0f);
}

static inline vec2_t vec2_one(void)
{
    return vec2_make(1.0f, 1.0f);
}

static inline vec2_t vec2_add(vec2_t a, vec2_t b)
{
    return vec2_make(a.x + b.x, a.y + b.y);
}

static inline vec2_t vec2_sub(vec2_t a, vec2_t b)
{
    return vec2_make(a.x - b.x, a.y - b.y);
}

static inline vec2_t vec2_mul(vec2_t a, vec2_t b)
{
    return vec2_make(a.x * b.x, a.y * b.y);
}

static inline vec2_t vec2_div(vec2_t a, vec2_t b)
{
    return vec2_make(a.x / b.x, a.y / b.y);
}

static inline vec2_t vec2_scale(vec2_t v, f32 s)
{
    return vec2_make(v.x * s, v.y * s);
}

// a + b * s，一次乘加。物理积分、粒子更新里最常用的形式。
static inline vec2_t vec2_mul_add(vec2_t a, vec2_t b, f32 s)
{
    return vec2_make(a.x + b.x * s, a.y + b.y * s);
}

static inline vec2_t vec2_negate(vec2_t v)
{
    return vec2_make(-v.x, -v.y);
}

static inline vec2_t vec2_abs(vec2_t v)
{
    return vec2_make(math_abs(v.x), math_abs(v.y));
}

static inline f32 vec2_dot(vec2_t a, vec2_t b)
{
    return a.x * b.x + a.y * b.y;
}

// 2D 叉积退化为标量（z 分量）。符号表示 b 在 a 的左侧还是右侧。
static inline f32 vec2_cross(vec2_t a, vec2_t b)
{
    return a.x * b.y - a.y * b.x;
}

static inline f32 vec2_length_sq(vec2_t v)
{
    return v.x * v.x + v.y * v.y;
}

static inline f32 vec2_length(vec2_t v)
{
    return sqrtf(vec2_length_sq(v));
}

static inline f32 vec2_distance_sq(vec2_t a, vec2_t b)
{
    return vec2_length_sq(vec2_sub(a, b));
}

static inline f32 vec2_distance(vec2_t a, vec2_t b)
{
    return sqrtf(vec2_distance_sq(a, b));
}

// 零长度输入返回零向量。不抛错也不断言：碰撞解算、法线计算中出现退化输入
// 是正常的，让调用方每次判断反而更容易漏。
static inline vec2_t vec2_normalize(vec2_t v)
{
    f32 len_sq = vec2_length_sq(v);
    if (len_sq <= 0.0f) {
        return vec2_zero();
    }
    return vec2_scale(v, 1.0f / sqrtf(len_sq));
}

// 退化时返回 fallback，用于"必须得到单位向量"的场合。
static inline vec2_t vec2_normalize_or(vec2_t v, vec2_t fallback)
{
    f32 len_sq = vec2_length_sq(v);
    if (len_sq <= 0.0f) {
        return fallback;
    }
    return vec2_scale(v, 1.0f / sqrtf(len_sq));
}

static inline vec2_t vec2_lerp(vec2_t a, vec2_t b, f32 t)
{
    return vec2_make(math_lerp(a.x, b.x, t), math_lerp(a.y, b.y, t));
}

static inline vec2_t vec2_min(vec2_t a, vec2_t b)
{
    return vec2_make(math_min(a.x, b.x), math_min(a.y, b.y));
}

static inline vec2_t vec2_max(vec2_t a, vec2_t b)
{
    return vec2_make(math_max(a.x, b.x), math_max(a.y, b.y));
}

// 逆时针旋转 90 度。
static inline vec2_t vec2_perp(vec2_t v)
{
    return vec2_make(-v.y, v.x);
}

static inline vec2_t vec2_rotate(vec2_t v, f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    return vec2_make(v.x * c - v.y * s, v.x * s + v.y * c);
}

static inline f32 vec2_angle(vec2_t v)
{
    return atan2f(v.y, v.x);
}

static inline vec2_t vec2_from_angle(f32 radians)
{
    return vec2_make(cosf(radians), sinf(radians));
}

static inline bool vec2_equal(vec2_t a, vec2_t b)
{
    return a.x == b.x && a.y == b.y;
}

static inline bool vec2_near(vec2_t a, vec2_t b, f32 tolerance)
{
    return math_near(a.x, b.x, tolerance) && math_near(a.y, b.y, tolerance);
}

// =============================================================================
//  vec3_t
// =============================================================================
typedef struct vec3_t {
    _Alignas(16) f32 x;
    f32 y;
    f32 z;
    f32 pad; // 仅用于对齐填充，不要读写
} vec3_t;

_Static_assert(sizeof(vec3_t) == 16, "vec3_t 必须是 16 字节（含对齐填充）");
_Static_assert(alignof(vec3_t) == 16, "vec3_t 必须是 16 字节对齐");
_Static_assert(ENGINE_OFFSETOF(vec3_t, y) == 4, "vec3_t.y 必须紧跟在 x 之后");
_Static_assert(ENGINE_OFFSETOF(vec3_t, z) == 8, "vec3_t.z 必须紧跟在 y 之后");

static inline vec3_t vec3_make(f32 x, f32 y, f32 z)
{
    vec3_t v;
    v.x   = x;
    v.y   = y;
    v.z   = z;
    v.pad = 0.0f;
    return v;
}

static inline vec3_t vec3_splat(f32 s)
{
    return vec3_make(s, s, s);
}

static inline vec3_t vec3_zero(void)
{
    return vec3_make(0.0f, 0.0f, 0.0f);
}

static inline vec3_t vec3_one(void)
{
    return vec3_make(1.0f, 1.0f, 1.0f);
}

static inline vec3_t vec3_unit_x(void)
{
    return vec3_make(1.0f, 0.0f, 0.0f);
}

static inline vec3_t vec3_unit_y(void)
{
    return vec3_make(0.0f, 1.0f, 0.0f);
}

static inline vec3_t vec3_unit_z(void)
{
    return vec3_make(0.0f, 0.0f, 1.0f);
}

static inline vec3_t vec3_add(vec3_t a, vec3_t b)
{
    return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline vec3_t vec3_sub(vec3_t a, vec3_t b)
{
    return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline vec3_t vec3_mul(vec3_t a, vec3_t b)
{
    return vec3_make(a.x * b.x, a.y * b.y, a.z * b.z);
}

static inline vec3_t vec3_div(vec3_t a, vec3_t b)
{
    return vec3_make(a.x / b.x, a.y / b.y, a.z / b.z);
}

static inline vec3_t vec3_scale(vec3_t v, f32 s)
{
    return vec3_make(v.x * s, v.y * s, v.z * s);
}

static inline vec3_t vec3_mul_add(vec3_t a, vec3_t b, f32 s)
{
    return vec3_make(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s);
}

static inline vec3_t vec3_negate(vec3_t v)
{
    return vec3_make(-v.x, -v.y, -v.z);
}

static inline vec3_t vec3_abs(vec3_t v)
{
    return vec3_make(math_abs(v.x), math_abs(v.y), math_abs(v.z));
}

static inline f32 vec3_dot(vec3_t a, vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3_t vec3_cross(vec3_t a, vec3_t b)
{
    return vec3_make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static inline f32 vec3_length_sq(vec3_t v)
{
    return vec3_dot(v, v);
}

static inline f32 vec3_length(vec3_t v)
{
    return sqrtf(vec3_length_sq(v));
}

static inline f32 vec3_distance_sq(vec3_t a, vec3_t b)
{
    return vec3_length_sq(vec3_sub(a, b));
}

static inline f32 vec3_distance(vec3_t a, vec3_t b)
{
    return sqrtf(vec3_distance_sq(a, b));
}

static inline vec3_t vec3_normalize(vec3_t v)
{
    f32 len_sq = vec3_length_sq(v);
    if (len_sq <= 0.0f) {
        return vec3_zero();
    }
    return vec3_scale(v, 1.0f / sqrtf(len_sq));
}

static inline vec3_t vec3_normalize_or(vec3_t v, vec3_t fallback)
{
    f32 len_sq = vec3_length_sq(v);
    if (len_sq <= 0.0f) {
        return fallback;
    }
    return vec3_scale(v, 1.0f / sqrtf(len_sq));
}

static inline vec3_t vec3_lerp(vec3_t a, vec3_t b, f32 t)
{
    return vec3_make(math_lerp(a.x, b.x, t), math_lerp(a.y, b.y, t), math_lerp(a.z, b.z, t));
}

static inline vec3_t vec3_min(vec3_t a, vec3_t b)
{
    return vec3_make(math_min(a.x, b.x), math_min(a.y, b.y), math_min(a.z, b.z));
}

static inline vec3_t vec3_max(vec3_t a, vec3_t b)
{
    return vec3_make(math_max(a.x, b.x), math_max(a.y, b.y), math_max(a.z, b.z));
}

// 绕单位法线 n 反射。n 必须已归一化，否则结果长度会错。
static inline vec3_t vec3_reflect(vec3_t v, vec3_t n)
{
    return vec3_sub(v, vec3_scale(n, 2.0f * vec3_dot(v, n)));
}

// 折射。eta = 入射侧折射率 / 折射侧折射率（例如 1.0/1.52）。
// 发生全反射时返回零向量 —— 调用方需自行处理（反射光才是物理上正确的结果）。
static inline vec3_t vec3_refract(vec3_t v, vec3_t n, f32 eta)
{
    f32  cos_i = -vec3_dot(n, v);
    f32  sin_t2 = eta * eta * (1.0f - cos_i * cos_i);
    if (sin_t2 > 1.0f) {
        return vec3_zero(); // 全反射
    }
    f32 cos_t = sqrtf(1.0f - sin_t2);
    return vec3_add(vec3_scale(v, eta), vec3_scale(n, eta * cos_i - cos_t));
}

// 两向量夹角，结果在 [0, PI]。用 atan2(|a x b|, a.b) 而不是 acos(a.b)：
// 后者在夹角接近 0 或 PI 时会因浮点精度丢失产生明显误差。
static inline f32 vec3_angle_between(vec3_t a, vec3_t b)
{
    return atan2f(vec3_length(vec3_cross(a, b)), vec3_dot(a, b));
}

// v 在 n 方向上的投影向量（n 需归一化）。
static inline vec3_t vec3_project(vec3_t v, vec3_t n)
{
    return vec3_scale(n, vec3_dot(v, n));
}

// v 垂直于 n 的分量（n 需归一化）。
static inline vec3_t vec3_reject(vec3_t v, vec3_t n)
{
    return vec3_sub(v, vec3_project(v, n));
}

// 绕任意轴旋转（Rodrigues 公式）。axis 需归一化。
static inline vec3_t vec3_rotate_axis(vec3_t v, vec3_t axis, f32 radians)
{
    f32    c = cosf(radians);
    f32    s = sinf(radians);
    vec3_t term1 = vec3_scale(v, c);
    vec3_t term2 = vec3_scale(vec3_cross(axis, v), s);
    vec3_t term3 = vec3_scale(axis, vec3_dot(axis, v) * (1.0f - c));
    return vec3_add(vec3_add(term1, term2), term3);
}

static inline bool vec3_equal(vec3_t a, vec3_t b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static inline bool vec3_near(vec3_t a, vec3_t b, f32 tolerance)
{
    return math_near(a.x, b.x, tolerance) && math_near(a.y, b.y, tolerance) &&
           math_near(a.z, b.z, tolerance);
}

static inline bool vec3_is_normalized(vec3_t v, f32 tolerance)
{
    return math_near(vec3_length_sq(v), 1.0f, tolerance);
}

// =============================================================================
//  vec4_t
// =============================================================================
typedef struct vec4_t {
    _Alignas(16) f32 x;
    f32 y;
    f32 z;
    f32 w;
} vec4_t;

_Static_assert(sizeof(vec4_t) == 16, "vec4_t 必须是 16 字节");
_Static_assert(alignof(vec4_t) == 16, "vec4_t 必须是 16 字节对齐");

static inline vec4_t vec4_make(f32 x, f32 y, f32 z, f32 w)
{
    vec4_t v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

static inline vec4_t vec4_splat(f32 s)
{
    return vec4_make(s, s, s, s);
}

static inline vec4_t vec4_zero(void)
{
    return vec4_make(0.0f, 0.0f, 0.0f, 0.0f);
}

static inline vec4_t vec4_one(void)
{
    return vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
}

// 点：w = 1，平移会生效。
static inline vec4_t vec4_from_point(vec3_t p)
{
    return vec4_make(p.x, p.y, p.z, 1.0f);
}

// 方向：w = 0，平移不生效。这是"变换一个法线/朝向"的正确做法。
static inline vec4_t vec4_from_direction(vec3_t d)
{
    return vec4_make(d.x, d.y, d.z, 0.0f);
}

static inline vec4_t vec4_from_vec3(vec3_t v, f32 w)
{
    return vec4_make(v.x, v.y, v.z, w);
}

static inline vec3_t vec4_xyz(vec4_t v)
{
    return vec3_make(v.x, v.y, v.z);
}

static inline vec4_t vec4_add(vec4_t a, vec4_t b)
{
    return vec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

static inline vec4_t vec4_sub(vec4_t a, vec4_t b)
{
    return vec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
}

static inline vec4_t vec4_mul(vec4_t a, vec4_t b)
{
    return vec4_make(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
}

static inline vec4_t vec4_scale(vec4_t v, f32 s)
{
    return vec4_make(v.x * s, v.y * s, v.z * s, v.w * s);
}

static inline vec4_t vec4_negate(vec4_t v)
{
    return vec4_make(-v.x, -v.y, -v.z, -v.w);
}

static inline f32 vec4_dot(vec4_t a, vec4_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

static inline f32 vec4_length_sq(vec4_t v)
{
    return vec4_dot(v, v);
}

static inline f32 vec4_length(vec4_t v)
{
    return sqrtf(vec4_length_sq(v));
}

static inline vec4_t vec4_normalize(vec4_t v)
{
    f32 len_sq = vec4_length_sq(v);
    if (len_sq <= 0.0f) {
        return vec4_zero();
    }
    return vec4_scale(v, 1.0f / sqrtf(len_sq));
}

static inline vec4_t vec4_lerp(vec4_t a, vec4_t b, f32 t)
{
    return vec4_make(math_lerp(a.x, b.x, t), math_lerp(a.y, b.y, t), math_lerp(a.z, b.z, t),
                     math_lerp(a.w, b.w, t));
}

// 透视除法：把裁剪空间坐标变成 NDC。w 接近 0 时返回零向量。
static inline vec3_t vec4_perspective_divide(vec4_t v)
{
    if (math_near_zero(v.w, F32_EPSILON)) {
        return vec3_zero();
    }
    f32 inv_w = 1.0f / v.w;
    return vec3_make(v.x * inv_w, v.y * inv_w, v.z * inv_w);
}

static inline bool vec4_equal(vec4_t a, vec4_t b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static inline bool vec4_near(vec4_t a, vec4_t b, f32 tolerance)
{
    return math_near(a.x, b.x, tolerance) && math_near(a.y, b.y, tolerance) &&
           math_near(a.z, b.z, tolerance) && math_near(a.w, b.w, tolerance);
}

#endif // ENGINE_CORE_VEC_H
