// =============================================================================
//  tests/test_frustum.c —— 视锥提取与剔除
//
//  这一组测试的重点是**用独立算法交叉验证**，而不是"抄一遍实现再比一次"。
//
//  平面提取的正确性用"裁剪空间判据"来对照：一个点是否在视锥内，
//  定义上就是它的裁剪坐标满足 -w <= x <= w、-w <= y <= w、0 <= z <= w。
//  所以测试里直接算 M*v 再判这组不等式，和 frustum_contains_point 比 ——
//  两个实现走的是完全不同的路径，同时错的可能性很低。
//
//  这比"手算几个点"强得多：手算的点只覆盖几个方向，
//  而列主序 + 右手系 + 深度 [0,1] 三者叠加出的错误往往只在特定方向暴露。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include "engine/render/frustum.h"

// 测试用相机：原点、看向 -Z、60° 竖直视角、16:9、近 0.1 远 100
static mat4_t test_view_projection(void)
{
    const mat4_t view = mat4_look_at(vec3_zero(), vec3_make(0.0f, 0.0f, -1.0f),
                                     vec3_make(0.0f, 1.0f, 0.0f));
    const mat4_t proj = mat4_perspective(math_radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    return mat4_mul(proj, view);
}

// 独立实现：用裁剪空间不等式判断点是否在视锥内。
// 这是"定义"，不是"另一种近似"，所以可以作为判据的黄金标准。
static bool clip_space_contains(mat4_t view_projection, vec3_t point)
{
    const vec4_t clip = mat4_mul_vec4(view_projection, vec4_make(point.x, point.y, point.z, 1.0f));
    const f32    w = clip.w;

    // w 必须为正：w <= 0 表示点在相机平面之后（或正好在相机上），
    // 此时裁剪不等式会给出无意义的结果（两边同时翻号）。
    if (w <= 0.0f) {
        return false;
    }
    return (clip.x >= -w) && (clip.x <= w) && (clip.y >= -w) && (clip.y <= w) && (clip.z >= 0.0f) &&
           (clip.z <= w);
}

// ---------------------------------------------------------------- 平面方向

static void frustum_planes_point_inward(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    // 相机在原点、看向 -Z：近平面与远平面的法线应该沿 ±Z。
    // 近平面法线朝 -Z（内侧是 z 更负的方向）。
    EXPECT_TRUE(f.planes[FRUSTUM_PLANE_NEAR].normal.z < 0.0f);
    EXPECT_TRUE(f.planes[FRUSTUM_PLANE_FAR].normal.z > 0.0f);

    // 近平面到原点的距离应该等于 znear（法线已归一化，所以 |distance| 就是距离）
    EXPECT_NEAR(f.planes[FRUSTUM_PLANE_NEAR].distance, -0.1f, 1.0e-4f);

    // 所有平面法线都必须是单位长度（没归一化的话球体剔除的半径判据会失效）
    for (u32 i = 0; i < FRUSTUM_PLANE_COUNT; ++i) {
        EXPECT_NEAR(vec3_length(f.planes[i].normal), 1.0f, 1.0e-4f);
    }
}

// ---------------------------------------------------------------- 点的包含

static void frustum_point_inside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -10.0f)));
    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -0.5f)));
    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -99.0f)));
    // 靠近左边缘（10 米处，半高 = 10*tan(30°) ≈ 5.77，半宽 ≈ 10.26）
    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(9.0f, 5.0f, -10.0f)));
}

static void frustum_point_behind_camera_is_outside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, 5.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, 0.001f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, 0.0f)));
}

// 这条是**深度 [0,1] 约定的守门测试**。
//
// 如果近平面误写成 `row3 + row2`（那是深度 [-1,1] 的写法），
// 近平面会被推到相机背后大约 znear/2 的位置。结果就是：
// 相机贴脸时，一小段本该被剔除的空间被当成可见 —— 不报错，只是多画。
//
// 0.05 米处在 znear = 0.1 之内，必须被判为不可见。
static void frustum_point_inside_near_plane_is_outside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -0.05f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -0.09f)));
    // 刚刚越过近平面则应该可见
    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -0.11f)));
}

static void frustum_point_beyond_far_is_outside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -99.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -101.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, -500.0f)));
}

