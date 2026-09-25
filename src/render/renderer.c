// =============================================================================
//  src/render/renderer.c —— 渲染器实现
//
//  renderer 是"引擎怎么组织一帧"的地方，backend 是"怎么和某个 API 说话"。
//  分工见 renderer.h 文件头。
// =============================================================================
#include "engine/render/renderer.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

// =============================================================================
//  资源池槽位
//
//  用户拿到的是 renderer 的池句柄；池槽里存后端的句柄。
//  两层结构的好处见 renderer.h 文件头。
// =============================================================================
typedef struct shader_slot_t {
    shader_handle_t backend_handle;
    const char     *name;
    u64             creation_frame;
} shader_slot_t;

typedef struct texture_slot_t {
    texture_handle_t backend_handle;
    u32              width;
    u32              height;
    pixel_format_t   format;
    const char      *name;
    u64              creation_frame;

    // false 表示这个槽位只是"指向渲染目标的附件纹理"，**后端句柄的所有权
    // 在渲染目标手里** —— 销毁渲染目标时后端已经释放过那个句柄了。
    // 这里再释放一次就是对后端纹理池的双重释放：
    // 同一个槽位被压进空闲链表两次，之后两次分配会拿到同一个槽位。
    // 症状是"两张毫不相干的纹理互相覆盖"，排查起来极其痛苦。
    bool owns_backend_handle;
} texture_slot_t;

typedef struct mesh_slot_t {
    mesh_handle_t backend_handle;
    u32           vertex_count;
    u32           index_count;
    vec3_t        bounds_min;
    vec3_t        bounds_max;
    const char   *name;
    u64           creation_frame;
} mesh_slot_t;

typedef struct pipeline_slot_t {
    pipeline_handle_t backend_handle;
    const char       *name;
    u64               creation_frame;
} pipeline_slot_t;

typedef struct render_target_slot_t {
    render_target_handle_t backend_handle;
    u32                    width;
    u32                    height;
    u32                    samples;
    const char            *name;

    // 附件纹理在**渲染器纹理池**里的句柄。
    //
    // 为什么必须存一份：后端创建的纹理句柄属于后端的池，渲染器的池是另一个池，
    // 两者的索引空间毫不相干。直接把后端句柄当引擎句柄返回，会在
    // resolve_texture 时查到"索引相同但代际不同"的槽位 —— 结果是拿到
    // TEXTURE_HANDLE_INVALID，绑定循环于是绑上默认白纹理。
    //
    // 症状极其隐蔽：present pass 采到白色，画面整体变成一片白，
    // 但没有任何报错，日志里也看不出问题（"渲染目标就绪"照常打印）。
    texture_handle_t texture;
} render_target_slot_t;

struct renderer_t {
    allocator_t      *alloc;
    render_backend_t *backend;

    arena_t         frame_arena;
    render_queue_t  queue;

    pool_t shaders;
    pool_t textures;
    pool_t meshes;
    pool_t pipelines;
    pool_t render_targets;

    camera_t         camera;
    frame_uniforms_t frame_uniforms;
    frame_desc_t     frame_desc;
    u64              frame_index;
    bool             has_camera;

    // ---- M4：3D 管线 ----
    light_set_t lights;

    // sRGB 管线：所有渲染先到离屏目标，最后由 present pass 编码后写到窗口。
    // 为什么必须离屏：Windows 的默认帧缓冲不一定是 sRGB 可写的，
    // 直接依赖 GL_FRAMEBUFFER_SRGB 可能静默失效（颜色偏暗且不报错）。
    render_target_handle_t scene_target;
    bool                   scene_target_enabled;
    shader_handle_t        present_shader;
    pipeline_handle_t      present_pipeline;
    shader_handle_t        sky_shader;
    pipeline_handle_t      sky_pipeline;
    mesh_handle_t          fullscreen_triangle;

    bool cull_enabled;
    u32  culled_count;
    u32  scene_msaa_samples;
    frustum_t frustum;

    texture_handle_t shadow_texture;
    f32              shadow_strength;
    f32              shadow_normal_bias;

    // 清屏设置
    bool   clear_color;
    vec4_t clear_color_value;
    bool   clear_depth;
    f32    clear_depth_value;

    // ---- present 像素回读（sRGB 自检用）----
    //
    // 窗口内容在交换缓冲之后就没了，所以"读窗口帧缓冲"必须发生在
    // end_frame 内部（present pass 之后、swap 之前）。调用方只能**预约**一次
    // 回读，结果存在这里，帧结束后再取。
    bool capture_requested;
    u32  capture_x;
    u32  capture_y;
    u8   captured_pixel[4];
    bool captured_valid;

    render_stats_t last_stats;
};

// 前向声明：init 的失败路径要调用 shutdown 回滚，
// 而 shutdown 定义在后面。
static bool renderer_init_internal(renderer_t *renderer, allocator_t *alloc,
                                   render_backend_t *backend);
static void renderer_shutdown_internal(renderer_t *renderer);

// =============================================================================
//  内置着色器：present（sRGB 编码 + 色调映射）与 skybox
//
//  ---- 为什么 sRGB 编码要单独一个 pass ----
//
//  这是整个引擎里**唯一**一处做线性 -> sRGB 转换的地方。
//
//  如果让每个着色器各自输出 sRGB，会有两个问题：
//    * 光照计算必须在线性空间做，着色器输出的是线性值；
//    * "哪里做了编码"会散落在几十个着色器里，改一次要全改。
//
//  收成一个 pass 之后，sRGB 的正确性只需要验证一个地方 ——
//  这正是 renderer_read_scene_pixel 存在的意义。
// =============================================================================
static const char *const k_present_vertex =
    "out vec2 v_uv;\n"
    "void main() {\n"
    // 全屏三角形：不用顶点缓冲，靠 gl_VertexID 算三个顶点。
    // 比"两个三角形拼的 quad"少一条对角线接缝。
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    v_uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const k_present_fragment =
    "in vec2 v_uv;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "\n"
    "vec3 tonemap_reinhard(vec3 c) { return c / (c + vec3(1.0)); }\n"
    "vec3 tonemap_aces(vec3 x) {\n"
    "    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;\n"
    "    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);\n"
    "}\n"
    "\n"
    // 精确的 IEC 61966-2-1 曲线。用 pow(c, 1/2.2) 近似的话暗部会有可见偏差，
    // 而且 sRGB 自检会失败 —— 自检要的就是"和标准值一致"。
    "vec3 linear_to_srgb(vec3 c) {\n"
    "    vec3 lo = c * 12.92;\n"
    "    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;\n"
    "    return mix(lo, hi, step(vec3(0.0031308), c));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec3 color = texture(u_albedo, v_uv).rgb;\n"
    "    color *= u_light_params.w;\n"
    "    int mode = int(u_sky_params.y + 0.5);\n"
    "    if (mode == 1) { color = tonemap_reinhard(color); }\n"
    "    else if (mode == 2) { color = tonemap_aces(color); }\n"
    "    frag_color = vec4(linear_to_srgb(color), 1.0);\n"
    "}\n";

static const char *const k_sky_vertex = k_present_vertex;

static const char *const k_sky_fragment =
    "in vec2 v_uv;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "    vec2 ndc = v_uv * 2.0 - 1.0;\n"
    // 用逆视图投影从 NDC 反推世界空间射线。
    // 取近远两点做差，得到的射线对透视与正交相机都成立。
    "    vec4 far_point = u_inv_view_projection * vec4(ndc, 1.0, 1.0);\n"
    "    vec4 near_point = u_inv_view_projection * vec4(ndc, -1.0, 1.0);\n"
    "    vec3 direction = normalize(far_point.xyz / far_point.w - near_point.xyz / near_point.w);\n"
    "\n"
    "    float t = direction.y;\n"
    "    vec3 color;\n"
    "    if (t >= 0.0) {\n"
    "        color = mix(u_sky_horizon.rgb, u_sky_zenith.rgb, pow(t, 0.55));\n"
    "    } else {\n"
    "        color = mix(u_sky_horizon.rgb, u_sky_ground.rgb, pow(-t, 0.45));\n"
    "    }\n"
    "    frag_color = vec4(color, 1.0);\n"
    "}\n";

