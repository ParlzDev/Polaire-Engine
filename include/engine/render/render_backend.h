// =============================================================================
//  engine/render/render_backend.h —— 渲染后端抽象接口
//
//  ============================================================================
//   设计原则：按 Vulkan 语义定义，让 OpenGL 去适配
//  ============================================================================
//
//  这不是一句口号，它决定了下面每一个函数签名。具体体现：
//
//  1. **没有"散装状态设置"**。
//     GL 允许 set_blend / set_depth / set_cull 分开调，Vulkan 的管线状态对象
//     是不可变的、必须一次性创建。所以这里只有 create_pipeline(desc) + bind_pipeline。
//
//  2. **有显式的 render pass**。
//     GL 只有"默认帧缓冲"，Vulkan 的 render pass 是显式对象。
//     保留这个概念后，M4 的离屏渲染、M6 的"3D 渲染到纹理"都不需要改调用方。
//
//  3. **资源创建/销毁成对且显式**。
//     Vulkan 没有"隐式释放"，每个资源都必须显式 destroy。
//
//  4. **uniform 是"块 + 绑定点"而不是"按名字"**。
//     Vulkan 没有按名字设 uniform 的概念。详见 uniforms.h。
//
//  5. **句柄制，接口里不出现任何图形 API 类型**。
//     GLuint / VkBuffer / ID3D11Buffer 一个都不许出现。
//
//  ============================================================================
//   怎么用
//  ============================================================================
//
//      render_backend_t *backend = gl_backend_create(alloc);
//      backend->init(backend, &desc);
//
//      shader_handle_t sh = backend->create_shader(backend, &shader_desc);
//      pipeline_handle_t pl = backend->create_pipeline(backend, &pipeline_desc);
//
//      backend->begin_frame(backend, &frame);
//      backend->begin_render_pass(backend, &pass);
//      backend->bind_pipeline(backend, pl);
//      backend->set_uniforms(backend, RENDER_UNIFORM_BINDING_OBJECT, &obj, sizeof(obj));
//      backend->draw(backend, mesh, &draw_call);
//      backend->end_render_pass(backend);
//      backend->end_frame(backend);
//
//  业务代码通常不直接调这一层，而是通过 renderer_t（见 renderer.h）。
//  backend 层存在的意义是"M13 加后端时只改这一层的实现"。
// =============================================================================
#ifndef ENGINE_RENDER_RENDER_BACKEND_H
#define ENGINE_RENDER_RENDER_BACKEND_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/platform/window.h"
#include "engine/render/render_types.h"
#include "engine/render/uniforms.h"

typedef struct render_backend_t render_backend_t;

// ---------------------------------------------------------------- 初始化
typedef struct render_backend_desc_t {
    platform_window_t *window;       // 渲染目标窗口
    u32                msaa_samples; // 1 = 关闭
    bool               vsync;
    bool               enable_debug_output; // 打开驱动的调试消息回调
    const char        *debug_name;
} render_backend_desc_t;

// 后端能力（对应 Vulkan 的 VkPhysicalDeviceFeatures / VkPhysicalDeviceLimits）
typedef struct render_backend_caps_t {
    char api_name[64];     // "OpenGL 4.5"
    char device_name[128]; // "NVIDIA GeForce RTX 5060/PCIe/SSE2"
    char vendor_name[64];

    u32 max_texture_size;
    u32 max_texture_units;
    u32 max_samples;
    u32 max_uniform_buffer_size;
    u32 max_vertex_attributes;

    bool supports_instancing;
    bool supports_wireframe;
    bool supports_debug_output;
} render_backend_caps_t;

// ---------------------------------------------------------------- 帧
typedef struct frame_desc_t {
    u32 target_width;   // 渲染目标尺寸（物理像素）
    u32 target_height;
    f32 delta_seconds;
    f64 elapsed_seconds;
    u64 frame_index;
} frame_desc_t;

// =============================================================================
//  后端接口
//
//  所有回调的第一个参数都是 self。实现方的结构体必须把 render_backend_t
//  作为**第一个成员**，这样回调里可以安全地 (concrete_t *)self 转换。
// =============================================================================
struct render_backend_t {
    const char *name;

