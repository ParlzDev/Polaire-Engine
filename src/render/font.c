// =============================================================================
//  src/render/font.c —— 字体（位图 / TrueType）
//
//  设计说明见 font.h 文件头。
// =============================================================================
#include "engine/render/font.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/hash.h"
#include "engine/core/log.h"
#include "engine/render/renderer.h"

#include "font8x8_basic.h" // 公有领域 8x8 位图字体（third_party/）
#include "stb_truetype.h"

// =============================================================================
//  内部结构
// =============================================================================
struct font_t {
    allocator_t *alloc;

    texture_handle_t texture;
    f32              pixel_height;
    f32              line_height;
    f32              ascent;
    f32              descent;

    bool        is_bitmap;
    const char *name;

    // codepoint -> 字形下标
    hash_t glyph_map;

    glyph_t *glyphs;
    u32      glyph_count;
    u32      glyph_capacity;
};

#define FONT_MAX_GLYPHS 8192u

static bool font_add_glyph(font_t *font, u32 codepoint, const glyph_t *glyph)
{
    if (font->glyph_count >= font->glyph_capacity) {
        LOG_WARN("字体字形数已达上限 %u，码点 U+%04X 被忽略", font->glyph_capacity, codepoint);
        return false;
    }

    const u32 index = font->glyph_count;
    font->glyphs[index] = *glyph;
    font->glyph_count++;

    return hash_set(&font->glyph_map, (u64)codepoint, (u64)index);
}

