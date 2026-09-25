// =============================================================================
//  tests/test_light.c —— 光源与 std140 布局
//
//  这一组测试有两个目的：
//
//  1. **方向约定**。light_t 里的 direction 是"光线传播方向"（指向被照物体），
//     而着色器需要的是"从表面指向光源"。转换发生在 light_to_gpu 里。
//     搞反的表现是"所有物体的光照都在背面"—— 显眼但很难想到是这里，
//     所以必须有测试钉住。
//
//  2. **std140 布局**。光源数组的每个元素在 std140 下必须按 16 字节对齐，
//     元素大小必须能被 16 整除。大小错了整个数组会错位，
//     症状是"第一盏灯正常、后面的灯参数全乱"，而且不报任何错。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include "engine/render/light.h"
#include "engine/render/uniforms.h"

// ---------------------------------------------------------------- 构造

static void light_directional_construction(void)
{
    // 传一个非单位向量，构造函数必须归一化
    const light_t l = light_directional(vec3_make(0.0f, -2.0f, 0.0f), vec3_one(), 3.0f);

    EXPECT_EQ_U64(l.type, LIGHT_TYPE_DIRECTIONAL);
    EXPECT_NEAR(vec3_length(l.direction), 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(l.direction.y, -1.0f, F32_TOLERANCE);
    EXPECT_NEAR(l.range, 0.0f, F32_TOLERANCE); // 平行光没有 range
    EXPECT_TRUE(l.cast_shadow);                // 平行光默认投影
    EXPECT_NEAR(l.intensity, 3.0f, F32_TOLERANCE);
}

static void light_point_construction(void)
{
    const light_t l = light_point(vec3_make(1.0f, 2.0f, 3.0f), vec3_one(), 5.0f, 8.0f);

    EXPECT_EQ_U64(l.type, LIGHT_TYPE_POINT);
    EXPECT_VEC3_NEAR(l.position, vec3_make(1.0f, 2.0f, 3.0f), F32_TOLERANCE);
    EXPECT_NEAR(l.range, 8.0f, F32_TOLERANCE);
    // 点光阴影需要 cubemap，M4 不做
    EXPECT_FALSE(l.cast_shadow);

    // range 传 0 应该给一个可用的默认值，而不是"照不到任何地方"
    const light_t fallback = light_point(vec3_zero(), vec3_one(), 1.0f, 0.0f);
    EXPECT_TRUE(fallback.range > 0.0f);
}

static void light_spot_construction(void)
{
    const light_t l = light_spot(vec3_make(0.0f, 5.0f, 0.0f), vec3_make(0.0f, -1.0f, 0.0f),
                                 vec3_one(), 4.0f, 12.0f, 20.0f, 35.0f);

    EXPECT_EQ_U64(l.type, LIGHT_TYPE_SPOT);
    EXPECT_NEAR(vec3_length(l.direction), 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(l.inner_angle, math_radians(20.0f), 1.0e-5f);
    EXPECT_NEAR(l.outer_angle, math_radians(35.0f), 1.0e-5f);
    EXPECT_TRUE(l.outer_angle > l.inner_angle);
}

// 外角必须严格大于内角，否则 cos(inner) < cos(outer)，
// 衰减公式里的分母会变成负数 —— 灯照出一圈"反向"的光。
// 构造函数要主动纠正，而不是让美术配错一个角度就把画面搞坏。
static void light_spot_corrects_inverted_angles(void)
{
    const light_t swapped = light_spot(vec3_zero(), vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f,
                                       10.0f, 50.0f, 20.0f); // 内外角写反了
    EXPECT_TRUE(swapped.outer_angle > swapped.inner_angle);
    EXPECT_TRUE(cosf(swapped.inner_angle) > cosf(swapped.outer_angle));

    const light_t equal = light_spot(vec3_zero(), vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f,
                                     10.0f, 30.0f, 30.0f); // 内外角相同
    EXPECT_TRUE(equal.outer_angle > equal.inner_angle);
}

// ---------------------------------------------------------------- GPU 转换

// 方向取反的守门测试。
static void light_to_gpu_negates_direction(void)
{
    // 光从上往下照：direction = (0, -1, 0)
    const light_t l = light_directional(vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f);
    const gpu_light_t g = light_to_gpu(&l);

    // 着色器要的"从表面指向光源"应该是 (0, +1, 0)
    EXPECT_NEAR(g.direction_range.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(g.direction_range.y, 1.0f, 1.0e-6f);
    EXPECT_NEAR(g.direction_range.z, 0.0f, 1.0e-6f);

    // 逐分量确认就是取反，而不是恰好某个轴对了
    EXPECT_NEAR(g.direction_range.x, -l.direction.x, 1.0e-6f);
    EXPECT_NEAR(g.direction_range.y, -l.direction.y, 1.0e-6f);
    EXPECT_NEAR(g.direction_range.z, -l.direction.z, 1.0e-6f);
}

static void light_to_gpu_packs_fields(void)
{
    light_t l = light_point(vec3_make(1.5f, 2.5f, -3.5f), vec3_make(0.2f, 0.4f, 0.6f), 7.0f, 9.0f);
    l.shadow_bias = 0.004f;

    const gpu_light_t g = light_to_gpu(&l);

    EXPECT_NEAR(g.position_type.x, 1.5f, 1.0e-6f);
    EXPECT_NEAR(g.position_type.y, 2.5f, 1.0e-6f);
    EXPECT_NEAR(g.position_type.z, -3.5f, 1.0e-6f);
    EXPECT_NEAR(g.position_type.w, (f32)LIGHT_TYPE_POINT, 1.0e-6f);

    EXPECT_NEAR(g.direction_range.w, 9.0f, 1.0e-6f); // range

    EXPECT_NEAR(g.color_intensity.x, 0.2f, 1.0e-6f);
    EXPECT_NEAR(g.color_intensity.y, 0.4f, 1.0e-6f);
    EXPECT_NEAR(g.color_intensity.z, 0.6f, 1.0e-6f);
    EXPECT_NEAR(g.color_intensity.w, 7.0f, 1.0e-6f); // intensity

    EXPECT_NEAR(g.spot_shadow.z, 0.0f, 1.0e-6f); // 点光不投影
    EXPECT_NEAR(g.spot_shadow.w, 0.004f, 1.0e-6f);
}

// 角度以 cos 值上传，省掉每个像素一次 acos。
static void light_to_gpu_stores_cosine_angles(void)
{
    const light_t l = light_spot(vec3_zero(), vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f, 10.0f,
                                 25.0f, 40.0f);
    const gpu_light_t g = light_to_gpu(&l);

    EXPECT_NEAR(g.spot_shadow.x, cosf(math_radians(25.0f)), 1.0e-5f);
    EXPECT_NEAR(g.spot_shadow.y, cosf(math_radians(40.0f)), 1.0e-5f);
    // cos 是递减的，所以内角的 cos 必须更大
    EXPECT_TRUE(g.spot_shadow.x > g.spot_shadow.y);
}

// ---------------------------------------------------------------- std140 布局

static void gpu_light_layout_is_std140_safe(void)
{
    // 数组元素跨度必须能被 16 整除，否则整个数组错位
    EXPECT_EQ_U64(sizeof(gpu_light_t) % 16, 0);
    EXPECT_EQ_U64(alignof(gpu_light_t), 16);

    // 字段偏移逐个钉住（和 uniforms.h 里的 _Static_assert 互为验证）
    EXPECT_EQ_U64(ENGINE_OFFSETOF(gpu_light_t, position_type), 0);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(gpu_light_t, direction_range), 16);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(gpu_light_t, color_intensity), 32);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(gpu_light_t, spot_shadow), 48);

    // 数组在结构体里的跨步必须正好是 sizeof
    EXPECT_EQ_U64(sizeof(gpu_light_t) * MAX_LIGHTS, 64u * MAX_LIGHTS);
}

