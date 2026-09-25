// =============================================================================
//  tests/test_render.c —— 渲染核心（不需要 GL 上下文的部分）
//
//  这里测的是**纯逻辑**：类型翻译、布局校验、相机矩阵、渲染队列排序、
//  OBJ 解析。这些出错时的表现是"画面顺序不对""光照方向不对"这类
//  很难用眼睛定位的问题，所以必须有测试兜住。
//
//  需要 GPU 的部分（GL 后端、资源创建）由 examples/02_render 端到端验证 ——
//  单元测试里造一个 GL 上下文会让测试依赖显卡驱动，CI 上必然不稳定。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// =============================================================================
//  render_types
// =============================================================================
static void pixel_format_properties(void)
{
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8_UNORM), 1u);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8G8_UNORM), 2u);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8G8B8A8_UNORM), 4u);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R16G16B16A16_FLOAT), 8u);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_INVALID), 0u);

    // sRGB 只对颜色格式成立
    EXPECT_TRUE(pixel_format_is_srgb(PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB));
    EXPECT_TRUE(pixel_format_is_srgb(PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB));
    EXPECT_FALSE(pixel_format_is_srgb(PIXEL_FORMAT_R8G8B8A8_UNORM));
    EXPECT_FALSE(pixel_format_is_srgb(PIXEL_FORMAT_R8_UNORM));

    EXPECT_TRUE(pixel_format_is_depth(PIXEL_FORMAT_D32_FLOAT));
    EXPECT_TRUE(pixel_format_is_depth(PIXEL_FORMAT_D24_UNORM_S8_UINT));
    EXPECT_FALSE(pixel_format_is_depth(PIXEL_FORMAT_R8G8B8A8_UNORM));
}

static void vertex_format_properties(void)
{
    EXPECT_EQ_U64(vertex_format_component_count(VERTEX_FORMAT_FLOAT3), 3u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_FLOAT3), 12u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_FLOAT2), 8u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM8X4), 4u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM16X4), 8u);

    // 分量数与字节数必须自洽。
    // 这里把每种"分量宽度"都覆盖到：只测 float 的话，UNORM8X4 / UNORM16X4
    // 这类紧凑格式算错字节数完全不会被发现 —— 而它们算错的后果是
    // 顶点布局校验通过、实际数据错位，画面一片混乱。
    const vertex_format_t float_formats[] = {
        VERTEX_FORMAT_FLOAT1, VERTEX_FORMAT_FLOAT2, VERTEX_FORMAT_FLOAT3, VERTEX_FORMAT_FLOAT4};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(float_formats); ++i) {
        EXPECT_EQ_U64(vertex_format_byte_size(float_formats[i]),
                      vertex_format_component_count(float_formats[i]) * 4u);
    }

    const vertex_format_t uint_formats[] = {
        VERTEX_FORMAT_UINT1, VERTEX_FORMAT_UINT2, VERTEX_FORMAT_UINT3, VERTEX_FORMAT_UINT4};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(uint_formats); ++i) {
        EXPECT_EQ_U64(vertex_format_byte_size(uint_formats[i]),
                      vertex_format_component_count(uint_formats[i]) * 4u);
    }

    // 8 位 / 16 位紧凑格式
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM8X4), 4u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_SNORM8X4), 4u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM16X2), 4u);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM16X4), 8u);
    EXPECT_EQ_U64(vertex_format_component_count(VERTEX_FORMAT_UNORM16X4), 4u);

    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_INVALID), 0u);
    EXPECT_EQ_U64(vertex_format_component_count(VERTEX_FORMAT_INVALID), 0u);
}

static void mesh_vertex_layout_is_consistent(void)
{
    const vertex_layout_t layout = vertex_layout_mesh_vertex();

    EXPECT_EQ_U64(layout.binding_count, 1u);
    EXPECT_EQ_U64(layout.attribute_count, 3u);
    EXPECT_EQ_U64(layout.bindings[0].stride, MESH_VERTEX_STRIDE);
    EXPECT_FALSE(layout.bindings[0].per_instance);

    const char *reason = NULL;
    EXPECT_TRUE(vertex_layout_validate(&layout, &reason));

    const i32 position = vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_POSITION);
    const i32 normal = vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_NORMAL);
    const i32 texcoord = vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_TEXCOORD0);
    EXPECT_EQ_I64(position, 0);
    EXPECT_EQ_I64(normal, 1);
    EXPECT_EQ_I64(texcoord, 2);

    EXPECT_EQ_U64(layout.attributes[position].offset, 0u);
    EXPECT_EQ_U64(layout.attributes[normal].offset, 12u);
    EXPECT_EQ_U64(layout.attributes[texcoord].offset, 24u);

    // 属性 location 用语义枚举值，与着色器里的 layout(location = N) 对应
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(VERTEX_SEMANTIC_POSITION), 0u);
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(VERTEX_SEMANTIC_NORMAL), 1u);
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(VERTEX_SEMANTIC_TEXCOORD0), 4u);

    // 不存在的语义返回 -1
    EXPECT_EQ_I64(vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_TANGENT), -1);
    EXPECT_EQ_I64(vertex_layout_find_attribute(NULL, VERTEX_SEMANTIC_POSITION), -1);

    // mesh_vertex_t 的字段偏移必须与布局描述一致
    EXPECT_EQ_U64(ENGINE_OFFSETOF(mesh_vertex_t, position), 0u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(mesh_vertex_t, normal), 12u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(mesh_vertex_t, texcoord), 24u);
}

