// =============================================================================
//  tests/test_mat.c —— 矩阵与投影
//
//  投影部分是本文件最重要的内容：坐标系约定一旦写错，整个引擎的渲染都会
//  是"看起来能跑但就是不对"的状态，而且很难定位。这里把约定用可执行的
//  断言钉死：近平面 -> 深度 0，远平面 -> 深度 1，相机看 -Z。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// ---------------------------------------------------------------- 布局
static void layout_matches_spec(void)
{
    EXPECT_EQ_U64(sizeof(mat3_t), 48);
    EXPECT_EQ_U64(alignof(mat3_t), 16);
    EXPECT_EQ_U64(sizeof(mat4_t), 64);
    EXPECT_EQ_U64(alignof(mat4_t), 16);

    static mat4_t storage[4];
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(storage); ++i) {
        EXPECT_EQ_U64((usize)(uintptr_t)&storage[i] % 16u, 0u);
    }
}

// ---------------------------------------------------------------- 基本运算
static void identity_and_translation(void)
{
    mat4_t identity = mat4_identity();
    EXPECT_MAT4_NEAR(mat4_mul(identity, identity), identity, 0.0f);

    mat4_t translate = mat4_translation(vec3_make(1.0f, 2.0f, 3.0f));
    EXPECT_MAT4_NEAR(mat4_mul(identity, translate), translate, 0.0f);
    EXPECT_MAT4_NEAR(mat4_mul(translate, identity), translate, 0.0f);

    EXPECT_VEC3_NEAR(mat4_transform_point(translate, vec3_zero()), vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);
    EXPECT_VEC3_NEAR(mat4_get_translation(translate), vec3_make(1.0f, 2.0f, 3.0f), 1e-6f);

    // 方向不受平移影响 —— w = 0 的作用
    EXPECT_VEC3_NEAR(mat4_transform_direction(translate, vec3_make(1.0f, 1.0f, 1.0f)),
                     vec3_make(1.0f, 1.0f, 1.0f), 1e-6f);
}

static void from_rows_matches_from_columns(void)
{
    mat4_t by_rows = mat4_from_rows(1.0f, 2.0f, 3.0f, 4.0f,   //
                                    5.0f, 6.0f, 7.0f, 8.0f,   //
                                    9.0f, 10.0f, 11.0f, 12.0f, //
                                    13.0f, 14.0f, 15.0f, 16.0f);

    mat4_t by_columns = mat4_from_columns(vec4_make(1.0f, 5.0f, 9.0f, 13.0f),
                                          vec4_make(2.0f, 6.0f, 10.0f, 14.0f),
                                          vec4_make(3.0f, 7.0f, 11.0f, 15.0f),
                                          vec4_make(4.0f, 8.0f, 12.0f, 16.0f));

    EXPECT_MAT4_NEAR(by_rows, by_columns, 0.0f);

    // 列主序下：第 0 行第 1 列的元素存放在 col[1].x
    EXPECT_NEAR(by_rows.col[1].x, 2.0f, 0.0f);
    EXPECT_NEAR(by_rows.col[0].y, 5.0f, 0.0f);
    EXPECT_NEAR(by_rows.col[3].w, 16.0f, 0.0f);
}

static void multiplication_order_semantics(void)
{
    mat4_t translate = mat4_translation(vec3_make(1.0f, 0.0f, 0.0f));
    mat4_t rotate = mat4_rotation_z(F32_HALF_PI);
    mat4_t scale = mat4_scale(vec3_make(2.0f, 2.0f, 2.0f));

    // 结合律
    EXPECT_MAT4_NEAR(mat4_mul(mat4_mul(translate, rotate), scale),
                     mat4_mul(translate, mat4_mul(rotate, scale)), 1e-5f);

    // 不可交换
    EXPECT_FALSE(mat4_near(mat4_mul(translate, rotate), mat4_mul(rotate, translate), 1e-4f));

    // (a*b) 作用于点 == a 作用于 (b 作用于点)
    vec3_t p = vec3_make(1.0f, 0.0f, 0.0f);
    vec3_t combined = mat4_transform_point(mat4_mul(translate, rotate), p);
    vec3_t staged = mat4_transform_point(translate, mat4_transform_point(rotate, p));
    EXPECT_VEC3_NEAR(combined, staged, 1e-5f);

    // 具体数值：先转 90 度 (1,0,0)->(0,1,0)，再平移 (1,0,0) -> (1,1,0)
    EXPECT_VEC3_NEAR(combined, vec3_make(1.0f, 1.0f, 0.0f), 1e-5f);
}

