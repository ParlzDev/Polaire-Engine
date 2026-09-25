// =============================================================================
//  engine/render/material.h —— 材质
//
//  材质 = "画一个网格需要哪些东西"：
//      用哪个管线（着色器 + 全部渲染状态）
//      绑哪些纹理（按槽位）
//      颜色调制
//
//  它是**值类型**而不是句柄。原因：
//  材质在每帧会被大量复制到渲染项里，句柄化只会多一层间接；
//  而且材质的组合往往在运行时临时拼（例如"同一个管线换一张贴图"），
//  做成资源反而更笨重。
//
//  真正需要资源化的（GPU 对象、需要跨帧复用）是 pipeline 与 texture，
//  它们本来就是句柄。
// =============================================================================
#ifndef ENGINE_RENDER_MATERIAL_H
#define ENGINE_RENDER_MATERIAL_H

#include "engine/render/render_types.h"
#include "engine/render/uniforms.h"

typedef struct material_t {
    pipeline_handle_t pipeline;

    texture_handle_t textures[RENDER_MAX_TEXTURE_SLOTS];
    sampler_desc_t   samplers[RENDER_MAX_TEXTURE_SLOTS];
    u8               texture_count;

    vec4_t tint; // 颜色调制（rgb + 不透明度）

    // ---- PBR 参数（M4）----
    //
    // 直接对应 object_uniforms_t 里的两个 vec4，而不是另造一套类型：
    // 这里多一层转换的话，"改了材质但画面上没变化"就又成了一个要排查的问题。
    //
    // 为什么放在材质里而不是让调用方去改渲染项：
    // 粗糙度/金属度**本来就是材质属性**。让调用方 push 完之后回填 uniform，
    // 等于把"材质"这个概念拆成两半，用起来别扭且容易漏。
    vec4_t pbr_params; // x = metallic，y = roughness，z = reflectance，w = ambient_occlusion
    vec4_t emissive;   // rgb = 自发光，a 保留
} material_t;

material_t material_default(void);

// 只指定管线，其余用默认（无纹理、白色）
material_t material_with_pipeline(pipeline_handle_t pipeline);

// 设置某个槽位的纹理。slot 取 RENDER_TEXTURE_SLOT_*。
// 会自动维护 texture_count（取所有已设置槽位的最大值 + 1）。
void material_set_texture(material_t *material, u32 slot, texture_handle_t texture,
                          const sampler_desc_t *sampler);

// 设置 PBR 参数。参数会原样写入（**不钳制**）——
// 钳制是 pbr_material_sanitize 的职责，在这里悄悄改数值会掩盖配置错误。
void material_set_pbr(material_t *material, f32 metallic, f32 roughness, f32 reflectance,
                      f32 ambient_occlusion);

void material_set_emissive(material_t *material, vec3_t emissive);

#endif // ENGINE_RENDER_MATERIAL_H
