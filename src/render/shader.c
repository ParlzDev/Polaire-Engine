// =============================================================================
//  src/render/shader.c —— 内置着色器
//
//  这些源码**不含** #version 与 uniform 块声明 —— 后端会在编译前自动前置
//  （版本号取上下文实际版本，uniform 块取自 uniforms.h 的单一来源）。
//  详见 shader.h 文件头。
//
//  属性 location 直接取 vertex_semantic_t 的枚举值：
//      POSITION  = 0
//      NORMAL    = 1
//      TEXCOORD0 = 4
// =============================================================================
#include "engine/render/shader.h"

#include <string.h>

#include "engine/render/renderer.h"

// =============================================================================
//  无光照 + 贴图
//
//  用途：2D 精灵、UI、不需要光照的 3D 物体（例如已烘焙好的场景）。
// =============================================================================
static const char *const k_unlit_vertex =
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 4) in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    v_texcoord = a_texcoord;\n"
    "    gl_Position = u_view_projection * u_model * vec4(a_position, 1.0);\n"
    "}\n";

static const char *const k_unlit_fragment =
    "in vec2 v_texcoord;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "    vec4 base = texture(u_albedo, v_texcoord) * u_tint;\n"
    // 丢弃几乎全透明的像素：让它们不写深度、不参与混合，
    // 半透明物体的排序错误会因此少很多（M3 的精灵批渲染会重度依赖这一点）。
    "    if (base.a < 0.001) { discard; }\n"
    "    frag_color = base;\n"
    "}\n";

// =============================================================================
//  单平行光 + 环境光的 Lambert
//
//  M4 会换成 PBR（含阴影、法线贴图、金属度粗糙度）。
//  M2 的目标是"证明光照数据能正确地从 uniform 块走到着色器"，
//  所以刻意保持最简单。
// =============================================================================
static const char *const k_lit_vertex =
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 4) in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "out vec3 v_world_normal;\n"
    "void main() {\n"
    "    vec4 world = u_model * vec4(a_position, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    // 用 normal_matrix（= (M^-1)^T 的左上 3x3）变换法线。
    // 直接乘 mat3(u_model) 在非均匀缩放下会让光照方向错。
    "    v_world_normal = mat3(u_normal_matrix) * a_normal;\n"
    "    gl_Position = u_view_projection * world;\n"
    "}\n";

static const char *const k_lit_fragment =
    "in vec2 v_texcoord;\n"
    "in vec3 v_world_normal;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "    vec4 base = texture(u_albedo, v_texcoord) * u_tint;\n"
    "    vec3 n = normalize(v_world_normal);\n"
    "    vec3 l = normalize(u_light_direction.xyz);\n"
    "    float ndotl = max(dot(n, l), 0.0);\n"
    // u_light_direction.w 兼作"是否启用平行光"的开关（0 = 关闭）
    "    vec3 diffuse = u_light_color.rgb * u_light_color.a * ndotl * u_light_direction.w;\n"
    "    vec3 ambient = u_ambient.rgb * u_ambient.a;\n"
    "    frag_color = vec4(base.rgb * (ambient + diffuse), base.a);\n"
    "}\n";


// =============================================================================
//  PBR（金属度-粗糙度）+ 多光源 + 阴影 + 雾
//
//  公式与 src/render/pbr.c 里的 CPU 参考实现**必须一致** ——
//  两边不一致的话，单元测试就变成了"验证一个和渲染无关的实现"。
//  改动任何一侧时另一侧要同步改。
//
//  ---- 深度比较为什么可以直接用 [0,1] ----
//
//  M0 把裁剪空间深度定成了 [0,1]（而不是 OpenGL 默认的 [-1,1]）。
//  所以投影后 proj.z 已经在 [0,1] 内，与深度贴图里存的值可以直接比较，
//  不需要 `* 0.5 + 0.5` 那一步 —— 少一步就少一处会写错的地方。
//  只有 xy 需要从 [-1,1] 映射到 [0,1]。
// =============================================================================
static const char *const k_pbr_vertex =
    "layout(location = 0) in vec3 a_position;\n"
    "layout(location = 1) in vec3 a_normal;\n"
    "layout(location = 4) in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "out vec3 v_world_position;\n"
    "out vec3 v_world_normal;\n"
    "void main() {\n"
    "    vec4 world = u_model * vec4(a_position, 1.0);\n"
    "    v_world_position = world.xyz;\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_world_normal = mat3(u_normal_matrix) * a_normal;\n"
    "    gl_Position = u_view_projection * world;\n"
    "}\n";

