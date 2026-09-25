// =============================================================================
//  src/render/uniforms.c —— uniform 块预设
// =============================================================================
#include "engine/render/uniforms.h"

#include <string.h>

#include "engine/core/assert.h"

frame_uniforms_t frame_uniforms_default(void)
{
    frame_uniforms_t uniforms;
    memset(&uniforms, 0, sizeof(uniforms));

    uniforms.view = mat4_identity();
    uniforms.projection = mat4_identity();
    uniforms.view_projection = mat4_identity();
    uniforms.inv_view_projection = mat4_identity();
    uniforms.camera_position = vec4_make(0.0f, 0.0f, 0.0f, 1.0f);

    // 默认给一点环境光，避免"忘了设光照"时画面全黑 —— 全黑最难排查，
    // 因为分不清是"没光"还是"矩阵错了"还是"没画出来"。
    uniforms.ambient = vec4_make(1.0f, 1.0f, 1.0f, 0.25f);
    uniforms.light_direction = vec4_make(0.0f, 0.0f, 0.0f, 0.0f); // w = 0 表示未启用
    uniforms.light_color = vec4_make(1.0f, 1.0f, 1.0f, 0.0f);

    // x = 光源数（0 表示用旧的单光源路径，兼容 M2/M3 的着色器）
    uniforms.light_params = vec4_make(0.0f, 0.0f, 1024.0f, 1.0f);
    uniforms.light_view_projection = mat4_identity();

    // x = 阴影法线偏移，y = PCF 半径（texel）
    // 0.035 是"1 单位 = 1 米"尺度下 1024 贴图的经验值：
    // 再小会在斜面上出现阴影痤疮，再大会让阴影明显脱离物体。
    uniforms.shadow_params = vec4_make(0.035f, 1.0f, 0.0f, 0.0f);

    // 雾默认关闭
    uniforms.fog_params = vec4_make(0.0f, 10.0f, 200.0f, 1.0f);
    uniforms.fog_color = vec4_make(0.6f, 0.7f, 0.85f, 1.0f);

    // 天空盒默认关闭，色调映射默认 Reinhard
    uniforms.sky_params = vec4_make(0.0f, 1.0f, 1.0f, 0.0f);
    uniforms.sky_horizon = vec4_make(0.62f, 0.72f, 0.86f, 1.0f);
    uniforms.sky_zenith = vec4_make(0.22f, 0.38f, 0.68f, 1.0f);
    uniforms.sky_ground = vec4_make(0.16f, 0.15f, 0.14f, 1.0f);

    return uniforms;
}

object_uniforms_t object_uniforms_default(void)
{
    object_uniforms_t uniforms;
    memset(&uniforms, 0, sizeof(uniforms));

    uniforms.model = mat4_identity();
    uniforms.normal_matrix = mat4_identity();
    uniforms.tint = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    // x = metallic, y = roughness, z = reflectance, w = ambient_occlusion
    uniforms.pbr_params = vec4_make(0.0f, 0.5f, 0.04f, 1.0f);
    uniforms.emissive = vec4_zero();
    return uniforms;
}

object_uniforms_t object_uniforms_from_model(mat4_t model)
{
    object_uniforms_t uniforms = object_uniforms_default();
    uniforms.model = model;
    // 法线矩阵必须走 (M^-1)^T：直接用 M 变换法线在非均匀缩放下会让光照方向错。
    // 这一步在 CPU 上算一次，比在顶点着色器里每个顶点算一次便宜得多。
    uniforms.normal_matrix = mat4_transpose(mat4_inverse(model));
    return uniforms;
}

void object_uniforms_set_pbr(object_uniforms_t *uniforms, f32 metallic, f32 roughness,
                             f32 reflectance, f32 ambient_occlusion)
{
    ASSERT(uniforms != NULL);

    // 这里不做钳制 —— 钳制是 pbr_material_sanitize 的职责。
    // 在这里悄悄改数值会让"材质参数错了"这件事被掩盖掉。
    uniforms->pbr_params = vec4_make(metallic, roughness, reflectance, ambient_occlusion);
}