static void vertex_layout_validate_rejects_bad_layouts(void)
{
    const char *reason = NULL;

    EXPECT_FALSE(vertex_layout_validate(NULL, &reason));

    // 没有 binding
    {
        vertex_layout_t layout;
        memset(&layout, 0, sizeof(layout));
        layout.attribute_count = 1;
        layout.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
        layout.attributes[0].format = VERTEX_FORMAT_FLOAT3;
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
        EXPECT_NOT_NULL(reason);
    }

    // stride 为 0
    {
        vertex_layout_t layout = vertex_layout_mesh_vertex();
        layout.bindings[0].stride = 0;
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    }

    // 属性引用了不存在的 binding
    {
        vertex_layout_t layout = vertex_layout_mesh_vertex();
        layout.attributes[0].binding = 3;
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    }

    // 属性越过 stride 边界
    {
        vertex_layout_t layout = vertex_layout_mesh_vertex();
        layout.attributes[0].offset = 30; // 30 + 12 > 32
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    }

    // 同一语义声明两次
    {
        vertex_layout_t layout = vertex_layout_mesh_vertex();
        layout.attributes[2].semantic = VERTEX_SEMANTIC_POSITION;
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    }

    // 无效的顶点格式
    {
        vertex_layout_t layout = vertex_layout_mesh_vertex();
        layout.attributes[0].format = VERTEX_FORMAT_INVALID;
        EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    }
}

static void blend_presets(void)
{
    const blend_state_t opaque = blend_state_opaque();
    EXPECT_FALSE(opaque.enabled);
    EXPECT_EQ_U64(opaque.write_mask, BLEND_WRITE_ALL);

    const blend_state_t alpha = blend_state_alpha();
    EXPECT_TRUE(alpha.enabled);
    EXPECT_EQ_U64(alpha.src_color, BLEND_FACTOR_SRC_ALPHA);
    EXPECT_EQ_U64(alpha.dst_color, BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    // 预乘 alpha 的源因子必须是 ONE。用 SRC_ALPHA 会乘两次，边缘发暗 ——
    // 这是 2D 精灵半透明边缘出现黑边的最常见原因。
    const blend_state_t premultiplied = blend_state_premultiplied();
    EXPECT_TRUE(premultiplied.enabled);
    EXPECT_EQ_U64(premultiplied.src_color, BLEND_FACTOR_ONE);
    EXPECT_EQ_U64(premultiplied.dst_color, BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

    const blend_state_t additive = blend_state_additive();
    EXPECT_TRUE(additive.enabled);
    EXPECT_EQ_U64(additive.dst_color, BLEND_FACTOR_ONE);
}

static void depth_presets(void)
{
    const depth_state_t standard = depth_state_default();
    EXPECT_TRUE(standard.test_enabled);
    EXPECT_TRUE(standard.write_enabled);
    EXPECT_EQ_U64(standard.compare, COMPARE_OP_LESS);

    // 半透明必须关掉深度写入：否则后面的透明物体会被挡住
    const depth_state_t read_only = depth_state_read_only();
    EXPECT_TRUE(read_only.test_enabled);
    EXPECT_FALSE(read_only.write_enabled);

    const depth_state_t disabled = depth_state_disabled();
    EXPECT_FALSE(disabled.test_enabled);
    EXPECT_FALSE(disabled.write_enabled);
    EXPECT_EQ_U64(disabled.compare, COMPARE_OP_ALWAYS);
}

static void raster_presets(void)
{
    const raster_state_t standard = raster_state_default();
    EXPECT_EQ_U64(standard.cull, CULL_MODE_BACK);
    EXPECT_EQ_U64(standard.front_face, FRONT_FACE_COUNTER_CLOCKWISE);
    EXPECT_EQ_U64(standard.fill, FILL_MODE_SOLID);

    EXPECT_EQ_U64(raster_state_no_cull().cull, CULL_MODE_NONE);
    EXPECT_EQ_U64(raster_state_wireframe().fill, FILL_MODE_WIREFRAME);
}

static void sampler_presets(void)
{
    const sampler_desc_t standard = sampler_desc_default();
    EXPECT_EQ_U64(standard.min_filter, SAMPLER_FILTER_LINEAR);
    EXPECT_EQ_U64(standard.mipmap_filter, SAMPLER_MIPMAP_LINEAR);
    EXPECT_EQ_U64(standard.wrap_u, SAMPLER_WRAP_REPEAT);
    EXPECT_NEAR(standard.max_anisotropy, 1.0f, 0.0f);

    // 像素风必须最近邻 + 钳制，否则放大后糊成一团、图集边缘会渗色
    const sampler_desc_t pixel_art = sampler_desc_pixel_art();
    EXPECT_EQ_U64(pixel_art.min_filter, SAMPLER_FILTER_NEAREST);
    EXPECT_EQ_U64(pixel_art.mag_filter, SAMPLER_FILTER_NEAREST);
    EXPECT_EQ_U64(pixel_art.mipmap_filter, SAMPLER_MIPMAP_NONE);
    EXPECT_EQ_U64(pixel_art.wrap_u, SAMPLER_WRAP_CLAMP_TO_EDGE);

    EXPECT_EQ_U64(sampler_desc_clamp_linear().wrap_v, SAMPLER_WRAP_CLAMP_TO_EDGE);
}

static void pipeline_and_draw_defaults(void)
{
    const pipeline_desc_t desc = pipeline_desc_default();
    EXPECT_EQ_U64(desc.shader, SHADER_HANDLE_INVALID);
    EXPECT_EQ_U64(desc.topology, PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    EXPECT_EQ_U64(desc.vertex_layout.attribute_count, 3u);
    EXPECT_FALSE(desc.blend.enabled);
    EXPECT_TRUE(desc.depth.test_enabled);

    const draw_call_t call = draw_call_all();
    EXPECT_EQ_U64(call.index_count, 0u); // 0 = 用整个网格
    EXPECT_EQ_U64(call.instance_count, 1u);
}

static void render_stats_accumulation(void)
{
    render_stats_t a;
    render_stats_reset(&a);
    EXPECT_EQ_U64(a.draw_calls, 0u);

    render_stats_t b;
    render_stats_reset(&b);
    b.draw_calls = 3;
    b.triangles = 100;
    b.buffer_upload_bytes = 1024;

    render_stats_accumulate(&a, &b);
    render_stats_accumulate(&a, &b);
    EXPECT_EQ_U64(a.draw_calls, 6u);
    EXPECT_EQ_U64(a.triangles, 200u);
    EXPECT_EQ_U64(a.buffer_upload_bytes, 2048u);

    // 空指针必须安全
    render_stats_accumulate(NULL, &b);
    render_stats_accumulate(&a, NULL);
    render_stats_reset(NULL);
}

// =============================================================================
//  uniforms（std140 布局）
//
//  注意：这些数字是 **M4 的布局**。M2 时 frame_uniforms 只有 256 字节，
//  M4 加了 view_projection 的逆矩阵、雾/天空/阴影参数和光源数组，
//  长到 1024 字节。布局变了必须同步改这里 —— 这正是"显式钉死"的意义：
//  改结构体时忘改 GLSL，会在运行期表现为"某个 uniform 永远是 0"。
// =============================================================================
static void std140_layout_matches_glsl(void)
{
    // 这些偏移与 uniforms.h 里的 GLSL 声明一一对应。
    // 任何一处对不上，症状是"某个 uniform 永远是 0"——极难定位，
    // 所以在编译期（_Static_assert）与运行期（这里）各钉一遍。
    EXPECT_EQ_U64(sizeof(frame_uniforms_t), 1024u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, view), 0u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, projection), 64u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, view_projection), 128u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, inv_view_projection), 192u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, camera_position), 256u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, ambient), 272u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, light_direction), 288u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, light_color), 304u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, light_params), 320u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, fog_params), 336u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, fog_color), 352u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, sky_params), 368u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, sky_horizon), 384u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, sky_zenith), 400u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, sky_ground), 416u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, light_view_projection), 432u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, shadow_params), 496u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, lights), 512u);

    EXPECT_EQ_U64(sizeof(object_uniforms_t), 176u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, model), 0u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, normal_matrix), 64u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, tint), 128u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, pbr_params), 144u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, emissive), 160u);

    // std140 要求结构体大小是 16 的倍数
    EXPECT_EQ_U64(sizeof(frame_uniforms_t) % 16u, 0u);
    EXPECT_EQ_U64(sizeof(object_uniforms_t) % 16u, 0u);

    // 光源数组的每个元素在 std140 下必须按 16 字节对齐 ——
    // 大小不是 16 的倍数的话，整个数组会错位，
    // 症状是"第一盏灯正常、后面的灯参数全乱"。
    EXPECT_EQ_U64(sizeof(gpu_light_t) % 16u, 0u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, lights) % 16u, 0u);

    // 绑定点不能撞
    EXPECT_TRUE(RENDER_UNIFORM_BINDING_FRAME != RENDER_UNIFORM_BINDING_OBJECT);
    EXPECT_EQ_U64(RENDER_UNIFORM_BINDING_COUNT, 2u);
}

