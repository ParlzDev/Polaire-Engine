// =============================================================================
//  src/render/shadow.c —— 阴影贴图
// =============================================================================
#include "engine/render/shadow.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/renderer.h"

// 深度 pass 的着色器：只算裁剪空间位置，片元着色器什么都不做。
//
// 为什么不用 lit 着色器代劳：它要采样贴图、算光照，在只写深度的 pass 里
// 全是浪费（而且采样未绑定的纹理可能触发驱动警告）。
static const char *const k_depth_vertex =
    "layout(location = 0) in vec3 a_position;\n"
    "void main() {\n"
    "    gl_Position = u_light_view_projection * u_model * vec4(a_position, 1.0);\n"
    "}\n";

static const char *const k_depth_fragment = "void main() {}\n";

bool shadow_map_init(shadow_map_t *shadow, renderer_t *renderer, u32 size)
{
    ASSERT(shadow != NULL);
    ASSERT(renderer != NULL);

    memset(shadow, 0, sizeof(*shadow));

    shadow->size = (size >= 256u) ? size : SHADOW_DEFAULT_SIZE;

    // 默认偏移。这两个数是"够用就好"的起点：
    // 出现条纹状自阴影就调大，出现阴影与物体分离就调小。
    shadow->depth_bias = 2.5f;
    shadow->slope_bias = 1.8f;
    shadow->normal_bias = 0.035f;
    shadow->strength = 0.75f;
    shadow->light_view_projection = mat4_identity();
    shadow->creation_frame = renderer_frame_index(renderer);

    // ---- 深度专用渲染目标 ----
    render_target_desc_t rt_desc = render_target_desc_default();
    rt_desc.width = shadow->size;
    rt_desc.height = shadow->size;
    rt_desc.samples = 1; // 阴影贴图不需要 MSAA
    rt_desc.has_depth = true;
    rt_desc.sampleable_color = false;
    rt_desc.depth_only = true; // 没有颜色附件
    rt_desc.depth_format = PIXEL_FORMAT_D32_FLOAT;
    rt_desc.debug_name = "shadow_map";

    shadow->target = render_target_create(renderer, &rt_desc);
    if (!render_target_is_valid(renderer, shadow->target)) {
        LOG_ERROR("阴影贴图创建失败（%ux%u）", shadow->size, shadow->size);
        return false;
    }

    // ---- 深度管线 ----
    {
        shader_desc_t shader_desc;
        memset(&shader_desc, 0, sizeof(shader_desc));
        shader_desc.vertex_source = k_depth_vertex;
        shader_desc.fragment_source = k_depth_fragment;
        shader_desc.debug_name = "builtin:depth_only";
        shadow->depth_shader = renderer_create_shader(renderer, &shader_desc);
        if (!shader_is_valid(renderer, shadow->depth_shader)) {
            LOG_ERROR("深度 pass 着色器创建失败");
            shadow_map_shutdown(shadow, renderer);
            return false;
        }

        pipeline_desc_t desc = pipeline_desc_default();
        desc.shader = shadow->depth_shader;
        desc.vertex_layout = vertex_layout_mesh_vertex();
        desc.blend = blend_state_opaque();
        // 深度 pass：测试开、写入开、LESS
        desc.depth = depth_state_default();
        // 只剔除背面。剔除正面（把背面渲染成阴影投射面）能更好地压制痤疮，
        // 但它要求网格是封闭的 —— 我们加载的 OBJ 里有很多单面片，会直接漏光。
        desc.raster = raster_state_default();
        // 关键：打开深度偏移，压制阴影痤疮
        desc.raster.depth_bias_enabled = true;
        desc.raster.depth_bias_constant = shadow->depth_bias;
        desc.raster.depth_bias_slope = shadow->slope_bias;
        desc.multisample.samples = 1;
        desc.debug_name = "shadow_depth";

        shadow->depth_pipeline = renderer_create_pipeline(renderer, &desc);
        if (!renderer_pipeline_is_valid(renderer, shadow->depth_pipeline)) {
            LOG_ERROR("深度 pass 管线创建失败");
            shadow_map_shutdown(shadow, renderer);
            return false;
        }
    }

    shadow->enabled = true;

    LOG_INFO("阴影贴图就绪: %ux%u，偏移 constant=%.1f slope=%.1f normal=%.3f", shadow->size,
             shadow->size, (f64)shadow->depth_bias, (f64)shadow->slope_bias,
             (f64)shadow->normal_bias);
    return true;
}

void shadow_map_shutdown(shadow_map_t *shadow, renderer_t *renderer)
{
    if (shadow == NULL || renderer == NULL) {
        return;
    }

    if (renderer_pipeline_is_valid(renderer, shadow->depth_pipeline)) {
        renderer_destroy_pipeline(renderer, shadow->depth_pipeline);
    }
    if (shader_is_valid(renderer, shadow->depth_shader)) {
        renderer_destroy_shader(renderer, shadow->depth_shader);
    }
    if (render_target_is_valid(renderer, shadow->target)) {
        render_target_destroy(renderer, shadow->target);
    }

    memset(shadow, 0, sizeof(*shadow));
}

