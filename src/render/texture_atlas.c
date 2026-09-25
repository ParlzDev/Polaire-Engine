// =============================================================================
//  src/render/texture_atlas.c —— 纹理图集（货架打包）
// =============================================================================
#include "engine/render/texture_atlas.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/renderer.h"

texture_atlas_desc_t texture_atlas_desc_default(void)
{
    texture_atlas_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = 1024;
    desc.height = 1024;
    // 图集默认用 UNORM 而不是 sRGB。
    //
    // 理由：M2/M3 阶段还没有渲染目标抽象，默认帧缓冲也不是 sRGB 的，
    // 所以"纹理值 -> 着色器 -> 屏幕"全程都是 sRGB 编码空间。
    // 用 sRGB 纹理会引入一次解码而输出端不编码，颜色会整体偏暗。
    //
    // M4 引入离屏渲染后会统一成"全线性 + sRGB 帧缓冲"的管线，
    // 届时这里改成 sRGB 即可（详见 docs/M3-设计决策.md）。
    desc.format = PIXEL_FORMAT_R8G8B8A8_UNORM;
    desc.padding = ATLAS_DEFAULT_PADDING;
    desc.debug_name = "atlas";
    return desc;
}

bool texture_atlas_init(texture_atlas_t *atlas, allocator_t *alloc,
                        const texture_atlas_desc_t *desc)
{
    ASSERT(atlas != NULL);
    ASSERT(alloc != NULL);

    texture_atlas_desc_t actual = (desc != NULL) ? *desc : texture_atlas_desc_default();
    if (actual.width == 0) {
        actual.width = 1024;
    }
    if (actual.height == 0) {
        actual.height = 1024;
    }

    memset(atlas, 0, sizeof(*atlas));
    atlas->alloc = alloc;
    atlas->width = actual.width;
    atlas->height = actual.height;
    atlas->format = actual.format;
    atlas->padding = (actual.padding == 0) ? ATLAS_DEFAULT_PADDING : actual.padding;

    const usize bytes = (usize)actual.width * actual.height * 4u;
    atlas->pixels = (u8 *)alloc->alloc(alloc, bytes, 16);
    if (atlas->pixels == NULL) {
        LOG_ERROR("图集画布分配失败（%llu 字节）", (unsigned long long)bytes);
        return false;
    }
    memset(atlas->pixels, 0, bytes); // 全透明
    atlas->owns_pixels = true;

    atlas->shelf_y = 0;
    atlas->shelf_height = 0;
    atlas->cursor_x = 0;
    return true;
}

void texture_atlas_shutdown(texture_atlas_t *atlas)
{
    if (atlas == NULL) {
        return;
    }

    if (atlas->owns_pixels && atlas->pixels != NULL && atlas->alloc != NULL) {
        const usize bytes = (usize)atlas->width * atlas->height * 4u;
        atlas->alloc->free(atlas->alloc, atlas->pixels, bytes);
    }
    memset(atlas, 0, sizeof(*atlas));
}

// =============================================================================
//  货架打包
// =============================================================================
bool texture_atlas_alloc_region(texture_atlas_t *atlas, u32 width, u32 height, const char *name,
                                atlas_region_t *out_region)
{
    ASSERT(atlas != NULL);
    ASSERT(out_region != NULL);

    if (width == 0 || height == 0) {
        return false;
    }
    if (atlas->region_count >= ATLAS_MAX_REGIONS) {
        LOG_WARN("图集区域数已达上限 %u", ATLAS_MAX_REGIONS);
        return false;
    }

    const u32 pad = atlas->padding;
    const u32 need_w = width + pad * 2u;
    const u32 need_h = height + pad * 2u;

    // 单个区域就超过整张图 —— 直接失败，不要让它把货架逻辑搞乱
    if (need_w > atlas->width || need_h > atlas->height) {
        return false;
    }

    // 当前货架放不下这一行的宽度 -> 换一行
    if (atlas->cursor_x + need_w > atlas->width) {
        atlas->shelf_y += atlas->shelf_height;
        atlas->shelf_height = 0;
        atlas->cursor_x = 0;
    }

    // 这一行需要更高
    if (need_h > atlas->shelf_height) {
        if (atlas->shelf_y + need_h > atlas->height) {
            return false; // 高度不够了
        }
        atlas->shelf_height = need_h;
    }

    if (atlas->shelf_y + atlas->shelf_height > atlas->height) {
        return false;
    }
    if (atlas->cursor_x + need_w > atlas->width) {
        return false;
    }

    atlas_region_t region;
    memset(&region, 0, sizeof(region));
    region.x = atlas->cursor_x + pad;
    region.y = atlas->shelf_y + pad;
    region.width = width;
    region.height = height;
    region.name = name;

    // UV 半像素内缩。
    //
    // 纹理坐标 u 对应"纹素中心"的位置是 (x + 0.5) / W。如果直接用 x/W，
    // 双线性采样会在左边缘混进 padding 的像素；用 (x + w) / W 会在右边缘
    // 混进右侧区域的像素 —— 表现为精灵边缘一圈杂色（texture bleeding）。
    //
    // 内缩半个纹素之后，采样点始终落在区域内部，配合 padding 就彻底安全了。
    region.uv_min = vec2_make(((f32)region.x + 0.5f) / (f32)atlas->width,
                              ((f32)region.y + 0.5f) / (f32)atlas->height);
    region.uv_max = vec2_make(((f32)region.x + (f32)region.width - 0.5f) / (f32)atlas->width,
                              ((f32)region.y + (f32)region.height - 0.5f) / (f32)atlas->height);

    atlas->cursor_x += need_w;
    atlas->regions[atlas->region_count] = region;
    atlas->region_count++;
    atlas->used_area += (u64)width * height;

    *out_region = region;
    return true;
}

