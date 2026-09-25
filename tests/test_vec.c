// =============================================================================
//  tests/test_vec.c —— 向量
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// ---------------------------------------------------------------- 内存布局
static void layout_matches_spec(void)
{
    EXPECT_EQ_U64(sizeof(vec2_t), 8);
    EXPECT_EQ_U64(alignof(vec2_t), 4);
    EXPECT_EQ_U64(sizeof(vec3_t), 16); // 含 4 字节对齐填充
    EXPECT_EQ_U64(alignof(vec3_t), 16);
    EXPECT_EQ_U64(sizeof(vec4_t), 16);
    EXPECT_EQ_U64(alignof(vec4_t), 16);

    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec3_t, x), 0);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec3_t, y), 4);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec3_t, z), 8);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(vec3_t, pad), 12);
}

static void array_of_vec3_keeps_alignment(void)
{
    // 这条是本引擎"vec3 16 字节对齐"承诺的实测验证：
    // 如果哪天有人把 _Alignas 写错了位置（见 vec.h 里的说明），这里会立刻挂。
    static vec3_t storage[8];
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(storage); ++i) {
        EXPECT_EQ_U64((usize)(uintptr_t)&storage[i] % 16u, 0u);
    }

    static vec4_t storage4[8];
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(storage4); ++i) {
        EXPECT_EQ_U64((usize)(uintptr_t)&storage4[i] % 16u, 0u);
    }
}