static void uniform_defaults_are_sane(void)
{
    const frame_uniforms_t frame = frame_uniforms_default();
    EXPECT_MAT4_NEAR(frame.view, mat4_identity(), 0.0f);
    EXPECT_MAT4_NEAR(frame.projection, mat4_identity(), 0.0f);

    // 默认给一点环境光：全黑画面分不清是"没光"还是"没画出来"
    EXPECT_GT_U64((u64)(frame.ambient.w * 100.0f), 0u);
    // 默认不启用平行光（w = 0）
    EXPECT_NEAR(frame.light_direction.w, 0.0f, 0.0f);

    const object_uniforms_t object = object_uniforms_default();
    EXPECT_MAT4_NEAR(object.model, mat4_identity(), 0.0f);
    EXPECT_MAT4_NEAR(object.normal_matrix, mat4_identity(), 0.0f);
    EXPECT_VEC4_NEAR(object.tint, vec4_make(1.0f, 1.0f, 1.0f, 1.0f), 0.0f);
}

static void object_uniforms_compute_normal_matrix(void)
{
    // 非均匀缩放：normal_matrix 必须把法线"反向缩放"回来，
    // 否则光照方向会错（表现为物体表面明暗不对，但形状是对的）。
    const mat4_t model = mat4_scale(vec3_make(1.0f, 8.0f, 1.0f));
    const object_uniforms_t uniforms = object_uniforms_from_model(model);

    EXPECT_MAT4_NEAR(uniforms.model, model, 0.0f);

    // 法线矩阵应当是 (M^-1)^T = diag(1, 1/8, 1)
    EXPECT_NEAR(uniforms.normal_matrix.col[0].x, 1.0f, 1e-5f);
    EXPECT_NEAR(uniforms.normal_matrix.col[1].y, 0.125f, 1e-5f);
    EXPECT_NEAR(uniforms.normal_matrix.col[2].z, 1.0f, 1e-5f);

    // 用法线矩阵变换后的法线，必须仍垂直于变换后的切向量
    const vec3_t normal = vec3_normalize(vec3_make(1.0f, 1.0f, 0.0f));
    const vec3_t tangent = vec3_make(1.0f, -1.0f, 0.0f);

    const vec3_t n_transformed =
        vec3_normalize(mat3_mul_vec3(mat4_to_mat3(uniforms.normal_matrix), normal));
    const vec3_t t_transformed = vec3_normalize(mat4_transform_direction(model, tangent));

    EXPECT_NEAR(vec3_dot(n_transformed, t_transformed), 0.0f, 1e-5f);
}

