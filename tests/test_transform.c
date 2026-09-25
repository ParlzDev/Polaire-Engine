// =============================================================================
//  tests/test_transform.c —— 位置/旋转/缩放
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void layout_matches_spec(void)
{
    EXPECT_EQ_U64(sizeof(transform_t), 48);
    EXPECT_EQ_U64(alignof(transform_t), 16);
}

static void identity_transform(void)
{
    transform_t t = transform_identity();

    EXPECT_VEC3_NEAR(t.position, vec3_zero(), 0.0f);
    EXPECT_QUAT_NEAR(t.rotation, quat_identity(), 0.0f);
    EXPECT_VEC3_NEAR(t.scale, vec3_one(), 0.0f);

    EXPECT_MAT4_NEAR(transform_to_mat4(t), mat4_identity(), 0.0f);
    EXPECT_VEC3_NEAR(transform_point(t, vec3_make(1.0f, 2.0f, 3.0f)), vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);
    EXPECT_VEC3_NEAR(transform_direction(t, vec3_make(1.0f, 2.0f, 3.0f)), vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);
}

static void trs_applies_scale_then_rotation_then_translation(void)
{
    vec3_t position = vec3_make(3.0f, -1.0f, 2.0f);
    quat_t rotation = quat_normalize(quat_from_euler(0.4f, 0.8f, -0.3f));
    vec3_t scale = vec3_make(2.0f, 0.5f, 1.5f);

    transform_t t = transform_make(position, rotation, scale);
    mat4_t m = transform_to_mat4(t);

    // 与显式 T * R * S 逐项一致
    mat4_t expected = mat4_mul(mat4_translation(position),
                               mat4_mul(quat_to_mat4(rotation), mat4_scale(scale)));
    EXPECT_MAT4_NEAR(m, expected, 1e-5f);

    // 点变换：先缩放，再旋转，最后平移
    vec3_t p = vec3_make(1.0f, 2.0f, 3.0f);
    vec3_t manual = vec3_add(position, quat_rotate_vec3(rotation, vec3_mul(p, scale)));
    EXPECT_VEC3_NEAR(transform_point(t, p), manual, 1e-5f);
    EXPECT_VEC3_NEAR(transform_point(t, p), mat4_transform_point(m, p), 1e-5f);

    // 方向变换：不受平移影响
    EXPECT_VEC3_NEAR(transform_direction(t, p), quat_rotate_vec3(rotation, vec3_mul(p, scale)), 1e-5f);
    EXPECT_VEC3_NEAR(transform_direction(t, p), mat4_transform_direction(m, p), 1e-5f);
}

static void translation_does_not_affect_directions(void)
{
    transform_t a = transform_identity();
    transform_t b = transform_identity();
    b.position = vec3_make(100.0f, -50.0f, 25.0f);

    vec3_t direction = vec3_make(0.0f, 1.0f, 0.0f);
    EXPECT_VEC3_NEAR(transform_direction(a, direction), transform_direction(b, direction), 1e-6f);

    // 但点会受影响
    EXPECT_FALSE(vec3_near(transform_point(a, vec3_zero()), transform_point(b, vec3_zero()), 1e-3f));
}

static void scale_zero_collapses(void)
{
    // 零缩放是合法的（用来做"消失"动画），不能崩也不能产生 NaN
    transform_t t = transform_identity();
    t.scale = vec3_zero();

    vec3_t p = transform_point(t, vec3_make(5.0f, 6.0f, 7.0f));
    EXPECT_VEC3_NEAR(p, vec3_zero(), 0.0f);
    EXPECT_TRUE(!isnan((f64)p.x));
}

static void equality_and_near(void)
{
    transform_t a = transform_make(vec3_make(1.0f, 2.0f, 3.0f), quat_rotation_y(0.5f), vec3_one());
    transform_t b = a;
    transform_t c = a;
    c.position.x += 1e-7f;

    EXPECT_TRUE(transform_equal(a, b));
    EXPECT_TRUE(transform_near(a, c, 1e-5f));

    c.position.x += 1.0f;
    EXPECT_FALSE(transform_near(a, c, 1e-5f));
    EXPECT_FALSE(transform_equal(a, c));

    // q 与 -q 是同一旋转，near 必须认
    transform_t d = a;
    d.rotation = quat_negate(a.rotation);
    EXPECT_TRUE(transform_near(a, d, 1e-5f));
}

static void full_pipeline_world_to_ndc(void)
{
    // 一个站在世界原点、朝向 -Z 的物体，被一台在 (0,0,5) 的相机看到，
    // 最终应落在屏幕中心且深度合法。这是 M0 能覆盖到的最完整链路。
    transform_t model = transform_identity();
    mat4_t model_matrix = transform_to_mat4(model);

    mat4_t view = mat4_look_at(vec3_make(0.0f, 0.0f, 5.0f), vec3_zero(), vec3_unit_y());
    mat4_t proj = mat4_perspective(math_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    vec4_t world = mat4_mul_vec4(model_matrix, vec4_make(0.0f, 0.0f, 0.0f, 1.0f));
    vec4_t clip = mat4_mul_vec4(proj, mat4_mul_vec4(view, world));
    vec3_t ndc = vec4_perspective_divide(clip);

    EXPECT_NEAR(ndc.x, 0.0f, 1e-5f);
    EXPECT_NEAR(ndc.y, 0.0f, 1e-5f);
    EXPECT_TRUE(ndc.z > 0.0f && ndc.z < 1.0f);
}

void test_transform_register(void)
{
    test_register("transform", "layout_matches_spec", layout_matches_spec);
    test_register("transform", "identity_transform", identity_transform);
    test_register("transform", "trs_applies_scale_then_rotation_then_translation",
                  trs_applies_scale_then_rotation_then_translation);
    test_register("transform", "translation_does_not_affect_directions",
                  translation_does_not_affect_directions);
    test_register("transform", "scale_zero_collapses", scale_zero_collapses);
    test_register("transform", "equality_and_near", equality_and_near);
    test_register("transform", "full_pipeline_world_to_ndc", full_pipeline_world_to_ndc);
}
