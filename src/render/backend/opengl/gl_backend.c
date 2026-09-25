// =============================================================================
//  src/render/backend/opengl/gl_backend.c —— render_backend_t 的 OpenGL 实现
//
//  ============================================================================
//   本文件的核心工作：把 Vulkan 语义的接口"降级适配"到 GL 的状态机
//  ============================================================================
//
//  接口是按 Vulkan 设计的（不可变管线状态对象、显式 render pass、uniform 块），
//  GL 没有这些概念。适配的做法：
//
//    pipeline_desc_t  ->  gl_pipeline_t（把状态摊平成一组字段）
//                         + gl_applied_state_t（缓存"当前 GL 实际处于什么状态"）
//                         bind_pipeline 时做 diff，只发真正变化的 glEnable/glBlendFunc
//
//    显式 render pass ->  begin_render_pass 里设置视口/裁剪/清屏
//                         （GL 只有默认帧缓冲，但调用方看到的语义与 Vulkan 一致）
//
//    uniform 块       ->  **真正的 UBO**（glBindBufferBase + glBufferSubData），
//                         而不是 glUniform*。强制 GL 走和 Vulkan 一样的路径。
//
//  ---- 为什么要有 gl_applied_state_t ----
//
//  GL 的状态切换是有成本的（驱动要校验、可能触发管线重编译）。
//  如果每次 bind_pipeline 都把全部状态重设一遍，一个 1000 物体的场景
//  就是几万次冗余状态调用。缓存后只发差异，实测能省掉 80% 以上的状态调用。
//
//  这也是"GL 后端要自己写而不是直接用现成封装"的价值所在：
//  状态缓存的策略直接影响性能，值得自己控制。
// =============================================================================
#include "engine/render/backend/opengl/gl_backend.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/gl_api.h"
#include "engine/render/gl_context.h"
#include "engine/render/gl_types.h"
#include "engine/render/render_backend.h"

// 前向声明：create_mesh 的失败路径会调用 destroy_mesh 做回滚，
// 而 destroy_mesh 定义在后面。
static void gl_backend_destroy_mesh(render_backend_t *self, mesh_handle_t mesh);

// =============================================================================
//  资源表
// =============================================================================
typedef struct gl_shader_resource_t {
    GLuint      program;
    const char *name;
    u64         creation_frame;
} gl_shader_resource_t;

typedef struct gl_texture_resource_t {
    GLuint         id;
    u32            width;
    u32            height;
    u32            mip_levels;
    pixel_format_t format;
    const char    *name;
    u64            creation_frame;
    // false 表示这张 GL 纹理的所有权不在纹理池手里
    // （例如它是某个渲染目标的附件）。销毁句柄时不删 GL 对象。
    bool           owns_id;
} gl_texture_resource_t;

// 渲染目标（FBO）。MSAA 时需要两个 FBO：多重采样的那个负责绘制，
// 单采样的那个（resolve）负责被采样。
typedef struct gl_render_target_resource_t {
    GLuint fbo;           // 绘制用（MSAA 时为多重采样 FBO）
    GLuint resolve_fbo;   // MSAA 解析目标（0 = 不需要解析）
    GLuint color_rb;      // 多重采样颜色 renderbuffer（仅 MSAA）
    GLuint depth_rb;      // 多重采样深度 renderbuffer（仅 MSAA）
    GLuint color_texture; // 可采样的颜色纹理
    GLuint depth_texture; // 可采样的深度纹理（阴影贴图用）

    texture_handle_t color_handle;
    texture_handle_t depth_handle;

    u32            width;
    u32            height;
    u32            samples;
    pixel_format_t color_format;
    pixel_format_t depth_format;
    bool           has_depth;
    bool           sampleable;
    bool           depth_only;
    bool           needs_resolve; // MSAA 且还没解析

    const char *name;
    u64         creation_frame;
} gl_render_target_resource_t;

// 前向声明：destroy / resize 在切走当前目标时要调用 bind
static void gl_backend_bind_render_target(render_backend_t *self, render_target_handle_t target);

typedef struct gl_mesh_resource_t {
    GLuint         vao;
    GLuint         vbo[RENDER_MAX_VERTEX_BINDINGS];
    GLuint         ibo;
    u32            binding_count;
    index_format_t index_format;
    u32            vertex_count;
    u32            index_count;
    const char    *name;
    u64            creation_frame;
} gl_mesh_resource_t;

// 管线：把 pipeline_desc_t 摊平成 GL 能直接用的形式。
// 创建时做一次翻译，绑定时只比较与下发 —— 这是"不可变状态对象"在 GL 上的等价物。
typedef struct gl_pipeline_resource_t {
    GLuint program;

    bool   blend_enabled;
    GLenum blend_src_color;
    GLenum blend_dst_color;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    GLenum blend_eq_color;
    GLenum blend_eq_alpha;
    u8     write_mask;

    bool   depth_test;
    bool   depth_write;
    GLenum depth_func;

    bool   cull_enabled;
    GLenum cull_face;
    GLenum front_face;
    GLenum polygon_mode;

    bool   multisample;
    GLenum topology;

    // 深度偏移（阴影贴图必需）。
    // 不做偏移会出现"阴影痤疮"（表面出现条纹自阴影）；
    // 偏移太大会出现"彼得潘现象"（阴影与物体分离）。
    bool depth_bias_enabled;
    f32  depth_bias_constant;
    f32  depth_bias_slope;

    const char *name;
    u64         creation_frame;
} gl_pipeline_resource_t;

// =============================================================================
//  已应用的 GL 状态
//
//  与 GL 的实际状态保持同步。bind_pipeline 时用它做 diff。
// =============================================================================
typedef struct gl_applied_state_t {
    bool   valid; // false 表示状态未知，必须全量重设
    GLuint program;

    bool   blend_enabled;
    GLenum blend_src_color;
    GLenum blend_dst_color;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    GLenum blend_eq_color;
    GLenum blend_eq_alpha;

    bool   depth_test;
    bool   depth_write;
    GLenum depth_func;

    bool   cull_enabled;
    GLenum cull_face;
    GLenum front_face;
    GLenum polygon_mode;

    bool multisample;
    bool scissor_enabled;
    bool depth_bias_enabled;

    u32    active_texture_unit;
    GLuint bound_texture[RENDER_MAX_TEXTURE_SLOTS];
    GLuint bound_sampler[RENDER_MAX_TEXTURE_SLOTS];
} gl_applied_state_t;

// 采样器对象缓存。
// GL 的采样参数存在两个地方：纹理对象上（glTexParameteri）和采样器对象上。
// 用采样器对象才能做到"同一张纹理配不同采样方式"（Vulkan 里 sampler 与 image
// 本来就是分开的对象）。这里做一个小缓存，避免每次绑定都创建/销毁。
#define GL_SAMPLER_CACHE_SIZE 16

typedef struct gl_sampler_cache_entry_t {
    sampler_desc_t desc;
    GLuint         id;
    bool           used;
} gl_sampler_cache_entry_t;

// =============================================================================
//  后端实例
// =============================================================================
typedef struct gl_backend_t {
    render_backend_t base; // 必须是第一个成员

    allocator_t       *alloc;
    platform_window_t *window;
    gl_context_t      *context;

    pool_t shaders;
    pool_t textures;
    pool_t meshes;
    pool_t pipelines;
    pool_t render_targets;

    GLuint uniform_buffers[RENDER_UNIFORM_BINDING_COUNT];
    u32    uniform_buffer_capacities[RENDER_UNIFORM_BINDING_COUNT];

    gl_applied_state_t    state;
    pipeline_handle_t     current_pipeline;
    render_target_handle_t current_target; // 当前绑定的渲染目标（INVALID = 默认帧缓冲）

    gl_sampler_cache_entry_t sampler_cache[GL_SAMPLER_CACHE_SIZE];
    u32                      sampler_cache_next;

    // 默认纹理：没有绑定纹理时用它们，保证着色器采样到的不是未定义内容
    GLuint default_white;
    GLuint default_flat_normal;

    render_backend_caps_t caps;
    render_stats_t        stats;
    frame_desc_t          frame;

    u64 resource_creation_counter;
    u32 render_pass_depth;
} gl_backend_t;

// 前向声明（需要 gl_backend_t 已定义）：shutdown 要遍历渲染目标释放附件
static void gl_render_target_release_attachments(gl_backend_t *backend,
                                                 gl_render_target_resource_t *rt);

// =============================================================================
//  枚举翻译
// =============================================================================

// 返回该格式的 (internal_format, external_format, gl_type)
static bool gl_translate_pixel_format(pixel_format_t format, GLint *out_internal,
                                      GLenum *out_external, GLenum *out_type)
{
    switch (format) {
    case PIXEL_FORMAT_R8_UNORM:
        *out_internal = GL_R8;
        *out_external = GL_RED;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_R8G8_UNORM:
        *out_internal = GL_RG8;
        *out_external = GL_RG;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_R8G8B8A8_UNORM:
        *out_internal = GL_RGBA8;
        *out_external = GL_RGBA;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB:
        *out_internal = GL_SRGB8_ALPHA8;
        *out_external = GL_RGBA;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_B8G8R8A8_UNORM:
        *out_internal = GL_RGBA8;
        *out_external = GL_BGRA;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB:
        *out_internal = GL_SRGB8_ALPHA8;
        *out_external = GL_BGRA;
        *out_type = GL_UNSIGNED_BYTE;
        return true;
    case PIXEL_FORMAT_R16G16B16A16_FLOAT:
        *out_internal = GL_RGBA16F;
        *out_external = GL_RGBA;
        *out_type = GL_FLOAT;
        return true;
    case PIXEL_FORMAT_R32_FLOAT:
        *out_internal = GL_R32F;
        *out_external = GL_RED;
        *out_type = GL_FLOAT;
        return true;
    case PIXEL_FORMAT_D32_FLOAT:
        *out_internal = GL_DEPTH_COMPONENT32F;
        *out_external = GL_DEPTH_COMPONENT;
        *out_type = GL_FLOAT;
        return true;
    case PIXEL_FORMAT_D24_UNORM_S8_UINT:
        *out_internal = GL_DEPTH24_STENCIL8;
        *out_external = GL_DEPTH_STENCIL;
        *out_type = GL_UNSIGNED_INT_24_8;
        return true;
    default:
        return false;
    }
}

static bool gl_translate_vertex_format(vertex_format_t format, GLint *out_components,
                                       GLenum *out_type, bool *out_normalized)
{
    *out_normalized = false;

    switch (format) {
    case VERTEX_FORMAT_FLOAT1:
        *out_components = 1;
        *out_type = GL_FLOAT;
        return true;
    case VERTEX_FORMAT_FLOAT2:
        *out_components = 2;
        *out_type = GL_FLOAT;
        return true;
    case VERTEX_FORMAT_FLOAT3:
        *out_components = 3;
        *out_type = GL_FLOAT;
        return true;
    case VERTEX_FORMAT_FLOAT4:
        *out_components = 4;
        *out_type = GL_FLOAT;
        return true;
    case VERTEX_FORMAT_UINT1:
        *out_components = 1;
        *out_type = GL_UNSIGNED_INT;
        return true;
    case VERTEX_FORMAT_UINT2:
        *out_components = 2;
        *out_type = GL_UNSIGNED_INT;
        return true;
    case VERTEX_FORMAT_UINT3:
        *out_components = 3;
        *out_type = GL_UNSIGNED_INT;
        return true;
    case VERTEX_FORMAT_UINT4:
        *out_components = 4;
        *out_type = GL_UNSIGNED_INT;
        return true;
    case VERTEX_FORMAT_UNORM8X4:
        *out_components = 4;
        *out_type = GL_UNSIGNED_BYTE;
        *out_normalized = true;
        return true;
    case VERTEX_FORMAT_SNORM8X4:
        *out_components = 4;
        *out_type = GL_BYTE;
        *out_normalized = true;
        return true;
    case VERTEX_FORMAT_UNORM16X2:
        *out_components = 2;
        *out_type = GL_UNSIGNED_SHORT;
        *out_normalized = true;
        return true;
    case VERTEX_FORMAT_UNORM16X4:
        *out_components = 4;
        *out_type = GL_UNSIGNED_SHORT;
        *out_normalized = true;
        return true;
    default:
        return false;
    }
}