// =============================================================================
//  UTF-8
// =============================================================================
u32 utf8_decode(const char **cursor)
{
    // 刻意不做 ASSERT(cursor != NULL)：下面已经有 NULL 兜底。
    // 断言和兜底逻辑同时存在是自相矛盾的 —— 调用方看到"这个函数能处理 NULL"
    // 就会放心地传 NULL，然后在 Debug 构建里被断言炸掉，Release 里又正常。
    // 二选一：要么断言且不兜底，要么兜底且不断言。这里选后者。
    if (cursor == NULL || *cursor == NULL) {
        return 0;
    }

    const u8 *p = (const u8 *)*cursor;
    if (*p == 0) {
        return 0;
    }

    u32 codepoint = 0xFFFD;
    u32 advance = 1;

    if (p[0] < 0x80u) {
        codepoint = p[0];
        advance = 1;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        if ((p[1] & 0xC0u) == 0x80u) {
            codepoint = ((u32)(p[0] & 0x1Fu) << 6) | (u32)(p[1] & 0x3Fu);
            advance = 2;
        }
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        if ((p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            codepoint = ((u32)(p[0] & 0x0Fu) << 12) | ((u32)(p[1] & 0x3Fu) << 6) |
                        (u32)(p[2] & 0x3Fu);
            advance = 3;
        }
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        if ((p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
            codepoint = ((u32)(p[0] & 0x07u) << 18) | ((u32)(p[1] & 0x3Fu) << 12) |
                        ((u32)(p[2] & 0x3Fu) << 6) | (u32)(p[3] & 0x3Fu);
            advance = 4;
        }
    }

    // 非法序列只前进 1 字节 —— 保证调用方不会死循环。
    // 这是"处理不可信文本"的基本要求：如果按声称的长度前进，
    // 一个坏字节就能让循环卡住。
    *cursor += advance;
    return codepoint;
}

u32 utf8_length(const char *text)
{
    if (text == NULL) {
        return 0;
    }

    u32 count = 0;
    const char *cursor = text;
    while (*cursor != '\0') {
        utf8_decode(&cursor);
        count++;
    }
    return count;
}

u32 utf8_encode(u32 codepoint, char out[4])
{
    ASSERT(out != NULL);

    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xC0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xE0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

// =============================================================================
//  内置 8x8 位图字体
// =============================================================================
//
//  图集尺寸：128 个字形，每个 8x8，padding 2 -> 每格 12x12。
//  16 列 x 8 行 = 192 x 96，取 256x128 留余量。
#define BITMAP8X8_ATLAS_WIDTH 256u
#define BITMAP8X8_ATLAS_HEIGHT 128u
#define BITMAP8X8_GLYPH_SIZE 8u

font_t *font_create_bitmap8x8(renderer_t *renderer, allocator_t *alloc)
{
    // renderer 允许为 NULL：表示"只要字形度量，不要 GPU 纹理"。
    // 单元测试用它验证 UTF-8 解码与文本排版，而不需要建 GL 上下文。
    ASSERT(alloc != NULL);

    font_t *font = (font_t *)alloc->alloc(alloc, sizeof(font_t), 16);
    if (font == NULL) {
        return NULL;
    }
    memset(font, 0, sizeof(*font));
    font->alloc = alloc;
    font->is_bitmap = true;
    font->name = "builtin:8x8";
    font->pixel_height = (f32)BITMAP8X8_GLYPH_SIZE;
    // 8x8 字体的基线在字形底部（第 7 行），上方 8 像素是字形高度，
    // 行高再加 2 像素的间隙。
    font->ascent = 8.0f;
    font->descent = 0.0f;
    font->line_height = 10.0f;

    font->glyphs = (glyph_t *)alloc->alloc(alloc, sizeof(glyph_t) * FONT_MAX_GLYPHS, 16);
    if (font->glyphs == NULL) {
        font_destroy(font);
        return NULL;
    }
    font->glyph_capacity = FONT_MAX_GLYPHS;

    if (!hash_init(&font->glyph_map, alloc, 256)) {
        font_destroy(font);
        return NULL;
    }

    texture_atlas_t atlas;
    texture_atlas_desc_t atlas_desc = texture_atlas_desc_default();
    atlas_desc.width = BITMAP8X8_ATLAS_WIDTH;
    atlas_desc.height = BITMAP8X8_ATLAS_HEIGHT;
    atlas_desc.debug_name = "font8x8_atlas";
    if (!texture_atlas_init(&atlas, alloc, &atlas_desc)) {
        font_destroy(font);
        return NULL;
    }

    // 每个字形转成 RGBA：RGB 恒为白，A = 覆盖度。
    // 这样精灵着色器里的 `tex * color` 得到的就是"用 color 着色的文字"。
    u8 pixels[BITMAP8X8_GLYPH_SIZE * BITMAP8X8_GLYPH_SIZE * 4];

    for (u32 codepoint = 0; codepoint < 128u; ++codepoint) {
        for (u32 row = 0; row < BITMAP8X8_GLYPH_SIZE; ++row) {
            // font8x8_basic 的每一行是一个字节，bit 0 = 最左边的像素
            const u8 bits = (u8)font8x8_basic[codepoint][row];
            for (u32 col = 0; col < BITMAP8X8_GLYPH_SIZE; ++col) {
                const bool on = ((bits >> col) & 1u) != 0u;
                u8 *pixel = &pixels[(row * BITMAP8X8_GLYPH_SIZE + col) * 4u];
                pixel[0] = 255;
                pixel[1] = 255;
                pixel[2] = 255;
                pixel[3] = on ? 255u : 0u;
            }
        }

        atlas_region_t region;
        if (!texture_atlas_add(&atlas, pixels, BITMAP8X8_GLYPH_SIZE, BITMAP8X8_GLYPH_SIZE, NULL,
                               &region)) {
            LOG_WARN("8x8 字体图集放不下码点 U+%04X", codepoint);
            break;
        }

        glyph_t glyph;
        memset(&glyph, 0, sizeof(glyph));
        glyph.uv_min = region.uv_min;
        glyph.uv_max = region.uv_max;
        glyph.width = (f32)BITMAP8X8_GLYPH_SIZE;
        glyph.height = (f32)BITMAP8X8_GLYPH_SIZE;
        glyph.offset_x = 0.0f;
        glyph.offset_y = (f32)BITMAP8X8_GLYPH_SIZE; // 字形顶部在基线上方 8 像素
        glyph.advance = (f32)BITMAP8X8_GLYPH_SIZE;

        font_add_glyph(font, codepoint, &glyph);
    }

    // renderer 为 NULL 表示"只要字形度量，不要 GPU 纹理"。
    // 单元测试用它来验证 UTF-8 解码与文本排版，而不需要建 GL 上下文 ——
    // 让测试依赖显卡驱动是 CI 不稳定的头号原因。
    if (renderer == NULL) {
        texture_atlas_shutdown(&atlas);
        font->texture = TEXTURE_HANDLE_INVALID;
        LOG_DEBUG("位图字体就绪（无 GPU 纹理）: %u 个字形，行高 %.0f 像素", font->glyph_count,
                  (f64)font->line_height);
        return font;
    }

    if (!texture_atlas_upload(renderer, &atlas)) {
        texture_atlas_shutdown(&atlas);
        font_destroy(font);
        return NULL;
    }
    font->texture = texture_atlas_texture(&atlas);

    // 纹理已经上传到 GPU，CPU 画布可以释放了
    texture_atlas_shutdown(&atlas);

    LOG_DEBUG("位图字体就绪: %u 个字形，行高 %.0f 像素", font->glyph_count, (f64)font->line_height);
    return font;
}

// =============================================================================
//  TrueType
// =============================================================================
static u32 font_choose_atlas_size(u32 glyph_estimate, f32 pixel_height)
{
    // 每个字形占 (pixel_height + 2*padding)^2，再乘 1.5 的余量（大小不一）。
    const f32 cell = pixel_height + 4.0f;
    const f64 needed = (f64)glyph_estimate * (f64)cell * (f64)cell * 1.5;

    u32 size = 256;
    while ((f64)size * (f64)size < needed && size < 2048u) {
        size *= 2u;
    }
    return size;
}

font_t *font_create_from_ttf(renderer_t *renderer, allocator_t *alloc, const void *ttf_data,
                             usize ttf_size, f32 pixel_height, const char *extra_utf8,
                             i32 font_index)
{
    // 同 font_create_bitmap8x8：renderer 允许为 NULL（只要度量，不要纹理）
    ASSERT(alloc != NULL);

    if (ttf_data == NULL || ttf_size < 12) {
        LOG_ERROR("TTF 数据无效");
        return NULL;
    }
    if (pixel_height < 6.0f || pixel_height > 256.0f) {
        LOG_ERROR("字号超出合理范围: %.1f（应在 6..256）", (f64)pixel_height);
        return NULL;
    }

    stbtt_fontinfo info;
    memset(&info, 0, sizeof(info));

    const int offset = stbtt_GetFontOffsetForIndex((const unsigned char *)ttf_data, font_index);
    if (offset < 0) {
        LOG_ERROR("TTF 字体索引 %d 不存在（可能是 .ttc 集合索引越界）", font_index);
        return NULL;
    }
    if (!stbtt_InitFont(&info, (const unsigned char *)ttf_data, offset)) {
        LOG_ERROR("TTF 初始化失败（文件损坏或不是 TrueType 格式）");
        return NULL;
    }

    const f32 scale = stbtt_ScaleForPixelHeight(&info, pixel_height);

    int raw_ascent = 0;
    int raw_descent = 0;
    int raw_line_gap = 0;
    stbtt_GetFontVMetrics(&info, &raw_ascent, &raw_descent, &raw_line_gap);

    font_t *font = (font_t *)alloc->alloc(alloc, sizeof(font_t), 16);
    if (font == NULL) {
        return NULL;
    }
    memset(font, 0, sizeof(*font));
    font->alloc = alloc;
    font->is_bitmap = false;
    font->pixel_height = pixel_height;
    font->ascent = (f32)raw_ascent * scale;
    // stbtt 的 descent 是负数，这里取正表示"基线以下的距离"
    font->descent = -(f32)raw_descent * scale;
    font->line_height = ((f32)(raw_ascent - raw_descent + raw_line_gap)) * scale;

    font->glyphs = (glyph_t *)alloc->alloc(alloc, sizeof(glyph_t) * FONT_MAX_GLYPHS, 16);
    if (font->glyphs == NULL) {
        font_destroy(font);
        return NULL;
    }
    font->glyph_capacity = FONT_MAX_GLYPHS;

    if (!hash_init(&font->glyph_map, alloc, 1024)) {
        font_destroy(font);
        return NULL;
    }

    // 收集要光栅化的码点：ASCII 可见字符 + 调用方声明的额外字符
    u32 codepoints[FONT_MAX_GLYPHS];
    u32 codepoint_count = 0;

    for (u32 c = 0x20u; c < 0x7Fu; ++c) {
        codepoints[codepoint_count++] = c;
    }

    if (extra_utf8 != NULL) {
        const char *cursor = extra_utf8;
        while (*cursor != '\0' && codepoint_count < FONT_MAX_GLYPHS) {
            const u32 codepoint = utf8_decode(&cursor);
            if (codepoint == 0 || codepoint == 0xFFFD) {
                continue;
            }
            // 去重：同一个汉字在 extra_utf8 里出现多次很常见
            bool duplicate = false;
            for (u32 i = 0; i < codepoint_count; ++i) {
                if (codepoints[i] == codepoint) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                codepoints[codepoint_count++] = codepoint;
            }
        }
    }

    texture_atlas_t atlas;
    texture_atlas_desc_t atlas_desc = texture_atlas_desc_default();
    atlas_desc.width = font_choose_atlas_size(codepoint_count, pixel_height);
    atlas_desc.height = atlas_desc.width;
    atlas_desc.debug_name = "ttf_atlas";
    if (!texture_atlas_init(&atlas, alloc, &atlas_desc)) {
        font_destroy(font);
        return NULL;
    }

    u32 rasterized = 0;

    for (u32 i = 0; i < codepoint_count; ++i) {
        const u32 codepoint = codepoints[i];

        const int glyph_index = stbtt_FindGlyphIndex(&info, (int)codepoint);
        if (glyph_index == 0 && codepoint != ' ') {
            // 字体里没有这个字形（中文常见：字体不含某个生僻字）
            continue;
        }

        int advance = 0;
        int left_side_bearing = 0;
        stbtt_GetGlyphHMetrics(&info, glyph_index, &advance, &left_side_bearing);

        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetGlyphBitmapBox(&info, glyph_index, scale, scale, &x0, &y0, &x1, &y1);

        const u32 width = (u32)((x1 > x0) ? (x1 - x0) : 0);
        const u32 height = (u32)((y1 > y0) ? (y1 - y0) : 0);

        glyph_t glyph;
        memset(&glyph, 0, sizeof(glyph));
        glyph.advance = (f32)advance * scale;
        glyph.offset_x = (f32)x0;
        // y0 是"字形顶部相对基线的偏移"，y 轴向下所以是负数；
        // 取反得到"字形顶部在基线上方多少像素"，与位图字体一致。
        glyph.offset_y = -(f32)y0;
        glyph.width = (f32)width;
        glyph.height = (f32)height;

        if (width == 0 || height == 0) {
            // 空格之类的空白字形：只有 advance，没有位图。
            // UV 留成 0，绘制时按 width/height == 0 跳过。
            glyph.uv_min = vec2_zero();
            glyph.uv_max = vec2_zero();
            font_add_glyph(font, codepoint, &glyph);
            continue;
        }

        atlas_region_t region;
        if (!texture_atlas_alloc_region(&atlas, width, height, NULL, &region)) {
            LOG_WARN("字体图集（%ux%u）放不下码点 U+%04X，"
                     "请减小字号或减少 extra_utf8 的字符数",
                     atlas_desc.width, atlas_desc.height, codepoint);
            continue;
        }

        // 光栅化成 8 位覆盖度，再扩成 RGBA（RGB=白，A=覆盖度）
        const u32 cell_bytes = width * height;
        u8 *coverage = (u8 *)alloc->alloc(alloc, cell_bytes, 16);
        u8 *rgba = (u8 *)alloc->alloc(alloc, (usize)cell_bytes * 4u, 16);
        if (coverage == NULL || rgba == NULL) {
            if (coverage != NULL) {
                alloc->free(alloc, coverage, cell_bytes);
            }
            if (rgba != NULL) {
                alloc->free(alloc, rgba, (usize)cell_bytes * 4u);
            }
            continue;
        }

        memset(coverage, 0, cell_bytes);
        stbtt_MakeGlyphBitmap(&info, coverage, (int)width, (int)height, (int)width, scale, scale,
                              glyph_index);

        for (u32 p = 0; p < cell_bytes; ++p) {
            rgba[p * 4u + 0u] = 255;
            rgba[p * 4u + 1u] = 255;
            rgba[p * 4u + 2u] = 255;
            rgba[p * 4u + 3u] = coverage[p];
        }

        texture_atlas_write_region(&atlas, &region, rgba, width, height);

        alloc->free(alloc, coverage, cell_bytes);
        alloc->free(alloc, rgba, (usize)cell_bytes * 4u);

        glyph.uv_min = region.uv_min;
        glyph.uv_max = region.uv_max;

        font_add_glyph(font, codepoint, &glyph);
        rasterized++;
    }

    if (rasterized == 0) {
        LOG_ERROR("没有光栅化出任何字形");
        texture_atlas_shutdown(&atlas);
        font_destroy(font);
        return NULL;
    }

    if (renderer == NULL) {
        texture_atlas_shutdown(&atlas);
        font->texture = TEXTURE_HANDLE_INVALID;
        LOG_INFO("TTF 字体就绪（无 GPU 纹理）: %.0f 像素，%u 个字形", (f64)pixel_height,
                 rasterized);
        return font;
    }

    if (!texture_atlas_upload(renderer, &atlas)) {
        texture_atlas_shutdown(&atlas);
        font_destroy(font);
        return NULL;
    }
    font->texture = texture_atlas_texture(&atlas);
    texture_atlas_shutdown(&atlas);

    LOG_INFO("TTF 字体就绪: %.0f 像素，%u 个字形（图集 %ux%u）", (f64)pixel_height, rasterized,
             atlas_desc.width, atlas_desc.height);
    return font;
}

font_t *font_create_from_ttf_file(renderer_t *renderer, allocator_t *alloc, const char *path,
                                  f32 pixel_height, const char *extra_utf8, i32 font_index)
{
    ASSERT(renderer != NULL);
    ASSERT(alloc != NULL);

    if (path == NULL) {
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开字体文件: %s", path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length <= 0) {
        LOG_ERROR("字体文件为空: %s", path);
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    // 字体数据在整个创建过程中都要有效（stb_truetype 会持有指针），
    // 所以用堆分配并在最后释放，不能用会被 reset 的 arena。
    void *data = alloc->alloc(alloc, (usize)length, 16);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }

    const usize read = fread(data, 1, (usize)length, file);
    fclose(file);

    if (read != (usize)length) {
        LOG_ERROR("字体文件读取不完整 (%llu / %ld)", (unsigned long long)read, length);
        alloc->free(alloc, data, (usize)length);
        return NULL;
    }

    font_t *font = font_create_from_ttf(renderer, alloc, data, (usize)length, pixel_height,
                                        extra_utf8, font_index);

    alloc->free(alloc, data, (usize)length);
    if (font != NULL) {
        font->name = path;
    }
    return font;
}

void font_destroy(font_t *font)
{
    if (font == NULL) {
        return;
    }

    allocator_t *alloc = font->alloc;

    hash_destroy(&font->glyph_map);

    if (font->glyphs != NULL && alloc != NULL) {
        alloc->free(alloc, font->glyphs, sizeof(glyph_t) * font->glyph_capacity);
    }

    // 纹理由 renderer 拥有，这里不销毁 —— font_t 只是引用了它。
    // 调用方要在 renderer 还活着的时候销毁字体（见 font.h 的说明）。

    if (alloc != NULL) {
        alloc->free(alloc, font, sizeof(font_t));
    }
}

// =============================================================================
//  查询
// =============================================================================
texture_handle_t font_texture(const font_t *font)
{
    ASSERT(font != NULL);
    return font->texture;
}

f32 font_pixel_height(const font_t *font)
{
    ASSERT(font != NULL);
    return font->pixel_height;
}

f32 font_line_height(const font_t *font)
{
    ASSERT(font != NULL);
    return font->line_height;
}

f32 font_ascent(const font_t *font)
{
    ASSERT(font != NULL);
    return font->ascent;
}

f32 font_descent(const font_t *font)
{
    ASSERT(font != NULL);
    return font->descent;
}

u32 font_glyph_count(const font_t *font)
{
    ASSERT(font != NULL);
    return font->glyph_count;
}

bool font_is_bitmap(const font_t *font)
{
    ASSERT(font != NULL);
    return font->is_bitmap;
}

const char *font_name(const font_t *font)
{
    ASSERT(font != NULL);
    return (font->name != NULL) ? font->name : "<font>";
}

const glyph_t *font_glyph(const font_t *font, u32 codepoint)
{
    ASSERT(font != NULL);

    u64 index = 0;
    if (!hash_get(&font->glyph_map, (u64)codepoint, &index)) {
        return NULL;
    }
    if (index >= font->glyph_count) {
        return NULL;
    }
    return &font->glyphs[index];
}