// ---------------------------------------------------------------- vec2
static void vec2_basics(void)
{
    vec2_t a = vec2_make(3.0f, 4.0f);

    EXPECT_NEAR(vec2_length(a), 5.0f, 1e-6f);
    EXPECT_NEAR(vec2_length_sq(a), 25.0f, 1e-6f);
    EXPECT_NEAR(vec2_dot(vec2_make(1.0f, 0.0f), vec2_make(0.0f, 1.0f)), 0.0f, 1e-6f);
    EXPECT_NEAR(vec2_cross(vec2_make(1.0f, 0.0f), vec2_make(0.0f, 1.0f)), 1.0f, 1e-6f);
    EXPECT_NEAR(vec2_cross(vec2_make(0.0f, 1.0f), vec2_make(1.0f, 0.0f)), -1.0f, 1e-6f);

    EXPECT_VEC2_NEAR(vec2_add(vec2_make(1.0f, 2.0f), vec2_make(3.0f, 4.0f)), vec2_make(4.0f, 6.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_sub(vec2_make(3.0f, 4.0f), vec2_make(1.0f, 2.0f)), vec2_make(2.0f, 2.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_mul(vec2_make(2.0f, 3.0f), vec2_make(4.0f, 5.0f)), vec2_make(8.0f, 15.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_scale(a, 2.0f), vec2_make(6.0f, 8.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_negate(a), vec2_make(-3.0f, -4.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_abs(vec2_make(-3.0f, 4.0f)), vec2_make(3.0f, 4.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_lerp(vec2_zero(), vec2_make(10.0f, 20.0f), 0.5f), vec2_make(5.0f, 10.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_min(vec2_make(1.0f, 5.0f), vec2_make(3.0f, 2.0f)), vec2_make(1.0f, 2.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_max(vec2_make(1.0f, 5.0f), vec2_make(3.0f, 2.0f)), vec2_make(3.0f, 5.0f), 1e-6f);
    EXPECT_NEAR(vec2_distance(vec2_make(1.0f, 0.0f), vec2_make(4.0f, 4.0f)), 5.0f, 1e-6f);
}

static void vec2_normalize_and_rotate(void)
{
    EXPECT_VEC2_NEAR(vec2_normalize(vec2_make(3.0f, 4.0f)), vec2_make(0.6f, 0.8f), 1e-6f);
    // 零向量必须安全返回零，而不是 NaN
    EXPECT_VEC2_NEAR(vec2_normalize(vec2_zero()), vec2_zero(), 0.0f);
    EXPECT_VEC2_NEAR(vec2_normalize_or(vec2_zero(), vec2_make(0.0f, 1.0f)), vec2_make(0.0f, 1.0f), 0.0f);

    EXPECT_VEC2_NEAR(vec2_perp(vec2_make(1.0f, 0.0f)), vec2_make(0.0f, 1.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_rotate(vec2_make(1.0f, 0.0f), F32_HALF_PI), vec2_make(0.0f, 1.0f), 1e-6f);
    EXPECT_VEC2_NEAR(vec2_rotate(vec2_make(1.0f, 0.0f), F32_PI), vec2_make(-1.0f, 0.0f), 1e-6f);

    EXPECT_NEAR(vec2_angle(vec2_make(0.0f, 1.0f)), F32_HALF_PI, 1e-6f);
    EXPECT_NEAR(vec2_angle(vec2_make(1.0f, 0.0f)), 0.0f, 1e-6f);
    EXPECT_VEC2_NEAR(vec2_from_angle(F32_HALF_PI), vec2_make(0.0f, 1.0f), 1e-6f);

    EXPECT_TRUE(vec2_equal(vec2_make(1.0f, 2.0f), vec2_make(1.0f, 2.0f)));
    EXPECT_TRUE(vec2_near(vec2_make(1.0f, 2.0f), vec2_make(1.0f + 1e-7f, 2.0f), 1e-5f));
}

// ---------------------------------------------------------------- vec3
static void vec3_arithmetic(void)
{
    vec3_t a = vec3_make(1.0f, 2.0f, 3.0f);
    vec3_t b = vec3_make(4.0f, 5.0f, 6.0f);

    EXPECT_VEC3_NEAR(vec3_add(a, b), vec3_make(5.0f, 7.0f, 9.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_sub(b, a), vec3_make(3.0f, 3.0f, 3.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_mul(a, b), vec3_make(4.0f, 10.0f, 18.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_div(b, a), vec3_make(4.0f, 2.5f, 2.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_scale(a, 2.0f), vec3_make(2.0f, 4.0f, 6.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_mul_add(vec3_one(), vec3_splat(2.0f), 3.0f), vec3_splat(7.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_negate(a), vec3_make(-1.0f, -2.0f, -3.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_abs(vec3_make(-1.0f, 2.0f, -3.0f)), vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);

    EXPECT_NEAR(vec3_dot(a, b), 32.0f, 1e-6f);
    EXPECT_VEC3_NEAR(vec3_min(a, b), a, 1e-6f);
    EXPECT_VEC3_NEAR(vec3_max(a, b), b, 1e-6f);
    EXPECT_VEC3_NEAR(vec3_lerp(vec3_zero(), vec3_splat(10.0f), 0.5f), vec3_splat(5.0f), 1e-6f);
}

static void vec3_cross_product(void)
{
    // 右手系：X × Y = Z，Y × Z = X，Z × X = Y
    EXPECT_VEC3_NEAR(vec3_cross(vec3_unit_x(), vec3_unit_y()), vec3_unit_z(), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_cross(vec3_unit_y(), vec3_unit_z()), vec3_unit_x(), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_cross(vec3_unit_z(), vec3_unit_x()), vec3_unit_y(), 1e-6f);

    // 反交换律
    EXPECT_VEC3_NEAR(vec3_cross(vec3_unit_y(), vec3_unit_x()), vec3_negate(vec3_unit_z()), 1e-6f);

    // 平行向量的叉积为零
    EXPECT_VEC3_NEAR(vec3_cross(vec3_unit_x(), vec3_scale(vec3_unit_x(), 5.0f)), vec3_zero(), 1e-6f);

    // 叉积结果垂直于两个输入
    vec3_t a = vec3_normalize(vec3_make(1.0f, 2.0f, 3.0f));
    vec3_t b = vec3_normalize(vec3_make(-2.0f, 1.0f, 0.5f));
    vec3_t c = vec3_cross(a, b);
    EXPECT_NEAR(vec3_dot(c, a), 0.0f, 1e-5f);
    EXPECT_NEAR(vec3_dot(c, b), 0.0f, 1e-5f);
    // |a × b| = |a||b|sin(theta)
    EXPECT_NEAR(vec3_length(c), sinf(vec3_angle_between(a, b)), 1e-5f);
}

static void vec3_length_and_normalize(void)
{
    vec3_t v = vec3_make(0.0f, 3.0f, 4.0f);
    EXPECT_NEAR(vec3_length(v), 5.0f, 1e-6f);
    EXPECT_NEAR(vec3_length_sq(v), 25.0f, 1e-6f);
    EXPECT_NEAR(vec3_distance(vec3_make(1.0f, 0.0f, 0.0f), vec3_make(4.0f, 0.0f, 4.0f)), 5.0f, 1e-6f);

    vec3_t n = vec3_normalize(v);
    EXPECT_VEC3_NEAR(n, vec3_make(0.0f, 0.6f, 0.8f), 1e-6f);
    EXPECT_NEAR(vec3_length(n), 1.0f, 1e-6f);
    EXPECT_TRUE(vec3_is_normalized(n, 1e-5f));

    EXPECT_VEC3_NEAR(vec3_normalize(vec3_zero()), vec3_zero(), 0.0f);
    EXPECT_VEC3_NEAR(vec3_normalize_or(vec3_zero(), vec3_unit_y()), vec3_unit_y(), 0.0f);
}

static void vec3_geometry(void)
{
    // 反射：以 Y 轴为法线反射 (1,-1,0) 得到 (1,1,0)
    EXPECT_VEC3_NEAR(vec3_reflect(vec3_make(1.0f, -1.0f, 0.0f), vec3_unit_y()),
                     vec3_make(1.0f, 1.0f, 0.0f), 1e-6f);
    // 反射两次回到原向量
    vec3_t v = vec3_normalize(vec3_make(1.0f, -2.0f, 0.5f));
    vec3_t n = vec3_unit_y();
    EXPECT_VEC3_NEAR(vec3_reflect(vec3_reflect(v, n), n), v, 1e-5f);
    // 反射不改变长度
    EXPECT_NEAR(vec3_length(vec3_reflect(v, n)), vec3_length(v), 1e-5f);

    // 投影与剔除
    EXPECT_VEC3_NEAR(vec3_project(vec3_make(1.0f, 1.0f, 0.0f), vec3_unit_y()), vec3_make(0.0f, 1.0f, 0.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_reject(vec3_make(1.0f, 1.0f, 0.0f), vec3_unit_y()), vec3_make(1.0f, 0.0f, 0.0f), 1e-6f);
    // 投影 + 剔除 = 原向量
    vec3_t p = vec3_normalize(vec3_make(1.0f, 5.0f, -2.0f));
    EXPECT_VEC3_NEAR(vec3_add(vec3_project(p, n), vec3_reject(p, n)), p, 1e-5f);

    // 夹角
    EXPECT_NEAR(vec3_angle_between(vec3_unit_x(), vec3_unit_y()), F32_HALF_PI, 1e-5f);
    EXPECT_NEAR(vec3_angle_between(vec3_unit_x(), vec3_unit_x()), 0.0f, 1e-5f);
    EXPECT_NEAR(vec3_angle_between(vec3_unit_x(), vec3_negate(vec3_unit_x())), F32_PI, 1e-4f);
    // 小夹角时仍然精确（acos 版本在这里会丢精度）
    vec3_t almost = vec3_normalize(vec3_make(1.0f, 1e-4f, 0.0f));
    EXPECT_NEAR(vec3_angle_between(vec3_unit_x(), almost), 1e-4f, 1e-6f);

    // 绕轴旋转
    EXPECT_VEC3_NEAR(vec3_rotate_axis(vec3_unit_x(), vec3_unit_z(), F32_HALF_PI), vec3_unit_y(), 1e-5f);
    EXPECT_VEC3_NEAR(vec3_rotate_axis(vec3_unit_y(), vec3_unit_z(), F32_HALF_PI), vec3_negate(vec3_unit_x()), 1e-5f);
    // 旋转不改变长度
    EXPECT_NEAR(vec3_length(vec3_rotate_axis(vec3_make(1.0f, 2.0f, 3.0f), vec3_normalize(vec3_make(1.0f, 1.0f, 0.0f)), 1.1f)),
                vec3_length(vec3_make(1.0f, 2.0f, 3.0f)), 1e-5f);

    // 折射：垂直入射不偏折
    EXPECT_VEC3_NEAR(vec3_refract(vec3_make(0.0f, 0.0f, -1.0f), vec3_make(0.0f, 0.0f, 1.0f), 0.66f),
                     vec3_make(0.0f, 0.0f, -1.0f), 1e-5f);
    // 折射结果必须保持单位长度（这是折射公式的正确性判据）
    vec3_t incident = vec3_normalize(vec3_make(1.0f, -1.0f, -1.0f));
    vec3_t normal = vec3_unit_y();
    vec3_t refracted = vec3_refract(incident, normal, 0.66f);
    EXPECT_NEAR(vec3_length(refracted), 1.0f, 1e-5f);
}

// ---------------------------------------------------------------- vec4
static void vec4_basics(void)
{
    vec4_t a = vec4_make(1.0f, 2.0f, 3.0f, 4.0f);

    EXPECT_NEAR(vec4_dot(a, a), 30.0f, 1e-5f);
    EXPECT_NEAR(vec4_length(a), sqrtf(30.0f), 1e-5f);
    EXPECT_VEC4_NEAR(vec4_add(a, a), vec4_make(2.0f, 4.0f, 6.0f, 8.0f), 1e-6f);
    EXPECT_VEC4_NEAR(vec4_sub(a, a), vec4_zero(), 1e-6f);
    EXPECT_VEC4_NEAR(vec4_scale(a, 0.5f), vec4_make(0.5f, 1.0f, 1.5f, 2.0f), 1e-6f);
    EXPECT_VEC4_NEAR(vec4_negate(a), vec4_make(-1.0f, -2.0f, -3.0f, -4.0f), 1e-6f);
    EXPECT_VEC4_NEAR(vec4_lerp(vec4_zero(), vec4_splat(4.0f), 0.5f), vec4_splat(2.0f), 1e-6f);
    EXPECT_VEC4_NEAR(vec4_normalize(vec4_make(0.0f, 0.0f, 0.0f, 2.0f)), vec4_make(0.0f, 0.0f, 0.0f, 1.0f), 1e-6f);
}

static void vec4_point_direction_semantics(void)
{
    // 点的 w = 1，方向的 w = 0。这个区别决定了平移是否生效，
    // 是"法线被错误平移"这类 bug 的根源，所以单独测。
    EXPECT_VEC4_NEAR(vec4_from_point(vec3_make(1.0f, 2.0f, 3.0f)), vec4_make(1.0f, 2.0f, 3.0f, 1.0f), 0.0f);
    EXPECT_VEC4_NEAR(vec4_from_direction(vec3_make(1.0f, 2.0f, 3.0f)), vec4_make(1.0f, 2.0f, 3.0f, 0.0f), 0.0f);
    EXPECT_VEC4_NEAR(vec4_from_vec3(vec3_make(1.0f, 2.0f, 3.0f), 0.5f), vec4_make(1.0f, 2.0f, 3.0f, 0.5f), 0.0f);
    EXPECT_VEC3_NEAR(vec4_xyz(vec4_make(1.0f, 2.0f, 3.0f, 4.0f)), vec3_make(1.0f, 2.0f, 3.0f), 0.0f);
}

static void vec4_perspective_divide_is_safe(void)
{
    EXPECT_VEC3_NEAR(vec4_perspective_divide(vec4_make(2.0f, 4.0f, 6.0f, 2.0f)),
                     vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec4_perspective_divide(vec4_make(1.0f, 1.0f, 1.0f, 1.0f)),
                     vec3_make(1.0f, 1.0f, 1.0f), 1e-6f);
    // w = 0 必须安全，不能产生 inf / NaN
    EXPECT_VEC3_NEAR(vec4_perspective_divide(vec4_make(1.0f, 1.0f, 1.0f, 0.0f)), vec3_zero(), 0.0f);
}

void test_vec_register(void)
{
    test_register("vec", "layout_matches_spec", layout_matches_spec);
    test_register("vec", "array_of_vec3_keeps_alignment", array_of_vec3_keeps_alignment);
    test_register("vec", "vec2_basics", vec2_basics);
    test_register("vec", "vec2_normalize_and_rotate", vec2_normalize_and_rotate);
    test_register("vec", "vec3_arithmetic", vec3_arithmetic);
    test_register("vec", "vec3_cross_product", vec3_cross_product);
    test_register("vec", "vec3_length_and_normalize", vec3_length_and_normalize);
    test_register("vec", "vec3_geometry", vec3_geometry);
    test_register("vec", "vec4_basics", vec4_basics);
    test_register("vec", "vec4_point_direction_semantics", vec4_point_direction_semantics);
    test_register("vec", "vec4_perspective_divide_is_safe", vec4_perspective_divide_is_safe);
}