static GLenum gl_translate_blend_factor(blend_factor_t factor)
{
    switch (factor) {
    case BLEND_FACTOR_ZERO:
        return GL_ZERO;
    case BLEND_FACTOR_ONE:
        return GL_ONE;
    case BLEND_FACTOR_SRC_COLOR:
        return GL_SRC_COLOR;
    case BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
        return GL_ONE_MINUS_SRC_COLOR;
    case BLEND_FACTOR_DST_COLOR:
        return GL_DST_COLOR;
    case BLEND_FACTOR_ONE_MINUS_DST_COLOR:
        return GL_ONE_MINUS_DST_COLOR;
    case BLEND_FACTOR_SRC_ALPHA:
        return GL_SRC_ALPHA;
    case BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BLEND_FACTOR_DST_ALPHA:
        return GL_DST_ALPHA;
    case BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
        return GL_ONE_MINUS_DST_ALPHA;
    case BLEND_FACTOR_SRC_ALPHA_SATURATE:
        return GL_SRC_ALPHA_SATURATE;
    default:
        return GL_ONE;
    }
}

static GLenum gl_translate_blend_op(blend_op_t op)
{
    switch (op) {
    case BLEND_OP_ADD:
        return GL_FUNC_ADD;
    case BLEND_OP_SUBTRACT:
        return GL_FUNC_SUBTRACT;
    case BLEND_OP_REVERSE_SUBTRACT:
        return GL_FUNC_REVERSE_SUBTRACT;
    case BLEND_OP_MIN:
        return GL_MIN;
    case BLEND_OP_MAX:
        return GL_MAX;
    default:
        return GL_FUNC_ADD;
    }
}

static GLenum gl_translate_compare_op(compare_op_t op)
{
    switch (op) {
    case COMPARE_OP_NEVER:
        return GL_NEVER;
    case COMPARE_OP_LESS:
        return GL_LESS;
    case COMPARE_OP_EQUAL:
        return GL_EQUAL;
    case COMPARE_OP_LESS_EQUAL:
        return GL_LEQUAL;
    case COMPARE_OP_GREATER:
        return GL_GREATER;
    case COMPARE_OP_NOT_EQUAL:
        return GL_NOTEQUAL;
    case COMPARE_OP_GREATER_EQUAL:
        return GL_GEQUAL;
    case COMPARE_OP_ALWAYS:
        return GL_ALWAYS;
    default:
        return GL_LESS;
    }
}

static GLenum gl_translate_topology(primitive_topology_t topology)
{
    switch (topology) {
    case PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        return GL_TRIANGLES;
    case PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        return GL_TRIANGLE_STRIP;
    case PRIMITIVE_TOPOLOGY_LINE_LIST:
        return GL_LINES;
    case PRIMITIVE_TOPOLOGY_LINE_STRIP:
        return GL_LINE_STRIP;
    case PRIMITIVE_TOPOLOGY_POINT_LIST:
        return GL_POINTS;
    default:
        return GL_TRIANGLES;
    }
}

static GLenum gl_translate_sampler_filter(sampler_filter_t filter)
{
    return (filter == SAMPLER_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
}

static GLenum gl_translate_sampler_wrap(sampler_wrap_t wrap)
{
    switch (wrap) {
    case SAMPLER_WRAP_MIRRORED_REPEAT:
        return GL_MIRRORED_REPEAT;
    case SAMPLER_WRAP_CLAMP_TO_EDGE:
        return GL_CLAMP_TO_EDGE;
    case SAMPLER_WRAP_CLAMP_TO_BORDER:
        return GL_CLAMP_TO_BORDER;
    default:
        return GL_REPEAT;
    }
}

static GLenum gl_translate_sampler_mipmap(sampler_filter_t min_filter,
                                          sampler_mipmap_filter_t mipmap)
{
    if (mipmap == SAMPLER_MIPMAP_NONE) {
        return gl_translate_sampler_filter(min_filter);
    }
    if (mipmap == SAMPLER_MIPMAP_NEAREST) {
        return (min_filter == SAMPLER_FILTER_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST
                                                      : GL_LINEAR_MIPMAP_NEAREST;
    }
    return (min_filter == SAMPLER_FILTER_NEAREST) ? GL_NEAREST_MIPMAP_LINEAR
                                                  : GL_LINEAR_MIPMAP_LINEAR;
}

// =============================================================================
//  状态下发
// =============================================================================
static void gl_apply_full_state(gl_backend_t *backend)
{
    const gl_api_t *gl = gl_api_load();
    gl_applied_state_t *state = &backend->state;

    // 全量重设：把缓存标记为"未知"，让后续 diff 一定下发。
    // 用于 render pass 开始时（此时 GL 状态可能被外部代码改过）。
    memset(state, 0, sizeof(*state));
    state->active_texture_unit = 0;
    state->valid = false;

    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_POLYGON_OFFSET_FILL);
    gl->glDepthMask(GL_FALSE);
    gl->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gl->glUseProgram(0);

    for (u32 i = 0; i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glBindSampler(i, 0);
    }
    gl->glActiveTexture(GL_TEXTURE0);
}

static void gl_apply_pipeline_state(gl_backend_t *backend, const gl_pipeline_resource_t *pipeline)
{
    const gl_api_t *gl = gl_api_load();
    gl_applied_state_t *state = &backend->state;

    if (state->program != pipeline->program) {
        gl->glUseProgram(pipeline->program);
        state->program = pipeline->program;
    }

    if (state->blend_enabled != pipeline->blend_enabled) {
        if (pipeline->blend_enabled) {
            gl->glEnable(GL_BLEND);
        } else {
            gl->glDisable(GL_BLEND);
        }
        state->blend_enabled = pipeline->blend_enabled;
    }

    if (pipeline->blend_enabled) {
        if (state->blend_src_color != pipeline->blend_src_color ||
            state->blend_dst_color != pipeline->blend_dst_color ||
            state->blend_src_alpha != pipeline->blend_src_alpha ||
            state->blend_dst_alpha != pipeline->blend_dst_alpha) {
            // 用 glBlendFuncSeparate 而不是 glBlendFunc：
            // 颜色与 alpha 通道的因子本来就该能分开设（Vulkan 也是分开的），
            // 用 glBlendFunc 会把两者绑死，透明叠加时 alpha 通道会算错。
            gl->glBlendFuncSeparate(pipeline->blend_src_color, pipeline->blend_dst_color,
                                    pipeline->blend_src_alpha, pipeline->blend_dst_alpha);
            state->blend_src_color = pipeline->blend_src_color;
            state->blend_dst_color = pipeline->blend_dst_color;
            state->blend_src_alpha = pipeline->blend_src_alpha;
            state->blend_dst_alpha = pipeline->blend_dst_alpha;
        }
        if (state->blend_eq_color != pipeline->blend_eq_color ||
            state->blend_eq_alpha != pipeline->blend_eq_alpha) {
            // GL 没有 glBlendEquationSeparate 之外的更细粒度接口，
            // 这里两个通道用同一个方程，所以取 color 的那个。
            gl->glBlendEquationSeparate(pipeline->blend_eq_color, pipeline->blend_eq_alpha);
            state->blend_eq_color = pipeline->blend_eq_color;
            state->blend_eq_alpha = pipeline->blend_eq_alpha;
        }
    }

    // 写掩码每次都比一次：它很便宜，但漏掉的后果（某些物体不写颜色）很怪
    gl->glColorMask((pipeline->write_mask & BLEND_WRITE_R) ? GL_TRUE : GL_FALSE,
                    (pipeline->write_mask & BLEND_WRITE_G) ? GL_TRUE : GL_FALSE,
                    (pipeline->write_mask & BLEND_WRITE_B) ? GL_TRUE : GL_FALSE,
                    (pipeline->write_mask & BLEND_WRITE_A) ? GL_TRUE : GL_FALSE);

    if (state->depth_test != pipeline->depth_test) {
        if (pipeline->depth_test) {
            gl->glEnable(GL_DEPTH_TEST);
        } else {
            gl->glDisable(GL_DEPTH_TEST);
        }
        state->depth_test = pipeline->depth_test;
    }
    if (pipeline->depth_test) {
        if (state->depth_func != pipeline->depth_func) {
            gl->glDepthFunc(pipeline->depth_func);
            state->depth_func = pipeline->depth_func;
        }
        if (state->depth_write != pipeline->depth_write) {
            gl->glDepthMask(pipeline->depth_write ? GL_TRUE : GL_FALSE);
            state->depth_write = pipeline->depth_write;
        }
    }

    if (state->cull_enabled != pipeline->cull_enabled) {
        if (pipeline->cull_enabled) {
            gl->glEnable(GL_CULL_FACE);
        } else {
            gl->glDisable(GL_CULL_FACE);
        }
        state->cull_enabled = pipeline->cull_enabled;
    }
    if (pipeline->cull_enabled) {
        if (state->cull_face != pipeline->cull_face) {
            gl->glCullFace(pipeline->cull_face);
            state->cull_face = pipeline->cull_face;
        }
    }
    if (state->front_face != pipeline->front_face) {
        gl->glFrontFace(pipeline->front_face);
        state->front_face = pipeline->front_face;
    }

    if (state->polygon_mode != pipeline->polygon_mode) {
        gl->glPolygonMode(GL_FRONT_AND_BACK, pipeline->polygon_mode);
        state->polygon_mode = pipeline->polygon_mode;
    }

    if (state->depth_bias_enabled != pipeline->depth_bias_enabled) {
        if (pipeline->depth_bias_enabled) {
            gl->glEnable(GL_POLYGON_OFFSET_FILL);
        } else {
            gl->glDisable(GL_POLYGON_OFFSET_FILL);
        }
        state->depth_bias_enabled = pipeline->depth_bias_enabled;
    }
    if (pipeline->depth_bias_enabled) {
        gl->glPolygonOffset(pipeline->depth_bias_slope, pipeline->depth_bias_constant);
    }

    if (state->multisample != pipeline->multisample) {
        if (pipeline->multisample) {
            gl->glEnable(GL_MULTISAMPLE);
        } else {
            gl->glDisable(GL_MULTISAMPLE);
        }
        state->multisample = pipeline->multisample;
    }
}

// =============================================================================
//  采样器对象
// =============================================================================
static bool sampler_desc_equal(const sampler_desc_t *a, const sampler_desc_t *b)
{
    return a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
           a->mipmap_filter == b->mipmap_filter && a->wrap_u == b->wrap_u &&
           a->wrap_v == b->wrap_v && a->wrap_w == b->wrap_w &&
           a->max_anisotropy == b->max_anisotropy;
}

static GLuint gl_get_sampler(gl_backend_t *backend, const sampler_desc_t *desc)
{
    const gl_api_t *gl = gl_api_load();

    if (desc == NULL) {
        return 0; // 0 = 使用纹理对象自带的采样参数
    }

    for (u32 i = 0; i < GL_SAMPLER_CACHE_SIZE; ++i) {
        if (backend->sampler_cache[i].used &&
            sampler_desc_equal(&backend->sampler_cache[i].desc, desc)) {
            return backend->sampler_cache[i].id;
        }
    }

    // 缓存满了就覆盖最老的一个（轮转）。
    // 16 个槽位对本引擎的材质数量绰绰有余，真不够时的代价只是多一次创建。
    const u32 slot = backend->sampler_cache_next;
    backend->sampler_cache_next = (backend->sampler_cache_next + 1u) % GL_SAMPLER_CACHE_SIZE;

    if (backend->sampler_cache[slot].used) {
        gl->glDeleteSamplers(1, &backend->sampler_cache[slot].id);
    }

    GLuint sampler = 0;
    gl->glGenSamplers(1, &sampler);
    if (sampler == 0) {
        return 0;
    }

    gl->glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                            (GLint)gl_translate_sampler_mipmap(desc->min_filter,
                                                               desc->mipmap_filter));
    gl->glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                            (GLint)gl_translate_sampler_filter(desc->mag_filter));
    gl->glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, (GLint)gl_translate_sampler_wrap(desc->wrap_u));
    gl->glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, (GLint)gl_translate_sampler_wrap(desc->wrap_v));
    gl->glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, (GLint)gl_translate_sampler_wrap(desc->wrap_w));

    if (desc->max_anisotropy > 1.0f) {
        // 各向异性过滤是扩展功能（GL_EXT_texture_filter_anisotropic）。
        // 驱动不支持时这个枚举会被静默忽略，不会报错，所以不必特判。
        gl->glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, desc->max_anisotropy);
    }

    GL_CHECK();

    backend->sampler_cache[slot].desc = *desc;
    backend->sampler_cache[slot].id = sampler;
    backend->sampler_cache[slot].used = true;
    return sampler;
}

