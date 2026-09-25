// =============================================================================
//  src/render/material.c —— 材质
// =============================================================================
#include "engine/render/material.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/render/pbr.h" // PBR_DIELECTRIC_F0：默认材质与 CPU 参考实现共用同一个常量

material_t material_default(void)
{
    material_t material;
    memset(&material, 0, sizeof(material));

    material.pipeline = PIPELINE_HANDLE_INVALID;
    material.tint = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);

    // PBR 默认：非金属、中等粗糙度、F0 = 0.04、无自发光。
    // 和 pbr_material_default() 的取值保持一致 —— 两处不一致的话，
    // "CPU 参考实现算出来的"和"GPU 画出来的"会不一样。
    material.pbr_params =
        vec4_make(0.0f, 0.5f, PBR_DIELECTRIC_F0, 1.0f);
    material.emissive = vec4_zero();

    for (u32 i = 0; i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
        material.textures[i] = TEXTURE_HANDLE_INVALID;
        material.samplers[i] = sampler_desc_default();
    }

    return material;
}

material_t material_with_pipeline(pipeline_handle_t pipeline)
{
    material_t material = material_default();
    material.pipeline = pipeline;
    return material;
}

void material_set_pbr(material_t *material, f32 metallic, f32 roughness, f32 reflectance,
                      f32 ambient_occlusion)
{
    ASSERT(material != NULL);
    material->pbr_params = vec4_make(metallic, roughness, reflectance, ambient_occlusion);
}

void material_set_emissive(material_t *material, vec3_t emissive)
{
    ASSERT(material != NULL);
    material->emissive = vec4_make(emissive.x, emissive.y, emissive.z, 0.0f);
}

void material_set_texture(material_t *material, u32 slot, texture_handle_t texture,
                          const sampler_desc_t *sampler)
{
    ASSERT(material != NULL);

    if (slot >= RENDER_MAX_TEXTURE_SLOTS) {
        return;
    }

    material->textures[slot] = texture;
    if (sampler != NULL) {
        material->samplers[slot] = *sampler;
    }

    // texture_count 表示"要绑定多少个连续槽位"，取所有已设置槽位的最大值 + 1。
    // 用"最大槽位 + 1"而不是"已设置的数量"：绑定循环要从 0 连续走到 count-1，
    // 中间的空槽会绑上默认纹理。
    if (slot + 1u > material->texture_count) {
        material->texture_count = (u8)(slot + 1u);
    }
}