static void transpose_and_determinant(void)
{
    mat4_t m = mat4_from_rows(1.0f, 2.0f, 3.0f, 4.0f,   //
                              5.0f, 6.0f, 7.0f, 8.0f,   //
                              9.0f, 10.0f, 11.0f, 12.0f, //
                              13.0f, 14.0f, 15.0f, 16.0f);

    mat4_t mt = mat4_transpose(m);
    EXPECT_MAT4_NEAR(mat4_transpose(mt), m, 0.0f);
    EXPECT_NEAR(mt.col[0].y, 2.0f, 0.0f); // 原 m.col[1].x

    // 该矩阵各行线性相关（相邻行差恒为 4,4,4,4），行列式必为 0
    EXPECT_NEAR(mat4_determinant(m), 0.0f, 1e-3f);
    EXPECT_NEAR(mat4_determinant(mat4_identity()), 1.0f, 1e-6f);
    EXPECT_NEAR(mat4_determinant(mat4_scale(vec3_make(2.0f, 3.0f, 4.0f))), 24.0f, 1e-5f);
    EXPECT_NEAR(mat4_determinant(mat4_zero()), 0.0f, 0.0f);

    // 行列式的乘法性质：det(AB) = det(A)det(B)
    mat4_t a = mat4_translation(vec3_make(3.0f, -1.0f, 2.0f));
    mat4_t b = mat4_scale(vec3_make(2.0f, 0.5f, 3.0f));
    EXPECT_NEAR(mat4_determinant(mat4_mul(a, b)),
                mat4_determinant(a) * mat4_determinant(b), 1e-4f);
}

static void inverse_roundtrip(void)
{
    mat4_t cases[5];
    cases[0] = mat4_identity();
    cases[1] = mat4_translation(vec3_make(3.0f, -2.0f, 7.0f));
    cases[2] = mat4_scale(vec3_make(2.0f, 0.5f, 3.0f));
    cases[3] = mat4_from_trs(vec3_make(1.0f, 2.0f, 3.0f), quat_from_euler(0.3f, -0.7f, 1.1f),
                             vec3_make(2.0f, 0.5f, 3.0f));
    cases[4] = mat4_mul(mat4_rotation_y(0.9f),
                        mat4_mul(mat4_scale(vec3_make(1.0f, 2.0f, 4.0f)),
                                 mat4_translation(vec3_make(-5.0f, 1.0f, 2.0f))));

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(cases); ++i) {
        mat4_t inverse = mat4_inverse(cases[i]);
        EXPECT_MAT4_NEAR(mat4_mul(cases[i], inverse), mat4_identity(), 1e-4f);
        EXPECT_MAT4_NEAR(mat4_mul(inverse, cases[i]), mat4_identity(), 1e-4f);
    }

    // 奇异矩阵的逆是零矩阵，而不是 NaN —— 让错误可预测
    EXPECT_MAT4_NEAR(mat4_inverse(mat4_zero()), mat4_zero(), 0.0f);

    // 用逆矩阵把点变换回去
    vec3_t p = vec3_make(4.0f, -3.0f, 9.0f);
    mat4_t m = cases[3];
    EXPECT_VEC3_NEAR(mat4_transform_point(mat4_inverse(m), mat4_transform_point(m, p)), p, 1e-4f);
}

