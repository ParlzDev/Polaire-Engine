// =============================================================================
//  src/render/texture.c —— 纹理创建与加载
//
//  文件解码交给 stb_image（实现单元在 third_party/stb_image_impl.c）。
//  本文件负责：解码结果 -> 引擎的 texture_desc_t -> 后端纹理对象。
//
//  ---- 关于 sRGB 的取舍（重要，直接影响观感） ----
//
//  显示器上的像素值是 sRGB 编码的（近似 gamma 2.2）。而光照计算必须在
//  线性空间做，否则明暗过渡会明显发灰发暗。
//
//  正确做法：
//    * 颜色纹理（albedo/UI）用 *_SRGB 格式上传，硬件自动解码成线性；
//    * 帧缓冲在写回时自动编码回 sRGB（M2 用默认帧缓冲，GL 会按
//      GL_FRAMEBUFFER_SRGB 处理，M4 引入离屏渲染时再显式管理）；
//    * 数据纹理（法线、粗糙度、遮罩）**不能**用 sRGB 格式，
//      它们的数值是物理量，不是颜色。
//
//  所以 texture_load_* 都带一个 srgb 参数，由调用方按用途决定。
// =============================================================================
#include "engine/render/texture.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/renderer.h"

#include "stb_image.h"

#define TEXTURE_DECODE_CHANNELS 4 // 统一解码成 RGBA8，避免 3 通道的对齐坑

// =============================================================================
//  从图片字节创建
// =============================================================================
texture_handle_t texture_load_from_memory(renderer_t *renderer, const void *data, usize size,
                                          const char *debug_name, bool srgb)
{
    ASSERT(renderer != NULL);

    if (data == NULL || size == 0) {
        LOG_ERROR("纹理数据为空 (%s)", (debug_name != NULL) ? debug_name : "<unnamed>");
        return TEXTURE_HANDLE_INVALID;
    }

    if (size > (usize)INT32_MAX) {
        LOG_ERROR("纹理数据过大 (%llu 字节)", (unsigned long long)size);
        return TEXTURE_HANDLE_INVALID;
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;

    // 强制解码成 4 通道：3 通道数据在宽度不是 4 的倍数时，
    // 上传会因为默认的 4 字节行对齐而错位（画面斜着撕裂）。
    // 统一成 4 通道就完全绕开了这个问题。
    stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *)data, (int)size, &width, &height,
                                            &source_channels, TEXTURE_DECODE_CHANNELS);
    if (pixels == NULL) {
        const char *reason = stbi_failure_reason();
        LOG_ERROR("图片解码失败 (%s): %s", (debug_name != NULL) ? debug_name : "<unnamed>",
                  (reason != NULL) ? reason : "未知原因");
        return TEXTURE_HANDLE_INVALID;
    }

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = (u32)width;
    desc.height = (u32)height;
    desc.mip_levels = 0;
    desc.array_layers = 1;
    desc.format = srgb ? PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB : PIXEL_FORMAT_R8G8B8A8_UNORM;
    desc.generate_mipmaps = true;
    desc.debug_name = debug_name;

    const texture_handle_t handle = renderer_create_texture(renderer, &desc, pixels);

    // stb_image 用的是自己的分配器（默认 malloc），必须用它的 free。
    stbi_image_free(pixels);

    return handle;
}