// =============================================================================
//  句柄解析：engine 句柄 -> backend 句柄
// =============================================================================
static shader_handle_t resolve_shader(const renderer_t *renderer, shader_handle_t shader)
{
    if (shader == SHADER_HANDLE_INVALID) {
        return SHADER_HANDLE_INVALID;
    }
    const shader_slot_t *slot = POOL_GET_AS(&renderer->shaders, shader, shader_slot_t);
    return (slot != NULL) ? slot->backend_handle : SHADER_HANDLE_INVALID;
}

static texture_handle_t resolve_texture(const renderer_t *renderer, texture_handle_t texture)
{
    if (texture == TEXTURE_HANDLE_INVALID) {
        return TEXTURE_HANDLE_INVALID;
    }
    const texture_slot_t *slot = POOL_GET_AS(&renderer->textures, texture, texture_slot_t);
    return (slot != NULL) ? slot->backend_handle : TEXTURE_HANDLE_INVALID;
}

static mesh_handle_t resolve_mesh(const renderer_t *renderer, mesh_handle_t mesh)
{
    if (mesh == MESH_HANDLE_INVALID) {
        return MESH_HANDLE_INVALID;
    }
    const mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    return (slot != NULL) ? slot->backend_handle : MESH_HANDLE_INVALID;
}

static pipeline_handle_t resolve_pipeline(const renderer_t *renderer, pipeline_handle_t pipeline)
{
    if (pipeline == PIPELINE_HANDLE_INVALID) {
        return PIPELINE_HANDLE_INVALID;
    }
    const pipeline_slot_t *slot = POOL_GET_AS(&renderer->pipelines, pipeline, pipeline_slot_t);
    return (slot != NULL) ? slot->backend_handle : PIPELINE_HANDLE_INVALID;
}

static render_target_handle_t resolve_render_target(const renderer_t *renderer,
                                                    render_target_handle_t target)
{
    if (target == RENDER_TARGET_HANDLE_INVALID) {
        return RENDER_TARGET_HANDLE_INVALID;
    }
    const render_target_slot_t *slot =
        POOL_GET_AS(&renderer->render_targets, target, render_target_slot_t);
    return (slot != NULL) ? slot->backend_handle : RENDER_TARGET_HANDLE_INVALID;
}

// =============================================================================
//  初始化
// =============================================================================
bool renderer_init_internal(renderer_t *renderer, allocator_t *alloc, render_backend_t *backend)
{
    ASSERT(renderer != NULL);
    ASSERT(alloc != NULL);

    memset(renderer, 0, sizeof(*renderer));
    renderer->alloc = alloc;
    renderer->backend = backend;

    if (!arena_init(&renderer->frame_arena, alloc, ENGINE_KB(256), "renderer_frame")) {
        LOG_ERROR("渲染器帧 arena 创建失败");
        return false;
    }

    if (!render_queue_init(&renderer->queue, alloc)) {
        LOG_ERROR("渲染队列创建失败");
        arena_destroy(&renderer->frame_arena);
        return false;
    }

    if (!pool_init(&renderer->shaders, alloc, sizeof(shader_slot_t), RENDERER_MAX_SHADERS,
                   "renderer_shaders") ||
        !pool_init(&renderer->textures, alloc, sizeof(texture_slot_t), RENDERER_MAX_TEXTURES,
                   "renderer_textures") ||
        !pool_init(&renderer->meshes, alloc, sizeof(mesh_slot_t), RENDERER_MAX_MESHES,
                   "renderer_meshes") ||
        !pool_init(&renderer->pipelines, alloc, sizeof(pipeline_slot_t), RENDERER_MAX_PIPELINES,
                   "renderer_pipelines") ||
        !pool_init(&renderer->render_targets, alloc, sizeof(render_target_slot_t),
                   RENDER_MAX_RENDER_TARGETS, "renderer_render_targets")) {
        LOG_ERROR("渲染器资源池创建失败");
        renderer_shutdown_internal(renderer);
        return false;
    }

    renderer->scene_target = RENDER_TARGET_HANDLE_INVALID;
    renderer->scene_target_enabled = true;
    renderer->cull_enabled = true;
    renderer->scene_msaa_samples = 4;
    renderer->shadow_texture = TEXTURE_HANDLE_INVALID;
    renderer->shadow_normal_bias = 0.035f;
    light_set_clear(&renderer->lights);

    // ---- 全屏三角形（present 与 skybox 共用）----
    // 顶点由 gl_VertexID 算出来，不需要任何顶点输入。
    // 这也是 vertex_layout_validate 允许 binding_count == 0 的原因。
    {
        mesh_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.layout.binding_count = 0;
        desc.layout.attribute_count = 0;
        desc.index_format = INDEX_FORMAT_NONE;
        desc.vertex_count = 3;
        desc.index_count = 0;
        desc.debug_name = "fullscreen_triangle";
        renderer->fullscreen_triangle = renderer_create_mesh(renderer, &desc);
        if (!mesh_is_valid(renderer, renderer->fullscreen_triangle)) {
            LOG_ERROR("全屏三角形创建失败");
            renderer_shutdown_internal(renderer);
            return false;
        }
    }

    // ---- present 管线（线性 -> sRGB 编码 + 色调映射）----
    {
        shader_desc_t shader_desc;
        memset(&shader_desc, 0, sizeof(shader_desc));
        shader_desc.vertex_source = k_present_vertex;
        shader_desc.fragment_source = k_present_fragment;
        shader_desc.debug_name = "builtin:present";
        renderer->present_shader = renderer_create_shader(renderer, &shader_desc);

        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = renderer->present_shader;
        memset(&desc.vertex_layout, 0, sizeof(desc.vertex_layout));
        desc.blend = blend_state_opaque();
        desc.depth = depth_state_disabled();
        desc.raster = raster_state_no_cull();
        desc.debug_name = "present";

        renderer->present_pipeline = renderer_create_pipeline(renderer, &desc);
        if (!renderer_pipeline_is_valid(renderer, renderer->present_pipeline)) {
            LOG_ERROR("present 管线创建失败");
            renderer_shutdown_internal(renderer);
            return false;
        }
    }

    // ---- skybox 管线 ----
    {
        shader_desc_t shader_desc;
        memset(&shader_desc, 0, sizeof(shader_desc));
        shader_desc.vertex_source = k_sky_vertex;
        shader_desc.fragment_source = k_sky_fragment;
        shader_desc.debug_name = "builtin:skybox";
        renderer->sky_shader = renderer_create_shader(renderer, &shader_desc);

        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = renderer->sky_shader;
        memset(&desc.vertex_layout, 0, sizeof(desc.vertex_layout));
        desc.blend = blend_state_opaque();
        // 天空盒**先画**（深度测试关、深度写关），后面的场景再覆盖上去。
        // 放在最后画的话半透明物体会被它盖住 —— 半透明不写深度。
        desc.depth = depth_state_disabled();
        desc.raster = raster_state_no_cull();
        desc.debug_name = "skybox";

        renderer->sky_pipeline = renderer_create_pipeline(renderer, &desc);
        if (!renderer_pipeline_is_valid(renderer, renderer->sky_pipeline)) {
            LOG_ERROR("skybox 管线创建失败");
            renderer_shutdown_internal(renderer);
            return false;
        }
    }

    renderer->camera = camera_default();
    renderer->has_camera = true;
    renderer->frame_uniforms = frame_uniforms_default();
    renderer->clear_color = true;
    renderer->clear_color_value = vec4_make(0.06f, 0.08f, 0.12f, 1.0f);
    renderer->clear_depth = true;
    renderer->clear_depth_value = 1.0f;

    render_stats_reset(&renderer->last_stats);
    return true;
}