// =============================================================================
//  camera
// =============================================================================
static void camera_basis_vectors(void)
{
    camera_t camera = camera_default();
    camera.position = vec3_zero();
    camera.rotation = quat_identity();

    // 默认朝向：局部 -Z
    EXPECT_VEC3_NEAR(camera_forward(&camera), vec3_make(0.0f, 0.0f, -1.0f), 1e-6f);
    EXPECT_VEC3_NEAR(camera_right(&camera), vec3_unit_x(), 1e-6f);
    EXPECT_VEC3_NEAR(camera_up(&camera), vec3_unit_y(), 1e-6f);

    camera_set_yaw_pitch(&camera, F32_HALF_PI, 0.0f);
    // 绕 Y 转 90 度后前方变成 -X
    EXPECT_VEC3_NEAR(camera_forward(&camera), vec3_make(-1.0f, 0.0f, 0.0f), 1e-5f);

    camera_set_yaw_pitch(&camera, 0.0f, F32_HALF_PI);
    // 抬头 90 度后前方变成 +Y
    EXPECT_VEC3_NEAR(camera_forward(&camera), vec3_make(0.0f, 1.0f, 0.0f), 1e-5f);
}

static void camera_view_matrix_places_world(void)
{
    camera_t camera = camera_perspective(vec3_make(0.0f, 0.0f, 10.0f), quat_identity(), 60.0f,
                                         16.0f / 9.0f, 0.1f, 100.0f);

    const mat4_t view = camera_view_matrix(&camera);
    // 相机自身位置 -> 原点
    EXPECT_VEC3_NEAR(mat4_transform_point(view, vec3_make(0.0f, 0.0f, 10.0f)), vec3_zero(), 1e-4f);
    // 世界原点 -> 相机前方 10 个单位
    EXPECT_VEC3_NEAR(mat4_transform_point(view, vec3_zero()), vec3_make(0.0f, 0.0f, -10.0f),
                     1e-4f);

    // 视图变换是刚体变换：保距
    const vec3_t p = vec3_make(3.0f, -2.0f, 1.0f);
    EXPECT_NEAR(vec3_distance(camera.position, p),
                vec3_length(mat4_transform_point(view, p)), 1e-3f);
}

static void camera_projection_respects_conventions(void)
{
    // 复用 M0 定死的约定：右手系 / 看 -Z / 深度 [0,1]
    camera_t camera = camera_perspective(vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), 60.0f, 1.0f,
                                         0.1f, 100.0f);

    const mat4_t projection = camera_projection_matrix(&camera);

    const vec4_t near_point = mat4_mul_vec4(projection, vec4_make(0.0f, 0.0f, -0.1f, 1.0f));
    EXPECT_NEAR(near_point.z / near_point.w, 0.0f, 1e-4f);

    const vec4_t far_point = mat4_mul_vec4(projection, vec4_make(0.0f, 0.0f, -100.0f, 1.0f));
    EXPECT_NEAR(far_point.z / far_point.w, 1.0f, 1e-3f);

    // 正交模式
    camera.projection = CAMERA_PROJECTION_ORTHOGRAPHIC;
    camera.ortho_height = 10.0f;
    const mat4_t ortho = camera_projection_matrix(&camera);

    // 正交下视口高度的一半映射到 NDC ±1
    const vec4_t top = mat4_mul_vec4(ortho, vec4_make(0.0f, 5.0f, -0.1f, 1.0f));
    EXPECT_NEAR(top.y / top.w, 1.0f, 1e-5f);

    // 正交下 w 恒为 1
    EXPECT_NEAR(top.w, 1.0f, 1e-6f);

    // 正交的 aspect 生效：宽度 = 高度 * aspect
    const vec4_t right = mat4_mul_vec4(ortho, vec4_make(5.0f, 0.0f, -0.1f, 1.0f));
    EXPECT_NEAR(right.x / right.w, 1.0f, 1e-5f);
}

static void camera_view_depth_and_containment(void)
{
    camera_t camera = camera_perspective(vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), 60.0f, 1.0f,
                                         0.1f, 100.0f);

    // 前方 5 个单位的点：深度 = 5
    EXPECT_NEAR(camera_view_depth(&camera, vec3_make(0.0f, 0.0f, -5.0f)), 5.0f, 1e-5f);
    // 相机背后的点：深度为负
    EXPECT_TRUE(camera_view_depth(&camera, vec3_make(0.0f, 0.0f, 5.0f)) < 0.0f);
    // 侧面的点：深度为 0（沿视线方向的投影）
    EXPECT_NEAR(camera_view_depth(&camera, vec3_make(5.0f, 0.0f, 0.0f)), 0.0f, 1e-5f);

    EXPECT_NEAR(camera_distance(&camera, vec3_make(0.0f, 0.0f, -5.0f)), 5.0f, 1e-5f);

    // 视锥包含判定
    EXPECT_TRUE(camera_contains_point(&camera, vec3_make(0.0f, 0.0f, -5.0f), 0.0f));
    // 相机背后
    EXPECT_FALSE(camera_contains_point(&camera, vec3_make(0.0f, 0.0f, 5.0f), 0.0f));
    // 太远
    EXPECT_FALSE(camera_contains_point(&camera, vec3_make(0.0f, 0.0f, -200.0f), 0.0f));
    // 侧向太远（60 度视场、aspect 1 时，z=-1 处半宽 = tan(30°) ≈ 0.577）
    EXPECT_FALSE(camera_contains_point(&camera, vec3_make(10.0f, 0.0f, -1.0f), 0.0f));
}

