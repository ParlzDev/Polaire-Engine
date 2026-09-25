// =============================================================================
//  engine/render/font.h —— 字体（位图 / TrueType）
//
//  ---- 两条创建路径，同一个数据结构 ----
//
//    1. font_create_bitmap8x8()   内置 8x8 位图字体（公有领域，见 third_party）
//       优点：不需要任何外部文件，永远能用。适合调试文字、英文 UI。
//       缺点：只有 ASCII，且字号固定。
//
//    2. font_create_from_ttf()    用 stb_truetype 光栅化 TrueType
//       优点：任意字号、支持中文（只要字体文件里有对应字形）。
//       缺点：需要字体文件；字形是**预先**光栅化进图集的。
//
//  ---- 关于"预光栅化"的限制（重要） ----
//
//  TTF 路径把 `字符集` 在创建时一次性光栅化进图集，图集大小固定。
//  所以创建时必须告诉它"要用到哪些非 ASCII 字符"。
//
//  对游戏 UI 来说这是合适的：按钮文字、菜单项、提示语都是固定的那几十个词。
//  真正需要"任意动态文本"（例如玩家昵称、聊天）时，需要按需光栅化 +
//  图集增长 + 重新上传 —— 那是 M7 资源层之后的事。
//
//  这个限制是**有意的**：按需光栅化会让 text_draw 变成"可能触发 GPU 上传"
//  的操作，破坏"帧内零 malloc"和"绘制阶段不做资源创建"这两条约定。
// =============================================================================
#ifndef ENGINE_RENDER_FONT_H
#define ENGINE_RENDER_FONT_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/render_types.h"
#include "engine/render/texture_atlas.h"

typedef struct renderer_t renderer_t;
typedef struct font_t font_t;

typedef struct glyph_t {
    vec2_t uv_min;
    vec2_t uv_max;
    f32    width;     // 字形位图宽（像素）
    f32    height;    // 字形位图高（像素）
    f32    offset_x;  // 相对笔位置的左上角偏移
    f32    offset_y;
    f32    advance;   // 画完这个字形后笔前进多少
} glyph_t;

// ---------------------------------------------------------------- 创建
//
// 内置 8x8 位图字体。包含 ASCII 0x00-0x7F。
font_t *font_create_bitmap8x8(renderer_t *renderer, allocator_t *alloc);

// 从内存中的 TTF/TTC 数据创建。
//
// pixel_height 是"字形高度"（近似字号），不是行高。
// extra_utf8 是"除 ASCII 之外还要预光栅化的字符"，UTF-8 编码，可为 NULL。
//   中文 UI 就把界面上会用到的汉字都写进去。
//
// font_index 用于 .ttc（字体集合）：0 = 第一个字体。非 ttc 传 0。
font_t *font_create_from_ttf(renderer_t *renderer, allocator_t *alloc, const void *ttf_data,
                             usize ttf_size, f32 pixel_height, const char *extra_utf8,
                             i32 font_index);

// 从文件创建（内部读文件）
font_t *font_create_from_ttf_file(renderer_t *renderer, allocator_t *alloc, const char *path,
                                  f32 pixel_height, const char *extra_utf8, i32 font_index);

void font_destroy(font_t *font);

// ---------------------------------------------------------------- 查询
texture_handle_t font_texture(const font_t *font);
f32              font_pixel_height(const font_t *font);
f32              font_line_height(const font_t *font);
f32              font_ascent(const font_t *font);
f32              font_descent(const font_t *font);
u32              font_glyph_count(const font_t *font);
bool             font_is_bitmap(const font_t *font);
const char      *font_name(const font_t *font);

// 查字形。找不到返回 NULL（调用方通常退化成画一个方框）。
const glyph_t *font_glyph(const font_t *font, u32 codepoint);

// ---------------------------------------------------------------- UTF-8
// 解码一个 UTF-8 码点，并把 cursor 前移。
// 非法序列返回 0xFFFD（替换字符）并只前进 1 字节 —— 保证不会死循环。
u32 utf8_decode(const char **cursor);

// 计算 UTF-8 字符串的码点个数（不包含结尾 NUL）
u32 utf8_length(const char *text);

// 把码点编码成 UTF-8，写入 out（至少 4 字节），返回写入的字节数
u32 utf8_encode(u32 codepoint, char out[4]);

#endif // ENGINE_RENDER_FONT_H
