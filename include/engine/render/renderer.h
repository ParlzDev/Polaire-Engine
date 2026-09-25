// =============================================================================
//  engine/render/renderer.h —— 渲染器（引擎级外观）
//
//  ============================================================================
//   renderer 与 backend 的分工
//  ============================================================================
//
//    render_backend_t   只管"怎么和某个图形 API 说话"。
//                       它不知道什么叫材质、相机、渲染队列。
//                       换后端 = 换这一层的实现，其它一行不改。
//
//    renderer_t         管"引擎怎么组织一帧"：
//                         * 资源句柄的发放与代际校验
//                         * 帧 arena（每帧临时数据）
//                         * 相机与 frame uniform
//                         * 渲染队列（提交 → 排序 → 提交给后端）
//                         * 渲染统计
//
//  业务代码只跟 renderer 打交道。它永远不会看到 render_backend_t，
//  更不会看到 GL 函数指针。
//
//  ============================================================================
//   一帧的标准流程
//  ============================================================================
//
//      renderer_begin_frame(r, &camera, &frame_desc);
//          renderer_push_item(r, &item);      // 想提交多少就提交多少
//          renderer_push_item(r, &item);
//      renderer_end_frame(r);                 // 排序 → 提交 → 交换缓冲
//
//  ============================================================================
//   资源句柄的两层结构
//  ============================================================================
//
//      用户拿到的：  shader_handle_t  ← 由 renderer 的池发放
//      池槽里存的：  backend 的内部句柄
//
//  为什么要两层：
//    * 代际校验、资源命名、创建帧号、热重载钩子全部集中在 renderer 一层；
//    * 后端只需要维护"内部句柄 → API 对象"的映射，职责单一；
//    * M7 做热重载时，只要遍历 renderer 的池就能找到"所有 shader"，
//      不需要后端提供遍历能力。
//  代价是每次绑定多一次池查找（几次内存访问），可以忽略。
// =============================================================================
#ifndef ENGINE_RENDER_RENDERER_H
#define ENGINE_RENDER_RENDERER_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/render/camera.h"
#include "engine/render/frustum.h"
#include "engine/render/light.h"
#include "engine/render/material.h"
#include "engine/render/mesh.h"
#include "engine/render/render_backend.h"
#include "engine/render/render_queue.h"
#include "engine/render/render_target.h"
#include "engine/render/render_types.h"
#include "engine/render/shader.h"
#include "engine/render/texture.h"
#include "engine/render/uniforms.h"

// renderer 是不透明类型：内部结构（资源池、帧 arena、渲染队列）属于实现细节，
// 暴露出去只会诱导外部代码直接改它的字段。访问一律通过下面的函数。
typedef struct renderer_t renderer_t;

// 各类资源的池容量定义在 render_types.h（后端也要用），这里只是别名。
#define RENDERER_MAX_SHADERS RENDER_MAX_SHADERS
#define RENDERER_MAX_TEXTURES RENDER_MAX_TEXTURES
#define RENDERER_MAX_MESHES RENDER_MAX_MESHES
#define RENDERER_MAX_PIPELINES RENDER_MAX_PIPELINES

// 创建 / 销毁。backend 的生命周期由调用方管理（renderer 只是引用它）。
renderer_t *renderer_create(allocator_t *alloc, render_backend_t *backend);
void        renderer_destroy(renderer_t *renderer);

render_backend_t       *renderer_backend(renderer_t *renderer);
const render_backend_t *renderer_backend_const(const renderer_t *renderer);

void renderer_get_caps(const renderer_t *renderer, render_backend_caps_t *out_caps);

// ---------------------------------------------------------------- 资源
shader_handle_t   renderer_create_shader(renderer_t *renderer, const shader_desc_t *desc);
texture_handle_t  renderer_create_texture(renderer_t *renderer, const texture_desc_t *desc,
                                          const void *pixels);
mesh_handle_t     renderer_create_mesh(renderer_t *renderer, const mesh_desc_t *desc);
pipeline_handle_t renderer_create_pipeline(renderer_t *renderer, const pipeline_desc_t *desc);