static const char *const k_pbr_fragment =
    "in vec2 v_texcoord;\n"
    "in vec3 v_world_position;\n"
    "in vec3 v_world_normal;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "\n"
    "const float PI = 3.14159265359;\n"
    "\n"
    "float distribution_ggx(float ndoth, float roughness) {\n"
    "    float a = roughness * roughness;\n"
    "    float a2 = a * a;\n"
    "    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;\n"
    "    return a2 / max(PI * d * d, 1e-8);\n"
    "}\n"
    "\n"
    "float geometry_schlick_ggx(float ndotv, float roughness) {\n"
    "    float r = roughness + 1.0;\n"
    "    float k = (r * r) / 8.0;\n"
    "    return ndotv / (ndotv * (1.0 - k) + k);\n"
    "}\n"
    "\n"
    "float geometry_smith(float ndotv, float ndotl, float roughness) {\n"
    "    return geometry_schlick_ggx(ndotv, roughness) * geometry_schlick_ggx(ndotl, roughness);\n"
    "}\n"
    "\n"
    "vec3 fresnel_schlick(float vdoth, vec3 f0) {\n"
    "    float f = pow(1.0 - vdoth, 5.0);\n"
    "    return f0 + (1.0 - f0) * f;\n"
    "}\n"
    "\n"
    "float distance_attenuation(float dist, float range) {\n"
    "    if (range <= 0.0) { return 1.0; }\n"
    "    if (dist >= range) { return 0.0; }\n"
    "    float r = dist / range;\n"
    "    float w = clamp(1.0 - r * r * r * r, 0.0, 1.0);\n"
    "    return w * w / max(dist * dist, 1e-4);\n"
    "}\n"
    "\n"
    "float spot_attenuation(vec3 to_light, vec3 spot_forward, float cos_inner, float cos_outer) {\n"
    // to_light 是"从表面指向光源"，spot_forward 是"从光源指向外"（聚光锥的轴线）。
    // 表面正好在锥轴上时 to_light == -spot_forward，所以必须取反后点积，
    // 否则整个锥体是反的 —— 正前方全黑、背后反而被照亮。
    "    float cd = clamp(dot(normalize(-to_light), normalize(spot_forward)), -1.0, 1.0);\n"
    "    float t = clamp((cd - cos_outer) / max(cos_inner - cos_outer, 1e-6), 0.0, 1.0);\n"
    "    return t * t;\n"
    "}\n"
    "\n"
    "float shadow_factor(vec3 world_position, vec3 normal, float ndotl) {\n"
    "    if (u_light_params.y <= 0.0) { return 1.0; }\n"
    "\n"
    // 沿法线偏移：斜面上的纹素覆盖范围大，不偏移会出现条纹状自阴影。
    // 偏移量要小 —— 太大会让阴影与物体分离（彼得潘现象）。
    "    vec3 biased = world_position + normal * u_shadow_params.x;\n"
    "    vec4 light_clip = u_light_view_projection * vec4(biased, 1.0);\n"
    // w <= 0 表示这个点在光源背后（透视除法会翻号，得到看起来"合法"的坐标）。
    // 不判这一条的话，光源背后的物体会被错误地判定为处于阴影中。
    "    if (light_clip.w <= 0.0) { return 1.0; }\n"
    "    vec3 proj = light_clip.xyz / light_clip.w;\n"
    "    proj.xy = proj.xy * 0.5 + 0.5;\n"
    // proj.z 已经在 [0,1] 里（深度约定是 [0,1]），不需要再映射
    "    if (proj.z > 1.0 || proj.z < 0.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 ||\n"
    "        proj.y > 1.0) {\n"
    "        return 1.0;\n"
    "    }\n"
    "\n"
    // 与光线夹角越大，深度误差越大，偏移也要越大
    "    float bias = max(0.0018 * (1.0 - ndotl), 0.0006);\n"
    "    float texel = 1.0 / max(u_light_params.z, 1.0);\n"
    "\n"
    // 3x3 PCF：单点采样会让阴影边缘呈明显锯齿
    "    float sum = 0.0;\n"
    "    for (int x = -1; x <= 1; ++x) {\n"
    "        for (int y = -1; y <= 1; ++y) {\n"
    "            float depth = texture(u_shadow_map, proj.xy + vec2(float(x), float(y)) * texel).r;\n"
    "            sum += (proj.z - bias > depth) ? 0.0 : 1.0;\n"
    "        }\n"
    "    }\n"
    "    return sum / 9.0;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec4 base = texture(u_albedo, v_texcoord) * u_tint;\n"
    "    if (base.a < 0.02) { discard; }\n"
    "\n"
    "    vec3 albedo = base.rgb;\n"
    "    float metallic = clamp(u_pbr_params.x, 0.0, 1.0);\n"
    "    float roughness = clamp(u_pbr_params.y, 0.045, 1.0);\n"
    "    float reflectance = clamp(u_pbr_params.z, 0.0, 1.0);\n"
    "    float ao = clamp(u_pbr_params.w, 0.0, 1.0);\n"
    "\n"
    "    vec3 n = normalize(v_world_normal);\n"
    "    vec3 v = normalize(u_camera_position.xyz - v_world_position);\n"
    "    float ndotv = max(dot(n, v), 1e-4);\n"
    "\n"
    "    vec3 f0 = mix(vec3(reflectance), albedo, metallic);\n"
    "\n"
    "    vec3 lo = vec3(0.0);\n"
    "    int light_count = int(u_light_params.x + 0.5);\n"
    "\n"
    "    for (int i = 0; i < MAX_LIGHTS; ++i) {\n"
    "        if (i >= light_count) { break; }\n"
    "\n"
    "        GpuLight light = u_lights[i];\n"
    "        int type = int(light.position_type.w + 0.5);\n"
    "\n"
    "        vec3 to_light;\n"
    "        vec3 radiance;\n"
    "\n"
    "        if (type == 0) {\n"
    "            to_light = light.direction_range.xyz;\n"
    "            radiance = light.color_intensity.rgb * light.color_intensity.a;\n"
    "        } else {\n"
    "            vec3 delta = light.position_type.xyz - v_world_position;\n"
    "            float dist = length(delta);\n"
    "            if (dist < 1e-5) { continue; }\n"
    "            to_light = delta / dist;\n"
    "            float atten = distance_attenuation(dist, light.direction_range.w);\n"
    "            if (atten <= 0.0) { continue; }\n"
    "            radiance = light.color_intensity.rgb * light.color_intensity.a * atten;\n"
    "            if (type == 2) {\n"
    // 聚光：direction_range.xyz 存的是"从表面指向光源"，聚光朝向要取反
    "                radiance *= spot_attenuation(to_light, -light.direction_range.xyz,\n"
    "                                             light.spot_shadow.x, light.spot_shadow.y);\n"
    "            }\n"
    "        }\n"
    "\n"
    "        float ndotl = max(dot(n, to_light), 0.0);\n"
    "        if (ndotl <= 0.0) { continue; }\n"
    "\n"
    "        vec3 h = normalize(v + to_light);\n"
    "        float ndoth = max(dot(n, h), 0.0);\n"
    "        float vdoth = max(dot(v, h), 0.0);\n"
    "\n"
    "        vec3 f = fresnel_schlick(vdoth, f0);\n"
    "        float d = distribution_ggx(ndoth, roughness);\n"
    "        float g = geometry_smith(ndotv, ndotl, roughness);\n"
    "        vec3 specular = f * (d * g / max(4.0 * ndotv * ndotl, 1e-4));\n"
    "        vec3 kd = (1.0 - f) * (1.0 - metallic);\n"
    "        vec3 diffuse = kd * albedo / PI;\n"
    "\n"
    // 只有第一盏灯投射阴影（M4 只有一张阴影贴图）
    "        float light_shadow = 1.0;\n"
    "        if (i == 0 && light.spot_shadow.z > 0.5) {\n"
    "            float raw = shadow_factor(v_world_position, n, ndotl);\n"
    "            light_shadow = mix(1.0, raw, u_light_params.y);\n"
    "        }\n"
    "\n"
    "        lo += (diffuse + specular) * radiance * ndotl * light_shadow;\n"
    "    }\n"
    "\n"
    // 环境光：IBL 的粗略近似（真 IBL 需要预过滤环境贴图，M6 之后）
    "    vec3 ambient_f = fresnel_schlick(ndotv, f0);\n"
    "    vec3 ambient_kd = (1.0 - ambient_f) * (1.0 - metallic);\n"
    "    vec3 ambient = (ambient_kd * albedo * ao + ambient_f * (1.0 - roughness) * ao) *\n"
    "                   u_ambient.rgb * u_ambient.a;\n"
    "\n"
    "    vec3 color = lo + ambient + u_emissive.rgb;\n"
    "\n"
    "    if (u_fog_params.x > 0.5) {\n"
    "        float dist = length(u_camera_position.xyz - v_world_position);\n"
    "        float t = clamp((dist - u_fog_params.y) / max(u_fog_params.z - u_fog_params.y, 0.01),\n"
    "                        0.0, 1.0);\n"
    "        float fog = 1.0 - exp(-pow(t * u_fog_params.w, 2.0));\n"
    "        color = mix(color, u_fog_color.rgb, fog);\n"
    "    }\n"
    "\n"
    "    frag_color = vec4(albedo, 1.0); // TEMP PROBE\n"
    "}\n";