// =============================================================================
//  创建 / 销毁（公开入口）
// =============================================================================
renderer_t *renderer_create(allocator_t *alloc, render_backend_t *backend)
{
    ASSERT(alloc != NULL);
    ASSERT(backend != NULL);

    renderer_t *renderer = (renderer_t *)alloc->alloc(alloc, sizeof(renderer_t), 16);
    if (renderer == NULL) {
        LOG_ERROR("渲染器对象分配失败");
        return NULL;
    }
    memset(renderer, 0, sizeof(*renderer));

    if (!renderer_init_internal(renderer, alloc, backend)) {
        alloc->free(alloc, renderer, sizeof(renderer_t));
        return NULL;
    }
    return renderer;
}

void renderer_destroy(renderer_t *renderer)
{
    if (renderer == NULL) {
        return;
    }

    allocator_t *alloc = renderer->alloc;
    renderer_shutdown_internal(renderer);

    if (alloc != NULL) {
        alloc->free(alloc, renderer, sizeof(renderer_t));
    }
}

void renderer_shutdown_internal(renderer_t *renderer)
{
    if (renderer == NULL) {
        return;
    }

    // 先销毁资源（需要 current 上下文），再销毁池。
    // 后端自己也有一层兜底（shutdown 时会遍历池释放残留资源），
    // 所以即使调用方漏了 destroy 也不会泄漏 GPU 对象。
    if (renderer->backend != NULL) {
        pool_iter_t it;
        handle_t    handle = HANDLE_INVALID;
        void       *ptr = NULL;

        pool_iter_begin(&it, &renderer->render_targets);
        while (pool_iter_next(&it, &handle, &ptr)) {
            render_target_slot_t *slot = (render_target_slot_t *)ptr;
            renderer->backend->destroy_render_target(renderer->backend, slot->backend_handle);
        }

        pool_iter_begin(&it, &renderer->pipelines);
        while (pool_iter_next(&it, &handle, &ptr)) {
            pipeline_slot_t *slot = (pipeline_slot_t *)ptr;
            renderer->backend->destroy_pipeline(renderer->backend, slot->backend_handle);
        }

        pool_iter_begin(&it, &renderer->meshes);
        while (pool_iter_next(&it, &handle, &ptr)) {
            mesh_slot_t *slot = (mesh_slot_t *)ptr;
            renderer->backend->destroy_mesh(renderer->backend, slot->backend_handle);
        }

        pool_iter_begin(&it, &renderer->textures);
        while (pool_iter_next(&it, &handle, &ptr)) {
            texture_slot_t *slot = (texture_slot_t *)ptr;
            // 渲染目标的附件纹理：后端句柄的所有权在渲染目标手里，
            // 上面那轮 destroy_render_target 已经释放过了。
            // 这里再释放一次就是对后端纹理池的双重释放。
            if (!slot->owns_backend_handle) {
                continue;
            }
            renderer->backend->destroy_texture(renderer->backend, slot->backend_handle);
        }

        pool_iter_begin(&it, &renderer->shaders);
        while (pool_iter_next(&it, &handle, &ptr)) {
            shader_slot_t *slot = (shader_slot_t *)ptr;
            renderer->backend->destroy_shader(renderer->backend, slot->backend_handle);
        }
    }

    pool_destroy(&renderer->render_targets);
    pool_destroy(&renderer->pipelines);
    pool_destroy(&renderer->meshes);
    pool_destroy(&renderer->textures);
    pool_destroy(&renderer->shaders);
    render_queue_destroy(&renderer->queue);
    arena_destroy(&renderer->frame_arena);

    memset(renderer, 0, sizeof(*renderer));
}

render_backend_t *renderer_backend(renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->backend;
}

const render_backend_t *renderer_backend_const(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->backend;
}

void renderer_get_caps(const renderer_t *renderer, render_backend_caps_t *out_caps)
{
    ASSERT(renderer != NULL);
    render_backend_get_caps(renderer->backend, out_caps);
}

// =============================================================================
//  资源创建
// =============================================================================
shader_handle_t renderer_create_shader(renderer_t *renderer, const shader_desc_t *desc)
{
    ASSERT(renderer != NULL);

    const shader_handle_t backend_handle = renderer->backend->create_shader(renderer->backend, desc);
    if (backend_handle == SHADER_HANDLE_INVALID) {
        return SHADER_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->shaders);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("着色器槽位耗尽");
        renderer->backend->destroy_shader(renderer->backend, backend_handle);
        return SHADER_HANDLE_INVALID;
    }

    shader_slot_t *slot = POOL_GET_AS(&renderer->shaders, handle, shader_slot_t);
    slot->backend_handle = backend_handle;
    slot->name = (desc != NULL) ? desc->debug_name : NULL;
    slot->creation_frame = renderer->frame_index;
    return handle;
}

texture_handle_t renderer_create_texture(renderer_t *renderer, const texture_desc_t *desc,
                                         const void *pixels)
{
    ASSERT(renderer != NULL);

    const texture_handle_t backend_handle =
        renderer->backend->create_texture(renderer->backend, desc, pixels);
    if (backend_handle == TEXTURE_HANDLE_INVALID) {
        return TEXTURE_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->textures);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("纹理槽位耗尽");
        renderer->backend->destroy_texture(renderer->backend, backend_handle);
        return TEXTURE_HANDLE_INVALID;
    }

    texture_slot_t *slot = POOL_GET_AS(&renderer->textures, handle, texture_slot_t);
    slot->backend_handle = backend_handle;
    slot->width = (desc != NULL) ? desc->width : 0;
    slot->height = (desc != NULL) ? desc->height : 0;
    slot->format = (desc != NULL) ? desc->format : PIXEL_FORMAT_INVALID;
    slot->name = (desc != NULL) ? desc->debug_name : NULL;
    slot->creation_frame = renderer->frame_index;
    slot->owns_backend_handle = true; // 我们自己创建的纹理，所有权归我们
    return handle;
}

mesh_handle_t renderer_create_mesh(renderer_t *renderer, const mesh_desc_t *desc)
{
    ASSERT(renderer != NULL);

    const mesh_handle_t backend_handle = renderer->backend->create_mesh(renderer->backend, desc);
    if (backend_handle == MESH_HANDLE_INVALID) {
        return MESH_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->meshes);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("网格槽位耗尽");
        renderer->backend->destroy_mesh(renderer->backend, backend_handle);
        return MESH_HANDLE_INVALID;
    }

    mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, handle, mesh_slot_t);
    memset(slot, 0, sizeof(*slot));
    slot->backend_handle = backend_handle;
    slot->vertex_count = (desc != NULL) ? desc->vertex_count : 0;
    slot->index_count = (desc != NULL) ? desc->index_count : 0;
    slot->name = (desc != NULL) ? desc->debug_name : NULL;
    slot->creation_frame = renderer->frame_index;

    // 包围盒在这里算一次并缓存：渲染队列每帧都要用它算排序深度，
    // 不能每帧重新扫描顶点数据。
    if (desc != NULL) {
        mesh_compute_bounds(desc, &slot->bounds_min, &slot->bounds_max);
    }

    return handle;
}

bool renderer_update_mesh(renderer_t *renderer, mesh_handle_t mesh, const void *vertex_data,
                          usize vertex_data_size, const void *index_data, usize index_data_size)
{
    ASSERT(renderer != NULL);

    mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    if (slot == NULL) {
        return false;
    }

    const bool ok = render_backend_update_mesh(renderer->backend, slot->backend_handle,
                                               vertex_data, vertex_data_size, index_data,
                                               index_data_size);
    if (ok) {
        // 顶点数可能变化（批渲染每帧的精灵数不同），同步更新记录，
        // 否则无索引绘制的 count 会不对。
        if (index_data != NULL && index_data_size > 0) {
            slot->index_count = (u32)(index_data_size / index_format_byte_size(INDEX_FORMAT_U32));
        }
    }
    return ok;
}