static void frame_uniforms_layout_is_stable(void)
{
    EXPECT_EQ_U64(sizeof(frame_uniforms_t), 1024);
    EXPECT_EQ_U64(alignof(frame_uniforms_t), 16);

    // lights 数组必须 16 字节对齐（std140 对数组首地址的要求）
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, lights) % 16, 0);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, shadow_params) % 16, 0);
    // 数组必须是结构体的最后一块，末尾才不会有多余填充
    EXPECT_EQ_U64(ENGINE_OFFSETOF(frame_uniforms_t, lights) + sizeof(gpu_light_t) * MAX_LIGHTS,
                  sizeof(frame_uniforms_t));
}

static void object_uniforms_layout_is_stable(void)
{
    EXPECT_EQ_U64(sizeof(object_uniforms_t), 176);
    EXPECT_EQ_U64(alignof(object_uniforms_t), 16);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, pbr_params), 144);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(object_uniforms_t, emissive), 160);
}

// ---------------------------------------------------------------- 光源集合

static void light_set_add_and_get(void)
{
    light_set_t set;
    light_set_clear(&set);
    EXPECT_EQ_U64(light_set_count(&set), 0);
    EXPECT_EQ_U64(set.shadow_caster, MAX_LIGHTS); // 没有投影灯
    EXPECT_NULL(light_set_get(&set, 0));

    const light_t a = light_directional(vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f);
    const light_t b = light_point(vec3_zero(), vec3_one(), 1.0f, 5.0f);
    EXPECT_TRUE(light_set_add(&set, &a));
    EXPECT_TRUE(light_set_add(&set, &b));

    EXPECT_EQ_U64(light_set_count(&set), 2);
    EXPECT_EQ_U64(light_set_get(&set, 0)->type, LIGHT_TYPE_DIRECTIONAL);
    EXPECT_EQ_U64(light_set_get(&set, 1)->type, LIGHT_TYPE_POINT);
    EXPECT_NULL(light_set_get(&set, 2)); // 越界返回 NULL 而不是野指针

    // 平行光默认投影，所以它应该是阴影投射灯
    EXPECT_EQ_U64(set.shadow_caster, 0);
}