// 更新动态网格的顶点/索引数据。传 NULL 表示这一部分不更新。
// 数据大小不能超过创建时的容量。
bool renderer_update_mesh(renderer_t *renderer, mesh_handle_t mesh, const void *vertex_data,
                          usize vertex_data_size, const void *index_data, usize index_data_size);

void renderer_destroy_shader(renderer_t *renderer, shader_handle_t shader);
void renderer_destroy_texture(renderer_t *renderer, texture_handle_t texture);
void renderer_destroy_mesh(renderer_t *renderer, mesh_handle_t mesh);
void renderer_destroy_pipeline(renderer_t *renderer, pipeline_handle_t pipeline);

// ---------------------------------------------------------------- 渲染目标
//
// 渲染目标的创建/销毁/查询实现放在 render_target.c（见 render_target.h），
// 这里只声明渲染器需要暴露给它的内部入口。
render_target_handle_t renderer_create_render_target_internal(renderer_t *renderer,
                                                              const render_target_desc_t *desc);
void renderer_destroy_render_target_internal(renderer_t *renderer, render_target_handle_t target);
bool renderer_resize_render_target_internal(renderer_t *renderer, render_target_handle_t target,
                                            u32 width, u32 height);

// 资源元信息查询（调试面板、热重载、资源浏览器用）
bool        renderer_shader_is_valid(const renderer_t *renderer, shader_handle_t shader);
bool        renderer_texture_is_valid(const renderer_t *renderer, texture_handle_t texture);
bool        renderer_mesh_is_valid(const renderer_t *renderer, mesh_handle_t mesh);
bool        renderer_pipeline_is_valid(const renderer_t *renderer, pipeline_handle_t pipeline);

const char *renderer_shader_name(const renderer_t *renderer, shader_handle_t shader);
const char *renderer_texture_name(const renderer_t *renderer, texture_handle_t texture);
const char *renderer_mesh_name(const renderer_t *renderer, mesh_handle_t mesh);
const char *renderer_pipeline_name(const renderer_t *renderer, pipeline_handle_t pipeline);

u32 renderer_texture_width(const renderer_t *renderer, texture_handle_t texture);
u32 renderer_texture_height(const renderer_t *renderer, texture_handle_t texture);

u32  renderer_mesh_vertex_count(const renderer_t *renderer, mesh_handle_t mesh);
u32  renderer_mesh_index_count(const renderer_t *renderer, mesh_handle_t mesh);
bool renderer_mesh_bounds(const renderer_t *renderer, mesh_handle_t mesh, vec3_t *out_min,
                          vec3_t *out_max);

// 当前存活的各类资源数量（用于泄漏检查）
u32 renderer_shader_count(const renderer_t *renderer);
u32 renderer_texture_count(const renderer_t *renderer);
u32 renderer_mesh_count(const renderer_t *renderer);
u32 renderer_pipeline_count(const renderer_t *renderer);

// 帧序号，资源创建时会记录，便于"这个纹理是哪一帧加载的"
u64 renderer_frame_index(const renderer_t *renderer);

// ---------------------------------------------------------------- 帧
//
// begin_frame 会：
//   * 清空渲染队列
//   * 记录相机
//   * 按相机算出 frame_uniforms 并上传
//   * 重置本帧统计
void renderer_begin_frame(renderer_t *renderer, const camera_t *camera,
                          const frame_desc_t *frame_desc);

// 提交一个渲染项（转发到渲染队列）
u32 renderer_push_item(renderer_t *renderer, const render_item_t *item);

// 便捷提交：给一个材质、网格、模型矩阵与图层，
// 自动算出视图深度、法线矩阵、纹理绑定。
u32 renderer_push_mesh(renderer_t *renderer, const material_t *material, mesh_handle_t mesh,
                       mat4_t model_matrix, render_layer_t layer);

// 结束一帧：排序 → 提交到后端 → 交换缓冲
void renderer_end_frame(renderer_t *renderer);

// 相机
const camera_t *renderer_camera(const renderer_t *renderer);
void            renderer_set_camera(renderer_t *renderer, const camera_t *camera);

// 本帧的清屏设置。默认清颜色（深蓝灰）与深度（1.0）。
void renderer_set_clear(renderer_t *renderer, bool clear_color, vec4_t color, bool clear_depth,
                        f32 depth_value);