// =============================================================================
//  着色器编译
// =============================================================================
#define GL_SHADER_LOG_CAPACITY 4096
#define GL_SHADER_SOURCE_CAPACITY 16384

static void gl_terminate_log(char *log, GLsizei length, usize capacity)
{
    const usize end = (length > 0 && (usize)length < capacity) ? (usize)length : (capacity - 1u);
    log[end] = '\0';
}

// 把 "#version ..." + uniform 块声明 + 用户源码 拼成最终源码。
//
// 版本号由上下文实际版本决定，而不是写死 450：驱动降级到 4.3 时
// 写死 450 会直接编译失败，而降级本来就是正常情况。
static bool gl_build_shader_source(gl_backend_t *backend, const char *user_source,
                                   char *out_buffer, usize capacity)
{
    const u32 major = gl_context_major_version(backend->context);
    const u32 minor = gl_context_minor_version(backend->context);

    const int written = snprintf(out_buffer, capacity, "#version %u%u0 core\n%s\n%s", major, minor,
                                 RENDER_UNIFORM_BLOCK_GLSL, user_source);

    // snprintf 的返回值是"本该写入的长度"。超出容量说明被截断了 ——
    // 而截断后的着色器报的是 "syntax error, unexpected $end"，
    // 完全指不到真正的原因。所以这里显式检查。
    if (written < 0 || (usize)written >= capacity) {
        LOG_ERROR("着色器源码超出缓冲区（需要 %d 字节，只有 %llu）", written,
                  (unsigned long long)capacity);
        return false;
    }
    return true;
}

static GLuint gl_compile_stage(const gl_api_t *gl, GLenum stage, const char *source,
                               const char *label)
{
    const GLuint shader = gl->glCreateShader(stage);
    if (shader == 0) {
        LOG_ERROR("glCreateShader 失败 (%s)", label);
        return 0;
    }

    gl->glShaderSource(shader, 1, &source, NULL);
    gl->glCompileShader(shader);

    GLint status = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        char    log[GL_SHADER_LOG_CAPACITY];
        GLsizei length = 0;
        gl->glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
        gl_terminate_log(log, length, sizeof(log));
        LOG_ERROR("%s 编译失败:\n%s", label, log);
        gl->glDeleteShader(shader);
        return 0;
    }

    return shader;
}

// =============================================================================
//  生命周期
// =============================================================================
static bool gl_backend_init(render_backend_t *self, const render_backend_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    if (desc == NULL || desc->window == NULL) {
        LOG_ERROR("GL 后端需要窗口");
        return false;
    }

    backend->window = desc->window;

    gl_context_desc_t context_desc = gl_context_desc_default();
    context_desc.vsync = desc->vsync;
    context_desc.msaa_samples = (desc->msaa_samples == 0) ? 1u : desc->msaa_samples;
    context_desc.debug_context = desc->enable_debug_output;

    // 先创建上下文。函数表**必须**在上下文 make current 之后才能加载：
    // wglGetProcAddress 在没有 current 上下文时一律返回 NULL。
    if (!gl_context_create(backend->alloc, desc->window, &context_desc, &backend->context)) {
        return false;
    }
    gl_context_make_current(backend->context);

    const gl_api_t *gl = gl_api_load();
    if (gl == NULL) {
        LOG_ERROR("OpenGL 函数表加载失败，GL 后端无法初始化");
        return false;
    }

    // 资源池
    if (!pool_init(&backend->shaders, backend->alloc, sizeof(gl_shader_resource_t),
                   RENDER_MAX_SHADERS, "gl_shaders") ||
        !pool_init(&backend->textures, backend->alloc, sizeof(gl_texture_resource_t),
                   RENDER_MAX_TEXTURES, "gl_textures") ||
        !pool_init(&backend->meshes, backend->alloc, sizeof(gl_mesh_resource_t),
                   RENDER_MAX_MESHES, "gl_meshes") ||
        !pool_init(&backend->pipelines, backend->alloc, sizeof(gl_pipeline_resource_t),
                   RENDER_MAX_PIPELINES, "gl_pipelines") ||
        !pool_init(&backend->render_targets, backend->alloc,
                   sizeof(gl_render_target_resource_t), RENDER_MAX_RENDER_TARGETS,
                   "gl_render_targets")) {
        LOG_ERROR("GL 后端资源池创建失败");
        return false;
    }

    // 能力查询
    memset(&backend->caps, 0, sizeof(backend->caps));
    snprintf(backend->caps.api_name, sizeof(backend->caps.api_name), "OpenGL %u.%u",
             gl_context_major_version(backend->context),
             gl_context_minor_version(backend->context));
    snprintf(backend->caps.device_name, sizeof(backend->caps.device_name), "%s",
             gl_context_renderer_string(backend->context));
    snprintf(backend->caps.vendor_name, sizeof(backend->caps.vendor_name), "%s",
             gl_context_vendor_string(backend->context));

    GLint value = 0;
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    backend->caps.max_texture_size = (u32)value;
    gl->glGetIntegerv(GL_MAX_SAMPLES, &value);
    backend->caps.max_samples = (u32)value;
    backend->caps.max_texture_units = RENDER_MAX_TEXTURE_SLOTS;
    backend->caps.max_uniform_buffer_size = 65536;
    backend->caps.max_vertex_attributes = RENDER_MAX_VERTEX_ATTRIBUTES;
    backend->caps.supports_instancing = true;
    backend->caps.supports_wireframe = true;
    backend->caps.supports_debug_output = gl_context_has_debug_output(backend->context);

    // uniform buffer：每个绑定点一个。
    // 用真正的 UBO 而不是 glUniform* —— 强制 GL 走和 Vulkan 一样的路径。
    for (u32 i = 0; i < RENDER_UNIFORM_BINDING_COUNT; ++i) {
        backend->uniform_buffers[i] = 0;
        backend->uniform_buffer_capacities[i] = 0;
    }

    const u32 uniform_sizes[RENDER_UNIFORM_BINDING_COUNT] = {
        (u32)sizeof(frame_uniforms_t),
        (u32)sizeof(object_uniforms_t),
    };

    for (u32 i = 0; i < RENDER_UNIFORM_BINDING_COUNT; ++i) {
        gl->glGenBuffers(1, &backend->uniform_buffers[i]);
        gl->glBindBuffer(GL_UNIFORM_BUFFER, backend->uniform_buffers[i]);
        gl->glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)uniform_sizes[i], NULL, GL_DYNAMIC_DRAW);
        gl->glBindBufferBase(GL_UNIFORM_BUFFER, i, backend->uniform_buffers[i]);
        backend->uniform_buffer_capacities[i] = uniform_sizes[i];

        backend->stats.buffer_uploads++;
        backend->stats.buffer_upload_bytes += uniform_sizes[i];
    }
    gl->glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // 默认纹理：1x1 白色与 1x1 平面法线。
    // 没有它们的话，"忘了绑纹理"会采样到未定义内容，画面出现随机颜色，
    // 而且 GL 不会报任何错。有了默认纹理，忘记绑定只会得到纯白。
    {
        const u8 white[4] = {255, 255, 255, 255};
        gl->glGenTextures(1, &backend->default_white);
        gl->glBindTexture(GL_TEXTURE_2D, backend->default_white);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        const u8 flat_normal[4] = {128, 128, 255, 255};
        gl->glGenTextures(1, &backend->default_flat_normal);
        gl->glBindTexture(GL_TEXTURE_2D, backend->default_flat_normal);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         flat_normal);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        gl->glBindTexture(GL_TEXTURE_2D, 0);
    }

    gl_apply_full_state(backend);

    LOG_INFO("GL 后端就绪: %s | %s", backend->caps.api_name, backend->caps.device_name);
    LOG_INFO("  最大纹理尺寸 %u，最大 MSAA %u，调试输出 %s", backend->caps.max_texture_size,
             backend->caps.max_samples, backend->caps.supports_debug_output ? "可用" : "不可用");

    GL_CHECK();
    return true;
}

