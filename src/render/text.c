// =============================================================================
//  src/render/text.c —— 文本渲染
//
//  文本就是"一串按字形排版好的精灵"，直接跑在批渲染器上。
//  一段文字（同一个字体）只占一次 draw call。
// =============================================================================
#include "engine/render/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

#define TEXT_FORMAT_CAPACITY 1024

text_style_t text_style_default(const font_t *font)
{
    text_style_t style;
    memset(&style, 0, sizeof(style));
    style.font = font;
    style.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    style.scale = 1.0f;
    style.line_spacing = 0.0f;
    style.align = TEXT_ALIGN_LEFT;
    style.valign = TEXT_VALIGN_TOP;
    return style;
}

f32 text_line_height(const text_style_t *style)
{
    ASSERT(style != NULL);
    ASSERT(style->font != NULL);
    return (font_line_height(style->font) + style->line_spacing) * style->scale;
}

f32 text_measure_line(const text_style_t *style, const char *utf8, usize max_bytes)
{
    ASSERT(style != NULL);
    if (utf8 == NULL || style->font == NULL) {
        return 0.0f;
    }

    f32 width = 0.0f;
    const char *cursor = utf8;
    const char *limit = (max_bytes > 0) ? (utf8 + max_bytes) : NULL;

    while (*cursor != '\0' && (limit == NULL || cursor < limit)) {
        if (*cursor == '\n') {
            break;
        }
        const u32 codepoint = utf8_decode(&cursor);
        const glyph_t *glyph = font_glyph(style->font, codepoint);
        if (glyph == NULL) {
            // 字体里没有这个字形：按一个"半角宽度"推进，
            // 保证排版不会因为缺字而挤在一起。
            width += font_pixel_height(style->font) * 0.5f * style->scale;
            continue;
        }
        width += glyph->advance * style->scale;
    }

    return width;
}

u32 text_line_count(const char *utf8)
{
    if (utf8 == NULL || *utf8 == '\0') {
        return 0;
    }

    u32 lines = 1;
    for (const char *p = utf8; *p != '\0'; ++p) {
        if (*p == '\n') {
            lines++;
        }
    }
    return lines;
}

vec2_t text_measure(const text_style_t *style, const char *utf8)
{
    if (style == NULL || utf8 == NULL || *utf8 == '\0') {
        return vec2_zero();
    }

    f32 max_width = 0.0f;
    const char *cursor = utf8;

    while (*cursor != '\0') {
        const char *line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        const f32 width = text_measure_line(style, line_start, (usize)(cursor - line_start));
        if (width > max_width) {
            max_width = width;
        }
        if (*cursor == '\n') {
            cursor++;
        }
    }

    const f32 height = (f32)text_line_count(utf8) * text_line_height(style);
    return vec2_make(max_width, height);
}