static void frustum_point_outside_sides(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(100.0f, 0.0f, -10.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(-100.0f, 0.0f, -10.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, 100.0f, -10.0f)));
    EXPECT_FALSE(frustum_contains_point(&f, vec3_make(0.0f, -100.0f, -10.0f)));
}

// 差分测试：和"裁剪空间不等式"这个定义逐个点比对。
static void frustum_matches_clip_space_definition(void)
{
    const mat4_t    vp = test_view_projection();
    const frustum_t f = frustum_from_view_projection(vp);

    u32 mismatches = 0;
    for (i32 xi = -12; xi <= 12; ++xi) {
        for (i32 yi = -12; yi <= 12; ++yi) {
            for (i32 zi = -12; zi <= 4; ++zi) {
                // 步长 1.5 米，覆盖相机前后与两侧
                const vec3_t p = vec3_make((f32)xi * 1.5f, (f32)yi * 1.5f, (f32)zi * 1.5f);

                const bool from_planes = frustum_contains_point(&f, p);
                const bool from_clip = clip_space_contains(vp, p);
                if (from_planes != from_clip) {
                    mismatches++;
                }
            }
        }
    }
    EXPECT_EQ_U64(mismatches, 0);
}

// 相机倾斜时也要对得上 —— 轴对齐的相机很容易掩盖矩阵乘序的错误。
static void frustum_matches_clip_space_when_camera_is_oblique(void)
{
    const vec3_t eye = vec3_make(3.0f, 2.0f, 4.0f);
    const vec3_t target = vec3_make(-1.0f, 0.5f, -2.0f);
    const mat4_t view = mat4_look_at(eye, target, vec3_make(0.0f, 1.0f, 0.0f));
    const mat4_t proj = mat4_perspective(math_radians(45.0f), 1.5f, 0.25f, 60.0f);
    const mat4_t vp = mat4_mul(proj, view);

    const frustum_t f = frustum_from_view_projection(vp);

    u32 mismatches = 0;
    for (i32 xi = -6; xi <= 6; ++xi) {
        for (i32 yi = -6; yi <= 6; ++yi) {
            for (i32 zi = -6; zi <= 6; ++zi) {
                const vec3_t p = vec3_make((f32)xi * 1.7f, (f32)yi * 1.7f, (f32)zi * 1.7f);
                if (frustum_contains_point(&f, p) != clip_space_contains(vp, p)) {
                    mismatches++;
                }
            }
        }
    }
    EXPECT_EQ_U64(mismatches, 0);
}

// ---------------------------------------------------------------- AABB

static void frustum_aabb_inside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(-1.0f, -1.0f, -11.0f),
                                        vec3_make(1.0f, 1.0f, -9.0f)));
    // 整个盒子包住相机 —— 相机在盒子内部，必须判为可见
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(-50.0f, -50.0f, -50.0f),
                                        vec3_make(50.0f, 50.0f, 50.0f)));
}

static void frustum_aabb_outside(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    // 相机背后
    EXPECT_FALSE(frustum_intersects_aabb(&f, vec3_make(-1.0f, -1.0f, 1.0f),
                                         vec3_make(1.0f, 1.0f, 3.0f)));
    // 远到超出远平面
    EXPECT_FALSE(frustum_intersects_aabb(&f, vec3_make(-1.0f, -1.0f, -200.0f),
                                         vec3_make(1.0f, 1.0f, -150.0f)));
    // 侧面之外
    EXPECT_FALSE(frustum_intersects_aabb(&f, vec3_make(200.0f, -1.0f, -11.0f),
                                         vec3_make(220.0f, 1.0f, -9.0f)));
}

// 保守性：与视锥边界相交的盒子必须判为可见。
// 宁可多画一个，也不能把可见物体判没了 —— 后者是"物体在视野边缘突然消失"。
static void frustum_aabb_straddling_is_conservative(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    // 横跨近平面
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(-1.0f, -1.0f, -0.5f),
                                        vec3_make(1.0f, 1.0f, 0.5f)));
    // 横跨远平面
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(-1.0f, -1.0f, -120.0f),
                                        vec3_make(1.0f, 1.0f, -80.0f)));
    // 横跨右平面（10 米处半宽约 10.26）
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(9.0f, -1.0f, -11.0f),
                                        vec3_make(12.0f, 1.0f, -9.0f)));
}

