// =============================================================================
//  src/render/pbr.c —— PBR 的 CPU 参考实现
//
//  这里的公式与 shader 里的**必须一致**。两边不一致的话，
//  单元测试就变成了"验证一个和渲染无关的实现"，毫无意义。
//  改动任何一侧时，另一侧要同步改，并跑 tests/test_pbr.c。
// =============================================================================
#include "engine/render/pbr.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/math.h"

pbr_material_t pbr_material_default(void)
{
    pbr_material_t material;
    memset(&material, 0, sizeof(material));
    material.base_color = vec3_make(0.8f, 0.8f, 0.8f);
    material.metallic = 0.0f;
    material.roughness = 0.5f;
    material.reflectance = PBR_DIELECTRIC_F0;
    material.ambient_occlusion = 1.0f;
    material.alpha = 1.0f;
    material.emissive = vec3_zero();
    return material;
}

bool pbr_material_sanitize(pbr_material_t *material)
{
    ASSERT(material != NULL);

    bool changed = false;

    // 粗糙度不能是 0：GGX 在 roughness = 0 时退化成狄拉克函数，
    // 分母里会出现 0 * 0，结果是 NaN。整个物体变成黑色或闪烁的白点。
    if (material->roughness < PBR_MIN_ROUGHNESS) {
        material->roughness = PBR_MIN_ROUGHNESS;
        changed = true;
    }
    if (material->roughness > 1.0f) {
        material->roughness = 1.0f;
        changed = true;
    }

    if (material->metallic < 0.0f) {
        material->metallic = 0.0f;
        changed = true;
    }
    if (material->metallic > 1.0f) {
        material->metallic = 1.0f;
        changed = true;
    }

    if (material->reflectance < 0.0f) {
        material->reflectance = 0.0f;
        changed = true;
    }
    if (material->reflectance > 1.0f) {
        material->reflectance = 1.0f;
        changed = true;
    }

    if (material->ambient_occlusion < 0.0f) {
        material->ambient_occlusion = 0.0f;
        changed = true;
    }
    if (material->ambient_occlusion > 1.0f) {
        material->ambient_occlusion = 1.0f;
        changed = true;
    }

    // base_color 允许超过 1（HDR 材质），但不能是负数
    if (material->base_color.x < 0.0f || material->base_color.y < 0.0f ||
        material->base_color.z < 0.0f) {
        material->base_color = vec3_max(material->base_color, vec3_zero());
        changed = true;
    }

    if (material->alpha < 0.0f) {
        material->alpha = 0.0f;
        changed = true;
    }
    if (material->alpha > 1.0f) {
        material->alpha = 1.0f;
        changed = true;
    }

    return changed;
}

vec3_t pbr_f0(const pbr_material_t *material)
{
    ASSERT(material != NULL);

    // 非金属的 F0 是一个很小的常量（0.04 左右）；
    // 金属的 F0 就是它自己的颜色（金子的 F0 是金色的）。
    // 用 lerp 一行表达两者的过渡，比 if/else 更接近实际材质的连续变化。
    return vec3_lerp(vec3_splat(material->reflectance), material->base_color, material->metallic);
}

// =============================================================================
//  BRDF 项
// =============================================================================
f32 pbr_distribution_ggx(f32 ndoth, f32 roughness)
{
    // a = roughness²：让粗糙度的变化在视觉上更接近线性
    const f32 a = roughness * roughness;
    const f32 a2 = a * a;

    const f32 d = ndoth * ndoth * (a2 - 1.0f) + 1.0f;
    const f32 denominator = F32_PI * d * d;

    // 分母为 0 只在 roughness = 0 且 ndoth = 1 时出现（狄拉克尖峰）。
    // 调用方应该已经用 pbr_material_sanitize 把 roughness 抬到 0.045 以上。
    if (denominator < 1.0e-8f) {
        return 0.0f;
    }
    return a2 / denominator;
}

f32 pbr_geometry_schlick_ggx(f32 ndotv, f32 roughness)
{
    // 直接光照用 k = (r+1)²/8；IBL 用 k = r²/2。
    // 用错的那个会让掠射角的边缘偏暗或偏亮。
    const f32 r = roughness + 1.0f;
    const f32 k = (r * r) / 8.0f;

    return ndotv / (ndotv * (1.0f - k) + k);
}

f32 pbr_geometry_smith(f32 ndotv, f32 ndotl, f32 roughness)
{
    const f32 ggx_v = pbr_geometry_schlick_ggx(ndotv, roughness);
    const f32 ggx_l = pbr_geometry_schlick_ggx(ndotl, roughness);
    return ggx_v * ggx_l;
}

