// =============================================================================
//  tests/test_render_target.c —— 渲染目标的尺寸/格式推导
//
//  为什么这些"看起来只是填默认值"的逻辑值得单独测：
//
//  它们原本写在 GL 后端的 create_render_target 里，于是**只有开了 GL 上下文
//  才能验证**。而它们恰恰是最容易写错、错了又完全不报错的一类：
//    * 忘了把深度专用目标的 samples 压回 1 -> 去申请多重采样的深度纹理，
//      驱动报 GL_INVALID_OPERATION，帧缓冲不完整，画面全黑；
//    * 颜色格式填成了深度格式 -> 把深度纹理挂到 COLOR_ATTACHMENT0，
//      帧缓冲不完整，同样是静默全黑；
//    * MSAA 传了 3 -> GL_INVALID_VALUE，同样是全黑。
//
//  抽成纯函数之后，"给一组输入、要什么输出"就能在没有窗口的情况下测清楚。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include "engine/render/render_types.h"

// ---------------------------------------------------------------- 像素格式

static void pixel_format_properties(void)
{
    // 每像素字节数
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8_UNORM), 1);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8G8_UNORM), 2);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R8G8B8A8_UNORM), 4);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_R16G16B16A16_FLOAT), 8);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_D32_FLOAT), 4);
    EXPECT_EQ_U64(pixel_format_bytes_per_pixel(PIXEL_FORMAT_INVALID), 0);

    // sRGB 标记：只有这两个格式会触发硬件的自动解码。
    // 判错了的表现是"贴图整体偏暗/偏亮"，肉眼很难判断。
    EXPECT_TRUE(pixel_format_is_srgb(PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB));
    EXPECT_TRUE(pixel_format_is_srgb(PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB));
    EXPECT_FALSE(pixel_format_is_srgb(PIXEL_FORMAT_R8G8B8A8_UNORM));
    EXPECT_FALSE(pixel_format_is_srgb(PIXEL_FORMAT_INVALID));

    // 深度格式
    EXPECT_TRUE(pixel_format_is_depth(PIXEL_FORMAT_D32_FLOAT));
    EXPECT_TRUE(pixel_format_is_depth(PIXEL_FORMAT_D24_UNORM_S8_UINT));
    EXPECT_FALSE(pixel_format_is_depth(PIXEL_FORMAT_R8G8B8A8_UNORM));
    EXPECT_FALSE(pixel_format_is_depth(PIXEL_FORMAT_INVALID));
}

// ---------------------------------------------------------------- 默认描述

static void render_target_default_desc(void)
{
    const render_target_desc_t d = render_target_desc_default();

    EXPECT_TRUE(d.width > 0);
    EXPECT_TRUE(d.height > 0);
    EXPECT_FALSE(pixel_format_is_depth(d.color_format));
    EXPECT_TRUE(pixel_format_is_depth(d.depth_format));
    EXPECT_EQ_U64(d.samples, 1);
    EXPECT_TRUE(d.has_depth);
    EXPECT_TRUE(d.sampleable_color);
    EXPECT_FALSE(d.depth_only);
    EXPECT_NOT_NULL(d.debug_name);

    // 默认描述归一化之后应该原样不变 —— 变了说明默认值和规则自相矛盾
    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_EQ_U64(norm.width, d.width);
    EXPECT_EQ_U64(norm.height, d.height);
    EXPECT_EQ_U64(norm.color_format, d.color_format);
    EXPECT_EQ_U64(norm.depth_format, d.depth_format);
    EXPECT_EQ_U64(norm.samples, d.samples);
}

// ---------------------------------------------------------------- 归一化