// AABB 判据必须比"中心点判据"更宽松：盒子中心在外但盒子本身跨进来时，
// 点判据会说不含，盒判据必须说相交。用这条把两者区分开，
// 避免有人图省事把 intersects_aabb 实现成"测中心点"。
static void frustum_aabb_is_looser_than_point_test(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    const vec3_t center = vec3_make(12.0f, 0.0f, -10.0f); // 在右侧平面之外
    EXPECT_FALSE(frustum_contains_point(&f, center));

    // 但一个足够大的盒子仍然应该与视锥相交
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(center.x - 5.0f, center.y - 5.0f,
                                                      center.z - 5.0f),
                                        vec3_make(center.x + 5.0f, center.y + 5.0f,
                                                  center.z + 5.0f)));
}

// ---------------------------------------------------------------- 球

static void frustum_sphere_tests(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    EXPECT_TRUE(frustum_intersects_sphere(&f, vec3_make(0.0f, 0.0f, -10.0f), 1.0f));
    // 中心在外，但半径够大伸进来
    EXPECT_TRUE(frustum_intersects_sphere(&f, vec3_make(14.0f, 0.0f, -10.0f), 5.0f));
    // 中心在外且够远
    EXPECT_FALSE(frustum_intersects_sphere(&f, vec3_make(40.0f, 0.0f, -10.0f), 5.0f));
    // 相机背后
    EXPECT_FALSE(frustum_intersects_sphere(&f, vec3_make(0.0f, 0.0f, 20.0f), 1.0f));
}

// 球的判据必须比 AABB 保守性一致：包住同一块区域的球和盒，
// 不应出现"球说可见、盒说不可见"这种自相矛盾（球是盒的内接/外接近似）。
static void frustum_sphere_containing_aabb_agrees(void)
{
    const frustum_t f = frustum_from_view_projection(test_view_projection());

    const vec3_t lo = vec3_make(-1.0f, -1.0f, -11.0f);
    const vec3_t hi = vec3_make(1.0f, 1.0f, -9.0f);
    const vec3_t center = vec3_scale(vec3_add(lo, hi), 0.5f);
    // 外接球半径 = 半对角线
    const f32 radius = vec3_length(vec3_scale(vec3_sub(hi, lo), 0.5f)) * 1.001f;

    EXPECT_TRUE(frustum_intersects_aabb(&f, lo, hi));
    EXPECT_TRUE(frustum_intersects_sphere(&f, center, radius));

    // 反过来：远处那个不可见的盒子，它的外接球也不可见
    const vec3_t far_lo = vec3_make(-1.0f, -1.0f, -200.0f);
    const vec3_t far_hi = vec3_make(1.0f, 1.0f, -150.0f);
    const vec3_t far_center = vec3_scale(vec3_add(far_lo, far_hi), 0.5f);
    const f32    far_radius = vec3_length(vec3_scale(vec3_sub(far_hi, far_lo), 0.5f)) * 1.001f;

    EXPECT_FALSE(frustum_intersects_aabb(&f, far_lo, far_hi));
    EXPECT_FALSE(frustum_intersects_sphere(&f, far_center, far_radius));
}

// ---------------------------------------------------------------- 变换包围盒

static void frustum_transform_aabb_translation(void)
{
    const mat4_t t = mat4_translation(vec3_make(10.0f, -5.0f, 3.0f));

    vec3_t lo;
    vec3_t hi;
    frustum_transform_aabb(t, vec3_make(-1.0f, -2.0f, -3.0f), vec3_make(1.0f, 2.0f, 3.0f), &lo, &hi);

    EXPECT_VEC3_NEAR(lo, vec3_make(9.0f, -7.0f, 0.0f), F32_TOLERANCE);
    EXPECT_VEC3_NEAR(hi, vec3_make(11.0f, -3.0f, 6.0f), F32_TOLERANCE);
}

