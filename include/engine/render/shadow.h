// =============================================================================
//  engine/render/shadow.h —— 阴影贴图
//
//  ============================================================================
//   原理（一张图说清）
//  ============================================================================
//
//  1. 从**光源视角**渲染一遍场景，只写深度（不写颜色）→ 深度贴图。
//  2. 正式渲染时，把世界坐标用同一个光源视图投影矩阵变换到光源空间，
//     比较"该点的深度"与"贴图里记录的深度"：
//         该点更远  → 被挡住了，在阴影里
//         该点更近  → 可见
//
//  ============================================================================
//   三个必须处理的坑
//  ============================================================================
//
//  1. **阴影痤疮（shadow acne）**：深度贴图的分辨率有限，一个纹素覆盖
//     世界空间的一小块区域。斜面与光线夹角大时，"该点深度"与"贴图深度"
//     会因为插值误差而互相穿插，表现为表面出现条纹状的自阴影。
//     解决：渲染深度图时用 glPolygonOffset（斜率 + 常数偏移），
//           采样时再加一个与法线夹角相关的小偏移。
//
//  2. **彼得潘现象（peter-panning）**：偏移太大会让阴影与物体分离，
//     看起来像浮在空中。所以偏移量必须**够用就好**，不能图省事给大值。
//
//  3. **视锥覆盖**：平行光的正交视锥只覆盖一个范围，范围越大分辨率越低。
//     这里用"包住场景包围球"的紧凑视锥。级联阴影（CSM）能解决大场景，
//     但复杂度高得多 —— 等真的出现"远处阴影糊成一片"时再做。
// =============================================================================
#ifndef ENGINE_RENDER_SHADOW_H
#define ENGINE_RENDER_SHADOW_H

#include "engine/core/mat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/light.h"
#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

#define SHADOW_DEFAULT_SIZE 1024u

typedef struct shadow_map_t {
    render_target_handle_t target;

    u32 size;
    f32 strength;    // 阴影强度（0 = 关闭）
    f32 depth_bias;  // glPolygonOffset 的常数项
    f32 slope_bias;  // glPolygonOffset 的斜率项
    f32 normal_bias; // 着色器里沿法线的偏移量（世界单位）

    mat4_t light_view_projection;

    // 深度专用管线（只写深度，不写颜色）
    pipeline_handle_t depth_pipeline;
    shader_handle_t   depth_shader;

    bool enabled;
    u64  creation_frame;
} shadow_map_t;

// 初始化。size 建议 1024 或 2048（512 以下会明显糊）。
bool shadow_map_init(shadow_map_t *shadow, renderer_t *renderer, u32 size);
void shadow_map_shutdown(shadow_map_t *shadow, renderer_t *renderer);

// 用一盏平行光 + 场景包围球，算出紧凑的光源视图投影矩阵。
//
// scene_center / scene_radius 是整个场景的包围球。
// 用一个球而不是 AABB：球对光源方向不敏感，光源转动时阴影分辨率不会跳变
// （用 AABB 的话，光源每转一点，正交视锥就要重新贴合，阴影边缘会抖）。
void shadow_map_fit_directional(shadow_map_t *shadow, const light_t *light, vec3_t scene_center,
                                f32 scene_radius);

// 执行深度 pass。
// **必须在所有 renderer_push_* 之后、renderer_end_frame 之前调用。**
void shadow_map_render(shadow_map_t *shadow, renderer_t *renderer);

// 把阴影贴图与参数交给渲染器（end_frame 时会自动绑定到 SHADOW 纹理槽）。
void shadow_map_apply(shadow_map_t *shadow, renderer_t *renderer);

texture_handle_t shadow_map_texture(const shadow_map_t *shadow, renderer_t *renderer);

void shadow_map_set_strength(shadow_map_t *shadow, f32 strength);
void shadow_map_set_bias(shadow_map_t *shadow, f32 depth_bias, f32 slope_bias, f32 normal_bias);

#endif // ENGINE_RENDER_SHADOW_H
