// =============================================================================
//  engine/render/render_queue.h —— 渲染队列与排序
//
//  ---- 为什么需要"队列"而不是直接画 ----
//
//  直接画的顺序 = 提交顺序，这带来两个问题：
//   1. 同一管线/同一纹理的物体如果被其它物体隔开，会产生多余的管线切换
//      与纹理绑定。队列排序把它们聚在一起。
//   2. 半透明物体必须从远到近画才正确，而提交顺序往往与距离无关。
//
//  ---- 排序键的设计 ----
//
//  一个 u64：高 32 位是 layer，低 32 位是量化后的深度。
//
//      63          32 31                    0
//      +--------------+----------------------+
//      |    layer     |    quantized depth   |
//      +--------------+----------------------+
//
//  * layer 决定"大类"的顺序（背景 → 3D 不透明 → 3D 半透明 → 2D → UI）。
//  * 深度在 layer 内部决定顺序，方向按 layer 的性质决定：
//      不透明层：近 → 远（early-z 能提前丢弃被遮挡的像素）
//      半透明层：远 → 近（混合才有正确结果）
//
//  ---- 排序的确定性 ----
//
//  `qsort` **不是稳定排序**。同 layer 同深度的两个物体，不同次运行可能排出
//  不同顺序，表现为"透明物体偶发闪烁"。
//
//  解决办法不是去实现一个稳定排序，而是让比较函数构成**全序**：
//  深度相同时再比 sequence（插入序号，唯一）。全序下任何正确的排序算法
//  都会给出唯一结果，稳定性问题自然消失。
// =============================================================================
#ifndef ENGINE_RENDER_RENDER_QUEUE_H
#define ENGINE_RENDER_RENDER_QUEUE_H

#include "engine/core/array.h"
#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/render/camera.h"
#include "engine/render/render_types.h"
#include "engine/render/uniforms.h"

typedef enum render_layer_t {
    RENDER_LAYER_BACKGROUND = 0,    // 天空盒、背景图
    RENDER_LAYER_OPAQUE_3D,         // 3D 不透明
    RENDER_LAYER_TRANSPARENT_3D,    // 3D 半透明
    RENDER_LAYER_OPAQUE_2D,         // 2D 不透明精灵
    RENDER_LAYER_TRANSPARENT_2D,    // 2D 半透明精灵
    RENDER_LAYER_UI,                // UI 覆盖层
    RENDER_LAYER_COUNT
} render_layer_t;

// 该层是否需要"从远到近"绘制。
bool render_layer_is_transparent(render_layer_t layer);

const char *render_layer_name(render_layer_t layer);

typedef struct render_item_t {
    pipeline_handle_t pipeline;
    mesh_handle_t     mesh;
    object_uniforms_t uniforms;

    texture_handle_t textures[RENDER_MAX_TEXTURE_SLOTS];
    sampler_desc_t   samplers[RENDER_MAX_TEXTURE_SLOTS];
    u8               texture_count;

    draw_call_t call;

    render_layer_t layer;
    f32            view_depth; // 相机空间深度（正值 = 前方）

    // 是否参与阴影深度 pass。
    //
    // 深度 pass 会把**整个队列**用深度管线重画一遍（这样阴影和可见内容天然一致，
    // 不会出现"某个物体没影子"）。但队列里不全是"实体几何"：
    // 线框、参考网格、调试绘制用的是别的图元拓扑，
    // 拿三角形深度管线去画它们会往阴影贴图里塞一堆垃圾三角形 ——
    // 表现为地面上出现莫名其妙的暗斑，而且极难归因。
    // 所以这类项要把 cast_shadow 置 false。
    bool cast_shadow;

    // 由 render_queue_push 计算，调用方不需要填
    u64 sort_key;
    u32 sequence;
} render_item_t;

// 初始化一个渲染项，填好所有默认值（默认管线/网格为无效句柄）。
render_item_t render_item_default(void);

typedef struct render_queue_t {
    array_t  items; // render_item_t
    camera_t camera;
    bool     has_camera;

    f32 depth_range_near;
    f32 depth_range_far;

    u32 next_sequence;
    u32 peak_item_count; // 历史峰值，用于观察数组是否还会增长
} render_queue_t;

bool render_queue_init(render_queue_t *queue, allocator_t *alloc);
void render_queue_destroy(render_queue_t *queue);

// 开始一帧：设置相机、清空队列、重置序号。
// 队列的底层数组**不清空内存**（array_clear 只把 size 置 0），
// 所以稳态下不会有任何分配 —— 这是"帧内零 malloc"的落点之一。
void render_queue_begin(render_queue_t *queue, const camera_t *camera);

void render_queue_clear(render_queue_t *queue);

// 提交一个渲染项，返回它的下标。内部会算好 sort_key 与 sequence。
u32 render_queue_push(render_queue_t *queue, const render_item_t *item);

// 按 sort_key 排序。提交完成后、draw 之前调用一次。
void render_queue_sort(render_queue_t *queue);

const render_item_t *render_queue_items(const render_queue_t *queue);
u32                  render_queue_count(const render_queue_t *queue);
u32                  render_queue_peak_count(const render_queue_t *queue);

// 计算排序键（公开出来是为了可测试：排序逻辑错了会表现为画面顺序错误，
// 而不是崩溃，很难定位）。
u64 render_queue_make_sort_key(render_layer_t layer, f32 view_depth, f32 depth_near,
                               f32 depth_far);

// 排序比较函数（全序）。公开同样是为了可测试。
int render_queue_compare_items(const void *a, const void *b);

#endif // ENGINE_RENDER_RENDER_QUEUE_H