texture_handle_t texture_load_from_file(renderer_t *renderer, const char *path, bool srgb)
{
    ASSERT(renderer != NULL);

    if (path == NULL) {
        return TEXTURE_HANDLE_INVALID;
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;

    stbi_uc *pixels = stbi_load(path, &width, &height, &source_channels, TEXTURE_DECODE_CHANNELS);
    if (pixels == NULL) {
        const char *reason = stbi_failure_reason();
        LOG_ERROR("图片加载失败: %s (%s)", path, (reason != NULL) ? reason : "未知原因");
        return TEXTURE_HANDLE_INVALID;
    }

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = (u32)width;
    desc.height = (u32)height;
    desc.array_layers = 1;
    desc.format = srgb ? PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB : PIXEL_FORMAT_R8G8B8A8_UNORM;
    desc.generate_mipmaps = true;
    desc.debug_name = path;

    const texture_handle_t handle = renderer_create_texture(renderer, &desc, pixels);
    stbi_image_free(pixels);

    if (handle_is_valid(handle)) {
        LOG_INFO("纹理已加载: %s (%dx%d)", path, width, height);
    }
    return handle;
}

// =============================================================================
//  程序化纹理
// =============================================================================
texture_handle_t texture_create_solid(renderer_t *renderer, u8 r, u8 g, u8 b, u8 a)
{
    ASSERT(renderer != NULL);

    const u8 pixels[4] = {r, g, b, a};

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = 1;
    desc.height = 1;
    desc.array_layers = 1;
    // 纯色纹理也是"颜色"，用 sRGB 格式与其它颜色纹理保持一致，
    // 避免"同一套着色器，纯色物体比贴图物体亮一档"这种诡异现象。
    desc.format = PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB;
    // 即使只有 1x1 也生成 mipmap：GL 里"MIN_FILTER 是 mipmap 过滤器但纹理
    // 只有一级"会让纹理不完整，采样结果是全黑。生成一次 mipmap 就没有这个坑了。
    desc.generate_mipmaps = true;
    desc.debug_name = "solid";

    return renderer_create_texture(renderer, &desc, pixels);
}

texture_handle_t texture_create_checkerboard(renderer_t *renderer, u32 size, u32 cell,
                                             const u8 color0[3], const u8 color1[3])
{
    ASSERT(renderer != NULL);

    if (size == 0 || cell == 0) {
        LOG_ERROR("棋盘格参数无效 (size=%u, cell=%u)", size, cell);
        return TEXTURE_HANDLE_INVALID;
    }
    if (size > 4096) {
        size = 4096; // 上限保护
    }

    arena_t *arena = renderer_frame_arena(renderer);
    u8 *pixels = ARENA_NEW_ARRAY(arena, u8, (usize)size * size * 4u);
    if (pixels == NULL) {
        return TEXTURE_HANDLE_INVALID;
    }

    const u8 c0[3] = {color0[0], color0[1], color0[2]};
    const u8 c1[3] = {color1[0], color1[1], color1[2]};

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool odd = (((x / cell) + (y / cell)) & 1u) != 0u;
            const u8 *c = odd ? c1 : c0;
            u8 *pixel = pixels + ((usize)y * size + x) * 4u;
            pixel[0] = c[0];
            pixel[1] = c[1];
            pixel[2] = c[2];
            pixel[3] = 255;
        }
    }

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = size;
    desc.height = size;
    desc.array_layers = 1;
    desc.format = PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.generate_mipmaps = true;
    desc.debug_name = "checkerboard";

    return renderer_create_texture(renderer, &desc, pixels);
}

texture_handle_t texture_create_gradient(renderer_t *renderer, u32 width, u32 height)
{
    ASSERT(renderer != NULL);

    if (width == 0 || height == 0) {
        return TEXTURE_HANDLE_INVALID;
    }
    if (width > 4096) {
        width = 4096;
    }
    if (height > 4096) {
        height = 4096;
    }

    arena_t *arena = renderer_frame_arena(renderer);
    u8 *pixels = ARENA_NEW_ARRAY(arena, u8, (usize)width * height * 4u);
    if (pixels == NULL) {
        return TEXTURE_HANDLE_INVALID;
    }

    // 四个角的颜色不同，能一眼看出 UV 是否正确（旋转/镜像/翻转都会立刻暴露）
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 fx = (f32)x / (f32)(width - 1u > 0u ? width - 1u : 1u);
            const f32 fy = (f32)y / (f32)(height - 1u > 0u ? height - 1u : 1u);
            u8 *pixel = pixels + ((usize)y * width + x) * 4u;
            pixel[0] = (u8)(fx * 255.0f);
            pixel[1] = (u8)(fy * 255.0f);
            pixel[2] = (u8)((1.0f - fx * 0.5f - fy * 0.5f) * 255.0f);
            pixel[3] = 255;
        }
    }

    texture_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = width;
    desc.height = height;
    desc.array_layers = 1;
    desc.format = PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.generate_mipmaps = true;
    desc.debug_name = "gradient";

    return renderer_create_texture(renderer, &desc, pixels);
}

// =============================================================================
//  查询
// =============================================================================
void texture_destroy(renderer_t *renderer, texture_handle_t texture)
{
    ASSERT(renderer != NULL);
    renderer_destroy_texture(renderer, texture);
}

bool texture_is_valid(const renderer_t *renderer, texture_handle_t texture)
{
    return renderer_texture_is_valid(renderer, texture);
}

u32 texture_width(const renderer_t *renderer, texture_handle_t texture)
{
    return renderer_texture_width(renderer, texture);
}

u32 texture_height(const renderer_t *renderer, texture_handle_t texture)
{
    return renderer_texture_height(renderer, texture);
}

const char *texture_debug_name(const renderer_t *renderer, texture_handle_t texture)
{
    return renderer_texture_name(renderer, texture);
}
