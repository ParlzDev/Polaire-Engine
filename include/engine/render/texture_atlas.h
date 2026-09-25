// =============================================================================
//  engine/render/texture_atlas.h —— 纹理图集
//
//  ---- 为什么需要图集 ----
//
//  批渲染的核心是"把多个精灵合并成一次 draw call"。但如果每个精灵来自
//  不同的纹理，就无法合并 —— 一次 draw call 只能绑定一组纹理。
//
//  图集把所有小图打进一张大图，于是"同一图集内的精灵"可以合批。
//  这就是 1 万个精灵能变成个位数 draw call 的根本原因。
//
//  ---- 打包算法：货架（shelf） ----
//
//  备选方案：
//    * 网格切分：每格等大。实现最简单，但大小差异大时浪费严重。
//    * 货架：按高度排序，一行一行铺。实现简单，利用率通常 70-85%。
//    * 天际线（skyline）：维护一条"天际线"，放置时找最合适的位置。
//      利用率更高（约 85-95%），实现复杂约 2 倍。
//    * MaxRects：工业界最好的算法之一，利用率最高，实现最复杂。
//
//  M3 选**货架**：对"精灵图集"这种大小相近的输入，货架与天际线的差距很小，
//  而实现复杂度差一倍。等真有"尺寸差异极大的图集"需求再换天际线。
//
//  ---- 采样渗色（texture bleeding） ----
//
//  相邻区域在图集里挨着，双线性采样会在边缘采到隔壁区域的像素 ——
//  表现为精灵边缘出现一圈别的颜色。这是 2D 渲染最常见的视觉 bug 之一。
//
//  双重防护：
//    1. 每个区域周围留 padding（默认 2 像素），并把边缘像素向外扩展
//       （这里用简单的"留空"策略，代价是利用率略降）；
//    2. UV 做半像素内缩：`(x + 0.5) / W` 到 `(x + w - 0.5) / W`。
//       这保证采样点永远不会落在区域边界外。
// =============================================================================
#ifndef ENGINE_RENDER_TEXTURE_ATLAS_H
#define ENGINE_RENDER_TEXTURE_ATLAS_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

#define ATLAS_DEFAULT_PADDING 2u
#define ATLAS_MAX_REGIONS 4096u

typedef struct atlas_region_t {
    u32 x;
    u32 y;
    u32 width;
    u32 height;

    // 已做半像素内缩的 UV
    vec2_t uv_min;
    vec2_t uv_max;

    const char *name;
} atlas_region_t;

typedef struct texture_atlas_t {
    allocator_t *alloc;

    u32            width;
    u32            height;
    pixel_format_t format;

    u8 *pixels; // RGBA8 的 CPU 侧画布
    bool owns_pixels;

    atlas_region_t regions[ATLAS_MAX_REGIONS];
    u32            region_count;

    // 货架打包的游标
    u32 shelf_y;
    u32 shelf_height;
    u32 cursor_x;

    u32 padding;

    texture_handle_t texture; // 上传后才有值
    bool             uploaded;

    // 统计
    u64 used_area;
} texture_atlas_t;

typedef struct texture_atlas_desc_t {
    u32            width;
    u32            height;
    pixel_format_t format;
    u32            padding;
    const char    *debug_name;
} texture_atlas_desc_t;

texture_atlas_desc_t texture_atlas_desc_default(void);

// 创建图集。内部会分配 width*height*4 字节的 CPU 画布。
bool texture_atlas_init(texture_atlas_t *atlas, allocator_t *alloc,
                        const texture_atlas_desc_t *desc);
void texture_atlas_shutdown(texture_atlas_t *atlas);

// 把一块 RGBA8 像素放进图集。失败（放不下）返回 false。
bool texture_atlas_add(texture_atlas_t *atlas, const void *pixels, u32 width, u32 height,
                       const char *name, atlas_region_t *out_region);

// 直接往图集的指定区域写像素（用于字形光栅化：先占位再填内容）
bool texture_atlas_alloc_region(texture_atlas_t *atlas, u32 width, u32 height, const char *name,
                                atlas_region_t *out_region);
void texture_atlas_write_region(texture_atlas_t *atlas, const atlas_region_t *region,
                                const void *pixels, u32 width, u32 height);

// 把 CPU 画布上传成 GPU 纹理。重复调用会重新上传（字形光栅化完再调一次）。
bool texture_atlas_upload(renderer_t *renderer, texture_atlas_t *atlas);

texture_handle_t texture_atlas_texture(const texture_atlas_t *atlas);
u32              texture_atlas_region_count(const texture_atlas_t *atlas);
const atlas_region_t *texture_atlas_region(const texture_atlas_t *atlas, u32 index);
f32              texture_atlas_used_ratio(const texture_atlas_t *atlas);

// 单色图集：整张图只有一个颜色。用于 2D 调试绘制（线段/矩形都是拉伸的纯色块）。
bool texture_atlas_make_solid(texture_atlas_t *atlas, u8 r, u8 g, u8 b, u8 a);

#endif // ENGINE_RENDER_TEXTURE_ATLAS_H