    // ---------------------------------------------------------------- 生命周期
    bool (*init)(render_backend_t *self, const render_backend_desc_t *desc);
    void (*shutdown)(render_backend_t *self);
    void (*get_caps)(const render_backend_t *self, render_backend_caps_t *out_caps);

    // ---------------------------------------------------------------- 帧
    void (*begin_frame)(render_backend_t *self, const frame_desc_t *desc);
    // 提交并呈现（GL: SwapBuffers；Vulkan: vkQueuePresentKHR）
    void (*end_frame)(render_backend_t *self);

    // ---------------------------------------------------------------- render pass
    void (*begin_render_pass)(render_backend_t *self, const render_pass_desc_t *desc);
    void (*end_render_pass)(render_backend_t *self);

    // 动态视口与裁剪（对应 vkCmdSetViewport / vkCmdSetScissor）
    void (*set_viewport)(render_backend_t *self, const viewport_t *viewport);
    void (*set_scissor)(render_backend_t *self, const scissor_rect_t *scissor);

    // ---------------------------------------------------------------- 资源
    shader_handle_t (*create_shader)(render_backend_t *self, const shader_desc_t *desc);
    void (*destroy_shader)(render_backend_t *self, shader_handle_t shader);

    texture_handle_t (*create_texture)(render_backend_t *self, const texture_desc_t *desc,
                                       const void *pixels);
    void (*destroy_texture)(render_backend_t *self, texture_handle_t texture);
    void (*update_texture)(render_backend_t *self, texture_handle_t texture, const void *pixels);

    mesh_handle_t (*create_mesh)(render_backend_t *self, const mesh_desc_t *desc);
    void (*destroy_mesh)(render_backend_t *self, mesh_handle_t mesh);

    // 更新动态网格的数据。数据大小不能超过创建时的容量。
    //
    // 只支持单个顶点 binding（批渲染的实际情况）。多 binding 的动态更新
    // 等真有需求时再加 —— 现在就做会让接口复杂一倍，而用不到。
    //
    // 传 NULL 表示"这一部分不更新"：批渲染每帧只更新顶点，
    // 索引在创建时一次生成后就不再变（所有精灵都是同一个 quad 模式）。
    //
    // Vulkan 上这对应"写进持久映射的 host-visible 缓冲"，不需要新概念。
    bool (*update_mesh)(render_backend_t *self, mesh_handle_t mesh, const void *vertex_data,
                        usize vertex_data_size, const void *index_data, usize index_data_size);

    pipeline_handle_t (*create_pipeline)(render_backend_t *self, const pipeline_desc_t *desc);
    void (*destroy_pipeline)(render_backend_t *self, pipeline_handle_t pipeline);

    // ---------------------------------------------------------------- 渲染目标
    //
    // 离屏渲染目标（FBO）。阴影贴图、后期处理、M6 的"3D 渲染到纹理"
    // 都建立在它之上。
    render_target_handle_t (*create_render_target)(render_backend_t *self,
                                                   const render_target_desc_t *desc);
    void (*destroy_render_target)(render_backend_t *self, render_target_handle_t target);

    // 重建附件。窗口尺寸变化时必须调用 ——
    // 漏掉的话帧缓冲会"不完整"，GL 会静默丢弃所有绘制，表现为"一缩放就全黑"。
    bool (*resize_render_target)(render_backend_t *self, render_target_handle_t target, u32 width,
                                 u32 height);

    // 取可采样的纹理句柄。
    // 普通目标返回颜色纹理；depth_only 目标返回深度纹理。
    texture_handle_t (*get_render_target_texture)(render_backend_t *self,
                                                  render_target_handle_t target);

    // 把后续的 render pass 定向到这个目标。
    // RENDER_TARGET_HANDLE_INVALID = 默认帧缓冲（窗口）。
    void (*bind_render_target)(render_backend_t *self, render_target_handle_t target);