void shadow_map_fit_directional(shadow_map_t *shadow, const light_t *light, vec3_t scene_center,
                                f32 scene_radius)
{
    ASSERT(shadow != NULL);

    if (light == NULL || scene_radius <= 0.0f) {
        shadow->light_view_projection = mat4_identity();
        shadow->enabled = false;
        return;
    }

    // light->direction 是"光线传播方向"（指向被照物体），
    // 光源在场景中心的反方向。
    const vec3_t to_light = vec3_negate(vec3_normalize(light->direction));

    // 光源退到场景球之外，保证整个球都在视锥里
    const f32 distance = scene_radius * 2.0f;
    const vec3_t eye = vec3_add(scene_center, vec3_scale(to_light, distance));

    // up 不能与视线平行（平行时会退化，mat4_look_at 内部虽然兜底了，
    // 但阴影矩阵抖动会很明显）。太阳接近天顶时换一个 up。
    vec3_t up = vec3_make(0.0f, 1.0f, 0.0f);
    if (fabsf(vec3_dot(to_light, up)) > 0.99f) {
        up = vec3_make(0.0f, 0.0f, 1.0f);
    }

    const mat4_t view = mat4_look_at(eye, scene_center, up);

    // 正交视锥：正方形（宽高比 1）刚好包住半径为 scene_radius 的球。
    // 乘 1.05 留一点余量，避免球边缘被裁掉。
    const f32 extent = scene_radius * 1.05f;
    // 深度范围要覆盖 [distance - radius, distance + radius]
    const f32 znear = distance - scene_radius * 1.1f;
    const f32 zfar = distance + scene_radius * 1.1f;

    const mat4_t projection = mat4_ortho(-extent, extent, -extent, extent,
                                         (znear > 0.01f) ? znear : 0.01f, zfar);

    shadow->light_view_projection = mat4_mul(projection, view);
    shadow->enabled = (shadow->strength > 0.0f);
}

void shadow_map_render(shadow_map_t *shadow, renderer_t *renderer)
{
    ASSERT(shadow != NULL);
    ASSERT(renderer != NULL);

    if (!shadow->enabled || !render_target_is_valid(renderer, shadow->target)) {
        return;
    }

    // 深度 pass 要把 light_view_projection 传给着色器，
    // 但那个矩阵是在帧 uniform 里的 —— 先写进去，再画。
    renderer_set_light_view_projection(renderer, shadow->light_view_projection);
    renderer_flush_frame_uniforms(renderer);

    renderer_render_depth_pass(renderer, shadow->target, shadow->depth_pipeline);
}

void shadow_map_apply(shadow_map_t *shadow, renderer_t *renderer)
{
    ASSERT(shadow != NULL);
    ASSERT(renderer != NULL);

    if (!shadow->enabled) {
        renderer_set_shadow_params(renderer, 0.0f, shadow->size);
        renderer_set_shadow_texture(renderer, TEXTURE_HANDLE_INVALID);
        return;
    }

    renderer_set_light_view_projection(renderer, shadow->light_view_projection);
    renderer_set_shadow_params(renderer, shadow->strength, shadow->size);
    // 法线偏移要同步给渲染器，否则着色器里用的还是默认值 ——
    // shadow_map_set_bias 改了 normal_bias 却看不到任何变化，就是这个原因。
    renderer_set_shadow_normal_bias(renderer, shadow->normal_bias);
    renderer_set_shadow_texture(renderer, shadow_map_texture(shadow, renderer));
}

texture_handle_t shadow_map_texture(const shadow_map_t *shadow, renderer_t *renderer)
{
    ASSERT(shadow != NULL);
    ASSERT(renderer != NULL);

    if (!render_target_is_valid(renderer, shadow->target)) {
        return TEXTURE_HANDLE_INVALID;
    }
    return render_target_texture(renderer, shadow->target);
}

void shadow_map_set_strength(shadow_map_t *shadow, f32 strength)
{
    ASSERT(shadow != NULL);
    shadow->strength = math_saturate(strength);
    shadow->enabled = (shadow->strength > 0.0f);
}

void shadow_map_set_bias(shadow_map_t *shadow, f32 depth_bias, f32 slope_bias, f32 normal_bias)
{
    ASSERT(shadow != NULL);
    shadow->depth_bias = depth_bias;
    shadow->slope_bias = slope_bias;
    shadow->normal_bias = normal_bias;
    // 偏移是创建管线时的参数，改了要重建 —— 这里只记录，
    // 调用方需要重建时自己调 shadow_map_init。
    LOG_DEBUG("阴影偏移已更新（需要重建管线才生效）: constant=%.1f slope=%.1f normal=%.3f",
              (f64)depth_bias, (f64)slope_bias, (f64)normal_bias);
}
