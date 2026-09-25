// =============================================================================
//  engine/render/light.h —— 光源
//
//  ---- std140 下的光源数组（最容易错的地方） ----
//
//  把多盏灯放进 uniform 数组时，std140 规定**数组的每个元素都必须按 16 字节
//  对齐**，而且元素的跨度会被向上取整到 16 的倍数。
//
//  所以 C 侧的 gpu_light_t 大小必须是 16 的倍数（这里是 64），
//  否则整个数组会错位 —— 症状是"第一盏灯正常，后面的灯参数全乱"。
//  这种错误不会崩溃，只是光照看起来莫名其妙，极难定位。
//
//  这也是为什么这里不直接用 `light_t`（C 侧逻辑表示），
//  而是分成两个结构体：一个给人用，一个给 GPU 用。
//  `gpu_light_t` 的字段全是 vec4，天然满足 std140。
// =============================================================================
#ifndef ENGINE_RENDER_LIGHT_H
#define ENGINE_RENDER_LIGHT_H

#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/uniforms.h"

typedef enum light_type_t {
    LIGHT_TYPE_DIRECTIONAL = 0, // 平行光（太阳）
    LIGHT_TYPE_POINT,           // 点光
    LIGHT_TYPE_SPOT,            // 聚光
    LIGHT_TYPE_COUNT
} light_type_t;

// C 侧的逻辑表示：给人读、给游戏逻辑用
typedef struct light_t {
    light_type_t type;
    vec3_t       position;    // 点光/聚光的位置（平行光忽略）
    vec3_t       direction;   // 平行光/聚光的方向（**指向被照物体**）
    vec3_t       color;
    f32          intensity;

    f32 range;       // 点光/聚光的有效半径，超出后衰减到 0
    f32 inner_angle; // 聚光内角（弧度，完全照亮）
    f32 outer_angle; // 聚光外角（弧度，完全变暗）

    bool cast_shadow;
    f32  shadow_bias; // 深度偏移，防阴影痤疮
} light_t;

light_t light_directional(vec3_t direction, vec3_t color, f32 intensity);
light_t light_point(vec3_t position, vec3_t color, f32 intensity, f32 range);
light_t light_spot(vec3_t position, vec3_t direction, vec3_t color, f32 intensity, f32 range,
                   f32 inner_degrees, f32 outer_degrees);

// ---------------------------------------------------------------- GPU 表示
//
// gpu_light_t 定义在 uniforms.h 里（它属于 std140 布局的一部分，
// 和 frame_uniforms_t 放在一起才能保证两边不会漂移）。
gpu_light_t light_to_gpu(const light_t *light);

// ---------------------------------------------------------------- 光源集合
//
// 定容数组而不是动态数组：每帧都要整块上传到 UBO，
// 动态分配在这里没有意义（而且会破坏"帧内零 malloc"）。
typedef struct light_set_t {
    light_t lights[MAX_LIGHTS];
    u32     count;
    u32     shadow_caster; // 投射阴影的那盏灯的索引；MAX_LIGHTS = 无
} light_set_t;

void light_set_clear(light_set_t *set);
bool light_set_add(light_set_t *set, const light_t *light);
const light_t *light_set_get(const light_set_t *set, u32 index);
u32  light_set_count(const light_set_t *set);

// 把集合写进 GPU 表示。out_lights 至少要能装 MAX_LIGHTS 个元素。
void light_set_to_gpu(const light_set_t *set, gpu_light_t *out_lights);

// 简单场景默认光照：一盏方向光 + 一盏补光 + 一点环境光。
// 有它才能保证"忘了配光照"时画面不是全黑 —— 全黑最难排查。
light_set_t light_set_default_scene(void);

#endif // ENGINE_RENDER_LIGHT_H