static void gl_backend_shutdown(render_backend_t *self)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    if (backend->context == NULL) {
        return;
    }

    gl_context_make_current(backend->context);
    const gl_api_t *gl = gl_api_load();

    // 销毁所有资源。用迭代器而不是记录句柄 ——
    // 这样即使调用方漏了 destroy，也不会把 GPU 对象泄漏到进程退出。
    {
        pool_iter_t it;
        handle_t handle = HANDLE_INVALID;
        void *ptr = NULL;

        pool_iter_begin(&it, &backend->shaders);
        while (pool_iter_next(&it, &handle, &ptr)) {
            gl_shader_resource_t *res = (gl_shader_resource_t *)ptr;
            if (res->program != 0) {
                gl->glDeleteProgram(res->program);
            }
        }

        pool_iter_begin(&it, &backend->textures);
        while (pool_iter_next(&it, &handle, &ptr)) {
            gl_texture_resource_t *res = (gl_texture_resource_t *)ptr;
            if (res->id != 0) {
                gl->glDeleteTextures(1, &res->id);
            }
        }

        pool_iter_begin(&it, &backend->meshes);
        while (pool_iter_next(&it, &handle, &ptr)) {
            gl_mesh_resource_t *res = (gl_mesh_resource_t *)ptr;
            if (res->vao != 0) {
                gl->glDeleteVertexArrays(1, &res->vao);
            }
            for (u32 i = 0; i < res->binding_count; ++i) {
                if (res->vbo[i] != 0) {
                    gl->glDeleteBuffers(1, &res->vbo[i]);
                }
            }
            if (res->ibo != 0) {
                gl->glDeleteBuffers(1, &res->ibo);
            }
        }
    }

    for (u32 i = 0; i < GL_SAMPLER_CACHE_SIZE; ++i) {
        if (backend->sampler_cache[i].used) {
            gl->glDeleteSamplers(1, &backend->sampler_cache[i].id);
            backend->sampler_cache[i].used = false;
        }
    }

    for (u32 i = 0; i < RENDER_UNIFORM_BINDING_COUNT; ++i) {
        if (backend->uniform_buffers[i] != 0) {
            gl->glDeleteBuffers(1, &backend->uniform_buffers[i]);
            backend->uniform_buffers[i] = 0;
        }
    }

    if (backend->default_white != 0) {
        gl->glDeleteTextures(1, &backend->default_white);
        backend->default_white = 0;
    }
    if (backend->default_flat_normal != 0) {
        gl->glDeleteTextures(1, &backend->default_flat_normal);
        backend->default_flat_normal = 0;
    }

    // 渲染目标要在纹理池之前销毁：它的附件纹理包装在纹理池里，
    // 顺序反了会先清掉纹理池，再去找句柄就找不到了。
    {
        pool_iter_t it;
        handle_t    handle = HANDLE_INVALID;
        void       *ptr = NULL;
        pool_iter_begin(&it, &backend->render_targets);
        while (pool_iter_next(&it, &handle, &ptr)) {
            gl_render_target_resource_t *rt = (gl_render_target_resource_t *)ptr;
            gl_render_target_release_attachments(backend, rt);
        }
    }

    pool_destroy(&backend->render_targets);
    pool_destroy(&backend->pipelines);
    pool_destroy(&backend->meshes);
    pool_destroy(&backend->textures);
    pool_destroy(&backend->shaders);

    gl_context_destroy(backend->context);
    backend->context = NULL;

    LOG_DEBUG("GL 后端已关闭");
}

static void gl_backend_get_caps(const render_backend_t *self, render_backend_caps_t *out_caps)
{
    const gl_backend_t *backend = (const gl_backend_t *)self;
    if (out_caps != NULL) {
        *out_caps = backend->caps;
    }
}

// =============================================================================
//  帧
// =============================================================================
static void gl_backend_begin_frame(render_backend_t *self, const frame_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (desc != NULL) {
        backend->frame = *desc;
    }

    render_stats_reset(&backend->stats);
    backend->current_pipeline = PIPELINE_HANDLE_INVALID;
    backend->render_pass_depth = 0;

    // 每帧开头把状态缓存作废：外部代码（或调试工具）可能改过 GL 状态。
    // 作废的代价只是下一帧第一次 bind_pipeline 多几次调用，
    // 比"状态缓存与真实状态不一致"这种玄学 bug 便宜得多。
    backend->state.valid = false;

    gl->glViewport(0, 0, (GLsizei)backend->frame.target_width,
                   (GLsizei)backend->frame.target_height);
}

static void gl_backend_end_frame(render_backend_t *self)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_context_swap_buffers(backend->context);
}

// =============================================================================
//  Render pass
// =============================================================================
static void gl_backend_begin_render_pass(render_backend_t *self, const render_pass_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    backend->render_pass_depth++;

    // 绑定渲染目标。Vulkan 的 vkCmdBeginRenderPass 也要指定 framebuffer，
    // 语义完全一致 —— 这正是 M2 保留 render pass 概念的价值。
    if (desc != NULL) {
        gl_backend_bind_render_target(self, desc->target);
    }

    // GL 没有显式 render pass，但这里做的事情与 Vulkan 的
    // vkCmdBeginRenderPass 语义一致：设置视口/裁剪、执行清屏。
    gl_apply_full_state(backend);

    GLbitfield clear_mask = 0;

    if (desc != NULL) {
        // 视口尺寸为 0 时用目标尺寸兜底：调用方常常只关心"清个屏"，
        // 不想每次都填视口。glViewport(0,0,0,0) 是合法的但会让所有绘制被丢弃。
        u32 viewport_w = (u32)desc->viewport.width;
        u32 viewport_h = (u32)desc->viewport.height;
        if (viewport_w == 0 || viewport_h == 0) {
            if (backend->current_target != RENDER_TARGET_HANDLE_INVALID) {
                gl_render_target_resource_t *rt =
                    POOL_GET_AS(&backend->render_targets, backend->current_target,
                                gl_render_target_resource_t);
                if (rt != NULL) {
                    viewport_w = rt->width;
                    viewport_h = rt->height;
                }
            } else {
                viewport_w = backend->frame.target_width;
                viewport_h = backend->frame.target_height;
            }
        }
        gl->glViewport((GLint)desc->viewport.x, (GLint)desc->viewport.y, (GLsizei)viewport_w,
                       (GLsizei)viewport_h);

        if (desc->has_scissor) {
            gl->glEnable(GL_SCISSOR_TEST);
            gl->glScissor(desc->scissor.x, desc->scissor.y, (GLsizei)desc->scissor.width,
                          (GLsizei)desc->scissor.height);
            backend->state.scissor_enabled = true;
        }

        if (desc->clear_color) {
            // 先解除颜色写掩码再清屏：如果上一次绘制把掩码设成了"只写红通道"，
            // 直接清屏会得到一个只被清了一半的帧缓冲 —— 这是很隐蔽的坑。
            gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            gl->glClearColor(desc->clear_color_value[0], desc->clear_color_value[1],
                             desc->clear_color_value[2], desc->clear_color_value[3]);
            clear_mask |= GL_COLOR_BUFFER_BIT;
        }

        if (desc->clear_depth) {
            // 同理：深度写被关掉时 glClear 不会清深度缓冲。
            gl->glDepthMask(GL_TRUE);
            gl->glClearDepth((GLdouble)desc->clear_depth_value);
            clear_mask |= GL_DEPTH_BUFFER_BIT;
        }

        if (desc->clear_stencil) {
            gl->glClearStencil((GLint)desc->clear_stencil_value);
            clear_mask |= GL_STENCIL_BUFFER_BIT;
        }
    }

    if (clear_mask != 0) {
        gl->glClear(clear_mask);
    }
}

static void gl_backend_end_render_pass(render_backend_t *self)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    if (backend->render_pass_depth > 0) {
        backend->render_pass_depth--;
    }
}

static void gl_backend_set_viewport(render_backend_t *self, const viewport_t *viewport)
{
    const gl_api_t *gl = gl_api_load();
    ENGINE_UNUSED(self);

    if (viewport != NULL) {
        gl->glViewport((GLint)viewport->x, (GLint)viewport->y, (GLsizei)viewport->width,
                       (GLsizei)viewport->height);
    }
}

static void gl_backend_set_scissor(render_backend_t *self, const scissor_rect_t *scissor)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (scissor == NULL) {
        gl->glDisable(GL_SCISSOR_TEST);
        backend->state.scissor_enabled = false;
        return;
    }

    gl->glEnable(GL_SCISSOR_TEST);
    gl->glScissor(scissor->x, scissor->y, (GLsizei)scissor->width, (GLsizei)scissor->height);
    backend->state.scissor_enabled = true;
}

// =============================================================================
//  着色器
// =============================================================================
static shader_handle_t gl_backend_create_shader(render_backend_t *self, const shader_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (desc == NULL || desc->vertex_source == NULL || desc->fragment_source == NULL) {
        LOG_ERROR("着色器描述不完整");
        return SHADER_HANDLE_INVALID;
    }

    const char *label = (desc->debug_name != NULL) ? desc->debug_name : "<unnamed>";

    char *vertex_source = (char *)backend->alloc->alloc(backend->alloc, GL_SHADER_SOURCE_CAPACITY,
                                                        16);
    char *fragment_source =
        (char *)backend->alloc->alloc(backend->alloc, GL_SHADER_SOURCE_CAPACITY, 16);
    if (vertex_source == NULL || fragment_source == NULL) {
        LOG_ERROR("着色器源码缓冲区分配失败 (%s)", label);
        if (vertex_source != NULL) {
            backend->alloc->free(backend->alloc, vertex_source, GL_SHADER_SOURCE_CAPACITY);
        }
        if (fragment_source != NULL) {
            backend->alloc->free(backend->alloc, fragment_source, GL_SHADER_SOURCE_CAPACITY);
        }
        return SHADER_HANDLE_INVALID;
    }

    GLuint program = 0;
    GLuint vertex_stage = 0;
    GLuint fragment_stage = 0;

    if (!gl_build_shader_source(backend, desc->vertex_source, vertex_source,
                                GL_SHADER_SOURCE_CAPACITY) ||
        !gl_build_shader_source(backend, desc->fragment_source, fragment_source,
                                GL_SHADER_SOURCE_CAPACITY)) {
        goto cleanup;
    }

    vertex_stage = gl_compile_stage(gl, GL_VERTEX_SHADER, vertex_source, "顶点着色器");
    if (vertex_stage == 0) {
        goto cleanup;
    }
    fragment_stage = gl_compile_stage(gl, GL_FRAGMENT_SHADER, fragment_source, "片元着色器");
    if (fragment_stage == 0) {
        goto cleanup;
    }

    program = gl->glCreateProgram();
    if (program == 0) {
        LOG_ERROR("glCreateProgram 失败 (%s)", label);
        goto cleanup;
    }

    gl->glAttachShader(program, vertex_stage);
    gl->glAttachShader(program, fragment_stage);
    gl->glLinkProgram(program);

    {
        GLint status = 0;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status == GL_FALSE) {
            char    log[GL_SHADER_LOG_CAPACITY];
            GLsizei length = 0;
            gl->glGetProgramInfoLog(program, (GLsizei)sizeof(log), &length, log);
            gl_terminate_log(log, length, sizeof(log));
            LOG_ERROR("着色器链接失败 (%s):\n%s", label, log);
            gl->glDeleteProgram(program);
            program = 0;
            goto cleanup;
        }
    }

    // 链接完成后 shader 对象就可以删了，program 已经持有它的副本。
    gl->glDeleteShader(vertex_stage);
    gl->glDeleteShader(fragment_stage);
    vertex_stage = 0;
    fragment_stage = 0;

    // ---- std140 布局校验 ----
    //
    // 问驱动"这个 uniform 块在 GPU 那边占多少字节"，和 C 结构体比。
    //
    // 为什么值得每次建着色器都查一遍：布局不一致**不会报错**。
    // 后果是 C 侧写进去的字节和 GLSL 读的偏移错位 ——
    // 症状是"某些 uniform 莫名其妙是 0 或乱值"。
    // 典型表现：光源数组整体偏移，画面变成一片黑，而日志里干干净净。
    //
    // 这条检查把一个"要靠猜的渲染问题"变成一行明确的错误日志。
    {
        struct {
            const char *name;
            usize       expected;
        } blocks[] = {
            {"FrameUniforms", sizeof(frame_uniforms_t)},
            {"ObjectUniforms", sizeof(object_uniforms_t)},
        };

        for (usize i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i) {
            const GLuint index = gl->glGetUniformBlockIndex(program, blocks[i].name);
            if (index == GL_INVALID_INDEX) {
                continue; // 这个着色器没用这个块，正常
            }
            GLint gpu_size = 0;
            gl->glGetActiveUniformBlockiv(program, index, GL_UNIFORM_BLOCK_DATA_SIZE, &gpu_size);
            if ((usize)gpu_size != blocks[i].expected) {
                LOG_ERROR("uniform 块 %s 的 std140 布局不一致 (%s): GPU %d 字节，C 侧 %llu 字节。"
                          "两边会错位，uniform 会读到乱值。",
                          blocks[i].name, label, (int)gpu_size,
                          (unsigned long long)blocks[i].expected);
            }
        }
    }

