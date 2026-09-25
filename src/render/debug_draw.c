// =============================================================================
//  src/render/debug_draw.c —— 2D 调试绘制
//
//  全部通过精灵批渲染器 + 1x1 白纹理实现。这样调试绘制与游戏内容共用同一条
//  渲染路径，不会出现"调试图形把批次打断了，性能数据失真"的情况。
// =============================================================================
#include "engine/render/debug_draw.h"

#include <string.h>

#include "engine/core/assert.h"

void debug_draw_init(debug_draw_t *debug, sprite_batch_t *batch, texture_handle_t white_texture)
{
    ASSERT(debug != NULL);

    memset(debug, 0, sizeof(*debug));
    debug->batch = batch;
    debug->white_texture = white_texture;
    // 默认放在 UI 层之下、普通精灵之上 —— 调试图形不应该被游戏内容挡住，
    // 但也不应该盖住 UI（否则分数被网格线切过去看不清）。
    debug->layer = RENDER_LAYER_TRANSPARENT_2D;
}

void debug_draw_begin(debug_draw_t *debug)
{
    ASSERT(debug != NULL);
    debug->primitives = 0;
}

void debug_draw_set_layer(debug_draw_t *debug, render_layer_t layer)
{
    ASSERT(debug != NULL);
    debug->layer = layer;
}

// 内部：把当前图层写进批渲染器
static void debug_push_layer(debug_draw_t *debug)
{
    if (debug->batch != NULL) {
        sprite_batch_set_layer(debug->batch, debug->layer);
    }
}

void debug_draw_line(debug_draw_t *debug, vec2_t a, vec2_t b, vec4_t color, f32 thickness)
{
    ASSERT(debug != NULL);

    const vec2_t delta = vec2_sub(b, a);
    const f32    length = vec2_length(delta);
    if (length < 1.0e-6f) {
        return;
    }

    debug_push_layer(debug);

    // 线段 = 一个细长的旋转精灵，中心在中点，旋转到与 delta 同向
    sprite_t sprite = sprite_make(vec2_scale(vec2_add(a, b), 0.5f),
                                  vec2_make(length, thickness > 0.0f ? thickness : 1.0f));
    sprite.rotation = atan2f(delta.y, delta.x);
    sprite.color = color;

    sprite_batch_draw(debug->batch, &sprite, debug->white_texture);
    debug->primitives++;
}

void debug_draw_rect(debug_draw_t *debug, vec2_t min, vec2_t max, vec4_t color, bool filled,
                     f32 thickness)
{
    ASSERT(debug != NULL);

    debug_push_layer(debug);

    if (filled) {
        const vec2_t center = vec2_scale(vec2_add(min, max), 0.5f);
        const vec2_t size = vec2_sub(max, min);
        sprite_t sprite = sprite_make(center, size);
        sprite.color = color;
        sprite_batch_draw(debug->batch, &sprite, debug->white_texture);
        debug->primitives++;
        return;
    }

    const vec2_t bl = vec2_make(min.x, min.y);
    const vec2_t br = vec2_make(max.x, min.y);
    const vec2_t tr = vec2_make(max.x, max.y);
    const vec2_t tl = vec2_make(min.x, max.y);

    debug_draw_line(debug, bl, br, color, thickness);
    debug_draw_line(debug, br, tr, color, thickness);
    debug_draw_line(debug, tr, tl, color, thickness);
    debug_draw_line(debug, tl, bl, color, thickness);
}

