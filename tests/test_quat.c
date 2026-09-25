// =============================================================================
//  tests/test_quat.c —— 四元数
//
//  重点覆盖三件事（都是历史上最容易写错的地方）：
//   1. 乘法顺序：quat_mul(a, b) 表示"先 b 后 a"
//   2. q 与 -q 表示同一旋转
//   3. 四元数 <-> 矩阵的往返一致性
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void layout_matches_spec(void)
{
    EXPECT_EQ_U64(sizeof(quat_t), 16);
    EXPECT_EQ_U64(alignof(quat_t), 16);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(quat_t, x), 0);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(quat_t, y), 4);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(quat_t, z), 8);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(quat_t, w), 12);
}

static void identity_does_nothing(void)
{
    quat_t q = quat_identity();
    EXPECT_QUAT_NEAR(q, quat_make(0.0f, 0.0f, 0.0f, 1.0f), 0.0f);
    EXPECT_NEAR(quat_length(q), 1.0f, 1e-6f);

    vec3_t v = vec3_make(1.0f, 2.0f, 3.0f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(q, v), v, 1e-6f);
    EXPECT_MAT3_NEAR(quat_to_mat3(q), mat3_identity(), 0.0f);
}

static void axis_angle_rotations_follow_handedness(void)
{
    // 右手系下的 90 度旋转，逐个轴验证方向。
    // 这三个断言一旦挂了，说明整个引擎的旋转方向反了。
    quat_t rx = quat_rotation_x(F32_HALF_PI);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(rx, vec3_unit_y()), vec3_unit_z(), 1e-6f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(rx, vec3_unit_z()), vec3_negate(vec3_unit_y()), 1e-6f);

    quat_t ry = quat_rotation_y(F32_HALF_PI);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(ry, vec3_unit_z()), vec3_unit_x(), 1e-6f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(ry, vec3_unit_x()), vec3_negate(vec3_unit_z()), 1e-6f);

    quat_t rz = quat_rotation_z(F32_HALF_PI);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(rz, vec3_unit_x()), vec3_unit_y(), 1e-6f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(rz, vec3_unit_y()), vec3_negate(vec3_unit_x()), 1e-6f);

    // 转 360 度回到原处
    quat_t full = quat_from_axis_angle(vec3_unit_z(), F32_TAU);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(full, vec3_unit_x()), vec3_unit_x(), 1e-5f);

    // 旋转不改变长度
    vec3_t v = vec3_make(1.0f, 2.0f, 3.0f);
    EXPECT_NEAR(vec3_length(quat_rotate_vec3(quat_from_axis_angle(vec3_normalize(vec3_make(1.0f, 1.0f, 1.0f)), 1.234f), v)),
                vec3_length(v), 1e-5f);
}

static void multiplication_order_is_b_then_a(void)
{
    quat_t qx = quat_rotation_x(F32_HALF_PI);
    quat_t qy = quat_rotation_y(F32_HALF_PI);

    vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);

    // quat_mul(qy, qx) 应该等价于"先 qx 再 qy"
    vec3_t x_then_y = quat_rotate_vec3(qy, quat_rotate_vec3(qx, v));
    vec3_t combined = quat_rotate_vec3(quat_mul(qy, qx), v);
    EXPECT_VEC3_NEAR(combined, x_then_y, 1e-6f);

    // 顺序反了必须得到不同结果（旋转不可交换）
    vec3_t y_then_x = quat_rotate_vec3(qx, quat_rotate_vec3(qy, v));
    EXPECT_FALSE(vec3_near(x_then_y, y_then_x, 1e-3f));

    // 与矩阵表示必须一致 —— 这是"四元数乘法约定"和"矩阵乘法约定"对齐的证据
    mat3_t m = mat3_mul(mat3_rotation_y(F32_HALF_PI), mat3_rotation_x(F32_HALF_PI));
    EXPECT_VEC3_NEAR(combined, mat3_mul_vec3(m, v), 1e-5f);

    // 具体数值：(0,0,1) 绕 X 转 90 度 -> (0,-1,0)，再绕 Y 转 90 度 -> (0,-1,0)
    EXPECT_VEC3_NEAR(combined, vec3_make(0.0f, -1.0f, 0.0f), 1e-5f);
}

