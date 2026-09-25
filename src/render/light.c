// =============================================================================
//  src/render/light.c —— 光源
// =============================================================================
#include "engine/render/light.h"

#include <string.h>

#include "engine/core/assert.h"

light_t light_directional(vec3_t direction, vec3_t color, f32 intensity)
{
    light_t light;
    memset(&light, 0, sizeof(light));
    light.type = LIGHT_TYPE_DIRECTIONAL;
    // 方向存成"指向被照物体"（光线的传播方向），与直觉一致；
    // 着色器里要的是"从表面指向光源"，转换在 light_to_gpu 里做。
    light.direction = vec3_normalize(direction);
    light.color = color;
    light.intensity = intensity;
    light.range = 0.0f;
    light.outer_angle = F32_PI;
    light.cast_shadow = true;
    light.shadow_bias = 0.0015f;
    return light;
}

light_t light_point(vec3_t position, vec3_t color, f32 intensity, f32 range)
{
    light_t light;
    memset(&light, 0, sizeof(light));
    light.type = LIGHT_TYPE_POINT;
    light.position = position;
    light.direction = vec3_make(0.0f, -1.0f, 0.0f);
    light.color = color;
    light.intensity = intensity;
    light.range = (range > 0.0f) ? range : 10.0f;
    light.outer_angle = F32_PI;
    light.cast_shadow = false; // 点光阴影需要 cubemap，M4 不做
    light.shadow_bias = 0.0025f;
    return light;
}

light_t light_spot(vec3_t position, vec3_t direction, vec3_t color, f32 intensity, f32 range,
                   f32 inner_degrees, f32 outer_degrees)
{
    light_t light;
    memset(&light, 0, sizeof(light));
    light.type = LIGHT_TYPE_SPOT;
    light.position = position;
    light.direction = vec3_normalize(direction);
    light.color = color;
    light.intensity = intensity;
    light.range = (range > 0.0f) ? range : 15.0f;

    // 内角必须小于外角，否则 cos(inner) < cos(outer)，衰减公式会算出负数。
    // 这里主动纠正而不是断言 —— 美术配错了角度不该让游戏崩。
    f32 inner = math_radians(inner_degrees);
    f32 outer = math_radians(outer_degrees);
    if (outer <= inner) {
        outer = inner + math_radians(2.0f);
    }
    light.inner_angle = inner;
    light.outer_angle = outer;

    light.cast_shadow = false;
    light.shadow_bias = 0.0025f;
    return light;
}

gpu_light_t light_to_gpu(const light_t *light)
{
    ASSERT(light != NULL);

    gpu_light_t gpu;
    memset(&gpu, 0, sizeof(gpu));

    gpu.position_type = vec4_make(light->position.x, light->position.y, light->position.z,
                                  (f32)light->type);

    // 着色器里需要的是"从表面指向光源"的方向，而 light_t 存的是
    // "光线传播方向"（指向被照物体）。这里取反 ——
    // 搞反的表现是"所有物体的光照都在背面"，一眼能看出，但很难想到是这里。
    const vec3_t to_light = vec3_negate(light->direction);
    gpu.direction_range = vec4_make(to_light.x, to_light.y, to_light.z, light->range);

    gpu.color_intensity =
        vec4_make(light->color.x, light->color.y, light->color.z, light->intensity);

    // 角度存 cos 值：着色器里可以直接用 dot 比较，省掉每个像素一次 acos。
    gpu.spot_shadow = vec4_make(cosf(light->inner_angle), cosf(light->outer_angle),
                                light->cast_shadow ? 1.0f : 0.0f, light->shadow_bias);

    return gpu;
}

// =============================================================================
//  光源集合
// =============================================================================
void light_set_clear(light_set_t *set)
{
    ASSERT(set != NULL);
    set->count = 0;
    set->shadow_caster = MAX_LIGHTS;
}

bool light_set_add(light_set_t *set, const light_t *light)
{
    ASSERT(set != NULL);

    if (light == NULL || set->count >= MAX_LIGHTS) {
        return false;
    }

    set->lights[set->count] = *light;

    // 只记录**第一盏**投射阴影的灯。
    // M4 只做单张阴影贴图（一个光源视角）。多光源阴影需要多张贴图 +
    // 多个采样器，那是 M6 之后的事。
    if (light->cast_shadow && set->shadow_caster >= MAX_LIGHTS) {
        set->shadow_caster = set->count;
    }

    set->count++;
    return true;
}

const light_t *light_set_get(const light_set_t *set, u32 index)
{
    ASSERT(set != NULL);
    return (index < set->count) ? &set->lights[index] : NULL;
}

u32 light_set_count(const light_set_t *set)
{
    ASSERT(set != NULL);
    return set->count;
}

void light_set_to_gpu(const light_set_t *set, gpu_light_t *out_lights)
{
    ASSERT(set != NULL);
    ASSERT(out_lights != NULL);

    for (u32 i = 0; i < set->count; ++i) {
        out_lights[i] = light_to_gpu(&set->lights[i]);
    }
    // 多余的槽位清零：不清的话着色器如果因为某种原因读到了
    // u_lights[u_light_params.x] 之外的槽位，会读到上一帧的残留数据。
    for (u32 i = set->count; i < MAX_LIGHTS; ++i) {
        memset(&out_lights[i], 0, sizeof(gpu_light_t));
    }
}

light_set_t light_set_default_scene(void)
{
    light_set_t set;
    light_set_clear(&set);

    // 主光：斜向下的暖色太阳，投射阴影
    light_t sun = light_directional(vec3_normalize(vec3_make(-0.45f, -0.80f, -0.40f)),
                                    vec3_make(1.00f, 0.95f, 0.86f), 3.2f);
    sun.cast_shadow = true;
    light_set_add(&set, &sun);

    // 补光：从反方向的冷色弱光，避免背光面死黑
    light_t fill = light_directional(vec3_normalize(vec3_make(0.55f, -0.25f, 0.60f)),
                                     vec3_make(0.55f, 0.68f, 0.90f), 0.9f);
    fill.cast_shadow = false;
    light_set_add(&set, &fill);

    // 点光：给场景一点局部暖色，展示点光与距离衰减
    light_t lamp = light_point(vec3_make(3.5f, 1.6f, 2.0f), vec3_make(1.0f, 0.72f, 0.42f), 12.0f,
                               9.0f);
    light_set_add(&set, &lamp);

    return set;
}