// 光照（M4：最多 MAX_LIGHTS 盏灯同时参与光照）
void renderer_set_lights(renderer_t *renderer, const light_set_t *lights);
const light_set_t *renderer_lights(const renderer_t *renderer);

// 旧的单光源接口（M2/M3 的着色器用，内部会转成一盏平行光）
void renderer_set_ambient(renderer_t *renderer, vec3_t color, f32 intensity);
void renderer_set_directional_light(renderer_t *renderer, vec3_t direction, vec3_t color,
                                    f32 intensity);

// 雾（指数平方衰减）
void renderer_set_fog(renderer_t *renderer, bool enabled, vec3_t color, f32 start_distance,
                      f32 end_distance, f32 density);

// 程序化天空盒（渐变，不需要 cubemap 资源）
void renderer_set_skybox(renderer_t *renderer, bool enabled, vec3_t horizon, vec3_t zenith,
                         vec3_t ground);

// 色调映射与曝光
typedef enum tonemap_mode_t {
    TONEMAP_NONE = 0, // 直接截断（调试用，会过曝）
    TONEMAP_REINHARD, // 便宜、不过曝，但高光发灰
    TONEMAP_ACES      // 保留高光对比度（默认）
} tonemap_mode_t;

void renderer_set_tonemap(renderer_t *renderer, tonemap_mode_t mode, f32 exposure);

// 阴影参数：strength = 阴影强度（0 = 关闭），shadow_map_size = 贴图边长（texel）
void renderer_set_shadow_params(renderer_t *renderer, f32 strength, u32 shadow_map_size);

// 光源视角的视图投影矩阵（由 shadow 模块算出后写入）
void renderer_set_light_view_projection(renderer_t *renderer, mat4_t light_view_projection);

// 多重采样数。改变后场景目标会在下一帧重建。
void renderer_set_msaa_samples(renderer_t *renderer, u32 samples);
u32  renderer_msaa_samples(const renderer_t *renderer);

// 把当前的帧 uniform 立刻上传（深度 pass 前需要）
void renderer_flush_frame_uniforms(renderer_t *renderer);

// 把当前渲染队列里的几何用指定管线重画到 target（阴影贴图用）。
// **必须在所有 renderer_push_* 之后、renderer_end_frame 之前调用。**
void renderer_render_depth_pass(renderer_t *renderer, render_target_handle_t target,
                                pipeline_handle_t depth_pipeline);

// 阴影贴图纹理与法线偏移
void renderer_set_shadow_texture(renderer_t *renderer, texture_handle_t shadow_texture);
void renderer_set_shadow_normal_bias(renderer_t *renderer, f32 normal_bias);
f32  renderer_shadow_normal_bias(const renderer_t *renderer);

// 取渲染目标槽位（供 render_target.c 使用）
const struct render_target_slot_t *renderer_render_target_slot(const renderer_t *renderer,
                                                               render_target_handle_t target);
texture_handle_t renderer_render_target_texture_internal(renderer_t *renderer,
                                                         render_target_handle_t target);

// 视锥剔除。开启后 renderer_push_mesh 会跳过视锥外的物体。
void renderer_set_culling_enabled(renderer_t *renderer, bool enabled);
bool renderer_culling_enabled(const renderer_t *renderer);

// 上一帧被剔除的物体数（用于验证剔除真的生效了 ——
// "画得更快"这件事如果没有数字支撑，很容易是错觉）
u32 renderer_culled_count(const renderer_t *renderer);

// ---------------------------------------------------------------- 统计
void renderer_get_stats(const renderer_t *renderer, render_stats_t *out_stats);
// 本帧的队列统计（提交项数、峰值项数）
u32 renderer_queue_count(const renderer_t *renderer);
u32 renderer_queue_peak_count(const renderer_t *renderer);
const render_queue_t *renderer_queue(const renderer_t *renderer);

// 每帧的临时数据分配区。渲染队列、剔除结果、排序临时数据都从这里出，
// reset 是 O(块数) 且不产生系统分配 —— "帧内零 malloc"的落点。
arena_t *renderer_frame_arena(renderer_t *renderer);

#endif // ENGINE_RENDER_RENDERER_H