    // 读回一个像素（RGBA8）。
    //
    // 这是**验证性**接口，不是渲染路径的一部分 —— 它的用途是"渲染一个已知值、
    // 读回来、和期望值比对"。sRGB 管线做错的表现是"整体偏暗"，肉眼无法判断，
    // 只有这种客观比对才能确定对错。
    //
    // target = RENDER_TARGET_HANDLE_INVALID 时读的是**默认帧缓冲**（窗口）。
    // 注意：窗口内容在交换缓冲之后就没了，所以这条路径必须在
    // end_frame 内部（present pass 之后、swap 之前）调用 ——
    // 帧结束之后再读，拿到的是下一帧还没画的后缓冲。
    //
    // 会强制同步（等待 GPU 完成），所以不要放进每帧的热路径。
    bool (*read_pixel)(render_backend_t *self, render_target_handle_t target, u32 x, u32 y,
                       u8 out_rgba[4]);

    // 读回一个矩形区域（RGBA8，行优先，每行 width*4 字节）。
    //
    // 为什么不复用 read_pixel 逐点读：每次 read_pixel 都是一次
    // glBindFramebuffer + glReadPixels，而 glReadPixels 会强制同步。
    // 一张 480x270 的图逐点读要 13 万次同步 —— 秒级卡顿。
    // 一次读一整块则只有一次同步。
    //
    // out 必须至少有 width * height * 4 字节。
    bool (*read_rect)(render_backend_t *self, render_target_handle_t target, u32 x, u32 y, u32 width,
                      u32 height, u8 *out_rgba);

    // ---------------------------------------------------------------- 绑定与绘制
    void (*bind_pipeline)(render_backend_t *self, pipeline_handle_t pipeline);
    void (*bind_texture)(render_backend_t *self, u32 slot, texture_handle_t texture,
                         const sampler_desc_t *sampler);
    void (*draw)(render_backend_t *self, mesh_handle_t mesh, const draw_call_t *call);

    // 上传 uniform 块。binding 取 RENDER_UNIFORM_BINDING_*。
    // 用 (binding, bytes) 而不是"按名字"，理由见 uniforms.h。
    void (*set_uniforms)(render_backend_t *self, u32 binding, const void *data, usize size);

    // ---------------------------------------------------------------- 统计
    void (*reset_stats)(render_backend_t *self);
    void (*get_stats)(const render_backend_t *self, render_stats_t *out_stats);
};

// ---------------------------------------------------------------- 便捷封装
//
// 后端指针的空检查集中在这里，避免每个调用点写一遍。
// 这些是 static inline，编译后与直接调函数指针等价。

static inline bool render_backend_init(render_backend_t *backend,
                                       const render_backend_desc_t *desc)
{
    return (backend != NULL && backend->init != NULL) ? backend->init(backend, desc) : false;
}

static inline void render_backend_shutdown(render_backend_t *backend)
{
    if (backend != NULL && backend->shutdown != NULL) {
        backend->shutdown(backend);
    }
}

static inline void render_backend_get_caps(const render_backend_t *backend,
                                           render_backend_caps_t *out_caps)
{
    if (backend != NULL && backend->get_caps != NULL) {
        backend->get_caps(backend, out_caps);
    }
}

static inline void render_backend_begin_frame(render_backend_t *backend,
                                              const frame_desc_t *desc)
{
    if (backend != NULL && backend->begin_frame != NULL) {
        backend->begin_frame(backend, desc);
    }
}

static inline void render_backend_end_frame(render_backend_t *backend)
{
    if (backend != NULL && backend->end_frame != NULL) {
        backend->end_frame(backend);
    }
}

static inline void render_backend_begin_render_pass(render_backend_t *backend,
                                                    const render_pass_desc_t *desc)
{
    if (backend != NULL && backend->begin_render_pass != NULL) {
        backend->begin_render_pass(backend, desc);
    }
}

static inline void render_backend_end_render_pass(render_backend_t *backend)
{
    if (backend != NULL && backend->end_render_pass != NULL) {
        backend->end_render_pass(backend);
    }
}