static void render_target_normalize_null_and_zero(void)
{
    // NULL 和 0 尺寸都返回"全 0"，让调用方据此判断失败。
    // 不在这里补一个默认尺寸：调用方传 0 通常意味着它算错了
    // （比如拿了一个还没初始化的视口），悄悄给 1280x720 会让错误传得很远。
    const render_target_desc_t from_null = render_target_desc_normalize(NULL);
    EXPECT_EQ_U64(from_null.width, 0);
    EXPECT_EQ_U64(from_null.height, 0);

    render_target_desc_t d = render_target_desc_default();
    d.width = 0;
    const render_target_desc_t zero_w = render_target_desc_normalize(&d);
    EXPECT_EQ_U64(zero_w.width, 0);

    d = render_target_desc_default();
    d.height = 0;
    const render_target_desc_t zero_h = render_target_desc_normalize(&d);
    EXPECT_EQ_U64(zero_h.height, 0);
}

static void render_target_normalize_fills_formats(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.color_format = PIXEL_FORMAT_INVALID;
    d.depth_format = PIXEL_FORMAT_INVALID;

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_FALSE(pixel_format_is_depth(norm.color_format));
    EXPECT_EQ_U64(norm.color_format, PIXEL_FORMAT_R8G8B8A8_UNORM);
    EXPECT_TRUE(pixel_format_is_depth(norm.depth_format));
    EXPECT_EQ_U64(norm.depth_format, PIXEL_FORMAT_D24_UNORM_S8_UINT);
}

// 把深度格式填进 color_format 会让帧缓冲静默不完整。
static void render_target_normalize_rejects_mismatched_formats(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.color_format = PIXEL_FORMAT_D32_FLOAT; // 颜色位置给了深度格式
    d.depth_format = PIXEL_FORMAT_R8G8B8A8_UNORM; // 深度位置给了颜色格式

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_FALSE(pixel_format_is_depth(norm.color_format));
    EXPECT_TRUE(pixel_format_is_depth(norm.depth_format));
}

static void render_target_normalize_samples(void)
{
    // 合法档位原样保留
    for (u32 s = 1; s <= 16; s <<= 1) {
        render_target_desc_t d = render_target_desc_default();
        d.samples = s;
        EXPECT_EQ_U64(render_target_desc_normalize(&d).samples, s);
    }

    // 非法档位一律回落到 1。
    // 不"就近取整"：把 3 悄悄变成 4 会让调用方以为拿到了他要的采样数。
    const u32 invalid[] = {0, 3, 5, 6, 7, 9, 12, 32, 1000};
    for (usize i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        render_target_desc_t d = render_target_desc_default();
        d.samples = invalid[i];
        EXPECT_EQ_U64(render_target_desc_normalize(&d).samples, 1);
    }
}

// 深度专用（阴影贴图）必须自洽：有深度、可采样、不要 MSAA。
static void render_target_normalize_depth_only(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.width = 2048;
    d.height = 2048;
    d.depth_only = true;
    d.depth_format = PIXEL_FORMAT_D32_FLOAT;
    // 故意填一堆矛盾的值，归一化必须全部纠正
    d.has_depth = false;
    d.sampleable_color = false;
    d.samples = 4;

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_TRUE(norm.depth_only);
    EXPECT_TRUE(norm.has_depth);
    EXPECT_TRUE(norm.sampleable_color);
    EXPECT_EQ_U64(norm.samples, 1);
    EXPECT_EQ_U64(norm.depth_format, PIXEL_FORMAT_D32_FLOAT);
    EXPECT_EQ_U64(norm.width, 2048);
    EXPECT_EQ_U64(norm.height, 2048);

    // 幂等：再归一化一次不变
    const render_target_desc_t again = render_target_desc_normalize(&norm);
    EXPECT_EQ_U64(again.samples, norm.samples);
    EXPECT_EQ_U64(again.depth_format, norm.depth_format);
    EXPECT_EQ_U64(again.sampleable_color, norm.sampleable_color);
}

// 不要深度附件时把格式清成 INVALID，下游就不用再判一次 has_depth。
static void render_target_normalize_without_depth(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.has_depth = false;
    d.depth_only = false;

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_FALSE(norm.has_depth);
    EXPECT_EQ_U64(norm.depth_format, PIXEL_FORMAT_INVALID);
    // 颜色部分不受影响
    EXPECT_EQ_U64(norm.color_format, PIXEL_FORMAT_R8G8B8A8_UNORM);
}

