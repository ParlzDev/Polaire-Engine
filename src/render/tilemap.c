// =============================================================================
//  src/render/tilemap.c —— 图块地图
// =============================================================================
#include "engine/render/tilemap.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

bool tilemap_init(tilemap_t *map, allocator_t *alloc, u32 width, u32 height, f32 tile_size,
                  u32 atlas_columns, u32 atlas_rows, f32 atlas_tile_px, f32 atlas_spacing)
{
    ASSERT(map != NULL);
    ASSERT(alloc != NULL);

    if (width == 0 || height == 0 || atlas_columns == 0 || atlas_rows == 0) {
        LOG_ERROR("Tilemap 参数无效: %ux%u, 图集 %ux%u", width, height, atlas_columns,
                  atlas_rows);
        return false;
    }

    memset(map, 0, sizeof(*map));
    map->alloc = alloc;
    map->width = width;
    map->height = height;
    map->tile_size = (tile_size > 0.0f) ? tile_size : 32.0f;
    map->atlas_columns = atlas_columns;
    map->atlas_rows = atlas_rows;
    map->atlas_tile_px = (atlas_tile_px > 0.0f) ? atlas_tile_px : map->tile_size;
    map->atlas_spacing = atlas_spacing;

    const usize bytes = (usize)width * height * sizeof(u16);
    map->tiles = (u16 *)alloc->alloc(alloc, bytes, 16);
    if (map->tiles == NULL) {
        LOG_ERROR("Tilemap 数据分配失败（%llu 字节）", (unsigned long long)bytes);
        return false;
    }
    // 0 = 空格子，所以 memset 就是"清空地图"
    memset(map->tiles, 0, bytes);

    return true;
}

void tilemap_shutdown(tilemap_t *map)
{
    if (map == NULL) {
        return;
    }

    if (map->tiles != NULL && map->alloc != NULL) {
        map->alloc->free(map->alloc, map->tiles, (usize)map->width * map->height * sizeof(u16));
    }
    memset(map, 0, sizeof(*map));
}

void tilemap_fill(tilemap_t *map, u16 tile)
{
    ASSERT(map != NULL);
    const u32 count = map->width * map->height;
    for (u32 i = 0; i < count; ++i) {
        map->tiles[i] = tile;
    }
}

bool tilemap_in_bounds(const tilemap_t *map, i32 x, i32 y)
{
    ASSERT(map != NULL);
    return x >= 0 && y >= 0 && (u32)x < map->width && (u32)y < map->height;
}

void tilemap_set(tilemap_t *map, u32 x, u32 y, u16 tile)
{
    ASSERT(map != NULL);
    if (x >= map->width || y >= map->height) {
        return;
    }
    map->tiles[y * map->width + x] = tile;
}

u16 tilemap_get(const tilemap_t *map, u32 x, u32 y)
{
    ASSERT(map != NULL);
    if (x >= map->width || y >= map->height) {
        return 0;
    }
    return map->tiles[y * map->width + x];
}

vec2_t tilemap_tile_center(const tilemap_t *map, vec2_t origin, u32 x, u32 y)
{
    ASSERT(map != NULL);

    // origin 是地图左上角的世界坐标。世界坐标 Y 向上，
    // 所以行号越大越靠下（Y 越小）。
    return vec2_make(origin.x + ((f32)x + 0.5f) * map->tile_size,
                     origin.y - ((f32)y + 0.5f) * map->tile_size);
}

