// =============================================================================
//  engine/render/render_target.h —— 离屏渲染目标（引擎层）
//
//  用途：阴影贴图、后期处理、M6 的"3D 渲染到纹理"。
//
//  ---- MSAA 解析（resolve）的时机 ----
//
//  多重采样的颜色缓冲**不能被着色器采样**。要用它必须先 blit 到一张单采样纹理。
//  本引擎把这个动作放在"切换渲染目标时"自动完成（见 GL 后端的 bind_render_target）——
//  调用方不需要显式 resolve，但**必须知道**：
//  在渲染目标被切走之前，它的纹理内容是未定义的（还是 MSAA 缓冲的原始状态）。
//
//  漏掉这一步的表现是"采样到黑屏或噪点"，而且不报任何错。
//
//  ---- 与 sRGB 的关系 ----
//
//  场景目标的颜色格式是**线性**的（R8G8B8A8_UNORM，当作线性值用），
//  线性→sRGB 的编码统一在最后的 present pass 里做。
//  详见 docs/M4-设计决策.md 的 sRGB 一节。
// =============================================================================
#ifndef ENGINE_RENDER_RENDER_TARGET_H
#define ENGINE_RENDER_RENDER_TARGET_H

#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

// 创建离屏渲染目标。失败返回 RENDER_TARGET_HANDLE_INVALID。
render_target_handle_t render_target_create(renderer_t *renderer,
                                            const render_target_desc_t *desc);

void render_target_destroy(renderer_t *renderer, render_target_handle_t target);

// 重建附件以匹配新尺寸。
// **窗口尺寸变化时必须调用** —— 漏掉的话帧缓冲会"不完整"，
// GL 会静默丢弃所有绘制，表现为"窗口一缩放画面就全黑"。
bool render_target_resize(renderer_t *renderer, render_target_handle_t target, u32 width,
                          u32 height);

// 取可采样的纹理句柄。普通目标返回颜色纹理，深度专用目标返回深度纹理。
texture_handle_t render_target_texture(renderer_t *renderer, render_target_handle_t target);

u32  render_target_width(const renderer_t *renderer, render_target_handle_t target);
u32  render_target_height(const renderer_t *renderer, render_target_handle_t target);
u32  render_target_samples(const renderer_t *renderer, render_target_handle_t target);
bool render_target_is_valid(const renderer_t *renderer, render_target_handle_t target);
const char *render_target_debug_name(const renderer_t *renderer, render_target_handle_t target);

// 场景目标（renderer 自己持有，用于 sRGB 管线与后期处理）。
// 窗口尺寸变化时 renderer 会自动 resize 它。
render_target_handle_t renderer_scene_target(const renderer_t *renderer);
texture_handle_t       renderer_scene_color_texture(renderer_t *renderer);

// 开关场景目标。
//   开启（默认）：所有渲染先到离屏目标，最后由 present pass 做
//                 线性 -> sRGB 编码后写到窗口。这是正确的 sRGB 管线。
//   关闭：直接渲染到窗口。性能略好，但颜色空间不再受控 ——
//         只建议在"只想看个大概"的调试场景下用。
void renderer_set_scene_target_enabled(renderer_t *renderer, bool enabled);
bool renderer_scene_target_enabled(const renderer_t *renderer);

// ---------------------------------------------------------------- sRGB 自检
//
// 把场景目标的内容读回 CPU，用于验证 sRGB 管线是否正确。
//
// 为什么需要它：sRGB 做错的表现是"整体偏暗/偏亮"，但因为所有东西都一起偏，
// 肉眼**无法判断对错**。只有"渲染一个已知值、读回、和期望值比对"才能确定。
//
// 用法：
//     renderer_read_scene_pixel(renderer, x, y, out_rgba8);
//     期望值：sRGB 0.5 的灰色在线性空间是 0.2158，读回来应该接近 128。
bool renderer_read_scene_pixel(renderer_t *renderer, u32 x, u32 y, u8 out_rgba[4]);

// 读回任意渲染目标的像素（同上）
bool render_target_read_pixel(renderer_t *renderer, render_target_handle_t target, u32 x, u32 y,
                              u8 out_rgba[4]);

// 读回场景目标的一块矩形（RGBA8）。
//
// **行序是自下而上的**（GL 的约定），要写成图片文件的话调用方自己翻转。
// 后端不做隐式翻转：隐式翻转会让"图为什么是倒的"变成一个需要猜的问题。
//
// 用途：把一帧画面转储成图片，用于**客观验证**渲染结果 ——
// "光照对不对""阴影有没有出现""材质参数有没有生效"这类问题，
// 光看日志是看不出来的。
bool renderer_read_scene_rect(renderer_t *renderer, u32 x, u32 y, u32 width, u32 height,
                              u8 *out_rgba);

// ---------------------------------------------------------------- present 回读
//
// 读回**窗口帧缓冲**（present pass 之后、交换缓冲之前）的一个像素。
//
// 为什么不能直接提供"读窗口像素"的同步函数：窗口内容在 swap 之后就没了，
// 帧结束后再读拿到的是下一帧还没绘制的后缓冲。所以只能这样两步走 ——
// 先预约，本帧结束时自动回读，帧结束后再取结果。
//
// 用途：端到端验证 sRGB 管线。
//   场景目标里存的应该是**线性**值（例如 50% 灰 -> 55），
//   而窗口里应该是**sRGB 编码后**的值（-> 128）。
//   两者都能读到，才说明"光照在线性空间算 + 输出时编码"这条链是通的。
void renderer_request_present_capture(renderer_t *renderer, u32 x, u32 y);
bool renderer_captured_pixel(const renderer_t *renderer, u8 out_rgba[4]);

#endif // ENGINE_RENDER_RENDER_TARGET_H