pipeline_handle_t renderer_create_pipeline(renderer_t *renderer, const pipeline_desc_t *desc)
{
    ASSERT(renderer != NULL);

    if (desc == NULL) {
        return PIPELINE_HANDLE_INVALID;
    }

    // 把 desc 里的引擎级着色器句柄换成后端句柄再往下传。
    pipeline_desc_t resolved = *desc;
    resolved.shader = resolve_shader(renderer, desc->shader);
    if (resolved.shader == SHADER_HANDLE_INVALID) {
        LOG_ERROR("创建管线时引用了无效的着色器句柄");
        return PIPELINE_HANDLE_INVALID;
    }

    const pipeline_handle_t backend_handle =
        renderer->backend->create_pipeline(renderer->backend, &resolved);
    if (backend_handle == PIPELINE_HANDLE_INVALID) {
        return PIPELINE_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->pipelines);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("管线槽位耗尽");
        renderer->backend->destroy_pipeline(renderer->backend, backend_handle);
        return PIPELINE_HANDLE_INVALID;
    }

    pipeline_slot_t *slot = POOL_GET_AS(&renderer->pipelines, handle, pipeline_slot_t);
    slot->backend_handle = backend_handle;
    slot->name = desc->debug_name;
    slot->creation_frame = renderer->frame_index;
    return handle;
}

// =============================================================================
//  资源销毁
// =============================================================================
void renderer_destroy_shader(renderer_t *renderer, shader_handle_t shader)
{
    ASSERT(renderer != NULL);

    shader_slot_t *slot = POOL_GET_AS(&renderer->shaders, shader, shader_slot_t);
    if (slot == NULL) {
        return;
    }
    renderer->backend->destroy_shader(renderer->backend, slot->backend_handle);
    pool_free(&renderer->shaders, shader);
}

void renderer_destroy_texture(renderer_t *renderer, texture_handle_t texture)
{
    ASSERT(renderer != NULL);

    texture_slot_t *slot = POOL_GET_AS(&renderer->textures, texture, texture_slot_t);
    if (slot == NULL) {
        return;
    }
    renderer->backend->destroy_texture(renderer->backend, slot->backend_handle);
    pool_free(&renderer->textures, texture);
}

void renderer_destroy_mesh(renderer_t *renderer, mesh_handle_t mesh)
{
    ASSERT(renderer != NULL);

    mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    if (slot == NULL) {
        return;
    }
    renderer->backend->destroy_mesh(renderer->backend, slot->backend_handle);
    pool_free(&renderer->meshes, mesh);
}

void renderer_destroy_pipeline(renderer_t *renderer, pipeline_handle_t pipeline)
{
    ASSERT(renderer != NULL);

    pipeline_slot_t *slot = POOL_GET_AS(&renderer->pipelines, pipeline, pipeline_slot_t);
    if (slot == NULL) {
        return;
    }
    renderer->backend->destroy_pipeline(renderer->backend, slot->backend_handle);
    pool_free(&renderer->pipelines, pipeline);
}

// =============================================================================
//  资源查询
// =============================================================================
#define DEFINE_RESOURCE_QUERY(kind, slot_type, pool_member)                                        \
    bool renderer_##kind##_is_valid(const renderer_t *renderer, kind##_handle_t handle)            \
    {                                                                                              \
        return POOL_GET_AS(&renderer->pool_member, handle, slot_type) != NULL;                      \
    }                                                                                              \
    const char *renderer_##kind##_name(const renderer_t *renderer, kind##_handle_t handle)         \
    {                                                                                              \
        const slot_type *slot = POOL_GET_AS(&renderer->pool_member, handle, slot_type);             \
        return (slot != NULL && slot->name != NULL) ? slot->name : "<unnamed>";                    \
    }

DEFINE_RESOURCE_QUERY(shader, shader_slot_t, shaders)
DEFINE_RESOURCE_QUERY(texture, texture_slot_t, textures)
DEFINE_RESOURCE_QUERY(mesh, mesh_slot_t, meshes)
DEFINE_RESOURCE_QUERY(pipeline, pipeline_slot_t, pipelines)

u32 renderer_texture_width(const renderer_t *renderer, texture_handle_t texture)
{
    const texture_slot_t *slot = POOL_GET_AS(&renderer->textures, texture, texture_slot_t);
    return (slot != NULL) ? slot->width : 0;
}

u32 renderer_texture_height(const renderer_t *renderer, texture_handle_t texture)
{
    const texture_slot_t *slot = POOL_GET_AS(&renderer->textures, texture, texture_slot_t);
    return (slot != NULL) ? slot->height : 0;
}

u32 renderer_mesh_vertex_count(const renderer_t *renderer, mesh_handle_t mesh)
{
    const mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    return (slot != NULL) ? slot->vertex_count : 0;
}

u32 renderer_mesh_index_count(const renderer_t *renderer, mesh_handle_t mesh)
{
    const mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    return (slot != NULL) ? slot->index_count : 0;
}

bool renderer_mesh_bounds(const renderer_t *renderer, mesh_handle_t mesh, vec3_t *out_min,
                          vec3_t *out_max)
{
    const mesh_slot_t *slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    if (slot == NULL) {
        return false;
    }
    if (out_min != NULL) {
        *out_min = slot->bounds_min;
    }
    if (out_max != NULL) {
        *out_max = slot->bounds_max;
    }
    return true;
}

u32 renderer_shader_count(const renderer_t *renderer)
{
    return pool_count(&renderer->shaders);
}
u32 renderer_texture_count(const renderer_t *renderer)
{
    return pool_count(&renderer->textures);
}
u32 renderer_mesh_count(const renderer_t *renderer)
{
    return pool_count(&renderer->meshes);
}
u32 renderer_pipeline_count(const renderer_t *renderer)
{
    return pool_count(&renderer->pipelines);
}

u64 renderer_frame_index(const renderer_t *renderer)
{
    return renderer->frame_index;
}

// =============================================================================
//  帧
// =============================================================================
void renderer_set_camera(renderer_t *renderer, const camera_t *camera)
{
    ASSERT(renderer != NULL);
    if (camera != NULL) {
        renderer->camera = *camera;
        renderer->has_camera = true;
    }
}

const camera_t *renderer_camera(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return &renderer->camera;
}

void renderer_set_ambient(renderer_t *renderer, vec3_t color, f32 intensity)
{
    ASSERT(renderer != NULL);
    renderer->frame_uniforms.ambient = vec4_make(color.x, color.y, color.z, intensity);
}

void renderer_set_directional_light(renderer_t *renderer, vec3_t direction, vec3_t color,
                                    f32 intensity)
{
    ASSERT(renderer != NULL);

    const vec3_t normalized = vec3_normalize(direction);
    // w = 是否启用。方向为零向量时视为关闭，避免 normalize 出 NaN 之后
    // 光照全黑（"没光"和"矩阵错了"看起来一样，很难排查）。
    const f32 enabled = (vec3_length_sq(direction) > 1.0e-12f) ? 1.0f : 0.0f;
    renderer->frame_uniforms.light_direction =
        vec4_make(normalized.x, normalized.y, normalized.z, enabled);
    renderer->frame_uniforms.light_color = vec4_make(color.x, color.y, color.z, intensity);
}

arena_t *renderer_frame_arena(renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return &renderer->frame_arena;
}