static void normal_matrix_for_nonuniform_scale(void)
{
    mat4_t model = mat4_scale(vec3_make(1.0f, 10.0f, 1.0f));
    vec3_t normal = vec3_normalize(vec3_make(1.0f, 1.0f, 0.0f));

    vec3_t naive = vec3_normalize(mat4_transform_direction(model, normal));
    vec3_t correct = vec3_normalize(mat3_mul_vec3(mat4_normal_matrix(model), normal));

    // 两者必须明显不同 —— 这就是"必须用法线矩阵"的实证
    EXPECT_FALSE(vec3_near(naive, correct, 1.0e-2f));

    // 法线矩阵的结果必须仍垂直于变换后的切向量
    vec3_t tangent = vec3_make(1.0f, -1.0f, 0.0f);
    vec3_t tangent_transformed = vec3_normalize(mat4_transform_direction(model, tangent));
    EXPECT_NEAR(vec3_dot(correct, tangent_transformed), 0.0f, 1e-5f);
}

// ---------------------------------------------------------------- 投影
static void ortho_maps_depth_to_zero_one(void)
{
    const f32 znear = 0.1f;
    const f32 zfar = 100.0f;
    mat4_t proj = mat4_ortho(-1.0f, 1.0f, -1.0f, 1.0f, znear, zfar);

    // 约定：右手系，相机看 -Z，近平面深度 0，远平面深度 1
    vec4_t near_point = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -znear, 1.0f));
    EXPECT_NEAR(near_point.z / near_point.w, 0.0f, 1e-5f);

    vec4_t far_point = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -zfar, 1.0f));
    EXPECT_NEAR(far_point.z / far_point.w, 1.0f, 1e-4f);

    // 左右/上下边界映射到 NDC ±1
    vec4_t right = mat4_mul_vec4(proj, vec4_make(1.0f, 0.0f, -znear, 1.0f));
    EXPECT_NEAR(right.x / right.w, 1.0f, 1e-5f);
    vec4_t left = mat4_mul_vec4(proj, vec4_make(-1.0f, 0.0f, -znear, 1.0f));
    EXPECT_NEAR(left.x / left.w, -1.0f, 1e-5f);
    vec4_t top = mat4_mul_vec4(proj, vec4_make(0.0f, 1.0f, -znear, 1.0f));
    EXPECT_NEAR(top.y / top.w, 1.0f, 1e-5f);
    vec4_t bottom = mat4_mul_vec4(proj, vec4_make(0.0f, -1.0f, -znear, 1.0f));
    EXPECT_NEAR(bottom.y / bottom.w, -1.0f, 1e-5f);

    // 正交投影下 w 恒为 1
    EXPECT_NEAR(far_point.w, 1.0f, 1e-6f);
}

static void ortho_2d_puts_z0_at_half_depth(void)
{
    mat4_t proj = mat4_ortho_2d(1920.0f, 1080.0f);

    // 原点在屏幕中心
    vec4_t center = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(center.x, 0.0f, 1e-5f);
    EXPECT_NEAR(center.y, 0.0f, 1e-5f);

    // z = 0 的精灵落在深度 0.5 —— 2.5D 混合渲染时 2D 内容才有前后排序空间
    EXPECT_NEAR(center.z / center.w, 0.5f, 1e-5f);

    // 右下角
    vec4_t corner = mat4_mul_vec4(proj, vec4_make(960.0f, -540.0f, 0.0f, 1.0f));
    EXPECT_NEAR(corner.x / corner.w, 1.0f, 1e-5f);
    EXPECT_NEAR(corner.y / corner.w, -1.0f, 1e-5f);
}

