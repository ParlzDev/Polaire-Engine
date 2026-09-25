// =============================================================================
//  engine/render/tilemap.h —— 图块地图
//
//  Tilemap 本身很薄：它就是"一张图块索引表 + 按视野把可见图块发成精灵"。
//  真正的工作在批渲染器那边（图块天然同图集、同管线，所以能全部合批）。
//
//  ---- 视口剔除在这里特别重要 ----
//
//  一张 256x256 的图块地图有 65536 个图块，全提交会直接把批渲染器的容量撑爆。
//  而屏幕一次只能看到几百个。所以 tilemap_draw 必须只提交可见范围内的图块。
//
//  ---- 图块索引 0 表示空 ----
//
//  索引从 1 开始，0 是"空格子"。这样 memset(0) 就是一张空地图，
//  不需要额外的"是否已填充"标记。
// =============================================================================
#ifndef ENGINE_RENDER_TILEMAP_H
#define ENGINE_RENDER_TILEMAP_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/sprite_batch.h"
#include "engine/render/texture_atlas.h"

typedef struct tilemap_t {
    allocator_t *alloc;
    u32          width;
    u32          height;
    u16         *tiles; // width * height，0 = 空

    f32 tile_size;      // 单个图块的显示边长（世界单位/像素）

    // 图集信息：图块在图集里按网格排列
    u32 atlas_columns;
    u32 atlas_rows;
    f32 atlas_tile_px;  // 图集里单个图块的像素尺寸（不含间隔）
    f32 atlas_spacing;  // 图块之间的间隔（像素），含 padding
} tilemap_t;

bool tilemap_init(tilemap_t *map, allocator_t *alloc, u32 width, u32 height, f32 tile_size,
                  u32 atlas_columns, u32 atlas_rows, f32 atlas_tile_px, f32 atlas_spacing);
void tilemap_shutdown(tilemap_t *map);

void tilemap_fill(tilemap_t *map, u16 tile);
void tilemap_set(tilemap_t *map, u32 x, u32 y, u16 tile);
u16  tilemap_get(const tilemap_t *map, u32 x, u32 y);
bool tilemap_in_bounds(const tilemap_t *map, i32 x, i32 y);

// 图块在世界里的中心位置（origin 是地图左上角的世界坐标）
vec2_t tilemap_tile_center(const tilemap_t *map, vec2_t origin, u32 x, u32 y);

// 提交所有图块
u32 tilemap_draw(sprite_batch_t *batch, const tilemap_t *map, texture_handle_t texture,
                 vec2_t origin, vec4_t tint);

// 只提交与给定世界矩形相交的图块（视口剔除）。
// 返回实际提交的图块数。**大图必须用这个版本。**
u32 tilemap_draw_visible(sprite_batch_t *batch, const tilemap_t *map, texture_handle_t texture,
                         vec2_t origin, vec2_t view_min, vec2_t view_max, vec4_t tint);

#endif // ENGINE_RENDER_TILEMAP_H
