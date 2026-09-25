// =============================================================================
//  engine/render/backend/opengl/gl_backend.h —— OpenGL 后端工厂
//
//  这个头文件存在的意义是"让 M13 加后端时有统一的入口形状"：
//  每个后端提供一个 create 函数，返回 render_backend_t*。
//
//  M13 会变成：
//      switch (backend_kind) {
//      case BACKEND_OPENGL: backend = gl_backend_create(alloc); break;
//      case BACKEND_D3D11:  backend = d3d11_backend_create(alloc); break;
//      case BACKEND_VULKAN: backend = vk_backend_create(alloc); break;
//      }
//
//  注意：**这个头文件是渲染层内部的东西**。业务代码不应该包含它，
//  更不应该知道 gl_backend_create 的存在 —— 业务代码只跟 renderer_t 打交道。
//  示例程序需要它，因为示例要负责"选择后端"这件事。
// =============================================================================
#ifndef ENGINE_RENDER_BACKEND_OPENGL_GL_BACKEND_H
#define ENGINE_RENDER_BACKEND_OPENGL_GL_BACKEND_H

#include "engine/core/memory.h"
#include "engine/render/render_backend.h"

// 创建 GL 后端。返回的对象由调用方负责（先 init 再 shutdown）。
// 失败返回 NULL。
render_backend_t *gl_backend_create(allocator_t *alloc);

// 关闭并释放后端对象。
//
// 为什么需要这个而不是让调用方自己 free：
// 后端结构体的大小是**实现细节**（定义在 .c 里），调用方不知道要释放多少字节。
// 让创建者提供销毁函数是 C 里处理不透明对象的标准做法。
// 内部会先调用 shutdown（幂等，未初始化时安全无操作）再释放。
void gl_backend_destroy(render_backend_t *backend);

#endif // ENGINE_RENDER_BACKEND_OPENGL_GL_BACKEND_H