// =============================================================================
//  纯色（不使用纹理）
//
//  用途：调试线框、碰撞体可视化、2D 调试绘制。
// =============================================================================
static const char *const k_solid_vertex =
    "layout(location = 0) in vec3 a_position;\n"
    "void main() {\n"
    "    gl_Position = u_view_projection * u_model * vec4(a_position, 1.0);\n"
    "}\n";

static const char *const k_solid_fragment =
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = u_tint;\n"
    "}\n";

// =============================================================================
//  公开接口
// =============================================================================
shader_desc_t shader_builtin_unlit_textured(void)
{
    shader_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_source = k_unlit_vertex;
    desc.fragment_source = k_unlit_fragment;
    desc.debug_name = "builtin:unlit_textured";
    return desc;
}

shader_desc_t shader_builtin_lit_textured(void)
{
    shader_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_source = k_lit_vertex;
    desc.fragment_source = k_lit_fragment;
    desc.debug_name = "builtin:lit_textured";
    return desc;
}

shader_desc_t shader_builtin_pbr_textured(void)
{
    shader_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_source = k_pbr_vertex;
    desc.fragment_source = k_pbr_fragment;
    desc.debug_name = "builtin:pbr_textured";
    return desc;
}

shader_desc_t shader_builtin_solid_color(void)
{
    shader_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_source = k_solid_vertex;
    desc.fragment_source = k_solid_fragment;
    desc.debug_name = "builtin:solid_color";
    return desc;
}

// =============================================================================
//  查询与销毁
//
//  这几个只是 renderer 对应函数的转发。保留它们是为了让"着色器相关的东西"
//  有一个统一的入口，调用方不需要为了销毁一个着色器去 include renderer.h。
// =============================================================================
shader_handle_t shader_create(renderer_t *renderer, const shader_desc_t *desc)
{
    return renderer_create_shader(renderer, desc);
}

void shader_destroy(renderer_t *renderer, shader_handle_t shader)
{
    renderer_destroy_shader(renderer, shader);
}

bool shader_is_valid(const renderer_t *renderer, shader_handle_t shader)
{
    return renderer_shader_is_valid(renderer, shader);
}

const char *shader_debug_name(const renderer_t *renderer, shader_handle_t shader)
{
    return renderer_shader_name(renderer, shader);
}