static inline void render_backend_bind_pipeline(render_backend_t *backend,
                                                pipeline_handle_t pipeline)
{
    if (backend != NULL && backend->bind_pipeline != NULL) {
        backend->bind_pipeline(backend, pipeline);
    }
}

static inline void render_backend_bind_texture(render_backend_t *backend, u32 slot,
                                               texture_handle_t texture,
                                               const sampler_desc_t *sampler)
{
    if (backend != NULL && backend->bind_texture != NULL) {
        backend->bind_texture(backend, slot, texture, sampler);
    }
}

static inline void render_backend_draw(render_backend_t *backend, mesh_handle_t mesh,
                                       const draw_call_t *call)
{
    if (backend != NULL && backend->draw != NULL) {
        backend->draw(backend, mesh, call);
    }
}

static inline bool render_backend_update_mesh(render_backend_t *backend, mesh_handle_t mesh,
                                              const void *vertex_data, usize vertex_data_size,
                                              const void *index_data, usize index_data_size)
{
    if (backend != NULL && backend->update_mesh != NULL) {
        return backend->update_mesh(backend, mesh, vertex_data, vertex_data_size, index_data,
                                    index_data_size);
    }
    return false;
}

static inline void render_backend_set_uniforms(render_backend_t *backend, u32 binding,
                                               const void *data, usize size)
{
    if (backend != NULL && backend->set_uniforms != NULL) {
        backend->set_uniforms(backend, binding, data, size);
    }
}

static inline render_target_handle_t
render_backend_create_render_target(render_backend_t *backend, const render_target_desc_t *desc)
{
    return (backend != NULL && backend->create_render_target != NULL)
               ? backend->create_render_target(backend, desc)
               : RENDER_TARGET_HANDLE_INVALID;
}

static inline void render_backend_destroy_render_target(render_backend_t *backend,
                                                        render_target_handle_t target)
{
    if (backend != NULL && backend->destroy_render_target != NULL) {
        backend->destroy_render_target(backend, target);
    }
}

static inline bool render_backend_resize_render_target(render_backend_t *backend,
                                                       render_target_handle_t target, u32 width,
                                                       u32 height)
{
    return (backend != NULL && backend->resize_render_target != NULL)
               ? backend->resize_render_target(backend, target, width, height)
               : false;
}

static inline texture_handle_t
render_backend_get_render_target_texture(render_backend_t *backend, render_target_handle_t target)
{
    return (backend != NULL && backend->get_render_target_texture != NULL)
               ? backend->get_render_target_texture(backend, target)
               : TEXTURE_HANDLE_INVALID;
}

static inline void render_backend_bind_render_target(render_backend_t *backend,
                                                     render_target_handle_t target)
{
    if (backend != NULL && backend->bind_render_target != NULL) {
        backend->bind_render_target(backend, target);
    }
}

static inline bool render_backend_read_pixel(render_backend_t *backend,
                                             render_target_handle_t target, u32 x, u32 y,
                                             u8 out_rgba[4])
{
    return (backend != NULL && backend->read_pixel != NULL)
               ? backend->read_pixel(backend, target, x, y, out_rgba)
               : false;
}

static inline bool render_backend_read_rect(render_backend_t *backend, render_target_handle_t target,
                                            u32 x, u32 y, u32 width, u32 height, u8 *out_rgba)
{
    return (backend != NULL && backend->read_rect != NULL)
               ? backend->read_rect(backend, target, x, y, width, height, out_rgba)
               : false;
}

static inline void render_backend_reset_stats(render_backend_t *backend)
{
    if (backend != NULL && backend->reset_stats != NULL) {
        backend->reset_stats(backend);
    }
}

static inline void render_backend_get_stats(const render_backend_t *backend,
                                            render_stats_t *out_stats)
{
    if (backend != NULL && backend->get_stats != NULL) {
        backend->get_stats(backend, out_stats);
    } else if (out_stats != NULL) {
        render_stats_reset(out_stats);
    }
}

#endif // ENGINE_RENDER_RENDER_BACKEND_H