void renderer_begin_frame(renderer_t *renderer, const camera_t *camera,
                          const frame_desc_t *frame_desc)
{
    ASSERT(renderer != NULL);

    renderer->frame_index++;

    // 帧 arena 归零。稳态下这里不产生任何系统分配 ——
    // 这是"帧内零 malloc"的落点。
    arena_reset(&renderer->frame_arena);

    if (camera != NULL) {
        renderer->camera = *camera;
        renderer->has_camera = true;
    }

    if (frame_desc != NULL) {
        renderer->frame_desc = *frame_desc;
    } else {
        renderer->frame_desc.target_width = 0;
        renderer->frame_desc.target_height = 0;
        renderer->frame_desc.frame_index = renderer->frame_index;
    }
    renderer->frame_desc.frame_index = renderer->frame_index;

    render_backend_begin_frame(renderer->backend, &renderer->frame_desc);
    render_backend_reset_stats(renderer->backend);

    // 帧常量：一次上传，供本帧所有 draw 使用
    renderer->frame_uniforms.view = camera_view_matrix(&renderer->camera);
    renderer->frame_uniforms.projection = camera_projection_matrix(&renderer->camera);
    renderer->frame_uniforms.view_projection =
        mat4_mul(renderer->frame_uniforms.projection, renderer->frame_uniforms.view);
    renderer->frame_uniforms.camera_position =
        vec4_make(renderer->camera.position.x, renderer->camera.position.y,
                  renderer->camera.position.z, 1.0f);

    // 逆视图投影：天空盒用它从 NDC 反推世界空间射线。
    // 每帧算一次（不是每个像素），代价可以忽略。
    renderer->frame_uniforms.inv_view_projection =
        mat4_inverse(renderer->frame_uniforms.view_projection);

    // 光源
    renderer->frame_uniforms.light_params.x = (f32)renderer->lights.count;
    light_set_to_gpu(&renderer->lights, renderer->frame_uniforms.lights);

    // 主平行光（兼容 M2/M3 的旧着色器）：取第一盏平行光
    {
        vec4_t direction = vec4_zero();
        vec4_t color = vec4_zero();
        for (u32 i = 0; i < renderer->lights.count; ++i) {
            const light_t *light = &renderer->lights.lights[i];
            if (light->type == LIGHT_TYPE_DIRECTIONAL) {
                const vec3_t to_light = vec3_negate(light->direction);
                direction = vec4_make(to_light.x, to_light.y, to_light.z, 1.0f);
                color = vec4_make(light->color.x, light->color.y, light->color.z, light->intensity);
                break;
            }
        }
        renderer->frame_uniforms.light_direction = direction;
        renderer->frame_uniforms.light_color = color;
    }

    // 视锥（用于剔除）
    renderer->frustum =
        frustum_from_view_projection(renderer->frame_uniforms.view_projection);
    renderer->culled_count = 0;

    // 场景目标尺寸跟随窗口。
    // **必须在这里同步** —— 窗口一缩放，尺寸不匹配的帧缓冲会变成"不完整"，
    // GL 会静默丢弃所有绘制（表现是"一缩放就全黑"）。
    if (renderer->scene_target_enabled) {
        const u32 target_width = renderer->frame_desc.target_width;
        const u32 target_height = renderer->frame_desc.target_height;

        if (target_width > 0 && target_height > 0) {
            if (renderer->scene_target == RENDER_TARGET_HANDLE_INVALID) {
                render_target_desc_t rt_desc = render_target_desc_default();
                rt_desc.width = target_width;
                rt_desc.height = target_height;
                rt_desc.samples = renderer->scene_msaa_samples;
                rt_desc.has_depth = true;
                rt_desc.sampleable_color = true;
                rt_desc.debug_name = "scene";
                renderer->scene_target =
                    renderer_create_render_target_internal(renderer, &rt_desc);
                if (renderer->scene_target == RENDER_TARGET_HANDLE_INVALID) {
                    LOG_ERROR("场景渲染目标创建失败，本帧将直接渲染到窗口");
                    renderer->scene_target_enabled = false;
                }
            } else {
                const render_target_slot_t *slot =
                    POOL_GET_AS(&renderer->render_targets, renderer->scene_target,
                                render_target_slot_t);
                if (slot != NULL &&
                    (slot->width != target_width || slot->height != target_height)) {
                    renderer_resize_render_target_internal(renderer, renderer->scene_target,
                                                           target_width, target_height);
                }
            }
        }
    }

    render_backend_set_uniforms(renderer->backend, RENDER_UNIFORM_BINDING_FRAME,
                                &renderer->frame_uniforms, sizeof(renderer->frame_uniforms));

    render_queue_begin(&renderer->queue, &renderer->camera);
}

u32 renderer_push_item(renderer_t *renderer, const render_item_t *item)
{
    ASSERT(renderer != NULL);
    return render_queue_push(&renderer->queue, item);
}

u32 renderer_push_mesh(renderer_t *renderer, const material_t *material, mesh_handle_t mesh,
                       mat4_t model_matrix, render_layer_t layer)
{
    ASSERT(renderer != NULL);

    const mesh_slot_t *mesh_slot = POOL_GET_AS(&renderer->meshes, mesh, mesh_slot_t);
    if (mesh_slot == NULL) {
        return 0xFFFFFFFFu;
    }

    // 视锥剔除。用**世界空间**的 AABB 做判据：
    // 局部包围盒经模型矩阵变换后，旋转过的物体会让包围盒变大（保守），
    // 但绝不会把可见物体判成不可见 —— 后者才是不能接受的错误。
    if (renderer->cull_enabled) {
        vec3_t world_min;
        vec3_t world_max;
        frustum_transform_aabb(model_matrix, mesh_slot->bounds_min, mesh_slot->bounds_max,
                               &world_min, &world_max);
        if (!frustum_intersects_aabb(&renderer->frustum, world_min, world_max)) {
            renderer->culled_count++;
            return 0xFFFFFFFFu;
        }
    }

    render_item_t item = render_item_default();
    item.mesh = mesh; // 引擎句柄；提交时再解析成后端句柄
    item.pipeline = (material != NULL) ? material->pipeline : PIPELINE_HANDLE_INVALID;
    item.layer = layer;
    item.uniforms = object_uniforms_from_model(model_matrix);

    if (material != NULL) {
        item.uniforms.tint = material->tint;
        // PBR 参数跟着材质走。不复制的话所有物体都会用 object_uniforms_default()
        // 里的默认值 —— 症状是"材质网格里 20 个立方体长得一模一样"。
        item.uniforms.pbr_params = material->pbr_params;
        item.uniforms.emissive = material->emissive;
        item.texture_count = material->texture_count;
        for (u32 i = 0; i < material->texture_count && i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
            item.textures[i] = material->textures[i];
            item.samplers[i] = material->samplers[i];
        }
    }

    // 用包围盒中心而不是模型矩阵的平移分量算深度。
    // 对大网格（例如地形）来说，平移分量可能离物体实际位置很远，
    // 排序结果会明显错误 —— 表现为"地形被画在半透明物体之后"。
    const vec3_t local_center =
        vec3_scale(vec3_add(mesh_slot->bounds_min, mesh_slot->bounds_max), 0.5f);
    const vec3_t world_center = mat4_transform_point(model_matrix, local_center);
    item.view_depth = renderer->has_camera
                          ? camera_view_depth(&renderer->camera, world_center)
                          : 0.0f;

    return render_queue_push(&renderer->queue, &item);
}