static void perspective_maps_depth_and_applies_aspect(void)
{
    const f32 fov = math_radians(60.0f);
    const f32 aspect = 16.0f / 9.0f;
    const f32 znear = 0.1f;
    const f32 zfar = 1000.0f;

    mat4_t proj = mat4_perspective(fov, aspect, znear, zfar);

    // 近平面中心 -> 深度 0
    vec4_t p_near = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -znear, 1.0f));
    EXPECT_NEAR(p_near.z / p_near.w, 0.0f, 1e-4f);

    // 远平面中心 -> 深度 1
    vec4_t p_far = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -zfar, 1.0f));
    EXPECT_NEAR(p_far.z / p_far.w, 1.0f, 1e-3f);

    // 裁剪空间 w 等于相机前方距离 —— 这是透视除法的物理含义
    EXPECT_NEAR(p_far.w, zfar, 1e-2f);
    vec4_t p_mid = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -10.0f, 1.0f));
    EXPECT_NEAR(p_mid.w, 10.0f, 1e-4f);

    // 垂直视场角：z = -1 处 y = tan(fov/2) 的点映射到 NDC y = 1
    const f32 y_at_unit = tanf(fov * 0.5f);
    vec4_t p_top = mat4_mul_vec4(proj, vec4_make(0.0f, y_at_unit, -1.0f, 1.0f));
    EXPECT_NEAR(p_top.y / p_top.w, 1.0f, 1e-4f);

    // 水平方向考虑 aspect
    vec4_t p_right = mat4_mul_vec4(proj, vec4_make(y_at_unit * aspect, 0.0f, -1.0f, 1.0f));
    EXPECT_NEAR(p_right.x / p_right.w, 1.0f, 1e-4f);

    // 更远的点在深度上更接近 1，且单调
    f32 previous = -1.0f;
    for (f32 d = 1.0f; d < 500.0f; d *= 2.0f) {
        vec4_t p = mat4_mul_vec4(proj, vec4_make(0.0f, 0.0f, -d, 1.0f));
        f32 depth = p.z / p.w;
        EXPECT_TRUE(depth > previous);
        EXPECT_TRUE(depth >= 0.0f && depth <= 1.0f);
        previous = depth;
    }
}

static void look_at_places_camera_correctly(void)
{
    // 相机在 (0,0,5) 看向原点：原点应在相机空间 (0,0,-5)
    mat4_t view = mat4_look_at(vec3_make(0.0f, 0.0f, 5.0f), vec3_zero(), vec3_unit_y());
    EXPECT_VEC3_NEAR(mat4_transform_point(view, vec3_zero()), vec3_make(0.0f, 0.0f, -5.0f), 1e-5f);
    // 相机自身位置变换到原点
    EXPECT_VEC3_NEAR(mat4_transform_point(view, vec3_make(0.0f, 0.0f, 5.0f)), vec3_zero(), 1e-5f);
    // 世界轴在相机空间里方向不变（相机没旋转）
    EXPECT_VEC3_NEAR(mat4_transform_direction(view, vec3_unit_x()), vec3_unit_x(), 1e-5f);
    EXPECT_VEC3_NEAR(mat4_transform_direction(view, vec3_unit_y()), vec3_unit_y(), 1e-5f);
    EXPECT_VEC3_NEAR(mat4_transform_direction(view, vec3_unit_z()), vec3_unit_z(), 1e-5f);

    // 相机在 (5,0,0) 看向原点：前方是 -X
    mat4_t view2 = mat4_look_at(vec3_make(5.0f, 0.0f, 0.0f), vec3_zero(), vec3_unit_y());
    vec3_t origin_in_view = mat4_transform_point(view2, vec3_zero());
    EXPECT_NEAR(origin_in_view.z, -5.0f, 1e-5f);
    EXPECT_NEAR(origin_in_view.y, 0.0f, 1e-5f);
    EXPECT_NEAR(origin_in_view.x, 0.0f, 1e-5f);

    // 视图矩阵不改变距离（纯刚体变换）
    vec3_t world_point = vec3_make(1.0f, 2.0f, 3.0f);
    vec3_t eye = vec3_make(-4.0f, 1.0f, 7.0f);
    mat4_t v3 = mat4_look_at(eye, vec3_make(0.0f, 1.0f, 0.0f), vec3_unit_y());
    EXPECT_NEAR(vec3_distance(eye, world_point),
                vec3_length(mat4_transform_point(v3, world_point)), 1e-4f);

    // 视线与 up 平行时不能产生 NaN
    mat4_t degenerate = mat4_look_at(vec3_zero(), vec3_make(0.0f, 1.0f, 0.0f), vec3_unit_y());
    for (int c = 0; c < 4; ++c) {
        EXPECT_TRUE(!isnan((f64)degenerate.col[c].x));
        EXPECT_TRUE(!isnan((f64)degenerate.col[c].y));
        EXPECT_TRUE(!isnan((f64)degenerate.col[c].z));
        EXPECT_TRUE(!isnan((f64)degenerate.col[c].w));
    }
}