void debug_draw_rotated_rect(debug_draw_t *debug, vec2_t center, vec2_t half_extents, f32 rotation,
                             vec4_t color, bool filled, f32 thickness)
{
    ASSERT(debug != NULL);

    debug_push_layer(debug);

    if (filled) {
        sprite_t sprite = sprite_make(center, vec2_scale(half_extents, 2.0f));
        sprite.rotation = rotation;
        sprite.color = color;
        sprite_batch_draw(debug->batch, &sprite, debug->white_texture);
        debug->primitives++;
        return;
    }

    // 四个角绕中心旋转
    const f32 c = cosf(rotation);
    const f32 s = sinf(rotation);
    const f32 hx = half_extents.x;
    const f32 hy = half_extents.y;

    const f32 local[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
    vec2_t    corners[4];
    for (u32 i = 0; i < 4; ++i) {
        corners[i].x = center.x + local[i][0] * c - local[i][1] * s;
        corners[i].y = center.y + local[i][0] * s + local[i][1] * c;
    }

    for (u32 i = 0; i < 4; ++i) {
        debug_draw_line(debug, corners[i], corners[(i + 1u) % 4u], color, thickness);
    }
}

void debug_draw_circle(debug_draw_t *debug, vec2_t center, f32 radius, vec4_t color, bool filled,
                       u32 segments, f32 thickness)
{
    ASSERT(debug != NULL);

    if (segments < 3) {
        segments = 3;
    }
    if (segments > 256) {
        segments = 256;
    }

    debug_push_layer(debug);

    if (filled) {
        // 用 N 个三角形扇形拼 —— 精灵批渲染器只有 quad，
        // 所以这里退化成"N 个细长的三角形"不划算；直接画一个内接正多边形：
        // 每段用一个 quad，从圆心到边缘。段数够多时看不出接缝。
        for (u32 i = 0; i < segments; ++i) {
            const f32 a0 = (f32)i / (f32)segments * F32_TAU;
            const f32 a1 = (f32)(i + 1u) / (f32)segments * F32_TAU;

            const vec2_t p0 = vec2_make(center.x + cosf(a0) * radius, center.y + sinf(a0) * radius);
            const vec2_t p1 = vec2_make(center.x + cosf(a1) * radius, center.y + sinf(a1) * radius);

            // 三角形 (center, p0, p1) 用 quad 表示会多画一半面积。
            // 简化处理：用"从 center 到 p0p1 中点的细长四边形"近似，
            // 段数足够时视觉上等价，且实现简单。
            const vec2_t mid = vec2_scale(vec2_add(p0, p1), 0.5f);
            const vec2_t delta = vec2_sub(mid, center);
            const f32    length = vec2_length(delta);
            if (length < 1.0e-6f) {
                continue;
            }

            sprite_t sprite = sprite_make(vec2_scale(vec2_add(center, mid), 0.5f),
                                          vec2_make(length, radius * F32_TAU / (f32)segments));
            sprite.rotation = atan2f(delta.y, delta.x);
            sprite.color = color;
            sprite_batch_draw(debug->batch, &sprite, debug->white_texture);
            debug->primitives++;
        }
        return;
    }

    for (u32 i = 0; i < segments; ++i) {
        const f32 a0 = (f32)i / (f32)segments * F32_TAU;
        const f32 a1 = (f32)(i + 1u) / (f32)segments * F32_TAU;

        const vec2_t p0 = vec2_make(center.x + cosf(a0) * radius, center.y + sinf(a0) * radius);
        const vec2_t p1 = vec2_make(center.x + cosf(a1) * radius, center.y + sinf(a1) * radius);
        debug_draw_line(debug, p0, p1, color, thickness);
    }
}

void debug_draw_cross(debug_draw_t *debug, vec2_t center, f32 size, vec4_t color, f32 thickness)
{
    debug_draw_line(debug, vec2_make(center.x - size, center.y),
                    vec2_make(center.x + size, center.y), color, thickness);
    debug_draw_line(debug, vec2_make(center.x, center.y - size),
                    vec2_make(center.x, center.y + size), color, thickness);
}

void debug_draw_arrow(debug_draw_t *debug, vec2_t from, vec2_t to, vec4_t color, f32 thickness)
{
    ASSERT(debug != NULL);

    debug_draw_line(debug, from, to, color, thickness);

    const vec2_t delta = vec2_sub(to, from);
    const f32    length = vec2_length(delta);
    if (length < 1.0e-6f) {
        return;
    }

    const f32    head = math_min(length * 0.25f, 12.0f);
    const vec2_t dir = vec2_scale(delta, 1.0f / length);

    // 两条短线组成箭头
    const vec2_t back = vec2_sub(to, vec2_scale(dir, head));
    const f32    angle = 0.5f; // 约 28 度

    const vec2_t left = vec2_rotate(dir, angle);
    const vec2_t right = vec2_rotate(dir, -angle);

    debug_draw_line(debug, to, vec2_add(back, vec2_scale(left, head)), color, thickness);
    debug_draw_line(debug, to, vec2_add(back, vec2_scale(right, head)), color, thickness);
}

void debug_draw_grid(debug_draw_t *debug, vec2_t min, vec2_t max, f32 cell_size, vec4_t color,
                     f32 thickness)
{
    ASSERT(debug != NULL);

    if (cell_size <= 0.0f) {
        return;
    }

    debug_push_layer(debug);

    for (f32 x = min.x; x <= max.x + 0.001f; x += cell_size) {
        debug_draw_line(debug, vec2_make(x, min.y), vec2_make(x, max.y), color, thickness);
    }
    for (f32 y = min.y; y <= max.y + 0.001f; y += cell_size) {
        debug_draw_line(debug, vec2_make(min.x, y), vec2_make(max.x, y), color, thickness);
    }
}

void debug_draw_aabb(debug_draw_t *debug, vec2_t min, vec2_t max, vec4_t color, f32 thickness)
{
    debug_draw_rect(debug, min, max, color, false, thickness);
}
