// =============================================================================
//  engine/render/debug_draw.h —— 2D 调试绘制
//
//  用途：把"看不见的东西"画出来 —— 碰撞盒、速度向量、路径、网格、坐标轴。
//  没有它，调碰撞和 AI 只能靠 printf 猜。
//
//  实现方式：全部走精灵批渲染器 + 一张 1x1 白色纹理。
//    * 线段 = 一个细长的旋转精灵
//    * 矩形边框 = 4 条线段
//    * 圆 = N 段线段（或 N 个三角形，见 debug_draw_filled_circle）
//
//  为什么不用 GL_LINES：线段图元的线宽支持在各驱动上不一致
//  （core profile 里 glLineWidth > 1 在很多驱动上被忽略），
//  而且线宽以像素为单位、不随相机缩放。用精灵画线段则完全可控。
//
//  M10 的物理调试可视化会直接复用这一层。
// =============================================================================
#ifndef ENGINE_RENDER_DEBUG_DRAW_H
#define ENGINE_RENDER_DEBUG_DRAW_H

#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/render_types.h"
#include "engine/render/sprite_batch.h"
#include "engine/render/texture_atlas.h"

typedef struct debug_draw_t {
    sprite_batch_t *batch;
    texture_handle_t white_texture;
    render_layer_t   layer;

    // 统计：本帧画了多少个图元（用于发现"调试绘制把性能吃掉了"）
    u32 primitives;
} debug_draw_t;

// 初始化。white_texture 由调用方提供（一般是 texture_create_solid 出来的 1x1 白）。
void debug_draw_init(debug_draw_t *debug, sprite_batch_t *batch, texture_handle_t white_texture);

void debug_draw_begin(debug_draw_t *debug);
void debug_draw_set_layer(debug_draw_t *debug, render_layer_t layer);

// 线段（a 到 b，thickness 是像素宽度）
void debug_draw_line(debug_draw_t *debug, vec2_t a, vec2_t b, vec4_t color, f32 thickness);

// 矩形：filled = true 画实心，false 画边框
void debug_draw_rect(debug_draw_t *debug, vec2_t min, vec2_t max, vec4_t color, bool filled,
                     f32 thickness);

// 旋转矩形（OBB 可视化）
void debug_draw_rotated_rect(debug_draw_t *debug, vec2_t center, vec2_t half_extents, f32 rotation,
                             vec4_t color, bool filled, f32 thickness);

// 圆：filled = false 时画 segments 段折线，filled = true 时画扇形
void debug_draw_circle(debug_draw_t *debug, vec2_t center, f32 radius, vec4_t color, bool filled,
                       u32 segments, f32 thickness);

// 十字（坐标原点、枢轴点可视化）
void debug_draw_cross(debug_draw_t *debug, vec2_t center, f32 size, vec4_t color, f32 thickness);

// 箭头（速度向量、朝向）
void debug_draw_arrow(debug_draw_t *debug, vec2_t from, vec2_t to, vec4_t color, f32 thickness);

// 网格（关卡边界、地面参考）
void debug_draw_grid(debug_draw_t *debug, vec2_t min, vec2_t max, f32 cell_size, vec4_t color,
                     f32 thickness);

// 2D 视锥/相机边界
void debug_draw_aabb(debug_draw_t *debug, vec2_t min, vec2_t max, vec4_t color, f32 thickness);

#endif // ENGINE_RENDER_DEBUG_DRAW_H