static void full_mvp_pipeline(void)
{
    // 一个点：世界 -> 相机 -> 裁剪空间，验证整条链路自洽。
    // 相机在 (0,0,10) 看向原点，点在原点：应该在屏幕正中央，深度介于 0 和 1。
    mat4_t view = mat4_look_at(vec3_make(0.0f, 0.0f, 10.0f), vec3_zero(), vec3_unit_y());
    mat4_t proj = mat4_perspective(math_radians(60.0f), 1.0f, 0.1f, 100.0f);

    vec4_t clip = mat4_mul_vec4(proj, mat4_mul_vec4(view, vec4_make(0.0f, 0.0f, 0.0f, 1.0f)));
    vec3_t ndc = vec4_perspective_divide(clip);

    EXPECT_NEAR(ndc.x, 0.0f, 1e-5f);
    EXPECT_NEAR(ndc.y, 0.0f, 1e-5f);
    EXPECT_TRUE(ndc.z > 0.0f && ndc.z < 1.0f);

    // 相机正前方 10 个单位处的点，也应该在屏幕中心
    vec4_t clip2 = mat4_mul_vec4(proj, mat4_mul_vec4(view, vec4_make(0.0f, 0.0f, 5.0f, 1.0f)));
    vec3_t ndc2 = vec4_perspective_divide(clip2);
    EXPECT_NEAR(ndc2.x, 0.0f, 1e-5f);
    EXPECT_NEAR(ndc2.y, 0.0f, 1e-5f);

    // 相机背后的点应产生负的 w（会被裁剪掉），而不是被错误地画出来
    vec4_t behind = mat4_mul_vec4(proj, mat4_mul_vec4(view, vec4_make(0.0f, 0.0f, 20.0f, 1.0f)));
    EXPECT_TRUE(behind.w < 0.0f);
}