static void euler_matches_explicit_product(void)
{
    const f32 x = 0.4f;
    const f32 y = -0.9f;
    const f32 z = 1.3f;

    // 文档承诺：quat_from_euler(x,y,z) == qz * qy * qx
    quat_t expected = quat_mul(quat_rotation_z(z), quat_mul(quat_rotation_y(y), quat_rotation_x(x)));
    EXPECT_QUAT_NEAR(quat_from_euler(x, y, z), expected, 1e-5f);

    // 只绕一个轴时，欧拉角与轴角必须一致
    EXPECT_QUAT_NEAR(quat_from_euler(0.5f, 0.0f, 0.0f), quat_rotation_x(0.5f), 1e-6f);
    EXPECT_QUAT_NEAR(quat_from_euler(0.0f, 0.5f, 0.0f), quat_rotation_y(0.5f), 1e-6f);
    EXPECT_QUAT_NEAR(quat_from_euler(0.0f, 0.0f, 0.5f), quat_rotation_z(0.5f), 1e-6f);
}

static void euler_roundtrip(void)
{
    const f32 cases[4][3] = {
        {0.3f, -0.7f, 1.1f},
        {-0.2f, 0.5f, -0.4f},
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.2f, -2.0f},
    };

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(cases); ++i) {
        quat_t q = quat_from_euler(cases[i][0], cases[i][1], cases[i][2]);
        vec3_t e = quat_to_euler(q);
        quat_t back = quat_from_euler(e.x, e.y, e.z);

        // 比较四元数而不是欧拉角：同一个旋转可以有多种欧拉角写法，
        // 但四元数（允许 ±）是唯一的。
        EXPECT_QUAT_NEAR(back, q, 1e-4f);
    }

    // 万向锁附近（Y 接近 ±90 度）也不能产生 NaN
    quat_t gimbal = quat_from_euler(0.5f, F32_HALF_PI, 0.5f);
    vec3_t e = quat_to_euler(gimbal);
    EXPECT_TRUE(!isnan((f64)e.x) && !isnan((f64)e.y) && !isnan((f64)e.z));
}

static void matrix_roundtrip(void)
{
    quat_t q = quat_normalize(quat_from_euler(0.7f, -1.2f, 0.35f));

    mat3_t m3 = quat_to_mat3(q);
    EXPECT_QUAT_NEAR(quat_from_mat3(m3), q, 1e-5f);

    mat4_t m4 = quat_to_mat4(q);
    EXPECT_QUAT_NEAR(quat_from_mat4(m4), q, 1e-5f);

    // 四元数旋转与矩阵旋转必须完全一致
    vec3_t v = vec3_make(0.3f, -0.8f, 1.4f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(q, v), mat3_mul_vec3(m3, v), 1e-5f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(q, v), mat4_transform_direction(m4, v), 1e-5f);

    // 覆盖 Shepperd 方法的四个分支：trace 为正、以及三个对角分量各自最大的情况。
    // 只测一个普通旋转的话，另外三个分支可能藏着 bug 却永远不会被触发。
    const f32 angles[] = {0.0f, F32_PI, F32_HALF_PI, -F32_HALF_PI, F32_PI * 0.999f, 2.5f};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(angles); ++i) {
        for (int axis = 0; axis < 3; ++axis) {
            vec3_t a = (axis == 0) ? vec3_unit_x() : ((axis == 1) ? vec3_unit_y() : vec3_unit_z());
            quat_t src = quat_from_axis_angle(a, angles[i]);
            quat_t recovered = quat_from_mat3(quat_to_mat3(src));
            EXPECT_QUAT_NEAR(recovered, src, 1e-4f);
        }
    }
}

static void conjugate_and_inverse(void)
{
    quat_t q = quat_normalize(quat_from_euler(0.2f, 0.6f, -0.9f));

    EXPECT_QUAT_NEAR(quat_mul(q, quat_inverse(q)), quat_identity(), 1e-5f);
    EXPECT_QUAT_NEAR(quat_mul(quat_inverse(q), q), quat_identity(), 1e-5f);

    // 单位四元数：逆 == 共轭
    EXPECT_QUAT_NEAR(quat_conjugate(q), quat_inverse(q), 1e-5f);

    // 逆旋转把向量转回来
    vec3_t v = vec3_make(1.0f, 2.0f, 3.0f);
    vec3_t rotated = quat_rotate_vec3(q, v);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(quat_inverse(q), rotated), v, 1e-5f);

    // quat_inverse 是数学意义上的逆 q* / |q|²，对非单位四元数结果也不是单位四元数。
    // 它满足 q * q⁻¹ = 1，这个性质对任意非零四元数都成立。
    // 注意不能拿非单位的逆去调 quat_rotate_vec3 —— 那个函数要求单位四元数，
    // 传错只会得到"既旋转又缩放"的结果，quat.h 里有断言拦这个。
    quat_t unnormalized = quat_scale(q, 3.0f);
    EXPECT_QUAT_NEAR(quat_mul(unnormalized, quat_inverse(unnormalized)), quat_identity(), 1e-5f);
    EXPECT_NEAR(quat_length(quat_inverse(unnormalized)), 1.0f / 3.0f, 1e-5f);
}