vec3_t pbr_fresnel_schlick(f32 vdoth, vec3_t f0)
{
    const f32 one_minus = 1.0f - vdoth;
    const f32 factor = one_minus * one_minus * one_minus * one_minus * one_minus;
    return vec3_add(f0, vec3_scale(vec3_sub(vec3_one(), f0), factor));
}

vec3_t pbr_fresnel_schlick_roughness(f32 vdoth, vec3_t f0, f32 roughness)
{
    // 用 max(1-roughness, F0) 作为上界：粗糙表面的掠射角反射不该亮到纯白，
    // 否则物体的边缘会出现一圈明显不自然的白边。
    const vec3_t upper = vec3_max(vec3_splat(1.0f - roughness), f0);
    const f32    one_minus = 1.0f - vdoth;
    const f32    factor = one_minus * one_minus * one_minus * one_minus * one_minus;
    return vec3_add(f0, vec3_scale(vec3_sub(upper, f0), factor));
}

// =============================================================================
//  光照
// =============================================================================
vec3_t pbr_direct_light(const pbr_material_t *material, vec3_t normal, vec3_t view_direction,
                        vec3_t light_direction, vec3_t radiance)
{
    ASSERT(material != NULL);

    const vec3_t n = vec3_normalize(normal);
    const vec3_t v = vec3_normalize(view_direction);
    const vec3_t l = vec3_normalize(light_direction);

    const f32 ndotl = math_max(vec3_dot(n, l), 0.0f);
    if (ndotl <= 0.0f) {
        return vec3_zero(); // 背光面
    }

    // N·V 要钳到一个小的正数：掠射角时 N·V 接近 0，
    // 分母里的 4*N·V*N·L 会趋近 0，镜面项爆掉出现白色噪点。
    const f32 ndotv = math_max(vec3_dot(n, v), 1.0e-4f);

    const vec3_t h = vec3_normalize(vec3_add(v, l));
    const f32    ndoth = math_max(vec3_dot(n, h), 0.0f);
    const f32    vdoth = math_max(vec3_dot(v, h), 0.0f);

    const vec3_t f0 = pbr_f0(material);

    const vec3_t f = pbr_fresnel_schlick(vdoth, f0);
    const f32    d = pbr_distribution_ggx(ndoth, material->roughness);
    const f32    g = pbr_geometry_smith(ndotv, ndotl, material->roughness);

    const f32 specular_denominator = math_max(4.0f * ndotv * ndotl, 1.0e-4f);
    const vec3_t specular = vec3_scale(f, d * g / specular_denominator);

    // 能量守恒：反射掉的那部分不再参与漫反射。
    // 少了 (1 - F) 这一项，金属会同时有很强的漫反射和高光 —— 像塑料而不像金属。
    const vec3_t kd = vec3_scale(vec3_sub(vec3_one(), f), 1.0f - material->metallic);
    const vec3_t diffuse = vec3_scale(vec3_mul(kd, material->base_color), 1.0f / F32_PI);

    const vec3_t lo = vec3_add(diffuse, specular);
    return vec3_scale(vec3_mul(lo, radiance), ndotl);
}

f32 pbr_distance_attenuation(f32 distance, f32 range)
{
    if (range <= 0.0f) {
        return 1.0f; // range = 0 视为无限远（平行光）
    }
    if (distance >= range) {
        return 0.0f;
    }

    const f32 d2 = distance * distance;
    const f32 ratio = distance / range;
    const f32 ratio4 = ratio * ratio * ratio * ratio;
    const f32 window = math_saturate(1.0f - ratio4);
    const f32 window2 = window * window;

    // 平方反比 + 平滑窗口函数。
    //
    // 只用平方反比的话，光在 range 处会突然截断成 0 —— 表现为地面上
    // 一个清晰的圆形亮斑边界。窗口函数让它在 range 附近平滑地衰减到 0。
    //
    // 分母加一个小量：光源贴着表面时 d2 接近 0，不钳制会算出无穷大。
    return window2 / math_max(d2, 1.0e-4f);
}

