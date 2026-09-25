// =============================================================================
//  engine/render/texture.h —— 纹理
//
//  上层拿到的永远是 texture_handle_t，不是 GPU 对象。
//  句柄带代际号，所以"纹理已销毁但句柄还在用"会返回失败而不是画出花屏。
// =============================================================================
#ifndef ENGINE_RENDER_TEXTURE_H
#define ENGINE_RENDER_TEXTURE_H

#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

// 从文件加载。格式支持由 stb_image 决定（PNG/JPG/TGA/BMP/GIF/PSD/HDR...）。
//
// srgb 参数的含义：
//   true  —— 颜色纹理（albedo、UI 图）。像素值会被当作 sRGB 解码成线性，
//            参与光照计算后再由帧缓冲编码回 sRGB。
//   false —— 数据纹理（法线、粗糙度、遮罩）。数值必须原样使用。
// 传错的后果：颜色纹理传 false 会显得发灰发暗；法线贴图传 true 会让光照方向错。
texture_handle_t texture_load_from_file(renderer_t *renderer, const char *path, bool srgb);

// 从内存中的图片字节加载（测试与打包资源用）
texture_handle_t texture_load_from_memory(renderer_t *renderer, const void *data, usize size,
                                          const char *debug_name, bool srgb);

// 1x1 纯色纹理。用途：占位、默认贴图、纯色调试物体。
// 有它才能保证"没绑纹理"时着色器仍然能正常采样，而不是采到一个未定义的纹理单元。
texture_handle_t texture_create_solid(renderer_t *renderer, u8 r, u8 g, u8 b, u8 a);

// 棋盘格。示例与调试用，不需要任何外部资源文件。
texture_handle_t texture_create_checkerboard(renderer_t *renderer, u32 size, u32 cell,
                                             const u8 color0[3], const u8 color1[3]);

// 程序化渐变。用来直观验证纹理坐标与采样是否正确。
texture_handle_t texture_create_gradient(renderer_t *renderer, u32 width, u32 height);

void texture_destroy(renderer_t *renderer, texture_handle_t texture);

bool        texture_is_valid(const renderer_t *renderer, texture_handle_t texture);
u32         texture_width(const renderer_t *renderer, texture_handle_t texture);
u32         texture_height(const renderer_t *renderer, texture_handle_t texture);
const char *texture_debug_name(const renderer_t *renderer, texture_handle_t texture);

#endif // ENGINE_RENDER_TEXTURE_H