cleanup:
    backend->alloc->free(backend->alloc, vertex_source, GL_SHADER_SOURCE_CAPACITY);
    backend->alloc->free(backend->alloc, fragment_source, GL_SHADER_SOURCE_CAPACITY);

    if (vertex_stage != 0) {
        gl->glDeleteShader(vertex_stage);
    }
    if (fragment_stage != 0) {
        gl->glDeleteShader(fragment_stage);
    }
    if (program == 0) {
        return SHADER_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&backend->shaders);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("着色器池已满（上限 %u），无法创建 %s", RENDER_MAX_SHADERS, label);
        gl->glDeleteProgram(program);
        return SHADER_HANDLE_INVALID;
    }

    gl_shader_resource_t *resource = POOL_GET_AS(&backend->shaders, handle, gl_shader_resource_t);
    resource->program = program;
    resource->name = desc->debug_name;
    resource->creation_frame = backend->frame.frame_index;

    GL_CHECK();
    return handle;
}

static void gl_backend_destroy_shader(render_backend_t *self, shader_handle_t shader)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_shader_resource_t *resource = POOL_GET_AS(&backend->shaders, shader, gl_shader_resource_t);
    if (resource == NULL) {
        return;
    }

    const gl_api_t *gl = gl_api_load();
    if (resource->program != 0) {
        gl->glDeleteProgram(resource->program);
    }
    pool_free(&backend->shaders, shader);
}

// =============================================================================
//  纹理
// =============================================================================
static texture_handle_t gl_backend_create_texture(render_backend_t *self,
                                                  const texture_desc_t *desc, const void *pixels)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (desc == NULL || desc->width == 0 || desc->height == 0) {
        LOG_ERROR("纹理描述无效");
        return TEXTURE_HANDLE_INVALID;
    }

    GLint  internal_format = 0;
    GLenum external_format = 0;
    GLenum pixel_type = 0;
    if (!gl_translate_pixel_format(desc->format, &internal_format, &external_format, &pixel_type)) {
        LOG_ERROR("不支持的像素格式 (%d)", (int)desc->format);
        return TEXTURE_HANDLE_INVALID;
    }

    const bool is_depth = pixel_format_is_depth(desc->format);
    if (is_depth && pixels != NULL) {
        LOG_WARN("深度格式纹理不接受初始像素数据，已忽略");
        pixels = NULL;
    }

    GLuint id = 0;
    gl->glGenTextures(1, &id);
    if (id == 0) {
        LOG_ERROR("glGenTextures 失败");
        return TEXTURE_HANDLE_INVALID;
    }

    // 解包对齐：默认是 4 字节。单通道/双通道纹理如果宽度不是 4 的倍数，
    // 用默认对齐会把每行数据错位，表现为画面斜着撕裂。
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl->glBindTexture(GL_TEXTURE_2D, id);

    const u32 layers = (desc->array_layers == 0) ? 1u : desc->array_layers;
    if (layers != 1) {
        LOG_WARN("M2 只支持单层 2D 纹理，array_layers=%u 已按 1 处理", layers);
    }

    gl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, (GLsizei)desc->width,
                     (GLsizei)desc->height, 0, external_format, pixel_type, pixels);

    // 采样参数先给一组合理的默认值。
    // 真正的采样方式由 bind_texture 传入的采样器对象决定（如果有的话）。
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (desc->generate_mipmaps && !is_depth) {
        gl->glGenerateMipmap(GL_TEXTURE_2D);
    }

    gl->glBindTexture(GL_TEXTURE_2D, 0);

    if (!gl_check_error(__FILE__, __LINE__)) {
        gl->glDeleteTextures(1, &id);
        return TEXTURE_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&backend->textures);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("纹理池已满（上限 %u）", RENDER_MAX_TEXTURES);
        gl->glDeleteTextures(1, &id);
        return TEXTURE_HANDLE_INVALID;
    }

    gl_texture_resource_t *resource =
        POOL_GET_AS(&backend->textures, handle, gl_texture_resource_t);
    resource->id = id;
    resource->owns_id = true;
    resource->width = desc->width;
    resource->height = desc->height;
    resource->mip_levels = desc->generate_mipmaps ? 0u : 1u;
    resource->format = desc->format;
    resource->name = desc->debug_name;
    resource->creation_frame = backend->frame.frame_index;

    return handle;
}

static void gl_backend_destroy_texture(render_backend_t *self, texture_handle_t texture)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_texture_resource_t *resource =
        POOL_GET_AS(&backend->textures, texture, gl_texture_resource_t);
    if (resource == NULL) {
        return;
    }

    const gl_api_t *gl = gl_api_load();

    // 解绑：如果这张纹理还绑在某个纹理单元上，删除后那个单元会指向
    // 一个已销毁的对象（GL 会自动解绑，但我们的状态缓存不知道），
    // 下次"以为它绑着"就会跳过绑定，导致采样到错误内容。
    for (u32 i = 0; i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
        if (backend->state.bound_texture[i] == resource->id) {
            gl->glActiveTexture(GL_TEXTURE0 + i);
            gl->glBindTexture(GL_TEXTURE_2D, 0);
            backend->state.bound_texture[i] = 0;
        }
    }
    gl->glActiveTexture(GL_TEXTURE0);
    backend->state.active_texture_unit = 0;

    // owns_id = false 的纹理是渲染目标的附件，所有权不在纹理池手里
    if (resource->id != 0 && resource->owns_id) {
        gl->glDeleteTextures(1, &resource->id);
    }
    pool_free(&backend->textures, texture);
}

static void gl_backend_update_texture(render_backend_t *self, texture_handle_t texture,
                                      const void *pixels)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_texture_resource_t *resource =
        POOL_GET_AS(&backend->textures, texture, gl_texture_resource_t);
    if (resource == NULL || pixels == NULL) {
        return;
    }

    const gl_api_t *gl = gl_api_load();

    GLint  internal_format = 0;
    GLenum external_format = 0;
    GLenum pixel_type = 0;
    if (!gl_translate_pixel_format(resource->format, &internal_format, &external_format,
                                   &pixel_type)) {
        return;
    }

    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glBindTexture(GL_TEXTURE_2D, resource->id);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)resource->width,
                        (GLsizei)resource->height, external_format, pixel_type, pixels);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    backend->stats.buffer_uploads++;
    backend->stats.buffer_upload_bytes +=
        (usize)resource->width * resource->height * pixel_format_bytes_per_pixel(resource->format);
}

// =============================================================================
//  网格
// =============================================================================
static mesh_handle_t gl_backend_create_mesh(render_backend_t *self, const mesh_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (desc == NULL) {
        LOG_ERROR("网格描述为空");
        return MESH_HANDLE_INVALID;
    }

    const char *reason = NULL;
    if (!vertex_layout_validate(&desc->layout, &reason)) {
        LOG_ERROR("顶点布局无效: %s", reason);
        return MESH_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&backend->meshes);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("网格池已满（上限 %u）", RENDER_MAX_MESHES);
        return MESH_HANDLE_INVALID;
    }

    gl_mesh_resource_t *resource = POOL_GET_AS(&backend->meshes, handle, gl_mesh_resource_t);
    memset(resource, 0, sizeof(*resource));
    resource->name = desc->debug_name;
    resource->binding_count = desc->layout.binding_count;
    resource->index_format = desc->index_format;
    resource->vertex_count = desc->vertex_count;
    resource->index_count = desc->index_count;
    resource->creation_frame = backend->frame.frame_index;

    gl->glGenVertexArrays(1, &resource->vao);
    gl->glBindVertexArray(resource->vao);

    const GLenum vertex_usage = desc->dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    const GLenum index_usage = desc->dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;

    for (u32 i = 0; i < desc->layout.binding_count; ++i) {
        gl->glGenBuffers(1, &resource->vbo[i]);
        gl->glBindBuffer(GL_ARRAY_BUFFER, resource->vbo[i]);
        gl->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)desc->bindings[i].size,
                         desc->bindings[i].data, vertex_usage);
        backend->stats.buffer_uploads++;
        backend->stats.buffer_upload_bytes += desc->bindings[i].size;
    }

    for (u32 i = 0; i < desc->layout.attribute_count; ++i) {
        const vertex_attribute_t *attr = &desc->layout.attributes[i];

        GLint  components = 0;
        GLenum type = 0;
        bool   normalized = false;
        if (!gl_translate_vertex_format(attr->format, &components, &type, &normalized)) {
            LOG_ERROR("不支持的顶点格式 (%d)", (int)attr->format);
            gl->glBindVertexArray(0);
            gl_backend_destroy_mesh(self, handle);
            return MESH_HANDLE_INVALID;
        }

        gl->glBindBuffer(GL_ARRAY_BUFFER, resource->vbo[attr->binding]);

        // 属性 location 直接取语义枚举值，与着色器里的 layout(location = N) 对应。
        // 见 shader.h 的约定。
        const GLuint location = (GLuint)attr->semantic;

        gl->glEnableVertexAttribArray(location);
        gl->glVertexAttribPointer(location, components, type,
                                  normalized ? GL_TRUE : GL_FALSE,
                                  (GLsizei)desc->layout.bindings[attr->binding].stride,
                                  (const void *)(uintptr_t)attr->offset);

        if (desc->layout.bindings[attr->binding].per_instance) {
            gl->glVertexAttribDivisor(location, 1);
        }
    }

    if (desc->index_data != NULL && desc->index_count > 0) {
        gl->glGenBuffers(1, &resource->ibo);
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, resource->ibo);
        gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)desc->index_data_size,
                         desc->index_data, index_usage);
        backend->stats.buffer_uploads++;
        backend->stats.buffer_upload_bytes += desc->index_data_size;
    }

    gl->glBindVertexArray(0);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    GL_CHECK();
    return handle;
}

static bool gl_backend_update_mesh(render_backend_t *self, mesh_handle_t mesh,
                                   const void *vertex_data, usize vertex_data_size,
                                   const void *index_data, usize index_data_size)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    gl_mesh_resource_t *resource = POOL_GET_AS(&backend->meshes, mesh, gl_mesh_resource_t);
    if (resource == NULL) {
        return false;
    }

    // 更新顶点数据。用 glBufferSubData 而不是重新 glBufferData：
    // 重新分配会让驱动丢掉已有的缓冲对象，每帧都做等于每帧都在申请显存。
    if (vertex_data != NULL && vertex_data_size > 0) {
        if (resource->binding_count == 0) {
            return false;
        }
        gl->glBindBuffer(GL_ARRAY_BUFFER, resource->vbo[0]);
        gl->glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vertex_data_size, vertex_data);
        gl->glBindBuffer(GL_ARRAY_BUFFER, 0);

        backend->stats.buffer_uploads++;
        backend->stats.buffer_upload_bytes += vertex_data_size;
    }

    // 索引数据同理。批渲染里索引是静态的（所有精灵都是同一个 quad 模式），
    // 所以这个分支实际上很少走。
    if (index_data != NULL && index_data_size > 0 && resource->ibo != 0) {
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, resource->ibo);
        gl->glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)index_data_size, index_data);
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        backend->stats.buffer_uploads++;
        backend->stats.buffer_upload_bytes += index_data_size;
    }

    return true;
}