static void slerp_and_nlerp(void)
{
    quat_t a = quat_identity();
    quat_t b = quat_rotation_z(F32_HALF_PI);

    EXPECT_QUAT_NEAR(quat_slerp(a, b, 0.0f), a, 1e-5f);
    EXPECT_QUAT_NEAR(quat_slerp(a, b, 1.0f), b, 1e-5f);
    EXPECT_QUAT_NEAR(quat_nlerp(a, b, 0.0f), a, 1e-5f);
    EXPECT_QUAT_NEAR(quat_nlerp(a, b, 1.0f), b, 1e-5f);

    // 中点应是 45 度
    vec3_t mid = quat_rotate_vec3(quat_slerp(a, b, 0.5f), vec3_unit_x());
    EXPECT_NEAR(mid.x, cosf(F32_QUARTER_PI), 1e-5f);
    EXPECT_NEAR(mid.y, sinf(F32_QUARTER_PI), 1e-5f);

    // 必须走短弧：b 取负表示同一旋转，结果应完全一致
    vec3_t via_negative = quat_rotate_vec3(quat_slerp(a, quat_negate(b), 0.5f), vec3_unit_x());
    EXPECT_NEAR(via_negative.x, cosf(F32_QUARTER_PI), 1e-5f);
    EXPECT_NEAR(via_negative.y, sinf(F32_QUARTER_PI), 1e-5f);

    // slerp 的角速度恒定：1/4 处应该是 22.5 度
    vec3_t quarter = quat_rotate_vec3(quat_slerp(a, b, 0.25f), vec3_unit_x());
    EXPECT_NEAR(atan2f(quarter.y, quarter.x), F32_QUARTER_PI * 0.5f, 1e-5f);
    vec3_t three_quarter = quat_rotate_vec3(quat_slerp(a, b, 0.75f), vec3_unit_x());
    EXPECT_NEAR(atan2f(three_quarter.y, three_quarter.x), F32_QUARTER_PI * 1.5f, 1e-5f);

    // 插值结果必须保持单位长度
    for (i32 i = 0; i <= 10; ++i) {
        f32 t = (f32)i / 10.0f;
        EXPECT_NEAR(quat_length(quat_slerp(a, b, t)), 1.0f, 1e-5f);
        EXPECT_NEAR(quat_length(quat_nlerp(a, b, t)), 1.0f, 1e-5f);
    }

    // 极小夹角时 slerp 退化为 nlerp，不能产生 NaN（sin(theta) 接近 0 的除法）
    quat_t nearly = quat_rotation_z(1e-7f);
    quat_t tiny = quat_slerp(a, nearly, 0.5f);
    EXPECT_TRUE(!isnan((f64)tiny.x) && !isnan((f64)tiny.w));
    EXPECT_NEAR(quat_length(tiny), 1.0f, 1e-5f);
}

static void angle_and_axis(void)
{
    const f32 angle = math_radians(60.0f);
    quat_t q = quat_from_axis_angle(vec3_unit_z(), angle);

    EXPECT_NEAR(quat_angle(q), angle, 1e-4f);
    EXPECT_VEC3_NEAR(quat_axis(q), vec3_unit_z(), 1e-5f);

    // 恒等旋转的轴无意义，必须安全返回一个默认值而不是 NaN
    EXPECT_VEC3_NEAR(quat_axis(quat_identity()), vec3_make(0.0f, 0.0f, 1.0f), 0.0f);
    EXPECT_NEAR(quat_angle(quat_identity()), 0.0f, 1e-6f);

    // q 与 -q 表示同一旋转，角度相同
    EXPECT_NEAR(quat_angle(quat_negate(q)), quat_angle(q), 1e-6f);
    EXPECT_TRUE(quat_near(q, quat_negate(q), 1e-6f));
    EXPECT_FALSE(quat_equal(q, quat_negate(q)));
}