void renderer_end_frame(renderer_t *renderer)
{
    ASSERT(renderer != NULL);

    render_queue_sort(&renderer->queue);

    const u32            count = render_queue_count(&renderer->queue);
    const render_item_t *items = render_queue_items(&renderer->queue);

    // 场景 pass 的目标：开了场景目标就渲染到离屏，否则直接画到窗口。
    const render_target_handle_t scene_target =
        renderer->scene_target_enabled ? resolve_render_target(renderer, renderer->scene_target)
                                       : RENDER_TARGET_HANDLE_INVALID;

    render_pass_desc_t pass;
    memset(&pass, 0, sizeof(pass));
    pass.target = scene_target;
    // 视口尺寸留 0，后端会用目标尺寸兜底
    pass.clear_color = true;
    pass.clear_color_value[0] = renderer->clear_color_value.x;
    pass.clear_color_value[1] = renderer->clear_color_value.y;
    pass.clear_color_value[2] = renderer->clear_color_value.z;
    pass.clear_color_value[3] = renderer->clear_color_value.w;
    pass.clear_depth = true;
    pass.clear_depth_value = renderer->clear_depth_value;
    pass.debug_name = "scene_pass";

    render_backend_begin_render_pass(renderer->backend, &pass);

    // 阴影贴图作为全局纹理绑定（槽位 5）。
    // 它不属于任何单个材质，所以在这里统一绑一次，
    // 而不是让每个材质都带上它。
    if (renderer->shadow_texture != TEXTURE_HANDLE_INVALID) {
        render_backend_bind_texture(renderer->backend, RENDER_TEXTURE_SLOT_SHADOW,
                                    resolve_texture(renderer, renderer->shadow_texture), NULL);
    }

    // ---- 天空盒先画 ----
    //
    // 顺序很重要：天空盒必须**最先**画，且关闭深度测试与深度写入。
    // 如果放在最后画，半透明物体会被它盖住 —— 因为半透明不写深度。
    if (renderer->frame_uniforms.sky_params.x > 0.5f) {
        render_backend_bind_pipeline(renderer->backend,
                                     resolve_pipeline(renderer, renderer->sky_pipeline));
        render_backend_draw(renderer->backend,
                            resolve_mesh(renderer, renderer->fullscreen_triangle), NULL);
    }

    // ---- 场景内容 ----
    {
        pipeline_handle_t bound_pipeline = PIPELINE_HANDLE_INVALID;

        for (u32 i = 0; i < count; ++i) {
            const render_item_t *item = &items[i];

            const pipeline_handle_t pipeline = resolve_pipeline(renderer, item->pipeline);
            if (pipeline == PIPELINE_HANDLE_INVALID) {
                continue;
            }

            // 管线切换由排序保证尽量少：同管线的物体已经被排到一起
            if (pipeline != bound_pipeline) {
                render_backend_bind_pipeline(renderer->backend, pipeline);
                bound_pipeline = pipeline;
            }

            for (u32 slot = 0; slot < item->texture_count && slot < RENDER_MAX_TEXTURE_SLOTS;
                 ++slot) {
                render_backend_bind_texture(renderer->backend, slot,
                                            resolve_texture(renderer, item->textures[slot]),
                                            &item->samplers[slot]);
            }

            render_backend_set_uniforms(renderer->backend, RENDER_UNIFORM_BINDING_OBJECT,
                                        &item->uniforms, sizeof(item->uniforms));

            render_backend_draw(renderer->backend, resolve_mesh(renderer, item->mesh), &item->call);
        }
    }

    render_backend_end_render_pass(renderer->backend);

    // ---- present pass：线性 -> sRGB 编码 ----
    //
    // 这是整个引擎里唯一一处做 sRGB 转换的地方。收成一个 pass 之后，
    // "颜色空间对不对"只需要验证这一个地方（见 renderer_read_scene_pixel）。
    if (renderer->scene_target_enabled &&
        renderer->scene_target != RENDER_TARGET_HANDLE_INVALID) {
        render_pass_desc_t present_pass;
        memset(&present_pass, 0, sizeof(present_pass));
        present_pass.target = RENDER_TARGET_HANDLE_INVALID; // 窗口
        present_pass.clear_color = false;                   // 全屏覆盖，不需要清
        present_pass.clear_depth = false;
        present_pass.debug_name = "present_pass";

        render_backend_begin_render_pass(renderer->backend, &present_pass);

        render_backend_bind_pipeline(renderer->backend,
                                     resolve_pipeline(renderer, renderer->present_pipeline));
        render_backend_bind_texture(
            renderer->backend, RENDER_TEXTURE_SLOT_ALBEDO,
            resolve_texture(renderer, renderer_scene_color_texture(renderer)), NULL);
        render_backend_draw(renderer->backend,
                            resolve_mesh(renderer, renderer->fullscreen_triangle), NULL);

        render_backend_end_render_pass(renderer->backend);
    }

    // ---- present 像素回读（sRGB 自检）----
    //
    // 必须在这里读：present pass 已经写完窗口的后缓冲，
    // 但 end_frame（内部会 swap buffers）还没调用。
    // 放在外面读的话拿到的是下一帧还没画的后缓冲，值是不可预测的。
    if (renderer->capture_requested) {
        renderer->captured_valid = render_backend_read_pixel(
            renderer->backend, RENDER_TARGET_HANDLE_INVALID, renderer->capture_x,
            renderer->capture_y, renderer->captured_pixel);
        renderer->capture_requested = false;
    }

    render_backend_get_stats(renderer->backend, &renderer->last_stats);
    renderer->last_stats.render_items = count;

    render_backend_end_frame(renderer->backend);
}

void renderer_get_stats(const renderer_t *renderer, render_stats_t *out_stats)
{
    ASSERT(renderer != NULL);
    if (out_stats != NULL) {
        *out_stats = renderer->last_stats;
    }
}

u32 renderer_queue_count(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return render_queue_count(&renderer->queue);
}

u32 renderer_queue_peak_count(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return render_queue_peak_count(&renderer->queue);
}

const render_queue_t *renderer_queue(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return &renderer->queue;
}

void renderer_set_clear(renderer_t *renderer, bool clear_color, vec4_t color, bool clear_depth,
                        f32 depth_value)
{
    ASSERT(renderer != NULL);
    renderer->clear_color = clear_color;
    renderer->clear_color_value = color;
    renderer->clear_depth = clear_depth;
    renderer->clear_depth_value = depth_value;
}

// =============================================================================
//  渲染目标（内部入口，供 render_target.c 调用）
// =============================================================================

// 把一个**已存在的后端纹理句柄**登记进渲染器的纹理池，返回引擎侧句柄。
//
// 注意它不创建后端纹理 —— 纹理是渲染目标的附件，由后端自己管。
// owns_backend_handle 置 false，所以销毁时不会去释放后端的句柄：
// 那是渲染目标销毁时的职责（后端在 release_attachments 里做）。
static texture_handle_t register_backend_texture(renderer_t *renderer,
                                                 texture_handle_t backend_handle, u32 width,
                                                 u32 height, pixel_format_t format,
                                                 const char *name)
{
    if (backend_handle == TEXTURE_HANDLE_INVALID) {
        return TEXTURE_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->textures);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("纹理槽位耗尽（渲染目标附件登记失败）");
        return TEXTURE_HANDLE_INVALID;
    }

    texture_slot_t *slot = POOL_GET_AS(&renderer->textures, handle, texture_slot_t);
    slot->backend_handle = backend_handle;
    slot->width = width;
    slot->height = height;
    slot->format = format;
    slot->name = name;
    slot->creation_frame = renderer->frame_index;
    slot->owns_backend_handle = false; // 所有权在渲染目标手里
    return handle;
}

static void unregister_backend_texture(renderer_t *renderer, texture_handle_t handle)
{
    if (handle == TEXTURE_HANDLE_INVALID) {
        return;
    }
    texture_slot_t *slot = POOL_GET_AS(&renderer->textures, handle, texture_slot_t);
    if (slot == NULL) {
        return;
    }
    if (slot->owns_backend_handle) {
        renderer->backend->destroy_texture(renderer->backend, slot->backend_handle);
    }
    pool_free(&renderer->textures, handle);
}

// 把渲染目标的附件登记进渲染器纹理池（创建与 resize 之后都要做一次）。
static texture_handle_t register_render_target_attachment(renderer_t *renderer,
                                                          const render_target_desc_t *desc,
                                                          render_target_handle_t backend_handle)
{
    const render_target_desc_t norm = render_target_desc_normalize(desc);
    const texture_handle_t backend_texture =
        render_backend_get_render_target_texture(renderer->backend, backend_handle);
    const pixel_format_t format =
        norm.depth_only ? norm.depth_format : norm.color_format;

    return register_backend_texture(renderer, backend_texture, norm.width, norm.height, format,
                                    norm.debug_name);
}

render_target_handle_t renderer_create_render_target_internal(renderer_t *renderer,
                                                              const render_target_desc_t *desc)
{
    ASSERT(renderer != NULL);

    const render_target_handle_t backend_handle =
        render_backend_create_render_target(renderer->backend, desc);
    if (backend_handle == RENDER_TARGET_HANDLE_INVALID) {
        return RENDER_TARGET_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&renderer->render_targets);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("渲染目标槽位耗尽");
        renderer->backend->destroy_render_target(renderer->backend, backend_handle);
        return RENDER_TARGET_HANDLE_INVALID;
    }

    render_target_slot_t *slot = POOL_GET_AS(&renderer->render_targets, handle,
                                             render_target_slot_t);
    memset(slot, 0, sizeof(*slot));
    slot->backend_handle = backend_handle;
    slot->width = (desc != NULL) ? desc->width : 0;
    slot->height = (desc != NULL) ? desc->height : 0;
    slot->samples = (desc != NULL && desc->samples > 0) ? desc->samples : 1;
    slot->name = (desc != NULL) ? desc->debug_name : NULL;

    // 把附件纹理登记进渲染器纹理池。漏掉这一步的话，所有通过
    // render_target_texture 拿到的句柄都指向错误的槽位 ——
    // 表现是 present pass / 阴影采样拿到默认白纹理，画面一片白且不报错。
    slot->texture = register_render_target_attachment(renderer, desc, backend_handle);
    if (slot->texture == TEXTURE_HANDLE_INVALID) {
        LOG_WARN("渲染目标 %s 的附件纹理登记失败", (slot->name != NULL) ? slot->name : "?");
    }

    return handle;
}