static void gl_backend_destroy_mesh(render_backend_t *self, mesh_handle_t mesh)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_mesh_resource_t *resource = POOL_GET_AS(&backend->meshes, mesh, gl_mesh_resource_t);
    if (resource == NULL) {
        return;
    }

    const gl_api_t *gl = gl_api_load();

    for (u32 i = 0; i < resource->binding_count; ++i) {
        if (resource->vbo[i] != 0) {
            gl->glDeleteBuffers(1, &resource->vbo[i]);
            resource->vbo[i] = 0;
        }
    }
    if (resource->ibo != 0) {
        gl->glDeleteBuffers(1, &resource->ibo);
        resource->ibo = 0;
    }
    if (resource->vao != 0) {
        gl->glDeleteVertexArrays(1, &resource->vao);
        resource->vao = 0;
    }

    pool_free(&backend->meshes, mesh);
}

// =============================================================================
//  管线
// =============================================================================
static pipeline_handle_t gl_backend_create_pipeline(render_backend_t *self,
                                                    const pipeline_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    if (desc == NULL) {
        LOG_ERROR("管线描述为空");
        return PIPELINE_HANDLE_INVALID;
    }

    gl_shader_resource_t *shader =
        POOL_GET_AS(&backend->shaders, desc->shader, gl_shader_resource_t);
    if (shader == NULL) {
        LOG_ERROR("管线引用了无效的着色器句柄");
        return PIPELINE_HANDLE_INVALID;
    }

    const char *reason = NULL;
    if (!vertex_layout_validate(&desc->vertex_layout, &reason)) {
        LOG_ERROR("管线顶点布局无效: %s", reason);
        return PIPELINE_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&backend->pipelines);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("管线池已满（上限 %u）", RENDER_MAX_PIPELINES);
        return PIPELINE_HANDLE_INVALID;
    }

    gl_pipeline_resource_t *pipeline =
        POOL_GET_AS(&backend->pipelines, handle, gl_pipeline_resource_t);
    memset(pipeline, 0, sizeof(*pipeline));

    pipeline->program = shader->program;
    pipeline->name = desc->debug_name;
    pipeline->creation_frame = backend->frame.frame_index;

    // 把不可变的 pipeline_desc_t 摊平成 GL 能直接用的字段。
    // 这一步只做一次，之后 bind_pipeline 就是纯比较 + 下发。
    pipeline->blend_enabled = desc->blend.enabled;
    pipeline->blend_src_color = gl_translate_blend_factor(desc->blend.src_color);
    pipeline->blend_dst_color = gl_translate_blend_factor(desc->blend.dst_color);
    pipeline->blend_src_alpha = gl_translate_blend_factor(desc->blend.src_alpha);
    pipeline->blend_dst_alpha = gl_translate_blend_factor(desc->blend.dst_alpha);
    pipeline->blend_eq_color = gl_translate_blend_op(desc->blend.color_op);
    pipeline->blend_eq_alpha = gl_translate_blend_op(desc->blend.alpha_op);
    pipeline->write_mask = desc->blend.write_mask;

    pipeline->depth_test = desc->depth.test_enabled;
    pipeline->depth_write = desc->depth.write_enabled;
    pipeline->depth_func = gl_translate_compare_op(desc->depth.compare);

    pipeline->cull_enabled = (desc->raster.cull != CULL_MODE_NONE);
    pipeline->cull_face = (desc->raster.cull == CULL_MODE_FRONT) ? GL_FRONT : GL_BACK;
    pipeline->front_face =
        (desc->raster.front_face == FRONT_FACE_CLOCKWISE) ? GL_CW : GL_CCW;
    pipeline->polygon_mode =
        (desc->raster.fill == FILL_MODE_WIREFRAME) ? GL_LINE : GL_FILL;

    pipeline->multisample = desc->multisample.samples > 1u;
    pipeline->topology = gl_translate_topology(desc->topology);
    pipeline->depth_bias_enabled = desc->raster.depth_bias_enabled;
    pipeline->depth_bias_constant = desc->raster.depth_bias_constant;
    pipeline->depth_bias_slope = desc->raster.depth_bias_slope;

    GL_CHECK();
    return handle;
}

static void gl_backend_destroy_pipeline(render_backend_t *self, pipeline_handle_t pipeline)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    gl_pipeline_resource_t *resource =
        POOL_GET_AS(&backend->pipelines, pipeline, gl_pipeline_resource_t);
    if (resource == NULL) {
        return;
    }

    // 管线本身不持有 GL 对象（program 归 shader 所有），只需要清掉
    // "当前绑定"的记录，否则下次 bind 会以为已经绑过而跳过。
    if (backend->current_pipeline == pipeline) {
        backend->current_pipeline = PIPELINE_HANDLE_INVALID;
        backend->state.program = 0;
    }

    pool_free(&backend->pipelines, pipeline);
}

// =============================================================================
//  渲染目标（离屏 FBO）
// =============================================================================
//
// 三种形态：
//   1. 深度专用（阴影贴图）：只有一张可采样的深度纹理，没有颜色附件。
//      GL 要求显式 glDrawBuffer(GL_NONE) / glReadBuffer(GL_NONE)，
//      否则帧缓冲会因为"没有颜色附件"而判定不完整。
//   2. 普通（无 MSAA）：颜色纹理 + 深度 renderbuffer。
//   3. MSAA：颜色/深度都用多重采样 renderbuffer，另外建一个单采样的
//      resolve FBO 承载可采样的颜色纹理。
//      **MSAA 缓冲不能直接采样**，必须在切走之前 blit 到 resolve FBO。

// 把一张已存在的 GL 纹理包装成引擎纹理句柄。
// owns_id = false 表示所有权在别处（渲染目标），销毁句柄时不删 GL 对象。
static texture_handle_t gl_wrap_texture(gl_backend_t *backend, GLuint id, u32 width, u32 height,
                                        pixel_format_t format, bool owns_id, const char *name)
{
    const handle_t handle = pool_alloc(&backend->textures);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("纹理池已满，无法包装渲染目标附件");
        return TEXTURE_HANDLE_INVALID;
    }

    gl_texture_resource_t *resource =
        POOL_GET_AS(&backend->textures, handle, gl_texture_resource_t);
    memset(resource, 0, sizeof(*resource));
    resource->id = id;
    resource->width = width;
    resource->height = height;
    resource->mip_levels = 1;
    resource->format = format;
    resource->owns_id = owns_id;
    resource->name = name;
    resource->creation_frame = backend->frame.frame_index;
    return handle;
}

static void gl_render_target_release_attachments(gl_backend_t *backend,
                                                 gl_render_target_resource_t *rt)
{
    const gl_api_t *gl = gl_api_load();

    if (rt->color_handle != TEXTURE_HANDLE_INVALID) {
        // 句柄指向的 GL 纹理归渲染目标所有，先清掉 owns_id 再释放句柄，
        // 避免 destroy_texture 把附件纹理删了。
        gl_texture_resource_t *resource =
            POOL_GET_AS(&backend->textures, rt->color_handle, gl_texture_resource_t);
        if (resource != NULL) {
            resource->owns_id = false;
            resource->id = 0;
        }
        pool_free(&backend->textures, rt->color_handle);
        rt->color_handle = TEXTURE_HANDLE_INVALID;
    }
    if (rt->depth_handle != TEXTURE_HANDLE_INVALID) {
        gl_texture_resource_t *resource =
            POOL_GET_AS(&backend->textures, rt->depth_handle, gl_texture_resource_t);
        if (resource != NULL) {
            resource->owns_id = false;
            resource->id = 0;
        }
        pool_free(&backend->textures, rt->depth_handle);
        rt->depth_handle = TEXTURE_HANDLE_INVALID;
    }

    if (rt->color_texture != 0) {
        gl->glDeleteTextures(1, &rt->color_texture);
        rt->color_texture = 0;
    }
    if (rt->depth_texture != 0) {
        gl->glDeleteTextures(1, &rt->depth_texture);
        rt->depth_texture = 0;
    }
    if (rt->color_rb != 0) {
        gl->glDeleteRenderbuffers(1, &rt->color_rb);
        rt->color_rb = 0;
    }
    if (rt->depth_rb != 0) {
        gl->glDeleteRenderbuffers(1, &rt->depth_rb);
        rt->depth_rb = 0;
    }
    if (rt->resolve_fbo != 0) {
        gl->glDeleteFramebuffers(1, &rt->resolve_fbo);
        rt->resolve_fbo = 0;
    }
    if (rt->fbo != 0) {
        gl->glDeleteFramebuffers(1, &rt->fbo);
        rt->fbo = 0;
    }
}

