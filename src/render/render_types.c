// =============================================================================
//  src/render/render_types.c —— 后端无关类型的辅助函数与预设
// =============================================================================
#include "engine/render/render_types.h"

#include <string.h>

#include "engine/core/assert.h"

// =============================================================================
//  像素格式
// =============================================================================
u32 pixel_format_bytes_per_pixel(pixel_format_t format)
{
    switch (format) {
    case PIXEL_FORMAT_R8_UNORM:
        return 1;
    case PIXEL_FORMAT_R8G8_UNORM:
        return 2;
    case PIXEL_FORMAT_R8G8B8A8_UNORM:
    case PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB:
    case PIXEL_FORMAT_B8G8R8A8_UNORM:
    case PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB:
    case PIXEL_FORMAT_R32_FLOAT:
    case PIXEL_FORMAT_D24_UNORM_S8_UINT:
    case PIXEL_FORMAT_D32_FLOAT:
        return 4;
    case PIXEL_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return 0;
    }
}

bool pixel_format_is_srgb(pixel_format_t format)
{
    return format == PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool pixel_format_is_depth(pixel_format_t format)
{
    return format == PIXEL_FORMAT_D32_FLOAT || format == PIXEL_FORMAT_D24_UNORM_S8_UINT;
}

// =============================================================================
//  顶点格式
// =============================================================================
u32 vertex_format_component_count(vertex_format_t format)
{
    switch (format) {
    case VERTEX_FORMAT_FLOAT1:
    case VERTEX_FORMAT_UINT1:
        return 1;
    case VERTEX_FORMAT_FLOAT2:
    case VERTEX_FORMAT_UINT2:
    case VERTEX_FORMAT_UNORM16X2:
        return 2;
    case VERTEX_FORMAT_FLOAT3:
    case VERTEX_FORMAT_UINT3:
        return 3;
    case VERTEX_FORMAT_FLOAT4:
    case VERTEX_FORMAT_UINT4:
    case VERTEX_FORMAT_UNORM8X4:
    case VERTEX_FORMAT_SNORM8X4:
    case VERTEX_FORMAT_UNORM16X4:
        return 4;
    default:
        return 0;
    }
}

u32 vertex_format_byte_size(vertex_format_t format)
{
    // 每个格式的字节数 = 分量数 × 单个分量的大小。
    // 别用"分量数 * 4"糊弄：UNORM8X4 是 4 字节、UNORM16X4 是 8 字节，
    // 一律按 4 字节算会让顶点布局的 stride 校验形同虚设。
    switch (format) {
    case VERTEX_FORMAT_FLOAT1:
        return 4;
    case VERTEX_FORMAT_FLOAT2:
        return 8;
    case VERTEX_FORMAT_FLOAT3:
        return 12;
    case VERTEX_FORMAT_FLOAT4:
        return 16;

    case VERTEX_FORMAT_UINT1:
        return 4;
    case VERTEX_FORMAT_UINT2:
        return 8;
    case VERTEX_FORMAT_UINT3:
        return 12;
    case VERTEX_FORMAT_UINT4:
        return 16;

    case VERTEX_FORMAT_UNORM8X4:
    case VERTEX_FORMAT_SNORM8X4:
        return 4; // 4 个 8 位分量

    case VERTEX_FORMAT_UNORM16X2:
        return 4; // 2 个 16 位分量
    case VERTEX_FORMAT_UNORM16X4:
        return 8; // 4 个 16 位分量

    default:
        return 0;
    }
}

i32 vertex_layout_find_attribute(const vertex_layout_t *layout, vertex_semantic_t semantic)
{
    if (layout == NULL) {
        return -1;
    }
    for (u32 i = 0; i < layout->attribute_count; ++i) {
        if (layout->attributes[i].semantic == semantic) {
            return (i32)i;
        }
    }
    return -1;
}

vertex_layout_t vertex_layout_mesh_vertex(void)
{
    vertex_layout_t layout;
    memset(&layout, 0, sizeof(layout));

    // 单个 binding：位置/法线/UV 交错在同一个缓冲区里。
    // 交错布局对 GPU 的顶点预取最友好（一个顶点的数据在相邻内存里）。
    layout.binding_count = 1;
    layout.bindings[0].stride = MESH_VERTEX_STRIDE;
    layout.bindings[0].per_instance = false;

    layout.attribute_count = 3;
    layout.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
    layout.attributes[0].binding = 0;
    layout.attributes[0].format = VERTEX_FORMAT_FLOAT3;
    layout.attributes[0].offset = 0;

    layout.attributes[1].semantic = VERTEX_SEMANTIC_NORMAL;
    layout.attributes[1].binding = 0;
    layout.attributes[1].format = VERTEX_FORMAT_FLOAT3;
    layout.attributes[1].offset = 12;

    layout.attributes[2].semantic = VERTEX_SEMANTIC_TEXCOORD0;
    layout.attributes[2].binding = 0;
    layout.attributes[2].format = VERTEX_FORMAT_FLOAT2;
    layout.attributes[2].offset = 24;

    return layout;
}

bool vertex_layout_validate(const vertex_layout_t *layout, const char **out_reason)
{
    if (layout == NULL) {
        if (out_reason != NULL) {
            *out_reason = "布局指针为空";
        }
        return false;
    }
    if (layout->binding_count > RENDER_MAX_VERTEX_BINDINGS) {
        if (out_reason != NULL) {
            *out_reason = "binding 数量超出上限";
        }
        return false;
    }
    if (layout->attribute_count > RENDER_MAX_VERTEX_ATTRIBUTES) {
        if (out_reason != NULL) {
            *out_reason = "属性数量超出上限";
        }
        return false;
    }

    // binding_count == 0 是合法的：全屏三角形这类"用 gl_VertexID 算顶点"
    // 的绘制不需要任何顶点输入。Vulkan 也允许 pipeline 的 vertex input
    // 状态为空。但"有属性却没有 binding"一定是配置错误。
    if (layout->binding_count == 0) {
        if (layout->attribute_count > 0) {
            if (out_reason != NULL) {
                *out_reason = "有顶点属性却没有 binding";
            }
            return false;
        }
        if (out_reason != NULL) {
            *out_reason = "";
        }
        return true;
    }

    for (u32 i = 0; i < layout->binding_count; ++i) {
        if (layout->bindings[i].stride == 0) {
            if (out_reason != NULL) {
                *out_reason = "binding 的 stride 为 0";
            }
            return false;
        }
    }

    for (u32 i = 0; i < layout->attribute_count; ++i) {
        const vertex_attribute_t *attr = &layout->attributes[i];

        if (attr->binding >= layout->binding_count) {
            if (out_reason != NULL) {
                *out_reason = "属性引用了不存在的 binding";
            }
            return false;
        }
        if (attr->format == VERTEX_FORMAT_INVALID) {
            if (out_reason != NULL) {
                *out_reason = "属性的顶点格式无效";
            }
            return false;
        }

        const u32 stride = layout->bindings[attr->binding].stride;
        const u32 need = attr->offset + vertex_format_byte_size(attr->format);
        if (need > stride) {
            if (out_reason != NULL) {
                *out_reason = "属性的 offset + 大小超出了 binding 的 stride";
            }
            return false;
        }

        // 同一语义出现两次 = 两个属性抢同一个着色器 location，
        // 后一个会静默覆盖前一个，表现为"某个属性永远是 0"。
        for (u32 j = i + 1; j < layout->attribute_count; ++j) {
            if (layout->attributes[j].semantic == attr->semantic) {
                if (out_reason != NULL) {
                    *out_reason = "同一个语义被声明了两次";
                }
                return false;
            }
        }
    }

    if (out_reason != NULL) {
        *out_reason = "";
    }
    return true;
}

// =============================================================================
//  索引
// =============================================================================
u32 index_format_byte_size(index_format_t format)
{
    switch (format) {
    case INDEX_FORMAT_U16:
        return 2;
    case INDEX_FORMAT_U32:
        return 4;
    default:
        return 0;
    }
}

// =============================================================================
//  混合预设
// =============================================================================
blend_state_t blend_state_opaque(void)
{
    blend_state_t state;
    memset(&state, 0, sizeof(state));
    state.enabled = false;
    state.src_color = BLEND_FACTOR_ONE;
    state.dst_color = BLEND_FACTOR_ZERO;
    state.color_op = BLEND_OP_ADD;
    state.src_alpha = BLEND_FACTOR_ONE;
    state.dst_alpha = BLEND_FACTOR_ZERO;
    state.alpha_op = BLEND_OP_ADD;
    state.write_mask = BLEND_WRITE_ALL;
    return state;
}

blend_state_t blend_state_alpha(void)
{
    blend_state_t state = blend_state_opaque();
    state.enabled = true;
    state.src_color = BLEND_FACTOR_SRC_ALPHA;
    state.dst_color = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.src_alpha = BLEND_FACTOR_ONE;
    state.dst_alpha = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    return state;
}

blend_state_t blend_state_premultiplied(void)
{
    blend_state_t state = blend_state_opaque();
    state.enabled = true;
    // 预乘 alpha：源颜色已经乘过 alpha，所以源因子用 ONE。
    // 用 SRC_ALPHA 会乘两次，边缘会明显发暗。
    state.src_color = BLEND_FACTOR_ONE;
    state.dst_color = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.src_alpha = BLEND_FACTOR_ONE;
    state.dst_alpha = BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    return state;
}

blend_state_t blend_state_additive(void)
{
    blend_state_t state = blend_state_opaque();
    state.enabled = true;
    state.src_color = BLEND_FACTOR_SRC_ALPHA;
    state.dst_color = BLEND_FACTOR_ONE;
    state.src_alpha = BLEND_FACTOR_ONE;
    state.dst_alpha = BLEND_FACTOR_ONE;
    return state;
}

// =============================================================================
//  深度预设
// =============================================================================
depth_state_t depth_state_default(void)
{
    depth_state_t state;
    state.test_enabled = true;
    state.write_enabled = true;
    state.compare = COMPARE_OP_LESS;
    return state;
}

depth_state_t depth_state_read_only(void)
{
    depth_state_t state = depth_state_default();
    // 半透明物体不能写深度：否则同一个像素上的后续透明物体会被它挡住，
    // 表现为"远处的透明物体消失"。
    state.write_enabled = false;
    return state;
}

depth_state_t depth_state_disabled(void)
{
    depth_state_t state;
    state.test_enabled = false;
    state.write_enabled = false;
    state.compare = COMPARE_OP_ALWAYS;
    return state;
}

// =============================================================================
//  光栅化预设
// =============================================================================
raster_state_t raster_state_default(void)
{
    raster_state_t state;
    memset(&state, 0, sizeof(state));
    state.cull = CULL_MODE_BACK;
    state.front_face = FRONT_FACE_COUNTER_CLOCKWISE;
    state.fill = FILL_MODE_SOLID;
    return state;
}

raster_state_t raster_state_no_cull(void)
{
    raster_state_t state = raster_state_default();
    state.cull = CULL_MODE_NONE;
    return state;
}

raster_state_t raster_state_wireframe(void)
{
    raster_state_t state = raster_state_default();
    state.fill = FILL_MODE_WIREFRAME;
    return state;
}

// =============================================================================
//  采样器预设
// =============================================================================
sampler_desc_t sampler_desc_default(void)
{
    sampler_desc_t sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.min_filter = SAMPLER_FILTER_LINEAR;
    sampler.mag_filter = SAMPLER_FILTER_LINEAR;
    sampler.mipmap_filter = SAMPLER_MIPMAP_LINEAR;
    sampler.wrap_u = SAMPLER_WRAP_REPEAT;
    sampler.wrap_v = SAMPLER_WRAP_REPEAT;
    sampler.wrap_w = SAMPLER_WRAP_REPEAT;
    sampler.max_anisotropy = 1.0f;
    return sampler;
}

sampler_desc_t sampler_desc_pixel_art(void)
{
    sampler_desc_t sampler;
    memset(&sampler, 0, sizeof(sampler));
    // 像素风必须用最近邻，否则放大后边缘会糊成一团
    sampler.min_filter = SAMPLER_FILTER_NEAREST;
    sampler.mag_filter = SAMPLER_FILTER_NEAREST;
    sampler.mipmap_filter = SAMPLER_MIPMAP_NONE;
    sampler.wrap_u = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_v = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_w = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.max_anisotropy = 1.0f;
    return sampler;
}

sampler_desc_t sampler_desc_clamp_linear(void)
{
    sampler_desc_t sampler = sampler_desc_default();
    sampler.wrap_u = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_v = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_w = SAMPLER_WRAP_CLAMP_TO_EDGE;
    return sampler;
}

sampler_desc_t sampler_desc_sprite(void)
{
    sampler_desc_t sampler;
    memset(&sampler, 0, sizeof(sampler));
    sampler.min_filter = SAMPLER_FILTER_LINEAR;
    sampler.mag_filter = SAMPLER_FILTER_LINEAR;
    // 图集不生成 mipmap，所以这里必须是 NONE。
    // 用 SAMPLER_MIPMAP_LINEAR 会让纹理"不完整"→ 采样全黑。
    sampler.mipmap_filter = SAMPLER_MIPMAP_NONE;
    // 钳制而不是重复：图集里相邻区域挨着，REPEAT 在边缘会绕到另一边。
    // （配合 padding + UV 半像素内缩，这是第三层防护。）
    sampler.wrap_u = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_v = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.wrap_w = SAMPLER_WRAP_CLAMP_TO_EDGE;
    sampler.max_anisotropy = 1.0f;
    return sampler;
}

// =============================================================================
//  管线与绘制预设
// =============================================================================
pipeline_desc_t pipeline_desc_default(void)
{
    pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    desc.shader = SHADER_HANDLE_INVALID;
    desc.vertex_layout = vertex_layout_mesh_vertex();
    desc.topology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    desc.raster = raster_state_default();
    desc.depth = depth_state_default();
    desc.blend = blend_state_opaque();
    desc.multisample.samples = 1;
    desc.debug_name = NULL;
    return desc;
}

draw_call_t draw_call_all(void)
{
    draw_call_t call;
    memset(&call, 0, sizeof(call));
    call.index_count = 0; // 0 = 使用整个网格
    call.instance_count = 1;
    return call;
}

// =============================================================================
//  统计
// =============================================================================
void render_stats_reset(render_stats_t *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
}

render_target_desc_t render_target_desc_default(void)
{
    render_target_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = 1280;
    desc.height = 720;
    desc.color_format = PIXEL_FORMAT_R8G8B8A8_UNORM;
    desc.depth_format = PIXEL_FORMAT_D24_UNORM_S8_UINT;
    desc.samples = 1;
    desc.has_depth = true;
    desc.sampleable_color = true;
    desc.depth_only = false;
    desc.debug_name = "render_target";
    return desc;
}

// MSAA 只支持 2 / 4 / 8 / 16 四档（GL 和 Vulkan 都是），
// 给别的值驱动会报 GL_INVALID_VALUE，所以这里直接回落到 1 而不是"就近取整"——
// 悄悄把 3 变成 4 会让调用方以为拿到了他要求的采样数。
static bool render_target_samples_supported(u32 samples)
{
    return samples == 2 || samples == 4 || samples == 8 || samples == 16;
}

render_target_desc_t render_target_desc_normalize(const render_target_desc_t *desc)
{
    render_target_desc_t out;
    memset(&out, 0, sizeof(out));

    if (desc == NULL) {
        return out; // 全 0 => 调用方视为失败
    }
    out = *desc;

    // 尺寸为 0 无法建附件。不在这里"补一个默认尺寸"：
    // 调用方传 0 通常意味着它算错了（比如拿了一个还没初始化的视口），
    // 悄悄给个 1280x720 会让错误一路传到很远的地方才爆发。
    if (out.width == 0 || out.height == 0) {
        out.width = 0;
        out.height = 0;
        return out;
    }

    const pixel_format_t default_color = PIXEL_FORMAT_R8G8B8A8_UNORM;
    const pixel_format_t default_depth = PIXEL_FORMAT_D24_UNORM_S8_UINT;

    // 颜色格式必须是颜色格式。填成 D32_FLOAT 的话 FBO 会因为
    // "把深度纹理挂到 COLOR_ATTACHMENT0"而不完整 —— 而且是静默的。
    if (out.color_format == PIXEL_FORMAT_INVALID || pixel_format_is_depth(out.color_format)) {
        out.color_format = default_color;
    }

    if (out.depth_format == PIXEL_FORMAT_INVALID || !pixel_format_is_depth(out.depth_format)) {
        out.depth_format = default_depth;
    }

    if (!render_target_samples_supported(out.samples)) {
        out.samples = 1;
    }

    // 深度专用（阴影贴图）：自洽化 —— 必须有深度、必须可采样、不需要 MSAA。
    if (out.depth_only) {
        out.has_depth = true;
        out.sampleable_color = true;
        out.samples = 1;
    }

    // 不要深度附件时把格式清掉，让下游不用再判一次 has_depth。
    if (!out.has_depth) {
        out.depth_format = PIXEL_FORMAT_INVALID;
    }

    return out;
}

void render_stats_accumulate(render_stats_t *target, const render_stats_t *source)
{
    if (target == NULL || source == NULL) {
        return;
    }
    target->draw_calls += source->draw_calls;
    target->render_items += source->render_items;
    target->triangles += source->triangles;
    target->vertices += source->vertices;
    target->pipeline_switches += source->pipeline_switches;
    target->texture_bindings += source->texture_bindings;
    target->buffer_uploads += source->buffer_uploads;
    target->buffer_upload_bytes += source->buffer_upload_bytes;
}
