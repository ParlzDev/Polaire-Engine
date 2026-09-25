// =============================================================================
//  engine/render/text.h —— 文本渲染
//
//  文本就是"一串按字形排版好的精灵"。所以它直接跑在精灵批渲染器上 ——
//  一段文字只占一次 draw call（前提是都在同一个字体图集里，这是必然的）。
//
//  ---- 排版要自己算的三件事 ----
//
//  1. 基线（baseline）：字形位图的左上角相对笔位置的偏移由 font 提供，
//     笔位置本身沿基线前进。换行时笔位置下降 line_height。
//  2. 行间距：`line_height + line_spacing`。中文字体的默认行高往往偏大，
//     所以 line_spacing 允许为负。
//  3. 中英混排：中文字形的 advance 通常是"全角"（= pixel_height），
//     英文是半角。两者混排时字距不齐是**正常的**，不要试图平均化 ——
//     那会让英文单词之间出现奇怪的间隙。
// =============================================================================
#ifndef ENGINE_RENDER_TEXT_H
#define ENGINE_RENDER_TEXT_H

#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/font.h"
#include "engine/render/sprite_batch.h"

typedef enum text_align_t {
    TEXT_ALIGN_LEFT = 0,
    TEXT_ALIGN_CENTER,
    TEXT_ALIGN_RIGHT
} text_align_t;

typedef enum text_valign_t {
    TEXT_VALIGN_TOP = 0,
    TEXT_VALIGN_MIDDLE,
    TEXT_VALIGN_BOTTOM
} text_valign_t;

typedef struct text_style_t {
    const font_t *font;
    vec4_t        color;
    f32           scale;
    f32           line_spacing; // 额外行间距（像素），可为负
    text_align_t  align;
    text_valign_t valign;
} text_style_t;

text_style_t text_style_default(const font_t *font);

// 单行宽度（像素）。遇到 '\n' 停止。
f32 text_measure_line(const text_style_t *style, const char *utf8, usize max_bytes);

// 整个文本块的包围盒（支持多行，'\n' 分隔）
vec2_t text_measure(const text_style_t *style, const char *utf8);

f32 text_line_height(const text_style_t *style);
u32 text_line_count(const char *utf8);

// 绘制。position 的含义由 style->align / valign 决定：
//   左对齐时 position.x 是文本左边缘，右对齐时是右边缘，居中时是中心。
//   垂直同理。这样调用方不用自己算偏移。
void text_draw(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
               const char *utf8);

void text_draw_fmt(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                   const char *fmt, ...) ENGINE_PRINTF_FMT(4, 5);

// 带描边的绘制（先画一圈偏移的黑色文本，再画本体）。
// 2D 游戏里文字压在复杂背景上时几乎必须描边，否则完全看不清。
void text_draw_outlined(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                        vec4_t outline_color, f32 outline_width, const char *utf8);

void text_draw_outlined_fmt(sprite_batch_t *batch, const text_style_t *style, vec2_t position,
                            vec4_t outline_color, f32 outline_width, const char *fmt, ...)
    ENGINE_PRINTF_FMT(6, 7);

#endif // ENGINE_RENDER_TEXT_H
