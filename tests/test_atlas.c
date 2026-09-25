// =============================================================================
//  tests/test_atlas.c —— 纹理图集、UTF-8、字体度量
//
//  图集的 UV 计算与打包逻辑错了不会崩，只会让精灵"边缘有一圈杂色"
//  或者"采到隔壁的图"—— 这类问题靠肉眼看很难定位，必须有测试兜住。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// =============================================================================
//  图集
// =============================================================================
static void atlas_init_and_shutdown_without_leak(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 256;
    desc.height = 256;

    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));
    EXPECT_EQ_U64(atlas.width, 256u);
    EXPECT_EQ_U64(atlas.height, 256u);
    EXPECT_EQ_U64(texture_atlas_region_count(&atlas), 0u);
    EXPECT_NEAR(texture_atlas_used_ratio(&atlas), 0.0f, 0.0f);
    EXPECT_NOT_NULL(atlas.pixels); // CPU 画布必须已分配

    texture_atlas_shutdown(&atlas);

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
    EXPECT_NULL(atlas.pixels);
}

static void atlas_uv_has_half_pixel_inset(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 256;
    desc.height = 256;
    desc.padding = 2;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    const u8 pixels[16 * 16 * 4] = {255};

    atlas_region_t region;
    EXPECT_TRUE(texture_atlas_add(&atlas, pixels, 16, 16, "test", &region));

    EXPECT_EQ_U64(region.width, 16u);
    EXPECT_EQ_U64(region.height, 16u);

    // 第一个区域落在 padding 之后
    EXPECT_EQ_U64(region.x, atlas.padding);
    EXPECT_EQ_U64(region.y, atlas.padding);

    // UV 的半像素内缩：uv_min * W - 0.5 == region.x
    // 这是"采样点永远落在区域内部"的保证。少了这半个纹素，
    // 双线性采样会在边缘混进 padding 的像素 —— 精灵边缘一圈杂色。
    EXPECT_NEAR(region.uv_min.x * (f32)atlas.width - 0.5f, (f32)region.x, 1e-4f);
    EXPECT_NEAR(region.uv_min.y * (f32)atlas.height - 0.5f, (f32)region.y, 1e-4f);
    EXPECT_NEAR(region.uv_max.x * (f32)atlas.width + 0.5f, (f32)(region.x + region.width),
                1e-4f);
    EXPECT_NEAR(region.uv_max.y * (f32)atlas.height + 0.5f, (f32)(region.y + region.height),
                1e-4f);

    // UV 必须在 [0,1] 内
    EXPECT_TRUE(region.uv_min.x > 0.0f && region.uv_max.x < 1.0f);
    EXPECT_TRUE(region.uv_min.y > 0.0f && region.uv_max.y < 1.0f);

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void atlas_regions_do_not_overlap(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 256;
    desc.height = 256;
    desc.padding = 2;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    // 缓冲区要按**最大**区域来开：下面的循环会请求到 40x40，
    // 而 texture_atlas_add 是按 width*height*4 拷贝的（它无从知道缓冲区实际多大）。
    // 这里以前按 32x32 开，40x40 那次就多读了 6400-4096 字节的栈 —— ASan 报的
    // stack-buffer-overflow 就是这个。
    const u8 pixels[40 * 40 * 4] = {255};

    // 放一堆大小不一的区域
    for (u32 i = 0; i < 40; ++i) {
        const u32 size = 8u + (i % 5u) * 8u;
        atlas_region_t region;
        if (!texture_atlas_add(&atlas, pixels, size, size, NULL, &region)) {
            break;
        }
    }

    const u32 count = texture_atlas_region_count(&atlas);
    EXPECT_GT_U64(count, 10u);

    // 两两检查：任意两个区域不能重叠（含 padding 的间隔）
    for (u32 i = 0; i < count; ++i) {
        const atlas_region_t *a = texture_atlas_region(&atlas, i);
        for (u32 j = i + 1; j < count; ++j) {
            const atlas_region_t *b = texture_atlas_region(&atlas, j);

            const bool separated = (a->x + a->width <= b->x) || (b->x + b->width <= a->x) ||
                                   (a->y + a->height <= b->y) || (b->y + b->height <= a->y);
            EXPECT_TRUE(separated);
            if (!separated) {
                break;
            }
        }
    }

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void atlas_rejects_oversized_regions(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 128;
    desc.height = 128;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    atlas_region_t region;

    // 比整张图还大 —— 必须失败而不是写越界
    EXPECT_FALSE(texture_atlas_alloc_region(&atlas, 256, 16, NULL, &region));
    EXPECT_FALSE(texture_atlas_alloc_region(&atlas, 16, 256, NULL, &region));

    // 尺寸为 0 也要拒绝
    EXPECT_FALSE(texture_atlas_alloc_region(&atlas, 0, 16, NULL, &region));
    EXPECT_FALSE(texture_atlas_alloc_region(&atlas, 16, 0, NULL, &region));

    // 正常尺寸可以放
    EXPECT_TRUE(texture_atlas_alloc_region(&atlas, 64, 64, NULL, &region));

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void atlas_exhaustion_is_reported(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 64;
    desc.height = 64;
    desc.padding = 1;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    // 64x64 的图最多放 4 个 30x30 的区域（每个占 32x32）
    u32 placed = 0;
    for (u32 i = 0; i < 100; ++i) {
        atlas_region_t region;
        if (texture_atlas_alloc_region(&atlas, 30, 30, NULL, &region)) {
            placed++;
        }
    }

    EXPECT_GT_U64(placed, 0u);
    EXPECT_LE_U64(placed, 4u);
    EXPECT_TRUE(texture_atlas_used_ratio(&atlas) <= 1.0f);

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void atlas_write_region_copies_pixels(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 64;
    desc.height = 64;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    atlas_region_t region;
    EXPECT_TRUE(texture_atlas_alloc_region(&atlas, 4, 4, NULL, &region));

    u8 source[4 * 4 * 4];
    for (u32 i = 0; i < 16; ++i) {
        source[i * 4 + 0] = (u8)i;
        source[i * 4 + 1] = 0;
        source[i * 4 + 2] = 0;
        source[i * 4 + 3] = 255;
    }

    texture_atlas_write_region(&atlas, &region, source, 4, 4);

    // 逐像素校验写入位置
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8 *pixel =
                &atlas.pixels[(((usize)(region.y + y) * atlas.width) + region.x + x) * 4u];
            EXPECT_EQ_U64(pixel[0], y * 4 + x);
            EXPECT_EQ_U64(pixel[3], 255u);
        }
    }

    // 写超出区域尺寸时只拷贝能放下的部分，不能越界。
    // 源必须真的按 16x16 开：write_region 用 width 做行 stride 去读源，
    // 给它一个 4x4 的缓冲区却说 16x16，越界的是**测试自己**（ASan 抓到的就是这个）。
    // 被拷验的是"源比区域大时目标不能溢出"，所以源开大才是本意。
    u8 oversized_source[16 * 16 * 4];
    for (u32 i = 0; i < 16u * 16u; ++i) {
        oversized_source[i * 4 + 0] = 0u;
        oversized_source[i * 4 + 1] = 255u;
        oversized_source[i * 4 + 2] = 0u;
        oversized_source[i * 4 + 3] = 255u;
    }
    texture_atlas_write_region(&atlas, &region, oversized_source, 16, 16);

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void atlas_solid_region_is_one_pixel(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    texture_atlas_t atlas;
    texture_atlas_desc_t desc = texture_atlas_desc_default();
    desc.width = 32;
    desc.height = 32;
    EXPECT_TRUE(texture_atlas_init(&atlas, &tracker.base, &desc));

    EXPECT_TRUE(texture_atlas_make_solid(&atlas, 255, 128, 64, 255));

    const atlas_region_t *region = texture_atlas_region(&atlas, 0);
    EXPECT_NOT_NULL(region);
    EXPECT_EQ_U64(region->width, 1u);
    EXPECT_EQ_U64(region->height, 1u);

    texture_atlas_shutdown(&atlas);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// =============================================================================
//  UTF-8
// =============================================================================
static void utf8_decodes_all_lengths(void)
{
    // 单字节（ASCII）
    {
        const char *cursor = "A";
        EXPECT_EQ_U64(utf8_decode(&cursor), 0x41u);
        EXPECT_EQ_U64(*cursor, 0u);
    }

    // 双字节（U+00E9 = é）
    {
        const char *cursor = "\xC3\xA9";
        EXPECT_EQ_U64(utf8_decode(&cursor), 0xE9u);
    }

    // 三字节（U+4E2D = 中）
    {
        const char *cursor = "\xE4\xB8\xAD";
        EXPECT_EQ_U64(utf8_decode(&cursor), 0x4E2Du);
    }

    // 四字节（U+1F600 = 😀）
    {
        const char *cursor = "\xF0\x9F\x98\x80";
        EXPECT_EQ_U64(utf8_decode(&cursor), 0x1F600u);
    }

    // 结束符返回 0
    {
        const char *cursor = "";
        EXPECT_EQ_U64(utf8_decode(&cursor), 0u);
    }
}

static void utf8_rejects_malformed_sequences(void)
{
    // 非法序列必须返回替换字符，并且**只前进 1 字节** ——
    // 如果按声称的长度前进，一个坏字节就能让解码循环跳过后续字符甚至卡住。
    // 注意 char 在 MSVC 上是**带符号**的，0xB8 直接转 u64 会变成天文数字。
    // 比较字节值时必须显式转成 u8。
    const char *cursor = "\xFF\x41";
    EXPECT_EQ_U64(utf8_decode(&cursor), 0xFFFDu);
    EXPECT_EQ_U64((u8)*cursor, 0x41u); // 停在下一个合法字符上

    // 截断的三字节序列
    const char *truncated = "\xE4\xB8";
    EXPECT_EQ_U64(utf8_decode(&truncated), 0xFFFDu);
    EXPECT_EQ_U64((u8)*truncated, 0xB8u); // 只前进 1 字节

    // NULL 安全
    EXPECT_EQ_U64(utf8_decode(NULL), 0u);
}

static void utf8_length_counts_codepoints(void)
{
    EXPECT_EQ_U64(utf8_length(""), 0u);
    EXPECT_EQ_U64(utf8_length("hello"), 5u);
    EXPECT_EQ_U64(utf8_length("你好"), 2u);
    EXPECT_EQ_U64(utf8_length("a中b"), 3u);
    EXPECT_EQ_U64(utf8_length(NULL), 0u);
}

static void utf8_encode_roundtrip(void)
{
    const u32 codepoints[] = {0x41u, 0xE9u, 0x4E2Du, 0x1F600u};

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(codepoints); ++i) {
        char buffer[4] = {0};
        const u32 written = utf8_encode(codepoints[i], buffer);
        EXPECT_GT_U64(written, 0u);

        const char *cursor = buffer;
        EXPECT_EQ_U64(utf8_decode(&cursor), codepoints[i]);
    }
}

// =============================================================================
//  字体（不需要 GL 上下文：renderer 传 NULL 表示"只要度量"）
// =============================================================================
static void bitmap_font_has_ascii_glyphs(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    font_t *font = font_create_bitmap8x8(NULL, &tracker.base);
    EXPECT_NOT_NULL(font);

    if (font != NULL) {
        EXPECT_TRUE(font_is_bitmap(font));
        EXPECT_EQ_U64(font_glyph_count(font), 128u);
        EXPECT_NEAR(font_pixel_height(font), 8.0f, 0.0f);
        EXPECT_GT_U64((u64)(font_line_height(font) * 100.0f), 0u);

        // 常用字符必须有字形
        EXPECT_NOT_NULL(font_glyph(font, 'A'));
        EXPECT_NOT_NULL(font_glyph(font, 'z'));
        EXPECT_NOT_NULL(font_glyph(font, '0'));
        EXPECT_NOT_NULL(font_glyph(font, ' '));

        // 字形度量
        const glyph_t *glyph = font_glyph(font, 'A');
        EXPECT_NEAR(glyph->width, 8.0f, 0.0f);
        EXPECT_NEAR(glyph->height, 8.0f, 0.0f);
        EXPECT_NEAR(glyph->advance, 8.0f, 0.0f);
        // UV 必须在 [0,1]
        EXPECT_TRUE(glyph->uv_min.x >= 0.0f && glyph->uv_max.x <= 1.0f);
        EXPECT_TRUE(glyph->uv_min.y >= 0.0f && glyph->uv_max.y <= 1.0f);

        // 中文没有字形（8x8 只有 ASCII）
        EXPECT_NULL(font_glyph(font, 0x4E2Du));

        font_destroy(font);
    }

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void text_measurement_is_consistent(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    font_t *font = font_create_bitmap8x8(NULL, &tracker.base);
    EXPECT_NOT_NULL(font);

    if (font != NULL) {
        text_style_t style = text_style_default(font);

        // 8x8 等宽字体：N 个字符宽度 = 8N
        EXPECT_NEAR(text_measure_line(&style, "AAAA", 0), 32.0f, 1e-4f);
        EXPECT_NEAR(text_measure_line(&style, "", 0), 0.0f, 1e-4f);

        // scale 线性缩放宽度
        style.scale = 2.0f;
        EXPECT_NEAR(text_measure_line(&style, "AAAA", 0), 64.0f, 1e-4f);
        style.scale = 1.0f;

        // 多行：宽度取最长行，高度 = 行数 * 行高
        const vec2_t size = text_measure(&style, "AB\nABCD");
        EXPECT_NEAR(size.x, 32.0f, 1e-4f);
        EXPECT_NEAR(size.y, 2.0f * text_line_height(&style), 1e-4f);

        // 行数统计
        EXPECT_EQ_U64(text_line_count(""), 0u);
        EXPECT_EQ_U64(text_line_count("abc"), 1u);
        EXPECT_EQ_U64(text_line_count("a\nb"), 2u);
        EXPECT_EQ_U64(text_line_count("a\nb\nc"), 3u);

        // 行间距
        style.line_spacing = 4.0f;
        EXPECT_NEAR(text_line_height(&style), font_line_height(font) + 4.0f, 1e-4f);

        // 空文本的度量是零
        const vec2_t empty = text_measure(&style, "");
        EXPECT_NEAR(empty.x, 0.0f, 0.0f);
        EXPECT_NEAR(empty.y, 0.0f, 0.0f);

        font_destroy(font);
    }

    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

void test_atlas_register(void)
{
    test_register("atlas", "atlas_init_and_shutdown_without_leak",
                  atlas_init_and_shutdown_without_leak);
    test_register("atlas", "atlas_uv_has_half_pixel_inset", atlas_uv_has_half_pixel_inset);
    test_register("atlas", "atlas_regions_do_not_overlap", atlas_regions_do_not_overlap);
    test_register("atlas", "atlas_rejects_oversized_regions", atlas_rejects_oversized_regions);
    test_register("atlas", "atlas_exhaustion_is_reported", atlas_exhaustion_is_reported);
    test_register("atlas", "atlas_write_region_copies_pixels", atlas_write_region_copies_pixels);
    test_register("atlas", "atlas_solid_region_is_one_pixel", atlas_solid_region_is_one_pixel);

    test_register("atlas", "utf8_decodes_all_lengths", utf8_decodes_all_lengths);
    test_register("atlas", "utf8_rejects_malformed_sequences", utf8_rejects_malformed_sequences);
    test_register("atlas", "utf8_length_counts_codepoints", utf8_length_counts_codepoints);
    test_register("atlas", "utf8_encode_roundtrip", utf8_encode_roundtrip);

    test_register("atlas", "bitmap_font_has_ascii_glyphs", bitmap_font_has_ascii_glyphs);
    test_register("atlas", "text_measurement_is_consistent", text_measurement_is_consistent);
}