static void from_to_rotation(void)
{
    // X 轴转到 Y 轴
    quat_t q = quat_from_to(vec3_unit_x(), vec3_unit_y());
    EXPECT_VEC3_NEAR(quat_rotate_vec3(q, vec3_unit_x()), vec3_unit_y(), 1e-5f);

    // 相同方向 -> 单位四元数
    EXPECT_QUAT_NEAR(quat_from_to(vec3_unit_x(), vec3_unit_x()), quat_identity(), 1e-5f);

    // 完全反向 -> 180 度
    quat_t opposite = quat_from_to(vec3_unit_x(), vec3_negate(vec3_unit_x()));
    EXPECT_VEC3_NEAR(quat_rotate_vec3(opposite, vec3_unit_x()), vec3_negate(vec3_unit_x()), 1e-4f);
    EXPECT_NEAR(quat_angle(opposite), F32_PI, 1e-4f);

    // 任意一对方向都要能正确对齐
    vec3_t from = vec3_normalize(vec3_make(1.0f, 2.0f, -3.0f));
    vec3_t to = vec3_normalize(vec3_make(-2.0f, 0.5f, 1.0f));
    quat_t any = quat_from_to(from, to);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(any, from), to, 1e-4f);
    EXPECT_NEAR(quat_length(any), 1.0f, 1e-5f);
}

static void look_rotation_aligns_negative_z(void)
{
    // 相机约定：局部 -Z 指向 forward，局部 +Y 尽量朝上
    vec3_t forward = vec3_normalize(vec3_make(1.0f, 0.0f, 0.0f));
    quat_t look = quat_look_rotation(forward, vec3_unit_y());

    EXPECT_VEC3_NEAR(quat_rotate_vec3(look, vec3_make(0.0f, 0.0f, -1.0f)), forward, 1e-5f);
    EXPECT_VEC3_NEAR(quat_rotate_vec3(look, vec3_unit_y()), vec3_unit_y(), 1e-5f);

    // 朝 -Z 看时应该是恒等旋转
    quat_t default_look = quat_look_rotation(vec3_make(0.0f, 0.0f, -1.0f), vec3_unit_y());
    EXPECT_QUAT_NEAR(default_look, quat_identity(), 1e-5f);

    // forward 与 up 平行时不能产生 NaN
    quat_t degenerate = quat_look_rotation(vec3_make(0.0f, 1.0f, 0.0f), vec3_unit_y());
    EXPECT_TRUE(!isnan((f64)degenerate.x) && !isnan((f64)degenerate.w));
}

static void trs_matrix_matches_step_by_step(void)
{
    vec3_t position = vec3_make(3.0f, -1.0f, 2.0f);
    quat_t rotation = quat_normalize(quat_from_euler(0.4f, 0.8f, -0.3f));
    vec3_t scale = vec3_make(2.0f, 0.5f, 1.5f);

    mat4_t combined = mat4_from_trs(position, rotation, scale);
    mat4_t step_by_step = mat4_mul(mat4_translation(position),
                                   mat4_mul(quat_to_mat4(rotation), mat4_scale(scale)));

    EXPECT_MAT4_NEAR(combined, step_by_step, 1e-5f);
}

void test_quat_register(void)
{
    test_register("quat", "layout_matches_spec", layout_matches_spec);
    test_register("quat", "identity_does_nothing", identity_does_nothing);
    test_register("quat", "axis_angle_rotations_follow_handedness",
                  axis_angle_rotations_follow_handedness);
    test_register("quat", "multiplication_order_is_b_then_a", multiplication_order_is_b_then_a);
    test_register("quat", "euler_matches_explicit_product", euler_matches_explicit_product);
    test_register("quat", "euler_roundtrip", euler_roundtrip);
    test_register("quat", "matrix_roundtrip", matrix_roundtrip);
    test_register("quat", "conjugate_and_inverse", conjugate_and_inverse);
    test_register("quat", "slerp_and_nlerp", slerp_and_nlerp);
    test_register("quat", "angle_and_axis", angle_and_axis);
    test_register("quat", "from_to_rotation", from_to_rotation);
    test_register("quat", "look_rotation_aligns_negative_z", look_rotation_aligns_negative_z);
    test_register("quat", "trs_matrix_matches_step_by_step", trs_matrix_matches_step_by_step);
}
