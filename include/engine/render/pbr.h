// =============================================================================
//  engine/render/pbr.h —— PBR（金属度-粗糙度工作流）
//
//  这里提供的是 **CPU 侧的参考实现**。它的主要用途不是渲染（渲染在 GPU 的
//  着色器里），而是：
//    * 单元测试：用同一套公式在 CPU 上算一遍，验证 GPU 的结果在合理范围内；
//    * 参数调优：想知道"粗糙度 0.3 的 GGX 分布长什么样"时可以直接算；
//    * 材质校验：检查 metallic/roughness 是否在合法区间。
//
//  ---- 为什么用"金属度-粗糙度"而不是"高光-光泽度" ----
//
//  金属度工作流只有 2 个参数（metallic + roughness），加上一个 baseColor，
//  就能表达从塑料到金属的整个范围，且能量守恒是天然的：
//    * 非金属（metallic = 0）：F0 = 0.04，漫反射保留 (1 - F)
//    * 金属（metallic = 1）：F0 = baseColor，没有漫反射
//  而高光-光泽度工作流有 3 个独立参数，很容易调出"反射能量 > 入射能量"
//  的材质，在光照变化时会明显穿帮。
//
//  ---- 关于 sRGB ----
//
//  **baseColor 必须是线性空间的**。贴图里的 baseColor 是 sRGB 编码的，
//  硬件采样时会自动解码（因为纹理格式是 _SRGB）。但如果是代码里直接给的
//  颜色常量（例如 vec3(0.5, 0.5, 0.5)），那就是线性值 ——
//  别把它当成"50% 灰"的 sRGB 值，那需要 0.2158。
// =============================================================================
#ifndef ENGINE_RENDER_PBR_H
#define ENGINE_RENDER_PBR_H

#include "engine/core/types.h"
#include "engine/core/vec.h"

// 非金属的默认反射率。绝大多数电介质（塑料、木头、皮肤）的 F0 都在 0.02-0.05，
// 0.04 是业界通用近似。
#define PBR_DIELECTRIC_F0 0.04f

// 粗糙度的合法下界。0 会让 GGX 分布退化成狄拉克函数（除零），
// 所以实际使用时钳制到 0.045 左右。
#define PBR_MIN_ROUGHNESS 0.045f

// ---------------------------------------------------------------- 材质
typedef struct pbr_material_t {
    vec3_t base_color; // 线性空间
    f32    metallic;   // 0 = 非金属，1 = 金属
    f32    roughness;  // 0.045 .. 1
    f32    reflectance; // 非金属的 F0（默认 0.04）
    f32    ambient_occlusion;
    f32    alpha;
    vec3_t emissive;
} pbr_material_t;

pbr_material_t pbr_material_default(void);

// 把材质参数钳制到合法区间。返回 false 表示有参数被改动了
// （材质加载后应该调用它，并把"被修正"记进日志 —— 静默钳制会掩盖美术的配置错误）。
bool pbr_material_sanitize(pbr_material_t *material);

// 由 base_color 与 metallic 算出 F0（垂直入射反射率）。
vec3_t pbr_f0(const pbr_material_t *material);

// ---------------------------------------------------------------- BRDF 项
//
// Cook-Torrance 的三个部分。参数都是"点积"而不是向量 ——
// 这样调用方可以复用已经算过的点积，也便于测试边界条件。
//
// 法线分布函数（GGX / Trowbridge-Reitz）
f32 pbr_distribution_ggx(f32 ndoth, f32 roughness);

// 几何遮蔽（Smith 方法 + Schlick-GGX 近似）
f32 pbr_geometry_schlick_ggx(f32 ndotv, f32 roughness);
f32 pbr_geometry_smith(f32 ndotv, f32 ndotl, f32 roughness);

// 菲涅尔（Schlick 近似）
vec3_t pbr_fresnel_schlick(f32 vdoth, vec3_t f0);
// 带粗糙度修正的版本（避免掠射角出现白边）
vec3_t pbr_fresnel_schlick_roughness(f32 vdoth, vec3_t f0, f32 roughness);

// ---------------------------------------------------------------- 光照
//
// 单盏灯的直接光照贡献。light_direction 是"从表面指向光源"的方向（已归一化），
// radiance 是光源颜色 × 强度（已含距离衰减）。
vec3_t pbr_direct_light(const pbr_material_t *material, vec3_t normal, vec3_t view_direction,
                        vec3_t light_direction, vec3_t radiance);

// 点光的距离衰减（平方反比 + 平滑截断到 range 之外为 0）
f32 pbr_distance_attenuation(f32 distance, f32 range);

// 聚光的角度衰减（内角内为 1，外角外为 0，中间平滑过渡）。
//
// 方向约定（写反的话锥体是反的：正前方全黑、背后被照亮）：
//   to_light     —— "从表面指向光源"，与 pbr_direct_light 的 light_direction 同一个方向
//   spot_forward —— 聚光锥的轴线，"从光源指向外"
f32 pbr_spot_attenuation(vec3_t to_light, vec3_t spot_forward, f32 cos_inner, f32 cos_outer);

// 环境光（用粗糙度近似 IBL 的粗略版本）。
// 不是物理正确的 IBL，但比"环境光直接加到漫反射上"好看得多 ——
// 金属在粗糙时不会出现塑料感。
vec3_t pbr_ambient(const pbr_material_t *material, vec3_t normal, vec3_t view_direction,
                   vec3_t ambient_radiance);

// ---------------------------------------------------------------- 色调映射
//
// Reinhard 是"最便宜且不会过曝"的选择。ACES 更好看但需要一张查找表或
// 一组拟合多项式 —— M4 先用 Reinhard，M6 的 2.5D 再做对比。
vec3_t pbr_tonemap_reinhard(vec3_t color);
vec3_t pbr_tonemap_aces(vec3_t color);

// 线性 -> sRGB 编码（用于 CPU 侧验证与最终的 present pass）
vec3_t pbr_linear_to_srgb(vec3_t linear);
// sRGB -> 线性
vec3_t pbr_srgb_to_linear(vec3_t srgb);

#endif // ENGINE_RENDER_PBR_H