static bool gl_render_target_build(gl_backend_t *backend, gl_render_target_resource_t *rt)
{
    const gl_api_t *gl = gl_api_load();

    GLint  color_internal = 0;
    GLenum color_external = 0;
    GLenum color_type = 0;
    if (!gl_translate_pixel_format(rt->color_format, &color_internal, &color_external,
                                   &color_type)) {
        LOG_ERROR("渲染目标的颜色格式不支持 (%d)", (int)rt->color_format);
        return false;
    }

    GLint  depth_internal = 0;
    GLenum depth_external = 0;
    GLenum depth_type = 0;
    if (!gl_translate_pixel_format(rt->depth_format, &depth_internal, &depth_external,
                                   &depth_type)) {
        LOG_ERROR("渲染目标的深度格式不支持 (%d)", (int)rt->depth_format);
        return false;
    }

    const bool depth_stencil = (rt->depth_format == PIXEL_FORMAT_D24_UNORM_S8_UINT);
    const GLenum depth_attachment =
        depth_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;

    gl->glGenFramebuffers(1, &rt->fbo);
    if (rt->fbo == 0) {
        return false;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

    if (rt->depth_only) {
        // ---- 深度专用（阴影贴图）----
        gl->glGenTextures(1, &rt->depth_texture);
        gl->glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, depth_internal, (GLsizei)rt->width, (GLsizei)rt->height,
                         0, depth_external, depth_type, NULL);
        // 阴影采样必须是 CLAMP_TO_EDGE + 线性/最近邻：
        // 用 REPEAT 会让阴影贴图边缘绕到另一边，出现"墙外也有影子"的怪现象。
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glBindTexture(GL_TEXTURE_2D, 0);

        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, depth_attachment, GL_TEXTURE_2D,
                                   rt->depth_texture, 0);

        // 没有颜色附件时必须显式声明，否则帧缓冲不完整
        gl->glDrawBuffer(GL_NONE);
        gl->glReadBuffer(GL_NONE);
    } else if (rt->samples > 1) {
        // ---- MSAA ----
        gl->glGenRenderbuffers(1, &rt->color_rb);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->color_rb);
        gl->glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)rt->samples, color_internal,
                                             (GLsizei)rt->width, (GLsizei)rt->height);
        gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                      rt->color_rb);

        if (rt->has_depth) {
            gl->glGenRenderbuffers(1, &rt->depth_rb);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->depth_rb);
            gl->glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)rt->samples,
                                                 depth_internal, (GLsizei)rt->width,
                                                 (GLsizei)rt->height);
            gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment, GL_RENDERBUFFER,
                                          rt->depth_rb);
        }
        gl->glBindRenderbuffer(GL_RENDERBUFFER, 0);

        // resolve 目标：单采样的颜色纹理
        if (rt->sampleable) {
            gl->glGenTextures(1, &rt->color_texture);
            gl->glBindTexture(GL_TEXTURE_2D, rt->color_texture);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, color_internal, (GLsizei)rt->width,
                             (GLsizei)rt->height, 0, color_external, color_type, NULL);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glBindTexture(GL_TEXTURE_2D, 0);

            gl->glGenFramebuffers(1, &rt->resolve_fbo);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->resolve_fbo);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       rt->color_texture, 0);

            const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOG_ERROR("渲染目标的 resolve FBO 不完整 (0x%04X)", (unsigned)status);
                return false;
            }

            gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        }
    } else {
        // ---- 普通（无 MSAA）----
        if (rt->sampleable) {
            gl->glGenTextures(1, &rt->color_texture);
            gl->glBindTexture(GL_TEXTURE_2D, rt->color_texture);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, color_internal, (GLsizei)rt->width,
                             (GLsizei)rt->height, 0, color_external, color_type, NULL);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glBindTexture(GL_TEXTURE_2D, 0);
            gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       rt->color_texture, 0);
        } else {
            gl->glGenRenderbuffers(1, &rt->color_rb);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->color_rb);
            gl->glRenderbufferStorage(GL_RENDERBUFFER, color_internal, (GLsizei)rt->width,
                                      (GLsizei)rt->height);
            gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                          rt->color_rb);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        if (rt->has_depth) {
            // 场景的深度不需要被采样（M4 不做 SSAO/深度后处理），
            // 用 renderbuffer 比纹理更省显存也更快。
            gl->glGenRenderbuffers(1, &rt->depth_rb);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->depth_rb);
            gl->glRenderbufferStorage(GL_RENDERBUFFER, depth_internal, (GLsizei)rt->width,
                                      (GLsizei)rt->height);
            gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_attachment, GL_RENDERBUFFER,
                                          rt->depth_rb);
            gl->glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }
    }

    const GLenum status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("渲染目标不完整 (0x%04X)：%s（%ux%u, samples=%u）", (unsigned)status,
                  (rt->name != NULL) ? rt->name : "?", rt->width, rt->height, rt->samples);
        return false;
    }

    // 把附件纹理包装成引擎句柄，让渲染器能像普通纹理一样绑定它们
    if (rt->color_texture != 0) {
        rt->color_handle = gl_wrap_texture(backend, rt->color_texture, rt->width, rt->height,
                                           rt->color_format, false, rt->name);
        if (rt->color_handle == TEXTURE_HANDLE_INVALID) {
            return false;
        }
    }
    if (rt->depth_texture != 0) {
        rt->depth_handle = gl_wrap_texture(backend, rt->depth_texture, rt->width, rt->height,
                                           rt->depth_format, false, rt->name);
        if (rt->depth_handle == TEXTURE_HANDLE_INVALID) {
            return false;
        }
    }

    return true;
}

static render_target_handle_t gl_backend_create_render_target(render_backend_t *self,
                                                              const render_target_desc_t *desc)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    // 描述归一化（补默认格式、压非法采样数、深度专用自洽）是**纯函数**，
    // 放在 render_types.c 里以便单元测试覆盖 —— 见 render_target_desc_normalize。
    // 这里只负责"归一化之后仍然不合法就拒绝"。
    const render_target_desc_t norm = render_target_desc_normalize(desc);
    if (norm.width == 0 || norm.height == 0) {
        LOG_ERROR("渲染目标描述无效（尺寸为 0）");
        return RENDER_TARGET_HANDLE_INVALID;
    }

    const handle_t handle = pool_alloc(&backend->render_targets);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("渲染目标池已满（上限 %u）", RENDER_MAX_RENDER_TARGETS);
        return RENDER_TARGET_HANDLE_INVALID;
    }

    gl_render_target_resource_t *rt =
        POOL_GET_AS(&backend->render_targets, handle, gl_render_target_resource_t);
    memset(rt, 0, sizeof(*rt));

    rt->width = norm.width;
    rt->height = norm.height;
    rt->samples = norm.samples;
    rt->has_depth = norm.has_depth;
    rt->sampleable = norm.sampleable_color;
    rt->depth_only = norm.depth_only;
    rt->color_format = norm.color_format;
    rt->depth_format = norm.depth_format;
    rt->name = norm.debug_name;
    rt->creation_frame = backend->frame.frame_index;
    rt->color_handle = TEXTURE_HANDLE_INVALID;
    rt->depth_handle = TEXTURE_HANDLE_INVALID;

    if (!gl_render_target_build(backend, rt)) {
        gl_render_target_release_attachments(backend, rt);
        pool_free(&backend->render_targets, handle);
        return RENDER_TARGET_HANDLE_INVALID;
    }

    LOG_DEBUG("渲染目标就绪: %s %ux%u samples=%u %s", (rt->name != NULL) ? rt->name : "?",
              rt->width, rt->height, rt->samples, rt->depth_only ? "(深度专用)" : "");
    return handle;
}

static void gl_backend_destroy_render_target(render_backend_t *self, render_target_handle_t target)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    gl_render_target_resource_t *rt =
        POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
    if (rt == NULL) {
        return;
    }

    // 正在被绑定的目标不能直接删：先切回默认帧缓冲
    if (backend->current_target == target) {
        gl_backend_bind_render_target(self, RENDER_TARGET_HANDLE_INVALID);
    }

    gl_render_target_release_attachments(backend, rt);
    pool_free(&backend->render_targets, target);
}

static bool gl_backend_resize_render_target(render_backend_t *self, render_target_handle_t target,
                                            u32 width, u32 height)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    gl_render_target_resource_t *rt =
        POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
    if (rt == NULL || width == 0 || height == 0) {
        return false;
    }
    if (rt->width == width && rt->height == height) {
        return true;
    }

    if (backend->current_target == target) {
        gl_backend_bind_render_target(self, RENDER_TARGET_HANDLE_INVALID);
    }

    // 记住格式再重建（release 会把附件清掉）
    const u32            samples = rt->samples;
    const bool           has_depth = rt->has_depth;
    const bool           sampleable = rt->sampleable;
    const bool           depth_only = rt->depth_only;
    const pixel_format_t color_format = rt->color_format;
    const pixel_format_t depth_format = rt->depth_format;
    const char          *name = rt->name;
    const u64            creation_frame = rt->creation_frame;

    gl_render_target_release_attachments(backend, rt);
    memset(rt, 0, sizeof(*rt));

    rt->width = width;
    rt->height = height;
    rt->samples = samples;
    rt->has_depth = has_depth;
    rt->sampleable = sampleable;
    rt->depth_only = depth_only;
    rt->color_format = color_format;
    rt->depth_format = depth_format;
    rt->name = name;
    rt->creation_frame = creation_frame;
    rt->color_handle = TEXTURE_HANDLE_INVALID;
    rt->depth_handle = TEXTURE_HANDLE_INVALID;

    return gl_render_target_build(backend, rt);
}

static texture_handle_t gl_backend_get_render_target_texture(render_backend_t *self,
                                                             render_target_handle_t target)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    gl_render_target_resource_t *rt =
        POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
    if (rt == NULL) {
        return TEXTURE_HANDLE_INVALID;
    }
    // 深度专用目标返回深度纹理，其余返回颜色纹理
    return rt->depth_only ? rt->depth_handle : rt->color_handle;
}

// 把 MSAA 缓冲解析到单采样纹理。
// **必须在切换渲染目标之前做** —— MSAA 缓冲不能直接采样。
static void gl_render_target_resolve(gl_backend_t *backend, gl_render_target_resource_t *rt)
{
    if (!rt->needs_resolve || rt->resolve_fbo == 0) {
        return;
    }

    const gl_api_t *gl = gl_api_load();

    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->resolve_fbo);
    gl->glBlitFramebuffer(0, 0, (GLint)rt->width, (GLint)rt->height, 0, 0, (GLint)rt->width,
                          (GLint)rt->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // 恢复原来的 DRAW 绑定：resolve 用了 GL_DRAW_FRAMEBUFFER 这个独立绑定点，
    // 不恢复的话当前 render pass 的绘制会跑到窗口上（或者干脆丢进黑洞）。
    if (backend->current_target != RENDER_TARGET_HANDLE_INVALID) {
        gl_render_target_resource_t *current =
            POOL_GET_AS(&backend->render_targets, backend->current_target,
                        gl_render_target_resource_t);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, (current != NULL) ? current->fbo : 0);
    } else {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    rt->needs_resolve = false;
    backend->stats.buffer_uploads++;
}

static void gl_backend_bind_render_target(render_backend_t *self, render_target_handle_t target)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (backend->current_target == target) {
        return;
    }

    // 切走之前把上一个目标的 MSAA 解析掉
    if (backend->current_target != RENDER_TARGET_HANDLE_INVALID) {
        gl_render_target_resource_t *previous =
            POOL_GET_AS(&backend->render_targets, backend->current_target,
                        gl_render_target_resource_t);
        if (previous != NULL) {
            gl_render_target_resolve(backend, previous);
        }
    }

    if (target == RENDER_TARGET_HANDLE_INVALID) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        gl_render_target_resource_t *rt =
            POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
        if (rt == NULL) {
            LOG_WARN("绑定了无效的渲染目标句柄");
            return;
        }
        gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        rt->needs_resolve = (rt->samples > 1) && (rt->resolve_fbo != 0);
    }

    backend->current_target = target;

    // 换目标后 GL 状态可能被外部改动，作废状态缓存
    backend->state.valid = false;
    backend->current_pipeline = PIPELINE_HANDLE_INVALID;
}

static bool gl_backend_read_pixel(render_backend_t *self, render_target_handle_t target, u32 x,
                                  u32 y, u8 out_rgba[4])
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (out_rgba == NULL) {
        return false;
    }

    // 默认帧缓冲（窗口）：FBO 0。
    // 窗口内容在 swap 之后就没了，所以调用方必须在 end_frame 内部、
    // present pass 之后、swap 之前读 —— 帧结束后再读拿到的是未绘制的后缓冲。
    if (target == RENDER_TARGET_HANDLE_INVALID) {
        if (x >= backend->frame.target_width || y >= backend->frame.target_height) {
            return false;
        }
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        // 默认帧缓冲的读取缓冲通常就是 GL_BACK，但显式指定一次更稳：
        // 某些驱动在双缓冲模式下默认读取的是 GL_FRONT，读回来永远是旧内容。
        gl->glReadBuffer(GL_BACK);
        gl->glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
        return true;
    }

    gl_render_target_resource_t *rt =
        POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
    if (rt == NULL || x >= rt->width || y >= rt->height) {
        return false;
    }
    if (rt->depth_only) {
        LOG_WARN("深度专用渲染目标不支持颜色读回");
        return false;
    }

    // MSAA 缓冲不能直接读，必须先解析
    if (rt->needs_resolve) {
        gl_render_target_resolve(backend, rt);
    }

    // 只改 READ 绑定，不动 DRAW 绑定 —— 这样不会打断正在进行的 render pass。
    const GLuint fbo = (rt->resolve_fbo != 0) ? rt->resolve_fbo : rt->fbo;

    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glReadPixels((GLint)x, (GLint)y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    return true;
}

