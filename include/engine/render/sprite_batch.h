// =============================================================================
//  engine/render/sprite_batch.h —— 精灵批渲染器
//
//  ============================================================================
//   它解决的问题：draw call 数量
//  ============================================================================
//
//  M2 的"一个物体一次 draw"在 2D 场景下会直接崩掉：
//  1 万个精灵 = 1 万次 draw call，每次 draw 的 CPU 开销（状态校验、
//  命令写入）就有几微秒，光这一项就超过 16ms 的帧预算。
//
//  批渲染把"1 万次 draw"压成"个位数 draw"：把所有精灵的顶点写进
//  **同一块顶点缓冲**，然后用不同的索引范围发出几次 draw。
//
//  ============================================================================
//   断批（flush）的判定条件 —— 本模块最核心的逻辑
//  ============================================================================
//
//  只有满足以下全部条件的精灵才能进同一个批次：
//    * 同一个纹理（一次 draw 只能绑定一组纹理）
//    * 同一个管线（着色器 + 全部渲染状态）
//    * 同一个 render layer（渲染队列按 layer 排序，跨 layer 必须断开）
//    * 同一个深度值（同 layer 内按深度排序）
//
//  任何一条不满足就必须 flush，把当前累积的精灵发成一个渲染项。
//  "纹理切换"是最容易漏的一条 —— 换图集时必须断开，否则精灵会采到
//  另一张图的 UV，画面完全错乱。
//
//  ============================================================================
//   顶点缓冲的容量是"整帧"的，不是"单批次"的
//  ============================================================================
//
//  这一点很关键：所有批次共用同一块顶点缓冲，每个批次只是它的一个
//  索引区间。因为渲染队列是在帧末统一绘制的，如果中途复用缓冲，
//  前面批次的顶点数据会被后面的覆盖。
//
//  所以 `max_sprites` 是**一帧内能画的精灵总数**，超出会记警告并丢弃。
//  （真正的解法是环形缓冲 + 多缓冲对象，等真有超大规模场景再做。）
// =============================================================================
#ifndef ENGINE_RENDER_SPRITE_BATCH_H
#define ENGINE_RENDER_SPRITE_BATCH_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/render/render_types.h"
#include "engine/render/renderer.h"
#include "engine/render/sprite.h"
#include "engine/render/texture_atlas.h"

typedef struct renderer_t renderer_t;

// 2D 精灵顶点：位置(2f) + UV(2f) + 颜色(u32) = 20 字节。
//
// 比 M2 的 mesh_vertex_t（32 字节）省 37%：2D 不需要法线。
// 1 万个精灵省下 480KB/帧的带宽 —— 在"1 万精灵 < 16ms"这个目标下，
// 这类细节是能决定成败的。
typedef struct sprite_vertex_t {
    f32 x;
    f32 y;
    f32 u;
    f32 v;
    u32 color; // RGBA8，内存顺序 R,G,B,A
} sprite_vertex_t;

_Static_assert(sizeof(sprite_vertex_t) == 20, "sprite_vertex_t 必须是 20 字节");

#define SPRITE_VERTEX_STRIDE 20u
#define SPRITE_VERTICES_PER_QUAD 4u
#define SPRITE_INDICES_PER_QUAD 6u

typedef struct sprite_batch_t {
    renderer_t  *renderer;
    allocator_t *alloc;

    // 顶点数据（CPU 侧）。一帧内的所有批次共用这一块。
    sprite_vertex_t *vertices;
    u32              vertex_capacity; // 单位：顶点
    u32              sprite_capacity; // = vertex_capacity / 4

    u32 sprite_count;         // 本帧已写入的精灵总数
    u32 peak_sprite_count;    // 历史峰值（用来判断容量够不够）
    u32 dropped_sprites;      // 因为超容量被丢弃的数量

    // 当前批次的累积状态
    texture_handle_t  current_texture;
    pipeline_handle_t current_pipeline;
    render_layer_t    current_layer;
    f32               current_depth;
    sampler_desc_t    sampler;
    u32               sprites_in_batch;
    u32               batch_first_index; // 当前批次在索引缓冲里的起点

    u32 batch_count; // 本帧 flush 了几次 = draw call 数

    mesh_handle_t mesh;

    // 内置管线（调用方没提供 pipeline 时由批渲染器自己创建并持有）
    pipeline_handle_t default_pipeline;
    shader_handle_t   default_shader;
    bool              owns_pipeline;
} sprite_batch_t;

typedef struct sprite_batch_desc_t {
    u32               max_sprites; // 一帧内能画的精灵总数
    pipeline_handle_t pipeline;    // 无效句柄 = 用内置的 2D 精灵管线
    sampler_desc_t    sampler;
} sprite_batch_desc_t;

sprite_batch_desc_t sprite_batch_desc_default(void);

bool sprite_batch_init(sprite_batch_t *batch, renderer_t *renderer, allocator_t *alloc,
                       const sprite_batch_desc_t *desc);
void sprite_batch_shutdown(sprite_batch_t *batch);

// 开始一帧。清空计数与当前批次状态。
void sprite_batch_begin(sprite_batch_t *batch);

// 提交一个精灵。texture 为该精灵所用的纹理（决定是否要断批）。
void sprite_batch_draw(sprite_batch_t *batch, const sprite_t *sprite, texture_handle_t texture);

// 直接按图集区域画（省掉手动填 UV）
void sprite_batch_draw_region(sprite_batch_t *batch, vec2_t center, vec2_t size, f32 rotation,
                              const atlas_region_t *region, vec4_t color,
                              texture_handle_t texture);

// 结束一帧：flush 最后一个批次，并把顶点数据上传到 GPU。
// **必须在 renderer_end_frame 之前调用。**
void sprite_batch_end(sprite_batch_t *batch);

// 手动断批。切换纹理/管线/图层时不想依赖自动判断的话可以显式调用。
void sprite_batch_flush(sprite_batch_t *batch);

void sprite_batch_set_pipeline(sprite_batch_t *batch, pipeline_handle_t pipeline);
void sprite_batch_set_layer(sprite_batch_t *batch, render_layer_t layer);
void sprite_batch_set_depth(sprite_batch_t *batch, f32 depth);
void sprite_batch_set_sampler(sprite_batch_t *batch, const sampler_desc_t *sampler);

// ---------------------------------------------------------------- 查询
u32  sprite_batch_batch_count(const sprite_batch_t *batch);   // 本帧的 draw call 数
u32  sprite_batch_sprite_count(const sprite_batch_t *batch);  // 本帧提交的精灵数
u32  sprite_batch_peak_sprite_count(const sprite_batch_t *batch);
u32  sprite_batch_dropped_sprites(const sprite_batch_t *batch);
bool sprite_batch_is_full(const sprite_batch_t *batch);
f32  sprite_batch_usage(const sprite_batch_t *batch); // 容量使用率 0..1

// 内置 2D 精灵着色器与顶点布局
shader_desc_t   sprite_batch_builtin_shader(void);
vertex_layout_t sprite_batch_vertex_layout(void);

#endif // ENGINE_RENDER_SPRITE_BATCH_H