static void light_set_respects_capacity(void)
{
    light_set_t set;
    light_set_clear(&set);

    const light_t l = light_point(vec3_zero(), vec3_one(), 1.0f, 5.0f);
    for (u32 i = 0; i < MAX_LIGHTS; ++i) {
        EXPECT_TRUE(light_set_add(&set, &l));
    }
    EXPECT_EQ_U64(light_set_count(&set), MAX_LIGHTS);

    // 超出的必须被拒绝，而不是静默覆盖第 0 盏
    EXPECT_FALSE(light_set_add(&set, &l));
    EXPECT_EQ_U64(light_set_count(&set), MAX_LIGHTS);
    EXPECT_EQ_U64(light_set_get(&set, 0)->type, LIGHT_TYPE_POINT);
}

static void light_set_shadow_caster_is_first_caster(void)
{
    light_set_t set;
    light_set_clear(&set);

    // 先加一盏不投影的点光，再加两盏投影的平行光
    light_t point = light_point(vec3_zero(), vec3_one(), 1.0f, 5.0f);
    point.cast_shadow = false;
    light_set_add(&set, &point);

    light_t sun_a = light_directional(vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 1.0f);
    sun_a.cast_shadow = true;
    light_set_add(&set, &sun_a);

    light_t sun_b = light_directional(vec3_make(0.0f, 0.0f, -1.0f), vec3_one(), 1.0f);
    sun_b.cast_shadow = true;
    light_set_add(&set, &sun_b);

    // 只记第一盏 —— M4 只有一张阴影贴图，记第二盏会让阴影贴图指向错的灯
    EXPECT_EQ_U64(set.shadow_caster, 1);
}

static void light_set_to_gpu_zeroes_unused_slots(void)
{
    light_set_t set;
    light_set_clear(&set);
    const light_t l = light_directional(vec3_make(0.0f, -1.0f, 0.0f), vec3_one(), 2.0f);
    light_set_add(&set, &l);

    gpu_light_t gpu[MAX_LIGHTS];

    // 先把整块内存填成垃圾，验证实现真的清了没用的槽位。
    // 不清的话，着色器万一读到了范围外的槽位就会拿到上一帧的残留数据 ——
    // 表现为"偶尔闪一下"，极难复现。
    for (u32 i = 0; i < MAX_LIGHTS; ++i) {
        memset(&gpu[i], 0xCD, sizeof(gpu_light_t));
    }

    light_set_to_gpu(&set, gpu);

    // 第 0 个是有效光源
    EXPECT_NEAR(gpu[0].color_intensity.w, 2.0f, 1.0e-6f);

    // 其余必须是全零
    for (u32 i = 1; i < MAX_LIGHTS; ++i) {
        const u8 *bytes = (const u8 *)&gpu[i];
        u32       nonzero = 0;
        for (usize b = 0; b < sizeof(gpu_light_t); ++b) {
            if (bytes[b] != 0) {
                nonzero++;
            }
        }
        EXPECT_EQ_U64(nonzero, 0);
    }
}

static void light_set_default_scene_is_usable(void)
{
    const light_set_t set = light_set_default_scene();

    EXPECT_GE_U64(set.count, 3);
    EXPECT_TRUE(set.count <= MAX_LIGHTS);

    // 必须有一盏能投影的灯，否则示例里"阴影不生效"会变成常态
    EXPECT_TRUE(set.shadow_caster < MAX_LIGHTS);

    // 至少一盏平行光（天空盒和雾都假设有主方向光）
    u32 directional = 0;
    for (u32 i = 0; i < set.count; ++i) {
        if (set.lights[i].type == LIGHT_TYPE_DIRECTIONAL) {
            directional++;
        }
    }
    EXPECT_GE_U64(directional, 1);

    // 所有方向都要归一化 —— 没归一化的话点积会超出 [-1,1]
    for (u32 i = 0; i < set.count; ++i) {
        if (set.lights[i].type != LIGHT_TYPE_POINT) {
            EXPECT_NEAR(vec3_length(set.lights[i].direction), 1.0f, 1.0e-4f);
        }
    }
}

void test_light_register(void)
{
    test_register("light", "directional_construction", light_directional_construction);
    test_register("light", "point_construction", light_point_construction);
    test_register("light", "spot_construction", light_spot_construction);
    test_register("light", "spot_angles_corrected", light_spot_corrects_inverted_angles);

    test_register("light", "gpu_negates_direction", light_to_gpu_negates_direction);
    test_register("light", "gpu_packs_fields", light_to_gpu_packs_fields);
    test_register("light", "gpu_cosine_angles", light_to_gpu_stores_cosine_angles);

    test_register("light", "gpu_light_std140", gpu_light_layout_is_std140_safe);
    test_register("light", "frame_uniforms_layout", frame_uniforms_layout_is_stable);
    test_register("light", "object_uniforms_layout", object_uniforms_layout_is_stable);

    test_register("light", "set_add_get", light_set_add_and_get);
    test_register("light", "set_capacity", light_set_respects_capacity);
    test_register("light", "set_shadow_caster", light_set_shadow_caster_is_first_caster);
    test_register("light", "set_to_gpu_zeroes", light_set_to_gpu_zeroes_unused_slots);
    test_register("light", "default_scene", light_set_default_scene_is_usable);
}