static void camera_look_at_orientation(void)
{
    camera_t camera = camera_default();
    camera.position = vec3_make(5.0f, 5.0f, 5.0f);
    camera_look_at(&camera, vec3_zero());

    // 前方应当指向原点
    const vec3_t expected = vec3_normalize(vec3_make(-5.0f, -5.0f, -5.0f));
    EXPECT_VEC3_NEAR(camera_forward(&camera), expected, 1e-5f);

    // 目标与相机重合时不能产生 NaN
    camera_look_at(&camera, camera.position);
    EXPECT_TRUE(!isnan((f64)camera.rotation.x) && !isnan((f64)camera.rotation.w));
}

// =============================================================================
//  render_queue
// =============================================================================
static void sort_key_layer_is_dominant(void)
{
    // 不管深度多大，layer 的顺序永远优先。
    // 这是"背景永远在最后面"的保证 —— 如果深度能压过 layer，
    // 一个很远的背景物体就可能被画到 3D 物体前面。
    const u64 background = render_queue_make_sort_key(RENDER_LAYER_BACKGROUND, 999.0f, 0.1f, 1000.0f);
    const u64 opaque = render_queue_make_sort_key(RENDER_LAYER_OPAQUE_3D, 0.1f, 0.1f, 1000.0f);

    EXPECT_LT_U64(background, opaque);

    // layer 字段在高 32 位
    EXPECT_EQ_U64(background >> 32, (u64)RENDER_LAYER_BACKGROUND);
    EXPECT_EQ_U64(opaque >> 32, (u64)RENDER_LAYER_OPAQUE_3D);
}

static void sort_key_depth_direction_depends_on_layer(void)
{
    // 不透明：近 -> 远（深度小的键小）
    const u64 near_opaque = render_queue_make_sort_key(RENDER_LAYER_OPAQUE_3D, 1.0f, 0.1f, 100.0f);
    const u64 far_opaque = render_queue_make_sort_key(RENDER_LAYER_OPAQUE_3D, 90.0f, 0.1f, 100.0f);
    EXPECT_LT_U64(near_opaque, far_opaque);

    // 半透明：远 -> 近（深度大的键小）
    const u64 near_transparent =
        render_queue_make_sort_key(RENDER_LAYER_TRANSPARENT_3D, 1.0f, 0.1f, 100.0f);
    const u64 far_transparent =
        render_queue_make_sort_key(RENDER_LAYER_TRANSPARENT_3D, 90.0f, 0.1f, 100.0f);
    EXPECT_LT_U64(far_transparent, near_transparent);

    // 超出 [near, far] 的深度要被钳制，不能产生越界或回绕
    const u64 too_close = render_queue_make_sort_key(RENDER_LAYER_OPAQUE_3D, -100.0f, 0.1f, 100.0f);
    const u64 too_far = render_queue_make_sort_key(RENDER_LAYER_OPAQUE_3D, 100000.0f, 0.1f, 100.0f);
    EXPECT_EQ_U64(too_close & 0xFFFFFFFFu, 0u);
    EXPECT_EQ_U64(too_far & 0xFFFFFFFFu, 0xFFFFFFFFu);

    EXPECT_TRUE(render_layer_is_transparent(RENDER_LAYER_TRANSPARENT_3D));
    EXPECT_TRUE(render_layer_is_transparent(RENDER_LAYER_TRANSPARENT_2D));
    EXPECT_FALSE(render_layer_is_transparent(RENDER_LAYER_OPAQUE_3D));
    EXPECT_FALSE(render_layer_is_transparent(RENDER_LAYER_UI));
}