u32 tilemap_draw_visible(sprite_batch_t *batch, const tilemap_t *map, texture_handle_t texture,
                         vec2_t origin, vec2_t view_min, vec2_t view_max, vec4_t tint)
{
    ASSERT(batch != NULL);
    ASSERT(map != NULL);

    if (map->tiles == NULL) {
        return 0;
    }

    // 把世界矩形换算成图块行列范围。
    //
    // 这一步是 tilemap 渲染的关键：一张 256x256 的地图有 65536 个图块，
    // 全提交会把批渲染器撑爆，而屏幕一次只能看到几百个。
    const f32 inv_tile = 1.0f / map->tile_size;

    i32 min_col = (i32)floorf((view_min.x - origin.x) * inv_tile);
    i32 max_col = (i32)ceilf((view_max.x - origin.x) * inv_tile);
    // 世界 Y 向上、行号向下，所以 Y 越小行号越大
    i32 min_row = (i32)floorf((origin.y - view_max.y) * inv_tile);
    i32 max_row = (i32)ceilf((origin.y - view_min.y) * inv_tile);

    if (min_col < 0) {
        min_col = 0;
    }
    if (min_row < 0) {
        min_row = 0;
    }
    if (max_col >= (i32)map->width) {
        max_col = (i32)map->width - 1;
    }
    if (max_row >= (i32)map->height) {
        max_row = (i32)map->height - 1;
    }

    if (min_col > max_col || min_row > max_row) {
        return 0;
    }

    const f32 inv_columns = 1.0f / (f32)map->atlas_columns;
    const f32 inv_rows = 1.0f / (f32)map->atlas_rows;

    // 图块之间的间隔（含 padding）按图集格子算。
    // atlas_spacing 是"每个格子额外的像素数"，UV 里就是 spacing / (tile_px * columns)。
    const f32 cell_u = inv_columns;
    const f32 cell_v = inv_rows;
    const f32 inset_u = (map->atlas_spacing * 0.5f) / ((map->atlas_tile_px + map->atlas_spacing) *
                                                       (f32)map->atlas_columns);
    const f32 inset_v = (map->atlas_spacing * 0.5f) / ((map->atlas_tile_px + map->atlas_spacing) *
                                                       (f32)map->atlas_rows);

    const f32 tile_size = map->tile_size;
    const f32 origin_x = origin.x;
    const f32 origin_y = origin.y;

    u32 drawn = 0;

    for (i32 row = min_row; row <= max_row; ++row) {
        for (i32 col = min_col; col <= max_col; ++col) {
            const u16 tile = map->tiles[(u32)row * map->width + (u32)col];
            if (tile == 0) {
                continue; // 空格子
            }

            const u32 index = (u32)tile - 1u;
            const u32 tile_col = index % map->atlas_columns;
            const u32 tile_row = index / map->atlas_columns;
            if (tile_row >= map->atlas_rows) {
                continue;
            }

            atlas_region_t region;
            memset(&region, 0, sizeof(region));
            region.uv_min = vec2_make((f32)tile_col * cell_u + inset_u,
                                      (f32)tile_row * cell_v + inset_v);
            region.uv_max = vec2_make((f32)(tile_col + 1u) * cell_u - inset_u,
                                      (f32)(tile_row + 1u) * cell_v - inset_v);

            const vec2_t center = vec2_make(origin_x + ((f32)col + 0.5f) * tile_size,
                                            origin_y - ((f32)row + 0.5f) * tile_size);

            sprite_batch_draw_region(batch, center, vec2_make(tile_size, tile_size), 0.0f, &region,
                                     tint, texture);
            drawn++;
        }
    }

    // 不再往 map 里写统计 —— 那需要去掉 const，而调用方本来就拿到了返回值。
    return drawn;
}

u32 tilemap_draw(sprite_batch_t *batch, const tilemap_t *map, texture_handle_t texture,
                 vec2_t origin, vec4_t tint)
{
    ASSERT(map != NULL);

    // 无剔除版本：把整张地图的包围盒当成"视野"。
    // 只在小地图（几百个图块）上用；大地图请用 tilemap_draw_visible。
    //
    // 注意 AABB 的构造：origin 是地图**左上角**（世界 Y 最大的那一边），
    // 所以矩形是 (origin.x, bottom) 到 (right, origin.y)。
    const vec2_t aabb_min = vec2_make(origin.x, origin.y - (f32)map->height * map->tile_size);
    const vec2_t aabb_max = vec2_make(origin.x + (f32)map->width * map->tile_size, origin.y);

    return tilemap_draw_visible(batch, map, texture, origin, aabb_min, aabb_max, tint);
}