void renderer_destroy_render_target_internal(renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);

    render_target_slot_t *slot =
        POOL_GET_AS(&renderer->render_targets, target, render_target_slot_t);
    if (slot == NULL) {
        return;
    }

    unregister_backend_texture(renderer, slot->texture);
    renderer->backend->destroy_render_target(renderer->backend, slot->backend_handle);
    pool_free(&renderer->render_targets, target);
}

bool renderer_resize_render_target_internal(renderer_t *renderer, render_target_handle_t target,
                                            u32 width, u32 height)
{
    ASSERT(renderer != NULL);

    render_target_slot_t *slot =
        POOL_GET_AS(&renderer->render_targets, target, render_target_slot_t);
    if (slot == NULL) {
        return false;
    }

    if (!render_backend_resize_render_target(renderer->backend, slot->backend_handle, width,
                                             height)) {
        return false;
    }

    // 重建附件会产生**新的**后端纹理句柄，旧的已经失效。
    // 不重新登记的话，窗口一缩放画面就会变成一片白 ——
    // 因为采样的是一个已经被销毁的纹理句柄。
    unregister_backend_texture(renderer, slot->texture);
    slot->texture = TEXTURE_HANDLE_INVALID;

    {
        render_target_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.width = width;
        desc.height = height;
        desc.samples = slot->samples;
        desc.has_depth = true;
        desc.sampleable_color = true;
        desc.debug_name = slot->name;
        slot->texture = register_render_target_attachment(renderer, &desc, slot->backend_handle);
    }

    slot->width = width;
    slot->height = height;
    return true;
}

texture_handle_t renderer_render_target_texture_internal(renderer_t *renderer,
                                                         render_target_handle_t target)
{
    ASSERT(renderer != NULL);

    const render_target_slot_t *slot =
        POOL_GET_AS(&renderer->render_targets, target, render_target_slot_t);
    if (slot == NULL) {
        return TEXTURE_HANDLE_INVALID;
    }

    // 返回**渲染器纹理池**里的句柄（创建/重建时登记好的）。
    //
    // 曾经这里直接返回后端句柄 —— 那是错的：两个池的索引空间不相干，
    // resolve_texture 会查到别的槽位或直接判定无效，
    // 结果是绑定循环静默绑上默认白纹理，画面整体变白且无任何报错。
    return slot->texture;
}

const render_target_slot_t *renderer_render_target_slot(const renderer_t *renderer,
                                                         render_target_handle_t target)
{
    return POOL_GET_AS(&renderer->render_targets, target, render_target_slot_t);
}

// =============================================================================
//  光照 / 雾 / 天空盒 / 色调映射 / 剔除
// =============================================================================
void renderer_set_lights(renderer_t *renderer, const light_set_t *lights)
{
    ASSERT(renderer != NULL);

    if (lights == NULL) {
        light_set_clear(&renderer->lights);
        return;
    }
    renderer->lights = *lights;
}

const light_set_t *renderer_lights(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return &renderer->lights;
}

void renderer_set_fog(renderer_t *renderer, bool enabled, vec3_t color, f32 start_distance,
                      f32 end_distance, f32 density)
{
    ASSERT(renderer != NULL);

    renderer->frame_uniforms.fog_params =
        vec4_make(enabled ? 1.0f : 0.0f, start_distance, math_max(end_distance, start_distance + 0.01f),
                  density);
    renderer->frame_uniforms.fog_color = vec4_make(color.x, color.y, color.z, 1.0f);
}

void renderer_set_skybox(renderer_t *renderer, bool enabled, vec3_t horizon, vec3_t zenith,
                         vec3_t ground)
{
    ASSERT(renderer != NULL);

    renderer->frame_uniforms.sky_params.x = enabled ? 1.0f : 0.0f;
    renderer->frame_uniforms.sky_horizon = vec4_make(horizon.x, horizon.y, horizon.z, 1.0f);
    renderer->frame_uniforms.sky_zenith = vec4_make(zenith.x, zenith.y, zenith.z, 1.0f);
    renderer->frame_uniforms.sky_ground = vec4_make(ground.x, ground.y, ground.z, 1.0f);
}

void renderer_set_tonemap(renderer_t *renderer, tonemap_mode_t mode, f32 exposure)
{
    ASSERT(renderer != NULL);

    renderer->frame_uniforms.sky_params.y = (f32)mode;
    renderer->frame_uniforms.light_params.w = (exposure > 0.0f) ? exposure : 1.0f;
}

void renderer_set_shadow_params(renderer_t *renderer, f32 strength, u32 shadow_map_size)
{
    ASSERT(renderer != NULL);

    renderer->frame_uniforms.light_params.y = math_saturate(strength);
    renderer->frame_uniforms.light_params.z = (f32)shadow_map_size;
}

void renderer_set_light_view_projection(renderer_t *renderer, mat4_t light_view_projection)
{
    ASSERT(renderer != NULL);
    renderer->frame_uniforms.light_view_projection = light_view_projection;
}

void renderer_set_culling_enabled(renderer_t *renderer, bool enabled)
{
    ASSERT(renderer != NULL);
    renderer->cull_enabled = enabled;
}

bool renderer_culling_enabled(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->cull_enabled;
}

u32 renderer_culled_count(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->culled_count;
}

void renderer_set_msaa_samples(renderer_t *renderer, u32 samples)
{
    ASSERT(renderer != NULL);
    // 改了之后场景目标会在下一帧按新采样数重建（尺寸判断走的是 width/height，
    // 所以这里直接把尺寸清零强制重建）。
    if (renderer->scene_msaa_samples != samples && renderer->scene_target != RENDER_TARGET_HANDLE_INVALID) {
        renderer_destroy_render_target_internal(renderer, renderer->scene_target);
        renderer->scene_target = RENDER_TARGET_HANDLE_INVALID;
    }
    renderer->scene_msaa_samples = (samples > 0) ? samples : 1;
}

u32 renderer_msaa_samples(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->scene_msaa_samples;
}

// =============================================================================
//  渲染目标（引擎层公开 API）
//
//  实现在 renderer.c 里而不是单独的 render_target.c：
//  这些函数需要访问 renderer 的私有结构（资源池），
//  拆出去就必须把内部结构暴露到头文件里 —— 得不偿失。
// =============================================================================


render_target_handle_t render_target_create(renderer_t *renderer,
                                            const render_target_desc_t *desc)
{
    ASSERT(renderer != NULL);

    if (desc == NULL || desc->width == 0 || desc->height == 0) {
        LOG_ERROR("渲染目标尺寸无效");
        return RENDER_TARGET_HANDLE_INVALID;
    }
    return renderer_create_render_target_internal(renderer, desc);
}

void render_target_destroy(renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    renderer_destroy_render_target_internal(renderer, target);
}

bool render_target_resize(renderer_t *renderer, render_target_handle_t target, u32 width,
                          u32 height)
{
    ASSERT(renderer != NULL);
    return renderer_resize_render_target_internal(renderer, target, width, height);
}

texture_handle_t render_target_texture(renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    // 不接收 const：后端查询纹理句柄的接口本身不是 const 的
    // （它可能触发懒解析）。为了几个调用点去 const_cast 不值得。
    return renderer_render_target_texture_internal(renderer, target);
}