static void queue_sorting_is_deterministic(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    camera_t camera = camera_perspective(vec3_zero(), quat_identity(), 60.0f, 1.0f, 0.1f, 100.0f);
    render_queue_begin(&queue, &camera);

    // 提交 100 个"同 layer 同深度"的项：它们必须按提交顺序稳定排列。
    // qsort 不是稳定排序，靠 sort_key + sequence 构成全序才保证了确定性。
    for (u32 i = 0; i < 100; ++i) {
        render_item_t item = render_item_default();
        item.layer = RENDER_LAYER_OPAQUE_3D;
        item.view_depth = 10.0f; // 全部相同
        render_queue_push(&queue, &item);
    }

    EXPECT_EQ_U64(render_queue_count(&queue), 100u);
    render_queue_sort(&queue);

    const render_item_t *items = render_queue_items(&queue);
    for (u32 i = 0; i < 100; ++i) {
        EXPECT_EQ_U64(items[i].sequence, i);
    }

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void queue_sorts_across_layers_and_depths(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    camera_t camera = camera_perspective(vec3_zero(), quat_identity(), 60.0f, 1.0f, 1.0f, 100.0f);
    render_queue_begin(&queue, &camera);

    // 故意乱序提交
    const struct {
        render_layer_t layer;
        f32            depth;
    } inputs[] = {
        {RENDER_LAYER_UI, 5.0f},
        {RENDER_LAYER_TRANSPARENT_3D, 5.0f},
        {RENDER_LAYER_OPAQUE_3D, 80.0f},
        {RENDER_LAYER_BACKGROUND, 50.0f},
        {RENDER_LAYER_TRANSPARENT_3D, 90.0f},
        {RENDER_LAYER_OPAQUE_3D, 10.0f},
    };

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(inputs); ++i) {
        render_item_t item = render_item_default();
        item.layer = inputs[i].layer;
        item.view_depth = inputs[i].depth;
        render_queue_push(&queue, &item);
    }

    render_queue_sort(&queue);
    const render_item_t *items = render_queue_items(&queue);

    // 期望顺序：
    //   Background(50)
    //   Opaque3D 近->远：10, 80
    //   Transparent3D 远->近：90, 5
    //   UI(5)
    EXPECT_EQ_U64(items[0].layer, RENDER_LAYER_BACKGROUND);
    EXPECT_EQ_U64(items[1].layer, RENDER_LAYER_OPAQUE_3D);
    EXPECT_NEAR(items[1].view_depth, 10.0f, 0.0f);
    EXPECT_EQ_U64(items[2].layer, RENDER_LAYER_OPAQUE_3D);
    EXPECT_NEAR(items[2].view_depth, 80.0f, 0.0f);
    EXPECT_EQ_U64(items[3].layer, RENDER_LAYER_TRANSPARENT_3D);
    EXPECT_NEAR(items[3].view_depth, 90.0f, 0.0f);
    EXPECT_EQ_U64(items[4].layer, RENDER_LAYER_TRANSPARENT_3D);
    EXPECT_NEAR(items[4].view_depth, 5.0f, 0.0f);
    EXPECT_EQ_U64(items[5].layer, RENDER_LAYER_UI);

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void queue_reuses_memory_across_frames(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    camera_t camera = camera_default();

    // 第一帧填满
    render_queue_begin(&queue, &camera);
    for (u32 i = 0; i < 500; ++i) {
        render_item_t item = render_item_default();
        item.view_depth = (f32)i;
        render_queue_push(&queue, &item);
    }
    EXPECT_EQ_U64(render_queue_peak_count(&queue), 500u);

    const usize allocations_after_first_frame = tracker.alloc_calls;

    // 之后每帧提交同样规模，不应该再有任何分配 —— 这是"帧内零 malloc"。
    for (u32 frame = 0; frame < 20; ++frame) {
        render_queue_begin(&queue, &camera);
        for (u32 i = 0; i < 500; ++i) {
            render_item_t item = render_item_default();
            item.view_depth = (f32)i;
            render_queue_push(&queue, &item);
        }
        render_queue_sort(&queue);
    }

    EXPECT_EQ_U64(tracker.alloc_calls, allocations_after_first_frame);
    EXPECT_EQ_U64(render_queue_peak_count(&queue), 500u);

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void queue_item_default_is_fully_initialized(void)
{
    const render_item_t item = render_item_default();
    EXPECT_EQ_U64(item.pipeline, PIPELINE_HANDLE_INVALID);
    EXPECT_EQ_U64(item.mesh, MESH_HANDLE_INVALID);
    EXPECT_EQ_U64(item.texture_count, 0u);
    for (u32 i = 0; i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
        EXPECT_EQ_U64(item.textures[i], TEXTURE_HANDLE_INVALID);
    }
    EXPECT_MAT4_NEAR(item.uniforms.model, mat4_identity(), 0.0f);
    EXPECT_VEC4_NEAR(item.uniforms.tint, vec4_one(), 0.0f);
}

// =============================================================================
//  material
// =============================================================================
static void material_texture_slots(void)
{
    material_t material = material_default();
    EXPECT_EQ_U64(material.texture_count, 0u);
    EXPECT_EQ_U64(material.pipeline, PIPELINE_HANDLE_INVALID);
    EXPECT_VEC4_NEAR(material.tint, vec4_make(1.0f, 1.0f, 1.0f, 1.0f), 0.0f);

    const texture_handle_t albedo = handle_make(1, 1);
    material_set_texture(&material, RENDER_TEXTURE_SLOT_ALBEDO, albedo, NULL);
    EXPECT_EQ_U64(material.texture_count, 1u);
    EXPECT_EQ_U64(material.textures[RENDER_TEXTURE_SLOT_ALBEDO], albedo);

    // 设到更高的槽位时，count 要跟着扩到"最大槽位 + 1"，
    // 否则渲染时中间的槽位不会被绑定
    const texture_handle_t normal = handle_make(2, 1);
    material_set_texture(&material, RENDER_TEXTURE_SLOT_NORMAL, normal, NULL);
    EXPECT_EQ_U64(material.texture_count, 2u);

    const sampler_desc_t pixel_art = sampler_desc_pixel_art();
    material_set_texture(&material, RENDER_TEXTURE_SLOT_ALBEDO, albedo, &pixel_art);
    EXPECT_EQ_U64(material.samplers[RENDER_TEXTURE_SLOT_ALBEDO].min_filter,
                  SAMPLER_FILTER_NEAREST);

    // 越界槽位必须被忽略而不是写坏内存
    material_set_texture(&material, 999, albedo, NULL);
    EXPECT_EQ_U64(material.texture_count, 2u);
}

// =============================================================================
//  OBJ 解析
// =============================================================================
static obj_mesh_t parse_obj(arena_t *arena, const char *text)
{
    obj_mesh_t mesh;
    const bool ok = obj_load_from_memory(arena, text, strlen(text), "test", &mesh);
    if (!ok) {
        memset(&mesh, 0, sizeof(mesh));
    }
    return mesh;
}

static void obj_parses_minimal_triangle(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    const char *obj = "# 一个三角形\n"
                      "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "f 1 2 3\n";

    const obj_mesh_t mesh = parse_obj(&arena, obj);
    EXPECT_EQ_U64(mesh.vertex_count, 3u);
    EXPECT_EQ_U64(mesh.index_count, 3u);
    EXPECT_EQ_U64(mesh.source_position_count, 3u);
    EXPECT_EQ_U64(mesh.source_face_count, 1u);

    EXPECT_EQ_U64(mesh.indices[0], 0u);
    EXPECT_EQ_U64(mesh.indices[1], 1u);
    EXPECT_EQ_U64(mesh.indices[2], 2u);

    // 法线缺失时必须补一个默认值，否则着色器 normalize(0,0,0) 会得到 NaN，
    // 表现为整个模型全黑
    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        const vec3_t n = vec3_make(mesh.vertices[i].normal[0], mesh.vertices[i].normal[1],
                                   mesh.vertices[i].normal[2]);
        EXPECT_GT_U64((u64)(vec3_length(n) * 1000.0f), 900u);
    }

    // 包围盒
    EXPECT_VEC3_NEAR(mesh.bounds_min, vec3_zero(), 1e-6f);
    EXPECT_VEC3_NEAR(mesh.bounds_max, vec3_make(1.0f, 1.0f, 0.0f), 1e-6f);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_supports_all_index_forms(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    // 四种索引写法混用：v / v/vt / v//vn / v/vt/vn
    const char *obj = "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "v 1 1 0\n"
                      "vt 0 0\n"
                      "vt 1 0\n"
                      "vn 0 0 1\n"
                      "f 1 2 3\n"          // 只有位置
                      "f 1/1 2/2 3/1\n"    // 位置 + UV
                      "f 1//1 2//1 4//1\n" // 位置 + 法线（中间空）
                      "f 1/1/1 3/1/1 4/2/1\n"; // 三者齐全

    const obj_mesh_t mesh = parse_obj(&arena, obj);
    EXPECT_EQ_U64(mesh.source_position_count, 4u);
    EXPECT_EQ_U64(mesh.source_texcoord_count, 2u);
    EXPECT_EQ_U64(mesh.source_normal_count, 1u);
    EXPECT_EQ_U64(mesh.source_face_count, 4u);
    EXPECT_EQ_U64(mesh.index_count, 12u); // 4 个三角形
    EXPECT_GT_U64(mesh.vertex_count, 0u);

    // 带 UV 的那两个面必须有非零 UV
    bool found_uv = false;
    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        if (mesh.vertices[i].texcoord[0] != 0.0f || mesh.vertices[i].texcoord[1] != 0.0f) {
            found_uv = true;
        }
    }
    EXPECT_TRUE(found_uv);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_supports_negative_indices(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    // -1 = 最后一个顶点，-3 = 倒数第三个
    const char *obj = "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 0 1 0\n"
                      "f -3 -2 -1\n";

    const obj_mesh_t mesh = parse_obj(&arena, obj);
    EXPECT_EQ_U64(mesh.index_count, 3u);
    EXPECT_EQ_U64(mesh.vertex_count, 3u);

    // 等价于 f 1 2 3
    EXPECT_VEC3_NEAR(vec3_make(mesh.vertices[mesh.indices[0]].position[0],
                               mesh.vertices[mesh.indices[0]].position[1],
                               mesh.vertices[mesh.indices[0]].position[2]),
                     vec3_make(0.0f, 0.0f, 0.0f), 1e-6f);
    EXPECT_VEC3_NEAR(vec3_make(mesh.vertices[mesh.indices[2]].position[0],
                               mesh.vertices[mesh.indices[2]].position[1],
                               mesh.vertices[mesh.indices[2]].position[2]),
                     vec3_make(0.0f, 1.0f, 0.0f), 1e-6f);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_triangulates_polygons(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    // 一个四边形面（扇形三角化后应产生 2 个三角形 = 6 个索引）
    const char *quad = "v 0 0 0\n"
                       "v 1 0 0\n"
                       "v 1 1 0\n"
                       "v 0 1 0\n"
                       "f 1 2 3 4\n";
    const obj_mesh_t quad_mesh = parse_obj(&arena, quad);
    EXPECT_EQ_U64(quad_mesh.index_count, 6u);

    arena_reset(&arena);

    // 五边形 -> 3 个三角形
    const char *pentagon = "v 0 0 0\n"
                           "v 1 0 0\n"
                           "v 2 1 0\n"
                           "v 1 2 0\n"
                           "v 0 1 0\n"
                           "f 1 2 3 4 5\n";
    const obj_mesh_t pentagon_mesh = parse_obj(&arena, pentagon);
    EXPECT_EQ_U64(pentagon_mesh.index_count, 9u);

    arena_reset(&arena);

    // 顶点数不足 3 的面要被跳过并计入 skipped。
    // 注意 "f 1 2 2" 有 3 个面角（虽然只有 2 个不同的顶点），
    // 解析器按面角数判断，所以它会被当成一个（退化的）三角形发出去 ——
    // 零面积三角形由 GPU 自己丢弃，解析器不需要特判。
    const char *degenerate = "v 0 0 0\n"
                             "v 1 0 0\n"
                             "f 1 2\n"
                             "f 1 2 2\n";
    const obj_mesh_t degenerate_mesh = parse_obj(&arena, degenerate);
    EXPECT_EQ_U64(degenerate_mesh.skipped_face_count, 1u); // 只有 "f 1 2" 被跳过
    EXPECT_EQ_U64(degenerate_mesh.index_count, 3u);        // "f 1 2 2" 产生 1 个三角形
    EXPECT_EQ_U64(degenerate_mesh.vertex_count, 2u);       // 去重后只有 2 个唯一顶点

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_deduplicates_vertices(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    // 两个三角形共享一条边：(v1,v2,v3) 与 (v1,v3,v4)。
    // 去重后应该是 4 个顶点 6 个索引，而不是 6 个顶点。
    const char *obj = "v 0 0 0\n"
                      "v 1 0 0\n"
                      "v 1 1 0\n"
                      "v 0 1 0\n"
                      "f 1 2 3\n"
                      "f 1 3 4\n";

    const obj_mesh_t mesh = parse_obj(&arena, obj);
    EXPECT_EQ_U64(mesh.vertex_count, 4u);
    EXPECT_EQ_U64(mesh.index_count, 6u);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_handles_malformed_input(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    // 空输入
    {
        obj_mesh_t mesh;
        EXPECT_FALSE(obj_load_from_memory(&arena, "", 0, "empty", &mesh));
        EXPECT_FALSE(obj_load_from_memory(&arena, NULL, 0, "null", &mesh));
    }

    // 只有注释和未知关键字
    {
        obj_mesh_t mesh;
        EXPECT_FALSE(obj_load_from_memory(&arena, "# 只有注释\nusemtl foo\n", 24, "comments", &mesh));
    }

    // 换行符缺失（最后一行没有 \n）
    {
        obj_mesh_t mesh;
        const char *no_newline = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3";
        EXPECT_TRUE(obj_load_from_memory(&arena, no_newline, strlen(no_newline), "no_newline",
                                         &mesh));
        EXPECT_EQ_U64(mesh.index_count, 3u);
    }

    // CRLF 换行
    {
        obj_mesh_t mesh;
        const char *crlf = "v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n";
        EXPECT_TRUE(obj_load_from_memory(&arena, crlf, strlen(crlf), "crlf", &mesh));
        EXPECT_EQ_U64(mesh.index_count, 3u);
    }

    // 科学计数法的坐标
    {
        obj_mesh_t mesh;
        const char *exponent = "v 1e-2 2.5e1 -0.5\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        EXPECT_TRUE(obj_load_from_memory(&arena, exponent, strlen(exponent), "exponent", &mesh));
        EXPECT_NEAR(mesh.vertices[0].position[0], 0.01f, 1e-6f);
        EXPECT_NEAR(mesh.vertices[0].position[1], 25.0f, 1e-4f);
        EXPECT_NEAR(mesh.vertices[0].position[2], -0.5f, 1e-6f);
    }

    // 越界索引：不能崩溃，越界的部分坐标保持 0
    {
        obj_mesh_t mesh;
        const char *out_of_range = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\n";
        EXPECT_TRUE(obj_load_from_memory(&arena, out_of_range, strlen(out_of_range), "oob", &mesh));
        EXPECT_EQ_U64(mesh.index_count, 3u);
    }

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void obj_to_mesh_desc_matches_layout(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, ENGINE_KB(64), "obj"));

    const char *obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const obj_mesh_t mesh = parse_obj(&arena, obj);

    const mesh_desc_t desc = obj_mesh_to_desc(&mesh);
    EXPECT_EQ_U64(desc.vertex_count, mesh.vertex_count);
    EXPECT_EQ_U64(desc.index_count, mesh.index_count);
    EXPECT_EQ_U64(desc.index_format, INDEX_FORMAT_U32);
    EXPECT_EQ_U64(desc.bindings[0].size, (usize)mesh.vertex_count * sizeof(mesh_vertex_t));
    EXPECT_EQ_U64(desc.index_data_size, (usize)mesh.index_count * sizeof(u32));
    EXPECT_EQ_U64(desc.layout.binding_count, 1u);
    EXPECT_EQ_U64(desc.layout.bindings[0].stride, MESH_VERTEX_STRIDE);

    const char *reason = NULL;
    EXPECT_TRUE(vertex_layout_validate(&desc.layout, &reason));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

void test_render_register(void)
{
    // 类型与预设
    test_register("render", "pixel_format_properties", pixel_format_properties);
    test_register("render", "vertex_format_properties", vertex_format_properties);
    test_register("render", "mesh_vertex_layout_is_consistent", mesh_vertex_layout_is_consistent);
    test_register("render", "vertex_layout_validate_rejects_bad_layouts",
                  vertex_layout_validate_rejects_bad_layouts);
    test_register("render", "blend_presets", blend_presets);
    test_register("render", "depth_presets", depth_presets);
    test_register("render", "raster_presets", raster_presets);
    test_register("render", "sampler_presets", sampler_presets);
    test_register("render", "pipeline_and_draw_defaults", pipeline_and_draw_defaults);
    test_register("render", "render_stats_accumulation", render_stats_accumulation);

    // uniform 块
    test_register("render", "std140_layout_matches_glsl", std140_layout_matches_glsl);
    test_register("render", "uniform_defaults_are_sane", uniform_defaults_are_sane);
    test_register("render", "object_uniforms_compute_normal_matrix",
                  object_uniforms_compute_normal_matrix);

    // 相机
    test_register("render", "camera_basis_vectors", camera_basis_vectors);
    test_register("render", "camera_view_matrix_places_world", camera_view_matrix_places_world);
    test_register("render", "camera_projection_respects_conventions",
                  camera_projection_respects_conventions);
    test_register("render", "camera_view_depth_and_containment", camera_view_depth_and_containment);
    test_register("render", "camera_look_at_orientation", camera_look_at_orientation);

    // 渲染队列
    test_register("render", "sort_key_layer_is_dominant", sort_key_layer_is_dominant);
    test_register("render", "sort_key_depth_direction_depends_on_layer",
                  sort_key_depth_direction_depends_on_layer);
    test_register("render", "queue_sorting_is_deterministic", queue_sorting_is_deterministic);
    test_register("render", "queue_sorts_across_layers_and_depths",
                  queue_sorts_across_layers_and_depths);
    test_register("render", "queue_reuses_memory_across_frames", queue_reuses_memory_across_frames);
    test_register("render", "queue_item_default_is_fully_initialized",
                  queue_item_default_is_fully_initialized);

    // 材质
    test_register("render", "material_texture_slots", material_texture_slots);

    // OBJ
    test_register("render", "obj_parses_minimal_triangle", obj_parses_minimal_triangle);
    test_register("render", "obj_supports_all_index_forms", obj_supports_all_index_forms);
    test_register("render", "obj_supports_negative_indices", obj_supports_negative_indices);
    test_register("render", "obj_triangulates_polygons", obj_triangulates_polygons);
    test_register("render", "obj_deduplicates_vertices", obj_deduplicates_vertices);
    test_register("render", "obj_handles_malformed_input", obj_handles_malformed_input);
    test_register("render", "obj_to_mesh_desc_matches_layout", obj_to_mesh_desc_matches_layout);
}