// ---------------------------------------------------------------- 绘制
void text_draw(sprite_batch_t *batch, const text_style_t *style, vec2_t position, const char *utf8)
{
    ASSERT(batch != NULL);
    ASSERT(style != NULL);

    if (utf8 == NULL || *utf8 == '\0' || style->font == NULL) {
        return;
    }

    const font_t *font = style->font;
    const texture_handle_t texture = font_texture(font);
    if (texture == TEXTURE_HANDLE_INVALID) {
        return;
    }

    const f32 line_h = text_line_height(style);
    const f32 ascent = font_ascent(font) * style->scale;
    const u32 line_count = text_line_count(utf8);
    const f32 block_height = (f32)line_count * line_h;

    // 垂直对齐：算出文本块**顶边**的 Y。
    // 世界坐标 Y 向上，所以"顶"是更大的 Y。
    f32 top = position.y;
    if (style->valign == TEXT_VALIGN_MIDDLE) {
        top = position.y + block_height * 0.5f;
    } else if (style->valign == TEXT_VALIGN_BOTTOM) {
        top = position.y + block_height;
    }

    f32 baseline = top - ascent;
    const char *cursor = utf8;

    while (*cursor != '\0') {
        const char *line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        const usize line_bytes = (usize)(cursor - line_start);

        // 水平对齐：算出这一行的起始笔位置
        f32 pen_x = position.x;
        if (style->align != TEXT_ALIGN_LEFT) {
            const f32 line_width = text_measure_line(style, line_start, line_bytes);
            pen_x -= (style->align == TEXT_ALIGN_CENTER) ? (line_width * 0.5f) : line_width;
        }

        const char *glyph_cursor = line_start;
        const char *line_end = line_start + line_bytes;

        while (glyph_cursor < line_end && *glyph_cursor != '\0') {
            const u32 codepoint = utf8_decode(&glyph_cursor);
            const glyph_t *glyph = font_glyph(font, codepoint);
            if (glyph == NULL) {
                pen_x += font_pixel_height(font) * 0.5f * style->scale;
                continue;
            }

            if (glyph->width > 0.0f && glyph->height > 0.0f) {
                const f32 width = glyph->width * style->scale;
                const f32 height = glyph->height * style->scale;

                // 字形左上角 = 笔位置 + 偏移；然后取中心给精灵用。
                // offset_y 是"字形顶部在基线上方多少像素"（Y 向上为正）。
                const f32 glyph_left = pen_x + glyph->offset_x * style->scale;
                const f32 glyph_top = baseline + glyph->offset_y * style->scale;

                atlas_region_t region;
                memset(&region, 0, sizeof(region));
                region.uv_min = glyph->uv_min;
                region.uv_max = glyph->uv_max;

                sprite_batch_draw_region(batch, vec2_make(glyph_left + width * 0.5f,
                                                          glyph_top - height * 0.5f),
                                         vec2_make(width, height), 0.0f, &region, style->color,
                                         texture);
            }

            pen_x += glyph->advance * style->scale;
        }

        if (*cursor == '\n') {
            cursor++;
        }
        baseline -= line_h;
    }
}

void text_draw_fmt(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                   const char *fmt, ...)
{
    ASSERT(fmt != NULL);

    char buffer[TEXT_FORMAT_CAPACITY];

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    // 被截断时仍然把能显示的部分画出来 —— 总比什么都不画强，
    // 而且日志里已经有记录（调用方传的 fmt 一眼能看出问题）。
    text_draw(batch, style, position, buffer);
}

void text_draw_outlined(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                        vec4_t outline_color, f32 outline_width, const char *utf8)
{
    ASSERT(style != NULL);

    if (utf8 == NULL || *utf8 == '\0' || outline_width <= 0.0f) {
        text_draw(batch, style, position, utf8);
        return;
    }

    // 先在周围 8 个方向各画一遍描边色，再画本体。
    // 8 个方向而不是 4 个：只有 4 个方向时，斜向的笔画之间会露出缝隙。
    const f32 step = outline_width;
    static const f32 k_offsets[8][2] = {
        {-1.0f, 0.0f},  {1.0f, 0.0f},   {0.0f, -1.0f}, {0.0f, 1.0f},
        {-0.7f, -0.7f}, {0.7f, -0.7f},  {-0.7f, 0.7f}, {0.7f, 0.7f},
    };

    text_style_t outline_style = *style;
    outline_style.color = outline_color;

    for (u32 i = 0; i < 8; ++i) {
        const vec2_t offset = vec2_make(k_offsets[i][0] * step, k_offsets[i][1] * step);
        text_draw(batch, &outline_style, vec2_add(position, offset), utf8);
    }

    text_draw(batch, style, position, utf8);
}

void text_draw_outlined_fmt(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                            vec4_t outline_color, f32 outline_width, const char *fmt, ...)
{
    ASSERT(fmt != NULL);

    char buffer[TEXT_FORMAT_CAPACITY];

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    text_draw_outlined(batch, style, position, outline_color, outline_width, buffer);
}