// 读回一块矩形。与 read_pixel 共用同一套"选 FBO / 选读取缓冲"的逻辑，
// 区别只是 glReadPixels 的宽高参数 —— 一次同步读一整块，
// 而不是每像素同步一次（后者在图较大时会慢到不可用）。
static bool gl_backend_read_rect(render_backend_t *self, render_target_handle_t target, u32 x,
                                 u32 y, u32 width, u32 height, u8 *out_rgba)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (out_rgba == NULL || width == 0 || height == 0) {
        return false;
    }

    GLuint read_fbo = 0;
    bool   use_default = false;

    if (target == RENDER_TARGET_HANDLE_INVALID) {
        if (x + width > backend->frame.target_width || y + height > backend->frame.target_height) {
            return false;
        }
        use_default = true;
    } else {
        gl_render_target_resource_t *rt =
            POOL_GET_AS(&backend->render_targets, target, gl_render_target_resource_t);
        if (rt == NULL || x + width > rt->width || y + height > rt->height) {
            return false;
        }
        if (rt->depth_only) {
            LOG_WARN("深度专用渲染目标不支持颜色读回");
            return false;
        }
        if (rt->needs_resolve) {
            gl_render_target_resolve(backend, rt);
        }
        read_fbo = (rt->resolve_fbo != 0) ? rt->resolve_fbo : rt->fbo;
    }

    // glReadPixels 按**行**填充，且第一行是窗口坐标的 y（从下往上）。
    // 调用方拿到的是"上下颠倒"的图 —— 这是 GL 的约定，转换由调用方做，
    // 后端不做隐式翻转（隐式翻转会让"图为什么是倒的"变成一个要猜的问题）。
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    gl->glReadBuffer(use_default ? GL_BACK : GL_COLOR_ATTACHMENT0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height, GL_RGBA,
                     GL_UNSIGNED_BYTE, out_rgba);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    return true;
}

// =============================================================================
//  绑定与绘制
// =============================================================================
static void gl_backend_bind_pipeline(render_backend_t *self, pipeline_handle_t pipeline)
{
    gl_backend_t *backend = (gl_backend_t *)self;

    if (pipeline == backend->current_pipeline && backend->state.valid) {
        return; // 同一管线重复绑定，直接跳过
    }

    gl_pipeline_resource_t *resource =
        POOL_GET_AS(&backend->pipelines, pipeline, gl_pipeline_resource_t);
    if (resource == NULL) {
        LOG_WARN("绑定了无效的管线句柄");
        return;
    }

    if (!backend->state.valid) {
        // 状态缓存刚被作废（帧开始或 render pass 开始），
        // 先把缓存填成"与 GL 实际状态一致的已知值"。
        // 这里选择的是保守做法：认为 GL 状态是干净的默认值，
        // 然后把管线状态整体下发一遍。
        memset(&backend->state, 0, sizeof(backend->state));
        backend->state.valid = true;
        backend->state.polygon_mode = GL_FILL;
        backend->state.front_face = GL_CCW;
        backend->state.depth_func = GL_LESS;
        backend->state.blend_eq_color = GL_FUNC_ADD;
        backend->state.blend_eq_alpha = GL_FUNC_ADD;
    }

    gl_apply_pipeline_state(backend, resource);
    backend->current_pipeline = pipeline;
    backend->stats.pipeline_switches++;
}

static void gl_backend_bind_texture(render_backend_t *self, u32 slot, texture_handle_t texture,
                                    const sampler_desc_t *sampler)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (slot >= RENDER_MAX_TEXTURE_SLOTS) {
        return;
    }

    GLuint texture_id = backend->default_white;
    if (texture != TEXTURE_HANDLE_INVALID) {
        gl_texture_resource_t *resource =
            POOL_GET_AS(&backend->textures, texture, gl_texture_resource_t);
        if (resource != NULL) {
            texture_id = resource->id;
        }
    }

    const GLuint sampler_id = gl_get_sampler(backend, sampler);

    if (backend->state.bound_texture[slot] != texture_id) {
        if (backend->state.active_texture_unit != slot) {
            gl->glActiveTexture(GL_TEXTURE0 + slot);
            backend->state.active_texture_unit = slot;
        }
        gl->glBindTexture(GL_TEXTURE_2D, texture_id);
        backend->state.bound_texture[slot] = texture_id;
        backend->stats.texture_bindings++;
    }

    if (backend->state.bound_sampler[slot] != sampler_id) {
        // glBindSampler 不受 glActiveTexture 影响，它直接作用于纹理单元。
        gl->glBindSampler(slot, sampler_id);
        backend->state.bound_sampler[slot] = sampler_id;
    }
}

static void gl_backend_draw(render_backend_t *self, mesh_handle_t mesh_handle,
                            const draw_call_t *call)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    gl_mesh_resource_t *mesh = POOL_GET_AS(&backend->meshes, mesh_handle, gl_mesh_resource_t);
    if (mesh == NULL) {
        LOG_WARN("绘制时引用了无效的网格句柄");
        return;
    }

    gl_pipeline_resource_t *pipeline =
        POOL_GET_AS(&backend->pipelines, backend->current_pipeline, gl_pipeline_resource_t);
    if (pipeline == NULL) {
        LOG_WARN("绘制前没有绑定管线");
        return;
    }

    const draw_call_t default_call = draw_call_all();
    if (call == NULL) {
        call = &default_call;
    }

    const u32 instance_count = (call->instance_count == 0u) ? 1u : call->instance_count;

    gl->glBindVertexArray(mesh->vao);

    if (mesh->index_count > 0 && mesh->ibo != 0) {
        const u32 count = (call->index_count > 0u) ? call->index_count : mesh->index_count;
        const u32 index_size = index_format_byte_size(mesh->index_format);
        const GLenum index_type = (mesh->index_format == INDEX_FORMAT_U16) ? GL_UNSIGNED_SHORT
                                                                          : GL_UNSIGNED_INT;
        const void *offset = (const void *)(uintptr_t)(call->first_index * index_size);

        if (instance_count > 1u) {
            gl->glDrawElementsInstanced(pipeline->topology, (GLsizei)count, index_type, offset,
                                        (GLsizei)instance_count);
        } else {
            gl->glDrawElements(pipeline->topology, (GLsizei)count, index_type, offset);
        }

        backend->stats.triangles += (count / 3u) * instance_count;
        backend->stats.vertices += count * instance_count;
    } else {
        const u32 count = (call->index_count > 0u) ? call->index_count : mesh->vertex_count;

        if (instance_count > 1u) {
            gl->glDrawArraysInstanced(pipeline->topology, (GLint)call->first_index, (GLsizei)count,
                                      (GLsizei)instance_count);
        } else {
            gl->glDrawArrays(pipeline->topology, (GLint)call->first_index, (GLsizei)count);
        }

        backend->stats.triangles += (count / 3u) * instance_count;
        backend->stats.vertices += count * instance_count;
    }

    backend->stats.draw_calls++;
}

static void gl_backend_set_uniforms(render_backend_t *self, u32 binding, const void *data,
                                    usize size)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    const gl_api_t *gl = gl_api_load();

    if (binding >= RENDER_UNIFORM_BINDING_COUNT || data == NULL) {
        return;
    }

    if (size > backend->uniform_buffer_capacities[binding]) {
        LOG_ERROR("uniform 块 %u 数据过大（%llu > %u）", binding, (unsigned long long)size,
                  backend->uniform_buffer_capacities[binding]);
        return;
    }

    // 注意：glBindBuffer(GL_UNIFORM_BUFFER, ...) 改的是"通用绑定"，
    // 而 glBindBufferBase 设的是"带索引的绑定"。两者互不干扰，
    // 所以这里更新数据不会破坏已经建立好的绑定点。
    gl->glBindBuffer(GL_UNIFORM_BUFFER, backend->uniform_buffers[binding]);
    gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)size, data);
    gl->glBindBuffer(GL_UNIFORM_BUFFER, 0);

    backend->stats.buffer_uploads++;
    backend->stats.buffer_upload_bytes += size;
}

// =============================================================================
//  统计
// =============================================================================
static void gl_backend_reset_stats(render_backend_t *self)
{
    gl_backend_t *backend = (gl_backend_t *)self;
    render_stats_reset(&backend->stats);
}

static void gl_backend_get_stats(const render_backend_t *self, render_stats_t *out_stats)
{
    const gl_backend_t *backend = (const gl_backend_t *)self;
    if (out_stats != NULL) {
        *out_stats = backend->stats;
    }
}

// =============================================================================
//  工厂
// =============================================================================
static render_backend_t *gl_backend_create_impl(allocator_t *alloc)
{
    ASSERT(alloc != NULL);

    gl_backend_t *backend = (gl_backend_t *)alloc->alloc(alloc, sizeof(gl_backend_t), 16);
    if (backend == NULL) {
        LOG_ERROR("GL 后端对象分配失败");
        return NULL;
    }
    memset(backend, 0, sizeof(*backend));

    backend->alloc = alloc;
    backend->base.name = "opengl";
    backend->base.init = gl_backend_init;
    backend->base.shutdown = gl_backend_shutdown;
    backend->base.get_caps = gl_backend_get_caps;
    backend->base.begin_frame = gl_backend_begin_frame;
    backend->base.end_frame = gl_backend_end_frame;
    backend->base.begin_render_pass = gl_backend_begin_render_pass;
    backend->base.end_render_pass = gl_backend_end_render_pass;
    backend->base.set_viewport = gl_backend_set_viewport;
    backend->base.set_scissor = gl_backend_set_scissor;
    backend->base.create_shader = gl_backend_create_shader;
    backend->base.destroy_shader = gl_backend_destroy_shader;
    backend->base.create_texture = gl_backend_create_texture;
    backend->base.destroy_texture = gl_backend_destroy_texture;
    backend->base.update_texture = gl_backend_update_texture;
    backend->base.create_mesh = gl_backend_create_mesh;
    backend->base.destroy_mesh = gl_backend_destroy_mesh;
    backend->base.update_mesh = gl_backend_update_mesh;
    backend->base.create_pipeline = gl_backend_create_pipeline;
    backend->base.destroy_pipeline = gl_backend_destroy_pipeline;
    backend->base.create_render_target = gl_backend_create_render_target;
    backend->base.destroy_render_target = gl_backend_destroy_render_target;
    backend->base.resize_render_target = gl_backend_resize_render_target;
    backend->base.get_render_target_texture = gl_backend_get_render_target_texture;
    backend->base.bind_render_target = gl_backend_bind_render_target;
    backend->base.read_pixel = gl_backend_read_pixel;
    backend->base.read_rect = gl_backend_read_rect;
    backend->base.bind_pipeline = gl_backend_bind_pipeline;
    backend->base.bind_texture = gl_backend_bind_texture;
    backend->base.draw = gl_backend_draw;
    backend->base.set_uniforms = gl_backend_set_uniforms;
    backend->base.reset_stats = gl_backend_reset_stats;
    backend->base.get_stats = gl_backend_get_stats;

    backend->current_pipeline = PIPELINE_HANDLE_INVALID;
    backend->current_target = RENDER_TARGET_HANDLE_INVALID;

    return &backend->base;
}

render_backend_t *gl_backend_create(allocator_t *alloc)
{
    return gl_backend_create_impl(alloc);
}

void gl_backend_destroy(render_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }

    gl_backend_t *concrete = (gl_backend_t *)backend;
    allocator_t  *alloc = concrete->alloc;

    // shutdown 是幂等的：context 为 NULL 时直接返回。
    // 这样"init 失败后直接 destroy"也是安全的。
    gl_backend_shutdown(backend);

    if (alloc != NULL) {
        alloc->free(alloc, concrete, sizeof(gl_backend_t));
    }
}