u32 render_target_width(const renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    const render_target_slot_t *slot = renderer_render_target_slot(renderer, target);
    return (slot != NULL) ? slot->width : 0;
}

u32 render_target_height(const renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    const render_target_slot_t *slot = renderer_render_target_slot(renderer, target);
    return (slot != NULL) ? slot->height : 0;
}

u32 render_target_samples(const renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    const render_target_slot_t *slot = renderer_render_target_slot(renderer, target);
    return (slot != NULL) ? slot->samples : 0;
}

bool render_target_is_valid(const renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    return renderer_render_target_slot(renderer, target) != NULL;
}

const char *render_target_debug_name(const renderer_t *renderer, render_target_handle_t target)
{
    ASSERT(renderer != NULL);
    const render_target_slot_t *slot = renderer_render_target_slot(renderer, target);
    return (slot != NULL && slot->name != NULL) ? slot->name : "<unnamed>";
}

// =============================================================================
//  场景目标
// =============================================================================
render_target_handle_t renderer_scene_target(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->scene_target_enabled ? renderer->scene_target : RENDER_TARGET_HANDLE_INVALID;
}

texture_handle_t renderer_scene_color_texture(renderer_t *renderer)
{
    ASSERT(renderer != NULL);

    if (!renderer->scene_target_enabled || renderer->scene_target == RENDER_TARGET_HANDLE_INVALID) {
        return TEXTURE_HANDLE_INVALID;
    }
    return renderer_render_target_texture_internal(renderer, renderer->scene_target);
}

void renderer_set_scene_target_enabled(renderer_t *renderer, bool enabled)
{
    ASSERT(renderer != NULL);
    renderer->scene_target_enabled = enabled;
}

bool renderer_scene_target_enabled(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->scene_target_enabled;
}

// =============================================================================
//  读回像素（sRGB 自检）
// =============================================================================
bool render_target_read_pixel(renderer_t *renderer, render_target_handle_t target, u32 x, u32 y,
                              u8 out_rgba[4])
{
    ASSERT(renderer != NULL);

    if (out_rgba == NULL) {
        return false;
    }
    memset(out_rgba, 0, 4);

    const render_target_slot_t *slot = renderer_render_target_slot(renderer, target);
    if (slot == NULL) {
        return false;
    }
    if (x >= slot->width || y >= slot->height) {
        return false;
    }

    // 读回必须走"当前绑定的渲染目标"。
    // 这里不改变绑定状态 —— 调用方应该在 begin_frame 之后、end_frame 之前调用它，
    // 此时场景目标正处于绑定状态（end_frame 里会切到窗口做 present）。
    return render_backend_read_pixel(renderer->backend, slot->backend_handle, x, y, out_rgba);
}

bool renderer_read_scene_pixel(renderer_t *renderer, u32 x, u32 y, u8 out_rgba[4])
{
    ASSERT(renderer != NULL);

    if (!renderer->scene_target_enabled || renderer->scene_target == RENDER_TARGET_HANDLE_INVALID) {
        LOG_WARN("场景目标未启用，无法读回像素");
        return false;
    }
    return render_target_read_pixel(renderer, renderer->scene_target, x, y, out_rgba);
}

bool renderer_read_scene_rect(renderer_t *renderer, u32 x, u32 y, u32 width, u32 height,
                              u8 *out_rgba)
{
    ASSERT(renderer != NULL);

    if (!renderer->scene_target_enabled || renderer->scene_target == RENDER_TARGET_HANDLE_INVALID) {
        LOG_WARN("场景目标未启用，无法读回像素");
        return false;
    }
    if (out_rgba == NULL || width == 0 || height == 0) {
        return false;
    }

    const render_target_slot_t *slot = renderer_render_target_slot(renderer, renderer->scene_target);
    if (slot == NULL || x + width > slot->width || y + height > slot->height) {
        return false;
    }

    return render_backend_read_rect(renderer->backend, slot->backend_handle, x, y, width, height,
                                    out_rgba);
}

void renderer_request_present_capture(renderer_t *renderer, u32 x, u32 y)
{
    ASSERT(renderer != NULL);
    renderer->capture_requested = true;
    renderer->capture_x = x;
    renderer->capture_y = y;
    renderer->captured_valid = false;
}

bool renderer_captured_pixel(const renderer_t *renderer, u8 out_rgba[4])
{
    ASSERT(renderer != NULL);
    if (!renderer->captured_valid || out_rgba == NULL) {
        return false;
    }
    memcpy(out_rgba, renderer->captured_pixel, 4);
    return true;
}

// =============================================================================
//  深度 pass 与阴影相关
// =============================================================================
void renderer_flush_frame_uniforms(renderer_t *renderer)
{
    ASSERT(renderer != NULL);

    // 把当前的帧 uniform 立刻上传。
    // 深度 pass 需要 u_light_view_projection 已经就位才能画，
    // 而它在 begin_frame 时上传过一次 —— 之后才算出阴影矩阵，
    // 所以要再传一次。
    render_backend_set_uniforms(renderer->backend, RENDER_UNIFORM_BINDING_FRAME,
                                &renderer->frame_uniforms, sizeof(renderer->frame_uniforms));
}

void renderer_render_depth_pass(renderer_t *renderer, render_target_handle_t target,
                                pipeline_handle_t depth_pipeline)
{
    ASSERT(renderer != NULL);

    const render_target_handle_t resolved_target = resolve_render_target(renderer, target);
    const pipeline_handle_t      resolved_pipeline = resolve_pipeline(renderer, depth_pipeline);

    if (resolved_target == RENDER_TARGET_HANDLE_INVALID ||
        resolved_pipeline == PIPELINE_HANDLE_INVALID) {
        return;
    }

    render_pass_desc_t pass;
    memset(&pass, 0, sizeof(pass));
    pass.target = resolved_target;
    // 只清深度：深度专用目标没有颜色附件，清颜色会触发 GL_INVALID_OPERATION
    pass.clear_color = false;
    pass.clear_depth = true;
    pass.clear_depth_value = 1.0f;
    pass.debug_name = "depth_pass";

    render_backend_begin_render_pass(renderer->backend, &pass);
    render_backend_bind_pipeline(renderer->backend, resolved_pipeline);

    // 把队列里的所有几何用深度管线重画一遍。
    // 用同一个队列而不是让调用方再提交一遍：保证阴影与可见内容完全一致
    // （漏掉一个物体就会出现"这个物体没有影子"的诡异画面）。
    const u32            count = render_queue_count(&renderer->queue);
    const render_item_t *items = render_queue_items(&renderer->queue);

    for (u32 i = 0; i < count; ++i) {
        // 线框/调试绘制这类非三角形拓扑的项不参与阴影 ——
        // 用三角形深度管线画它们会往阴影贴图里塞垃圾三角形。
        if (!items[i].cast_shadow) {
            continue;
        }
        render_backend_set_uniforms(renderer->backend, RENDER_UNIFORM_BINDING_OBJECT,
                                    &items[i].uniforms, sizeof(items[i].uniforms));
        render_backend_draw(renderer->backend, resolve_mesh(renderer, items[i].mesh),
                            &items[i].call);
    }

    render_backend_end_render_pass(renderer->backend);
}

void renderer_set_shadow_texture(renderer_t *renderer, texture_handle_t shadow_texture)
{
    ASSERT(renderer != NULL);
    renderer->shadow_texture = shadow_texture;
}

void renderer_set_shadow_normal_bias(renderer_t *renderer, f32 normal_bias)
{
    ASSERT(renderer != NULL);
    renderer->shadow_normal_bias = math_max(normal_bias, 0.0f);
    // 同步到 uniform。这一步不能漏：只存进 renderer 而不上传的话，
    // 调用方以为改生效了，画面却一点不变 —— 典型的静默失效。
    renderer->frame_uniforms.shadow_params.x = renderer->shadow_normal_bias;
}

f32 renderer_shadow_normal_bias(const renderer_t *renderer)
{
    ASSERT(renderer != NULL);
    return renderer->shadow_normal_bias;
}