// ---------------------------------------------------------------- 后端适配
static void ndc_helpers(void)
{
    mat4_t flip = mat4_ndc_y_flip();
    vec4_t v = mat4_mul_vec4(flip, vec4_make(1.0f, 1.0f, 0.5f, 1.0f));
    EXPECT_NEAR(v.y, -1.0f, 0.0f);
    EXPECT_NEAR(v.x, 1.0f, 0.0f);

    mat4_t remap = mat4_ndc_depth_to_negative_one();
    vec4_t z0 = mat4_mul_vec4(remap, vec4_make(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(z0.z, -1.0f, 1e-6f);
    vec4_t z1 = mat4_mul_vec4(remap, vec4_make(0.0f, 0.0f, 1.0f, 1.0f));
    EXPECT_NEAR(z1.z, 1.0f, 1e-6f);
    vec4_t z_half = mat4_mul_vec4(remap, vec4_make(0.0f, 0.0f, 0.5f, 1.0f));
    EXPECT_NEAR(z_half.z, 0.0f, 1e-6f);
}

// ---------------------------------------------------------------- mat3
static void mat3_basics(void)
{
    mat3_t identity = mat3_identity();
    EXPECT_MAT3_NEAR(mat3_mul(identity, identity), identity, 0.0f);
    EXPECT_NEAR(mat3_determinant(identity), 1.0f, 1e-6f);

    mat3_t rotate = mat3_rotation_z(F32_HALF_PI);
    EXPECT_VEC3_NEAR(mat3_mul_vec3(rotate, vec3_unit_x()), vec3_unit_y(), 1e-6f);

    // 旋转矩阵是正交矩阵：逆 = 转置
    EXPECT_MAT3_NEAR(mat3_inverse(rotate), mat3_transpose(rotate), 1e-5f);

    // 一般矩阵求逆
    mat3_t m = mat3_from_rows(2.0f, 1.0f, 0.0f, //
                              1.0f, 3.0f, 1.0f, //
                              0.0f, 1.0f, 4.0f);
    EXPECT_NEAR(mat3_determinant(m), 18.0f, 1e-5f);
    EXPECT_MAT3_NEAR(mat3_mul(m, mat3_inverse(m)), mat3_identity(), 1e-5f);
    EXPECT_MAT3_NEAR(mat3_mul(mat3_inverse(m), m), mat3_identity(), 1e-5f);

    // 奇异矩阵
    EXPECT_MAT3_NEAR(mat3_inverse(mat3_zero()), mat3_zero(), 0.0f);

    // 乘法顺序语义与 mat4 一致
    mat3_t a = mat3_rotation_x(0.4f);
    mat3_t b = mat3_rotation_y(0.9f);
    vec3_t v = vec3_make(0.3f, -0.2f, 0.8f);
    EXPECT_VEC3_NEAR(mat3_mul_vec3(mat3_mul(a, b), v),
                     mat3_mul_vec3(a, mat3_mul_vec3(b, v)), 1e-5f);
}

static void mat3_mat4_bridge(void)
{
    mat4_t m4 = mat4_from_trs(vec3_make(9.0f, 9.0f, 9.0f), quat_identity(), vec3_make(2.0f, 3.0f, 4.0f));
    mat3_t m3 = mat4_to_mat3(m4);

    // 只取左上 3x3，平移被丢掉
    EXPECT_NEAR(m3.col[0].x, 2.0f, 1e-6f);
    EXPECT_NEAR(m3.col[1].y, 3.0f, 1e-6f);
    EXPECT_NEAR(m3.col[2].z, 4.0f, 1e-6f);
    // 没有旋转，非对角元素应为 0
    EXPECT_NEAR(m3.col[1].x, 0.0f, 1e-6f);
    EXPECT_NEAR(m3.col[0].y, 0.0f, 1e-6f);

    // 往返
    EXPECT_MAT3_NEAR(mat4_to_mat3(mat4_from_mat3(m3)), m3, 0.0f);

    // from_mat3 会把平移置零、w 置 1
    mat4_t round = mat4_from_mat3(m3);
    EXPECT_NEAR(round.col[3].x, 0.0f, 0.0f);
    EXPECT_NEAR(round.col[3].y, 0.0f, 0.0f);
    EXPECT_NEAR(round.col[3].z, 0.0f, 0.0f);
    EXPECT_NEAR(round.col[3].w, 1.0f, 0.0f);
}

void test_mat_register(void)
{
    test_register("mat", "layout_matches_spec", layout_matches_spec);
    test_register("mat", "identity_and_translation", identity_and_translation);
    test_register("mat", "from_rows_matches_from_columns", from_rows_matches_from_columns);
    test_register("mat", "multiplication_order_semantics", multiplication_order_semantics);
    test_register("mat", "transpose_and_determinant", transpose_and_determinant);
    test_register("mat", "inverse_roundtrip", inverse_roundtrip);
    test_register("mat", "normal_matrix_for_nonuniform_scale", normal_matrix_for_nonuniform_scale);
    test_register("mat", "ortho_maps_depth_to_zero_one", ortho_maps_depth_to_zero_one);
    test_register("mat", "ortho_2d_puts_z0_at_half_depth", ortho_2d_puts_z0_at_half_depth);
    test_register("mat", "perspective_maps_depth_and_applies_aspect",
                  perspective_maps_depth_and_applies_aspect);
    test_register("mat", "look_at_places_camera_correctly", look_at_places_camera_correctly);
    test_register("mat", "full_mvp_pipeline", full_mvp_pipeline);
    test_register("mat", "ndc_helpers", ndc_helpers);
    test_register("mat", "mat3_basics", mat3_basics);
    test_register("mat", "mat3_mat4_bridge", mat3_mat4_bridge);
}