// 绕 Y 轴转 45° 后，单位立方体的 XZ 范围应该扩大到 ±√2。
//
// 这条测试专门抓"只变换 min/max 两个角点"的偷懒实现：
// 那样算出来的范围还是 ±1，物体转到 45° 时会被错误剔除。
static void frustum_transform_aabb_rotation_expands(void)
{
    const mat4_t r = mat4_rotation_y(math_radians(45.0f));

    vec3_t lo;
    vec3_t hi;
    frustum_transform_aabb(r, vec3_make(-1.0f, -1.0f, -1.0f), vec3_make(1.0f, 1.0f, 1.0f), &lo, &hi);

    const f32 expected = 1.41421356f;
    EXPECT_NEAR(hi.x, expected, 1.0e-4f);
    EXPECT_NEAR(hi.z, expected, 1.0e-4f);
    EXPECT_NEAR(lo.x, -expected, 1.0e-4f);
    EXPECT_NEAR(lo.z, -expected, 1.0e-4f);
    // Y 方向不受绕 Y 旋转影响
    EXPECT_NEAR(hi.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(lo.y, -1.0f, 1.0e-5f);
}

static void frustum_transform_aabb_uniform_scale(void)
{
    const mat4_t s = mat4_scale(vec3_splat(3.0f));

    vec3_t lo;
    vec3_t hi;
    frustum_transform_aabb(s, vec3_make(-1.0f, -1.0f, -1.0f), vec3_make(1.0f, 1.0f, 1.0f), &lo, &hi);

    EXPECT_VEC3_NEAR(lo, vec3_splat(-3.0f), F32_TOLERANCE);
    EXPECT_VEC3_NEAR(hi, vec3_splat(3.0f), F32_TOLERANCE);
}

// ---------------------------------------------------------------- 退化输入

// 退化矩阵（全零）会让所有平面退化成"永远在内侧"。
// 这必须导致"全都画"，而不是"全都不画" —— 后者会让整个场景消失，
// 而且看不出任何报错。
static void frustum_degenerate_matrix_never_culls(void)
{
    const frustum_t f = frustum_from_view_projection(mat4_zero());

    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(0.0f, 0.0f, 0.0f)));
    EXPECT_TRUE(frustum_contains_point(&f, vec3_make(1.0e6f, -1.0e6f, 1.0e6f)));
    EXPECT_TRUE(frustum_intersects_aabb(&f, vec3_make(100.0f, 100.0f, 100.0f),
                                        vec3_make(200.0f, 200.0f, 200.0f)));
    EXPECT_TRUE(frustum_intersects_sphere(&f, vec3_make(1.0e6f, 0.0f, 0.0f), 1.0f));
}

void test_frustum_register(void)
{
    test_register("frustum", "planes_point_inward", frustum_planes_point_inward);
    test_register("frustum", "point_inside", frustum_point_inside);
    test_register("frustum", "point_behind_camera", frustum_point_behind_camera_is_outside);
    test_register("frustum", "point_inside_near_plane",
                  frustum_point_inside_near_plane_is_outside);
    test_register("frustum", "point_beyond_far", frustum_point_beyond_far_is_outside);
    test_register("frustum", "point_outside_sides", frustum_point_outside_sides);
    test_register("frustum", "matches_clip_space", frustum_matches_clip_space_definition);
    test_register("frustum", "matches_clip_space_oblique",
                  frustum_matches_clip_space_when_camera_is_oblique);
    test_register("frustum", "aabb_inside", frustum_aabb_inside);
    test_register("frustum", "aabb_outside", frustum_aabb_outside);
    test_register("frustum", "aabb_straddling", frustum_aabb_straddling_is_conservative);
    test_register("frustum", "aabb_looser_than_point", frustum_aabb_is_looser_than_point_test);
    test_register("frustum", "sphere", frustum_sphere_tests);
    test_register("frustum", "sphere_containing_aabb", frustum_sphere_containing_aabb_agrees);
    test_register("frustum", "transform_aabb_translation", frustum_transform_aabb_translation);
    test_register("frustum", "transform_aabb_rotation", frustum_transform_aabb_rotation_expands);
    test_register("frustum", "transform_aabb_scale", frustum_transform_aabb_uniform_scale);
    test_register("frustum", "degenerate_matrix", frustum_degenerate_matrix_never_culls);
}