f32 pbr_spot_attenuation(vec3_t to_light, vec3_t spot_forward, f32 cos_inner, f32 cos_outer)
{
    // 约定必须说清楚，否则这里极易写反：
    //   to_light       —— "从表面指向光源"（与 pbr_direct_light 的 light_direction 同一个方向）
    //   spot_forward   —— 聚光锥的轴线，"从光源指向外"
    //
    // 表面正好落在锥轴上时 to_light == -spot_forward，所以要先取反再点积。
    // 忘记取反的话整个锥体是反的：正前方全黑、光源背后反而被照亮 ——
    // 症状很显眼，但直觉上会先去怀疑角度参数，很难想到是方向约定。
    const f32 cd = math_clamp(vec3_dot(vec3_normalize(vec3_negate(to_light)),
                                       vec3_normalize(spot_forward)),
                              -1.0f, 1.0f);

    if (cos_inner - cos_outer < 1.0e-6f) {
        // 内外角几乎相同：退化成硬边聚光
        return (cd > cos_outer) ? 1.0f : 0.0f;
    }

    const f32 t = math_saturate((cd - cos_outer) / (cos_inner - cos_outer));
    // 平方一下让边缘过渡更柔和（线性过渡会看到明显的"圆环"）
    return t * t;
}

vec3_t pbr_ambient(const pbr_material_t *material, vec3_t normal, vec3_t view_direction,
                   vec3_t ambient_radiance)
{
    ASSERT(material != NULL);

    const vec3_t n = vec3_normalize(normal);
    const vec3_t v = vec3_normalize(view_direction);
    const f32    ndotv = math_max(vec3_dot(n, v), 1.0e-4f);

    const vec3_t f0 = pbr_f0(material);

    // 这是 IBL 的粗略近似，不是物理正确的。
    // 真 IBL 需要预过滤的环境贴图 + BRDF 查找表（M6 之后）。
    // 但比起"环境光直接加到漫反射上"，这个版本至少让金属在粗糙时
    // 不会出现塑料感 —— 环境镜面的强度会随粗糙度下降。
    const vec3_t f = pbr_fresnel_schlick_roughness(ndotv, f0, material->roughness);
    const vec3_t kd = vec3_scale(vec3_sub(vec3_one(), f), 1.0f - material->metallic);

    // 环境光的两部分都要乘 AO。
    // 只对漫反射乘 AO 是个常见的偷懒做法，但那样"墙角"处的高光会显得浮起来 ——
    // 因为被遮住的方向本来也不该有环境镜面反射。
    // 注意这一项必须和 shader 里的 ambient 表达式保持一致。
    const vec3_t diffuse =
        vec3_scale(vec3_mul(kd, material->base_color), material->ambient_occlusion);
    const vec3_t specular =
        vec3_scale(f, (1.0f - material->roughness) * material->ambient_occlusion);

    const vec3_t ambient = vec3_add(diffuse, specular);
    return vec3_add(vec3_mul(ambient, ambient_radiance), material->emissive);
}

// =============================================================================
//  色调映射与色彩空间
// =============================================================================
vec3_t pbr_tonemap_reinhard(vec3_t color)
{
    // c / (1 + c)：永远 < 1，实现只有一次除法。
    // 缺点是高光会整体发灰（对比度下降）。
    return vec3_make(color.x / (1.0f + color.x), color.y / (1.0f + color.y),
                     color.z / (1.0f + color.z));
}

vec3_t pbr_tonemap_aces(vec3_t color)
{
    // Narkowicz 的 ACES 拟合。比 Reinhard 保留更多高光对比度，
    // 代价是暗部会略微压暗。
    const f32 a = 2.51f;
    const f32 b = 0.03f;
    const f32 c = 2.43f;
    const f32 d = 0.59f;
    const f32 e = 0.14f;

    const vec3_t numerator = vec3_add(vec3_scale(color, a), vec3_splat(b));
    const vec3_t denominator = vec3_add(vec3_scale(color, c), vec3_splat(d));
    const vec3_t result =
        vec3_div(vec3_mul(color, numerator), vec3_add(vec3_mul(color, denominator), vec3_splat(e)));

    return vec3_make(math_saturate(result.x), math_saturate(result.y), math_saturate(result.z));
}

vec3_t pbr_linear_to_srgb(vec3_t linear)
{
    // IEC 61966-2-1 的分段函数。线性段（< 0.0031308）不能省 ——
    // 省掉的话暗部会出现明显的色带。
    vec3_t out;
    const f32 *in = &linear.x;
    f32       *dst = &out.x;

    for (u32 i = 0; i < 3; ++i) {
        const f32 c = math_saturate(in[i]);
        dst[i] = (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
    }
    return out;
}

vec3_t pbr_srgb_to_linear(vec3_t srgb)
{
    vec3_t    out;
    const f32 *in = &srgb.x;
    f32       *dst = &out.x;

    for (u32 i = 0; i < 3; ++i) {
        const f32 c = math_saturate(in[i]);
        dst[i] = (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    return out;
}