// 颜色纹理用 sRGB 格式时必须原样保留 —— 归一化不能把 _SRGB 抹掉，
// 抹掉的话硬件不再自动解码，贴图会明显偏亮。
static void render_target_normalize_preserves_srgb(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.color_format = PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB;

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_TRUE(pixel_format_is_srgb(norm.color_format));
    EXPECT_EQ_U64(norm.color_format, PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB);
}

// 归一化必须保留 debug_name —— 丢了的话出错时日志里只有 "?"，
// 而"是哪个渲染目标不完整"恰恰是排障时最需要的信息。
static void render_target_normalize_preserves_debug_name(void)
{
    render_target_desc_t d = render_target_desc_default();
    d.debug_name = "shadow_map";

    const render_target_desc_t norm = render_target_desc_normalize(&d);
    EXPECT_NOT_NULL(norm.debug_name);
    EXPECT_STR_EQ(norm.debug_name, "shadow_map");
}

// ---------------------------------------------------------------- 顶点布局
//
// 顺带把 M4 新增的"允许 binding_count == 0"这条规则钉住：
// 全屏三角形用 gl_VertexID 算顶点，不需要任何顶点输入。

static void vertex_layout_allows_no_bindings(void)
{
    vertex_layout_t layout;
    memset(&layout, 0, sizeof(layout));

    const char *reason = NULL;
    EXPECT_TRUE(vertex_layout_validate(&layout, &reason));

    // 但"有属性却没有 binding"一定是配置错误
    layout.attribute_count = 1;
    layout.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
    layout.attributes[0].binding = 0;
    layout.attributes[0].format = VERTEX_FORMAT_FLOAT3;
    layout.attributes[0].offset = 0;
    EXPECT_FALSE(vertex_layout_validate(&layout, &reason));
    EXPECT_NOT_NULL(reason);
}

static void vertex_layout_stride_uses_real_byte_size(void)
{
    // UNORM8X4 是 4 字节、UNORM16X4 是 8 字节。
    // 一律按"分量数 × 4"算的话，stride 校验会形同虚设。
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM8X4), 4);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_SNORM8X4), 4);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM16X2), 4);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_UNORM16X4), 8);
    EXPECT_EQ_U64(vertex_format_byte_size(VERTEX_FORMAT_FLOAT3), 12);

    // 分量数
    EXPECT_EQ_U64(vertex_format_component_count(VERTEX_FORMAT_UNORM8X4), 4);
    EXPECT_EQ_U64(vertex_format_component_count(VERTEX_FORMAT_FLOAT3), 3);
}

void test_render_target_register(void)
{
    test_register("render_target", "pixel_format_properties", pixel_format_properties);
    test_register("render_target", "default_desc", render_target_default_desc);

    test_register("render_target", "normalize_null_zero",
                  render_target_normalize_null_and_zero);
    test_register("render_target", "normalize_fills_formats",
                  render_target_normalize_fills_formats);
    test_register("render_target", "normalize_mismatched_formats",
                  render_target_normalize_rejects_mismatched_formats);
    test_register("render_target", "normalize_samples", render_target_normalize_samples);
    test_register("render_target", "normalize_depth_only", render_target_normalize_depth_only);
    test_register("render_target", "normalize_without_depth",
                  render_target_normalize_without_depth);
    test_register("render_target", "normalize_preserves_srgb",
                  render_target_normalize_preserves_srgb);
    test_register("render_target", "normalize_preserves_name",
                  render_target_normalize_preserves_debug_name);

    test_register("render_target", "vertex_layout_no_bindings",
                  vertex_layout_allows_no_bindings);
    test_register("render_target", "vertex_layout_byte_size",
                  vertex_layout_stride_uses_real_byte_size);
}