void texture_atlas_write_region(texture_atlas_t *atlas, const atlas_region_t *region,
                                const void *pixels, u32 width, u32 height)
{
    ASSERT(atlas != NULL);
    ASSERT(region != NULL);

    if (pixels == NULL || width == 0 || height == 0) {
        return;
    }

    const u32 copy_w = (width < region->width) ? width : region->width;
    const u32 copy_h = (height < region->height) ? height : region->height;

    const u8 *src = (const u8 *)pixels;
    for (u32 row = 0; row < copy_h; ++row) {
        u8 *dst = atlas->pixels +
                  (((usize)(region->y + row) * atlas->width) + region->x) * 4u;
        memcpy(dst, src + (usize)row * width * 4u, (usize)copy_w * 4u);
    }
}

bool texture_atlas_add(texture_atlas_t *atlas, const void *pixels, u32 width, u32 height,
                       const char *name, atlas_region_t *out_region)
{
    atlas_region_t region;
    if (!texture_atlas_alloc_region(atlas, width, height, name, &region)) {
        return false;
    }
    texture_atlas_write_region(atlas, &region, pixels, width, height);
    if (out_region != NULL) {
        *out_region = region;
    }
    return true;
}

bool texture_atlas_make_solid(texture_atlas_t *atlas, u8 r, u8 g, u8 b, u8 a)
{
    const u8 pixel[4] = {r, g, b, a};
    return texture_atlas_add(atlas, pixel, 1, 1, "solid", NULL);
}

// =============================================================================
//  上传
// =============================================================================
bool texture_atlas_upload(renderer_t *renderer, texture_atlas_t *atlas)
{
    ASSERT(renderer != NULL);
    ASSERT(atlas != NULL);

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = atlas->width;
    desc.height = atlas->height;
    desc.array_layers = 1;
    desc.format = atlas->format;
    // 图集不生成 mipmap：精灵通常按原始尺寸绘制，mipmap 只会让
    // 边缘在缩放时更模糊，而且会加剧渗色（mip 层是把整张图集一起缩小的）。
    desc.generate_mipmaps = false;
    desc.debug_name = "texture_atlas";

    // 已经上传过就换一张新的。图集的构建流程是"先放满再上传一次"，
    // 正常只会走到这里一次；重复上传是为了支持"字形按需追加"的场景。
    if (atlas->uploaded && atlas->texture != TEXTURE_HANDLE_INVALID) {
        renderer_destroy_texture(renderer, atlas->texture);
        atlas->texture = TEXTURE_HANDLE_INVALID;
        atlas->uploaded = false;
    }

    atlas->texture = renderer_create_texture(renderer, &desc, atlas->pixels);
    if (atlas->texture == TEXTURE_HANDLE_INVALID) {
        LOG_ERROR("图集纹理创建失败 (%ux%u)", atlas->width, atlas->height);
        return false;
    }

    atlas->uploaded = true;
    return true;
}

texture_handle_t texture_atlas_texture(const texture_atlas_t *atlas)
{
    ASSERT(atlas != NULL);
    return atlas->texture;
}

u32 texture_atlas_region_count(const texture_atlas_t *atlas)
{
    ASSERT(atlas != NULL);
    return atlas->region_count;
}

const atlas_region_t *texture_atlas_region(const texture_atlas_t *atlas, u32 index)
{
    ASSERT(atlas != NULL);
    return (index < atlas->region_count) ? &atlas->regions[index] : NULL;
}

f32 texture_atlas_used_ratio(const texture_atlas_t *atlas)
{
    ASSERT(atlas != NULL);
    const u64 total = (u64)atlas->width * atlas->height;
    return (total > 0) ? (f32)((f64)atlas->used_area / (f64)total) : 0.0f;
}
