// =============================================================================
//  tests/test_pbr.c —— PBR 光照数学
//
//  这里测的是 **CPU 参考实现**（src/render/pbr.c）。它的价值在于：
//  shader 里的公式和它必须一致（两边都在文件头写了这条约定），
//  所以只要这一组测试通过，"公式本身是对的"这件事就有了保证；
//  剩下的风险只剩"GPU 上是不是照抄了同一份公式"，那靠 sRGB 自检 + 目视。
//
//  重点不是"把实现再抄一遍比一次"，而是测**物理不变量**：
//    * 能量守恒：NDF 在整个半球的加权积分必须是 1
//    * 互易性：交换视线与光线方向，BRDF 结果必须相同
//    * 边界值：F(0°) = F0，F(90°) = 1，G(0°) = 0，G(90°) = 1
//    * 单调性：粗糙度越大镜面峰越矮越宽
//  这些性质"实现怎么改都必须成立"，比数值对拍更能挡住错误。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include "engine/render/pbr.h"

// ---------------------------------------------------------------- 材质

static void pbr_material_default_is_sane(void)
{
    const pbr_material_t m = pbr_material_default();

    EXPECT_EQ_U64(m.metallic, 0);           // 默认是电介质
    EXPECT_NEAR(m.roughness, 0.5f, F32_TOLERANCE);
    EXPECT_NEAR(m.reflectance, PBR_DIELECTRIC_F0, F32_TOLERANCE);
    EXPECT_NEAR(m.ambient_occlusion, 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.alpha, 1.0f, F32_TOLERANCE);

    // 默认材质必须是"干净的"—— sanitize 不应该改动它。
    // 改动了说明默认值和合法区间不一致，属于自相矛盾。
    pbr_material_t copy = m;
    EXPECT_FALSE(pbr_material_sanitize(&copy));
}

static void pbr_material_sanitize_clamps(void)
{
    pbr_material_t m = pbr_material_default();

    // roughness = 0 会让 GGX 退化成狄拉克函数，分母出现 0*0 -> NaN
    m.roughness = 0.0f;
    EXPECT_TRUE(pbr_material_sanitize(&m));
    EXPECT_NEAR(m.roughness, PBR_MIN_ROUGHNESS, F32_TOLERANCE);

    m = pbr_material_default();
    m.roughness = 5.0f;
    m.metallic = -1.0f;
    m.reflectance = 2.0f;
    m.ambient_occlusion = 3.0f;
    m.alpha = -0.5f;
    m.base_color = vec3_make(-1.0f, 0.5f, 2.0f);
    EXPECT_TRUE(pbr_material_sanitize(&m));
    EXPECT_NEAR(m.roughness, 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.metallic, 0.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.reflectance, 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.ambient_occlusion, 1.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.alpha, 0.0f, F32_TOLERANCE);
    EXPECT_NEAR(m.base_color.x, 0.0f, F32_TOLERANCE);
    // base_color 允许超过 1（HDR 材质），只钳下界
    EXPECT_NEAR(m.base_color.z, 2.0f, F32_TOLERANCE);

    // 第二次调用不应该再改动任何东西（幂等）
    EXPECT_FALSE(pbr_material_sanitize(&m));
}

static void pbr_f0_interpolates_metal(void)
{
    pbr_material_t m = pbr_material_default();
    m.base_color = vec3_make(1.0f, 0.766f, 0.336f); // 金子的颜色

    // 非金属：F0 = reflectance 的灰色
    EXPECT_VEC3_NEAR(pbr_f0(&m), vec3_splat(PBR_DIELECTRIC_F0), 1.0e-6f);

    // 金属：F0 = base_color（金子的反射就是金色的）
    m.metallic = 1.0f;
    EXPECT_VEC3_NEAR(pbr_f0(&m), m.base_color, 1.0e-6f);

    // 半金属：线性插值
    m.metallic = 0.5f;
    EXPECT_NEAR(pbr_f0(&m).x, 0.5f * PBR_DIELECTRIC_F0 + 0.5f * 1.0f, 1.0e-6f);
}

// ---------------------------------------------------------------- NDF

static void pbr_ggx_is_uniform_at_roughness_one(void)
{
    // roughness = 1 时 a = a2 = 1，d 恒等于 1，于是 D = 1/π（与角度无关）。
    // 这是个精确解，可以用来验证公式没有多余的因子。
    for (u32 i = 0; i <= 20; ++i) {
        const f32 ndoth = (f32)i / 20.0f;
        EXPECT_NEAR(pbr_distribution_ggx(ndoth, 1.0f), 1.0f / F32_PI, 1.0e-5f);
    }
}

static void pbr_ggx_peak_grows_as_roughness_drops(void)
{
    const f32 peak_smooth = pbr_distribution_ggx(1.0f, 0.1f);
    const f32 peak_mid = pbr_distribution_ggx(1.0f, 0.5f);
    const f32 peak_rough = pbr_distribution_ggx(1.0f, 1.0f);

    EXPECT_TRUE(peak_smooth > peak_mid);
    EXPECT_TRUE(peak_mid > peak_rough);
    EXPECT_TRUE(peak_smooth > 1.0f);
    EXPECT_NEAR(peak_rough, 1.0f / F32_PI, 1.0e-5f);
}

static void pbr_ggx_is_decreasing_in_ndoth(void)
{
    // 镜面峰在法线方向（ndoth = 1），离开它必须单调下降。
    for (f32 roughness = 0.1f; roughness <= 1.0f; roughness += 0.1f) {
        f32 previous = pbr_distribution_ggx(1.0f, roughness);
        for (i32 i = 19; i >= 0; --i) {
            const f32 ndoth = (f32)i / 20.0f;
            const f32 d = pbr_distribution_ggx(ndoth, roughness);
            if (!(d <= previous + 1.0e-6f)) {
                EXPECT_TRUE(false);
                return;
            }
            previous = d;
        }
    }
    EXPECT_TRUE(true);
}

// 能量守恒：∫_hemisphere D(n·h) (n·h) dω = 1
//
// 用 u = cosθ 换元把积分化简成一维：
//     ∫ D u dω = 2π ∫₀¹ D(u) u du
// 换元之后积分区间有限、被积函数没有 sinθ 的小量，数值上比在 θ 上积分稳得多。
//
// 这条测试是"公式里有没有多余的 π 或 4"的照妖镜 —— 少一个 4 就会得到 4 或 1/4。
static f32 ggx_hemisphere_integral(f32 roughness, u32 steps)
{
    // 中点法
    const f32 du = 1.0f / (f32)steps;
    f32       sum = 0.0f;
    for (u32 i = 0; i < steps; ++i) {
        const f32 u = ((f32)i + 0.5f) * du;
        sum += pbr_distribution_ggx(u, roughness) * u * du;
    }
    return sum * 2.0f * F32_PI;
}

static void pbr_ggx_energy_conservation(void)
{
    // roughness = 1 时 D 是常数，中点法精确
    EXPECT_NEAR(ggx_hemisphere_integral(1.0f, 4096), 1.0f, 1.0e-5f);
    // 粗糙度越小峰越尖，需要更多采样
    EXPECT_NEAR(ggx_hemisphere_integral(0.5f, 200000), 1.0f, 2.0e-3f);
    EXPECT_NEAR(ggx_hemisphere_integral(0.25f, 400000), 1.0f, 2.0e-2f);
}

// ---------------------------------------------------------------- 几何项

static void pbr_geometry_boundary_values(void)
{
    // N·V = 1 时不管粗糙度多少，G1 都必须是 1（没有任何遮蔽）
    for (f32 roughness = 0.0f; roughness <= 1.0f; roughness += 0.1f) {
        EXPECT_NEAR(pbr_geometry_schlick_ggx(1.0f, roughness), 1.0f, 1.0e-5f);
    }

    // N·V = 0（掠射）时 G1 = 0：完全被自身遮蔽
    EXPECT_NEAR(pbr_geometry_schlick_ggx(0.0f, 0.5f), 0.0f, 1.0e-6f);

    // 任何输入下 G 都不能超过 1，也不能为负
    for (u32 i = 0; i <= 20; ++i) {
        const f32 ndotv = (f32)i / 20.0f;
        for (u32 j = 0; j <= 10; ++j) {
            const f32 g = pbr_geometry_schlick_ggx(ndotv, (f32)j / 10.0f);
            if (!(g >= 0.0f && g <= 1.0f + 1.0e-6f)) {
                EXPECT_TRUE(false);
                return;
            }
        }
    }
    EXPECT_TRUE(true);
}

static void pbr_geometry_is_monotonic_in_ndotv(void)
{
    f32 previous = 0.0f;
    for (u32 i = 0; i <= 20; ++i) {
        const f32 g = pbr_geometry_schlick_ggx((f32)i / 20.0f, 0.5f);
        EXPECT_TRUE(g >= previous - 1.0e-6f);
        previous = g;
    }
}

static void pbr_geometry_smith_is_product(void)
{
    const f32 expected = pbr_geometry_schlick_ggx(0.7f, 0.4f) * pbr_geometry_schlick_ggx(0.3f, 0.4f);
    EXPECT_NEAR(pbr_geometry_smith(0.7f, 0.3f, 0.4f), expected, 1.0e-6f);
    // N·L = N·V = 1 时 Smith 项也是 1
    EXPECT_NEAR(pbr_geometry_smith(1.0f, 1.0f, 0.6f), 1.0f, 1.0e-5f);
}

// ---------------------------------------------------------------- 菲涅尔

static void pbr_fresnel_boundary_values(void)
{
    const vec3_t f0 = vec3_make(0.04f, 0.04f, 0.04f);

    // 垂直入射（V·H = 1）：反射率就是 F0
    EXPECT_VEC3_NEAR(pbr_fresnel_schlick(1.0f, f0), f0, 1.0e-6f);

    // 掠射（V·H = 0）：所有材质都趋近全反射
    EXPECT_VEC3_NEAR(pbr_fresnel_schlick(0.0f, f0), vec3_one(), 1.0e-6f);
}

static void pbr_fresnel_is_monotonic(void)
{
    const vec3_t f0 = vec3_make(0.04f, 0.04f, 0.04f);
    f32          previous = 1.0f;
    for (u32 i = 0; i <= 20; ++i) {
        const f32 vdoth = (f32)i / 20.0f;
        const f32 f = pbr_fresnel_schlick(vdoth, f0).x;
        EXPECT_TRUE(f <= previous + 1.0e-6f);
        previous = f;
    }
}

// 带粗糙度修正的版本必须把掠射角的上界从 1 压到 max(1-roughness, F0) ——
// 否则粗糙表面的边缘会有一圈明显不自然的白边。
static void pbr_fresnel_roughness_limits_grazing(void)
{
    const vec3_t f0 = vec3_make(0.04f, 0.04f, 0.04f);

    const vec3_t rough = pbr_fresnel_schlick_roughness(0.0f, f0, 0.8f);
    EXPECT_NEAR(rough.x, 0.2f, 1.0e-5f); // max(1-0.8, 0.04) = 0.2

    // 光滑表面退化回普通 Schlick
    const vec3_t smooth = pbr_fresnel_schlick_roughness(0.0f, f0, 0.0f);
    EXPECT_NEAR(smooth.x, 1.0f, 1.0e-5f);

    // 垂直入射时两者都等于 F0
    EXPECT_VEC3_NEAR(pbr_fresnel_schlick_roughness(1.0f, f0, 0.7f), f0, 1.0e-6f);
}

// ---------------------------------------------------------------- 直接光照

static void pbr_direct_light_backface_is_black(void)
{
    const pbr_material_t m = pbr_material_default();
    const vec3_t         n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t         v = vec3_make(0.0f, 0.0f, 1.0f);

    // 光从背面来
    EXPECT_VEC3_NEAR(pbr_direct_light(&m, n, v, vec3_make(0.0f, 0.0f, -1.0f), vec3_one()),
                     vec3_zero(), 1.0e-6f);
    // 光正好与法线垂直（N·L = 0）
    EXPECT_VEC3_NEAR(pbr_direct_light(&m, n, v, vec3_make(1.0f, 0.0f, 0.0f), vec3_one()),
                     vec3_zero(), 1.0e-6f);
    // 入射辐照度为 0
    EXPECT_VEC3_NEAR(pbr_direct_light(&m, n, v, vec3_make(0.0f, 0.0f, 1.0f), vec3_zero()),
                     vec3_zero(), 1.0e-6f);
}

static void pbr_direct_light_is_reciprocal(void)
{
    // BRDF 必须满足互易性：交换入射与出射方向结果不变。
    //
    // 取关于法线对称的一对方向，这样 N·V = N·L，
    // 出射辐射度里那个 cos 因子也相同，可以直接比对最终结果。
    pbr_material_t m = pbr_material_default();
    m.roughness = 0.35f;
    m.metallic = 0.2f;

    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const f32    s = sinf(math_radians(45.0f));
    const f32    c = cosf(math_radians(45.0f));

    const vec3_t v = vec3_make(s, 0.0f, c);
    const vec3_t l = vec3_make(-s, 0.0f, c);
    const vec3_t radiance = vec3_make(1.0f, 0.9f, 0.8f);

    const vec3_t a = pbr_direct_light(&m, n, v, l, radiance);
    const vec3_t b = pbr_direct_light(&m, n, l, v, radiance);

    EXPECT_VEC3_NEAR(a, b, 1.0e-5f);
    // 而且必须真的被照亮了（防止两边都是 0 也算"相等"）
    EXPECT_TRUE(a.x > 0.01f);
}

// 直接光照的几个必须成立的不变量。
//
// ⚠ 这里刻意**不测**"单点 BRDF 值 <= 1"：
//   镜面 BRDF 的单点值本来就可以远大于 1（那是"镜面反射"的定义），
//   金属在 roughness = 0.15 时算出来是 2.8，完全正常。
//   真正守恒的是"整个半球的积分"，见下面的 pbr_directional_albedo_is_bounded。
//   一开始把这两件事混在一起，测试就变成了在断言一个错误的前提。
static void pbr_direct_light_invariants(void)
{
    for (f32 roughness = 0.15f; roughness <= 1.0f; roughness += 0.15f) {
        for (f32 metallic = 0.0f; metallic <= 1.0f; metallic += 0.25f) {
            pbr_material_t m = pbr_material_default();
            m.roughness = roughness;
            m.metallic = metallic;

            const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
            const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);
            const vec3_t l = vec3_make(0.0f, 0.0f, 1.0f);
            const vec3_t radiance = vec3_make(1.0f, 0.9f, 0.8f);

            const vec3_t out = pbr_direct_light(&m, n, v, l, radiance);

            // 1) 任何合法输入都不能算出负数或 NaN
            if (!(out.x >= 0.0f && out.y >= 0.0f && out.z >= 0.0f)) {
                EXPECT_TRUE(false);
                return;
            }

            // 2) 对辐照度必须严格线性：辐照度翻倍，结果翻倍。
            //    不线性说明公式里混进了非线性项（例如把 radiance 塞进了除法分母）。
            const vec3_t doubled =
                pbr_direct_light(&m, n, v, l, vec3_scale(radiance, 2.0f));
            if (fabsf(doubled.x - out.x * 2.0f) > 1.0e-4f) {
                EXPECT_TRUE(false);
                return;
            }

            // 3) 通道之间不能串色：radiance 的 r 分量只应该影响输出的 r
            const vec3_t red_only = pbr_direct_light(&m, n, v, l, vec3_make(1.0f, 0.0f, 0.0f));
            if (!(red_only.y == 0.0f && red_only.z == 0.0f)) {
                EXPECT_TRUE(false);
                return;
            }
        }
    }
    EXPECT_TRUE(true);
}

// 正入射时，粗糙度越大镜面峰越低。
// 这条把"粗糙度参数有没有真的传进 GGX"钉住 —— 传错了（例如恒传 1）不会报错，
// 只会让所有材质看起来都一样糊。
static void pbr_roughness_controls_specular_peak(void)
{
    pbr_material_t m = pbr_material_default();
    m.base_color = vec3_splat(0.8f);
    m.metallic = 1.0f; // 纯金属，只看镜面项

    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t l = vec3_make(0.0f, 0.0f, 1.0f);

    f32 previous = F32_MAX;
    for (f32 roughness = 0.2f; roughness <= 1.0f; roughness += 0.2f) {
        m.roughness = roughness;
        const f32 peak = pbr_direct_light(&m, n, v, l, vec3_one()).x;
        if (!(peak < previous)) {
            EXPECT_TRUE(false);
            return;
        }
        previous = peak;
    }
    EXPECT_TRUE(true);
}

// 定向反照率（directional albedo）：固定视线方向，BRDF 在整个半球上的
// 加权积分。这是"能量守恒"在单点之外的正确表述，必须 <= 1。
//
//      albedo(v) = ∫ f(v, l) (n·l) dω_l
//
// 对 metallic = 0、base_color = 1 的材质：
//   漫反射部分积分恰好是 (1 - F_avg) * 1 ≈ 0.96
//   镜面部分再加几个百分点
// 所以结果应该落在 0.9 附近并且严格小于 1。
static f32 pbr_directional_albedo(pbr_material_t material, u32 theta_steps, u32 phi_steps)
{
    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);

    const f32 d_theta = (F32_PI * 0.5f) / (f32)theta_steps;
    const f32 d_phi = (2.0f * F32_PI) / (f32)phi_steps;

    f32 sum = 0.0f;
    for (u32 ti = 0; ti < theta_steps; ++ti) {
        const f32 theta = ((f32)ti + 0.5f) * d_theta;
        const f32 sin_t = sinf(theta);
        const f32 cos_t = cosf(theta);
        if (cos_t <= 0.0f) {
            continue;
        }
        for (u32 pi = 0; pi < phi_steps; ++pi) {
            const f32 phi = ((f32)pi + 0.5f) * d_phi;
            const vec3_t l = vec3_make(sin_t * cosf(phi), sin_t * sinf(phi), cos_t);

            const vec3_t f = pbr_direct_light(&material, n, v, l, vec3_one());
            // 注意：pbr_direct_light 的返回值里**已经乘过 N·L**，
            // 所以这里只需要补上立体角元素 dω = sinθ dθ dφ，
            // 再乘一次 cosθ 就是重复计入（会得到一个偏小的 albedo）。
            sum += f.x * sin_t * d_theta * d_phi;
        }
    }
    return sum;
}

static void pbr_directional_albedo_is_bounded(void)
{
    pbr_material_t m = pbr_material_default();
    m.base_color = vec3_one();
    m.metallic = 0.0f;

    // 中等粗糙度：镜面瓣足够宽，64x128 的网格积分已经很准
    m.roughness = 0.5f;
    const f32 albedo_mid = pbr_directional_albedo(m, 64, 128);
    EXPECT_NEAR(albedo_mid, 0.9944f, 5.0e-3f);

    // 完全粗糙：漫反射占绝对主导，结果应该非常接近 (1 - F) * 1
    m.roughness = 1.0f;
    const f32 albedo_rough = pbr_directional_albedo(m, 64, 128);
    EXPECT_NEAR(albedo_rough, 0.9723f, 5.0e-3f);

    // 粗糙度越大，镜面能量被摊得越开、单次散射丢掉的能量越多
    // （单次散射 GGX 的已知局限，多次散射补偿留到后面的里程碑）。
    // 但总积分不应该随粗糙度剧烈变化。
    EXPECT_TRUE(albedo_rough <= albedo_mid);
    EXPECT_NEAR(albedo_mid, albedo_rough, 0.05f);

    // 金属：没有漫反射，只剩镜面。单位颜色金属的定向反照率也必须在 1 以内。
    m.metallic = 1.0f;
    m.roughness = 0.4f;
    const f32 albedo_metal = pbr_directional_albedo(m, 64, 128);
    EXPECT_NEAR(albedo_metal, 0.9338f, 5.0e-3f);

    // 所有情况都必须严格 <= 1：这就是"能量守恒"的可测形式。
    EXPECT_TRUE(albedo_mid <= 1.0f);
    EXPECT_TRUE(albedo_rough <= 1.0f);
    EXPECT_TRUE(albedo_metal <= 1.0f);
}

// 金属不能有漫反射：metallic = 1 时，只要粗糙度不低到镜面峰扫过，
// 反射应该显著低于"同样颜色但非金属"的漫反射能量。
static void pbr_metal_has_no_diffuse(void)
{
    pbr_material_t dielectric = pbr_material_default();
    dielectric.base_color = vec3_splat(0.9f);
    dielectric.metallic = 0.0f;
    dielectric.roughness = 1.0f; // 完全粗糙，镜面项很弱

    pbr_material_t metal = dielectric;
    metal.metallic = 1.0f;

    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t l = vec3_make(0.0f, 0.0f, 1.0f);

    const f32 dielectric_energy = pbr_direct_light(&dielectric, n, v, l, vec3_one()).x;
    const f32 metal_energy = pbr_direct_light(&metal, n, v, l, vec3_one()).x;

    // 粗糙金属几乎只剩很弱的镜面反射，应该远小于电介质的漫反射
    EXPECT_TRUE(metal_energy < dielectric_energy);
}

// ---------------------------------------------------------------- 距离衰减

static void pbr_distance_attenuation_boundaries(void)
{
    // range <= 0 视为无限远（平行光）
    EXPECT_NEAR(pbr_distance_attenuation(50.0f, 0.0f), 1.0f, F32_TOLERANCE);
    // 超出 range 直接为 0 —— 这是"灯照不到"的硬边界
    EXPECT_NEAR(pbr_distance_attenuation(10.0f, 10.0f), 0.0f, F32_TOLERANCE);
    EXPECT_NEAR(pbr_distance_attenuation(11.0f, 10.0f), 0.0f, F32_TOLERANCE);
}

static void pbr_distance_attenuation_decreases(void)
{
    f32 previous = F32_MAX;
    for (u32 i = 1; i <= 99; ++i) {
        const f32 d = (f32)i * 0.1f; // 0.1 .. 9.9（range = 10）
        const f32 a = pbr_distance_attenuation(d, 10.0f);
        EXPECT_TRUE(a <= previous + 1.0e-6f);
        EXPECT_TRUE(a >= 0.0f);
        previous = a;
    }
}

// 平滑窗口的意义：在 range 附近必须连续地趋近 0，
// 而不是"突然断掉"（突然断掉会在地面上留下一个清晰的圆形亮斑边界）。
static void pbr_distance_attenuation_is_smooth_near_range(void)
{
    const f32 a_before = pbr_distance_attenuation(9.99f, 10.0f);
    EXPECT_TRUE(a_before < 1.0e-4f); // 已经非常接近 0
    EXPECT_TRUE(a_before >= 0.0f);

    // 中点附近仍有可观的能量（不是提前就衰减没了）
    const f32 a_mid = pbr_distance_attenuation(5.0f, 10.0f);
    EXPECT_TRUE(a_mid > a_before);
    EXPECT_TRUE(a_mid > 0.0f);
}

// ---------------------------------------------------------------- 聚光衰减

// 这一组是**方向约定的守门测试**。
//
// 约定：to_light 从表面指向光源；spot_forward 是锥轴，从光源指向外。
// 表面正好在锥轴上时 to_light == -spot_forward。
//
// 如果实现里漏了那个取反，锥体就是反的：正下方全黑、光源背后反而被照亮。
// 下面第一条断言会直接抓住它。
static void pbr_spot_attenuation_axis_is_bright(void)
{
    const vec3_t forward = vec3_make(0.0f, -1.0f, 0.0f); // 灯朝下照
    const f32    cos_inner = cosf(math_radians(20.0f));
    const f32    cos_outer = cosf(math_radians(35.0f));

    // 表面在灯的正下方 5 米处：to_light 从表面指向灯 = (0,1,0)
    const vec3_t to_light = vec3_make(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(pbr_spot_attenuation(to_light, forward, cos_inner, cos_outer), 1.0f, 1.0e-5f);
}

static void pbr_spot_attenuation_outside_cone_is_dark(void)
{
    const vec3_t forward = vec3_make(0.0f, -1.0f, 0.0f);
    const f32    cos_inner = cosf(math_radians(20.0f));
    const f32    cos_outer = cosf(math_radians(35.0f));

    // 表面在 (0, -5, 10)：与锥轴夹角 atan(10/5) ≈ 63°，远在 35° 之外
    const vec3_t surface = vec3_make(0.0f, -5.0f, 10.0f);
    const vec3_t to_light = vec3_normalize(vec3_negate(surface));
    EXPECT_NEAR(pbr_spot_attenuation(to_light, forward, cos_inner, cos_outer), 0.0f, 1.0e-5f);

    // 光源背后（表面在灯上方）：必须也是 0
    const vec3_t above = vec3_make(0.0f, 5.0f, 0.0f);
    const vec3_t to_light_above = vec3_normalize(vec3_negate(above));
    EXPECT_NEAR(pbr_spot_attenuation(to_light_above, forward, cos_inner, cos_outer), 0.0f, 1.0e-5f);
}

static void pbr_spot_attenuation_is_monotonic_in_angle(void)
{
    const vec3_t forward = vec3_make(0.0f, -1.0f, 0.0f);
    const f32    cos_inner = cosf(math_radians(15.0f));
    const f32    cos_outer = cosf(math_radians(30.0f));

    f32 previous = 2.0f;
    // 从锥轴开始逐渐向外偏
    for (u32 i = 0; i <= 30; ++i) {
        const f32    angle = math_radians((f32)i); // 0° .. 30°
        // 表面在灯下方 1 米、水平偏移 tan(angle) 米
        const vec3_t surface = vec3_make(tanf(angle), -1.0f, 0.0f);
        const vec3_t to_light = vec3_normalize(vec3_negate(surface));

        const f32 a = pbr_spot_attenuation(to_light, forward, cos_inner, cos_outer);
        EXPECT_TRUE(a <= previous + 1.0e-6f);
        EXPECT_TRUE(a >= 0.0f && a <= 1.0f);
        previous = a;
    }
}

static void pbr_spot_attenuation_inside_inner_angle_is_full(void)
{
    const vec3_t forward = vec3_make(0.0f, -1.0f, 0.0f);
    const f32    cos_inner = cosf(math_radians(25.0f));
    const f32    cos_outer = cosf(math_radians(40.0f));

    // 夹角 10° < 内角 25°：满强度
    const f32    angle = math_radians(10.0f);
    const vec3_t surface = vec3_make(tanf(angle), -1.0f, 0.0f);
    const vec3_t to_light = vec3_normalize(vec3_negate(surface));
    EXPECT_NEAR(pbr_spot_attenuation(to_light, forward, cos_inner, cos_outer), 1.0f, 1.0e-4f);
}

static void pbr_spot_attenuation_degenerate_is_hard_edged(void)
{
    const vec3_t forward = vec3_make(0.0f, -1.0f, 0.0f);
    const f32    cos_a = cosf(math_radians(30.0f));

    // 内外角相同 -> 退化成硬边，不能出现 0/0
    const f32 inside = pbr_spot_attenuation(vec3_make(0.0f, 1.0f, 0.0f), forward, cos_a, cos_a);
    EXPECT_NEAR(inside, 1.0f, 1.0e-5f);

    const f32 outside =
        pbr_spot_attenuation(vec3_normalize(vec3_make(0.0f, 1.0f, -3.0f)), forward, cos_a, cos_a);
    EXPECT_NEAR(outside, 0.0f, 1.0e-5f);
}

// ---------------------------------------------------------------- 环境光

static void pbr_ambient_includes_emissive(void)
{
    pbr_material_t m = pbr_material_default();
    m.emissive = vec3_make(0.3f, 0.0f, 0.0f);

    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);

    // 环境光辐照度为 0 时，剩下的就是自发光
    const vec3_t with_zero_ambient = pbr_ambient(&m, n, v, vec3_zero());
    EXPECT_VEC3_NEAR(with_zero_ambient, m.emissive, 1.0e-6f);

    // 加上环境光后必须更亮
    const vec3_t with_ambient = pbr_ambient(&m, n, v, vec3_splat(0.5f));
    EXPECT_TRUE(with_ambient.x > with_zero_ambient.x);
}

static void pbr_ambient_scales_with_occlusion(void)
{
    pbr_material_t m = pbr_material_default();
    m.ambient_occlusion = 1.0f;

    const vec3_t n = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t v = vec3_make(0.0f, 0.0f, 1.0f);
    const vec3_t radiance = vec3_splat(0.5f);

    const f32 full = pbr_ambient(&m, n, v, radiance).x;
    m.ambient_occlusion = 0.0f;
    const f32 none = pbr_ambient(&m, n, v, radiance).x;

    EXPECT_NEAR(none, 0.0f, 1.0e-6f);
    EXPECT_TRUE(full > none);
}

// ---------------------------------------------------------------- 色调映射

static void pbr_tonemap_maps_to_display_range(void)
{
    // 从 0 到很大的 HDR 值，输出都必须落在 [0,1]
    for (u32 i = 0; i <= 40; ++i) {
        const f32    c = (f32)i * 2.5f; // 0 .. 100
        const vec3_t v = vec3_splat(c);

        const vec3_t r = pbr_tonemap_reinhard(v);
        EXPECT_TRUE(r.x >= 0.0f && r.x < 1.0f);

        const vec3_t a = pbr_tonemap_aces(v);
        EXPECT_TRUE(a.x >= 0.0f && a.x <= 1.0f);
    }

    // 0 映射到 0
    EXPECT_NEAR(pbr_tonemap_reinhard(vec3_zero()).x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(pbr_tonemap_aces(vec3_zero()).x, 0.0f, 1.0e-6f);
}

static void pbr_tonemap_is_monotonic(void)
{
    f32 prev_r = -1.0f;
    f32 prev_a = -1.0f;
    for (u32 i = 0; i <= 60; ++i) {
        const f32 c = (f32)i * 0.5f;
        const f32 r = pbr_tonemap_reinhard(vec3_splat(c)).x;
        const f32 a = pbr_tonemap_aces(vec3_splat(c)).x;
        EXPECT_TRUE(r >= prev_r - 1.0e-6f);
        EXPECT_TRUE(a >= prev_a - 1.0e-6f);
        prev_r = r;
        prev_a = a;
    }
}

// ---------------------------------------------------------------- sRGB
//
// 这一节是 M4 最关键的一组数值断言。
// sRGB 做错的表现是"整体偏暗/偏亮"，因为所有东西一起偏，肉眼根本判断不出来。
// 唯一可靠的判据就是"已知的输入 -> 已知的输出"。
//
// ⚠ 一个常见的混淆：**"50% 灰"和"sRGB 0.5"不是一回事**。
//   像素值 128 对应的 sRGB 是 128/255 = 0.50196，它的线性值是 0.21586；
//   而 sRGB 恰好 0.5 的线性值是 0.21404。
//   两个数只差 0.002，但测试容差设成 1e-4 的话就会误判 —— 写测试时踩过一次。

static void pbr_srgb_known_values(void)
{
    // 像素值 128（"50% 灰"）：sRGB 0.50196 -> 线性 0.21586
    const vec3_t from_pixel128 = pbr_srgb_to_linear(vec3_splat(128.0f / 255.0f));
    EXPECT_NEAR(from_pixel128.x, 0.21586f, 1.0e-4f);

    // sRGB 恰好 0.5 -> 线性 0.21404
    const vec3_t from_exact_half = pbr_srgb_to_linear(vec3_splat(0.5f));
    EXPECT_NEAR(from_exact_half.x, 0.21404f, 1.0e-4f);

    // 反过来：线性 0.21586 编码回 sRGB 应该是 128/255
    const vec3_t back = pbr_linear_to_srgb(vec3_splat(0.21586f));
    EXPECT_NEAR(back.x, 128.0f / 255.0f, 1.0e-4f);

    // 两个端点必须精确
    EXPECT_NEAR(pbr_srgb_to_linear(vec3_zero()).x, 0.0f, 1.0e-7f);
    EXPECT_NEAR(pbr_srgb_to_linear(vec3_one()).x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(pbr_linear_to_srgb(vec3_zero()).x, 0.0f, 1.0e-7f);
    EXPECT_NEAR(pbr_linear_to_srgb(vec3_one()).x, 1.0f, 1.0e-6f);

    // 线性空间里 0.5 编码出来应该比 0.5 亮（约 0.735）
    EXPECT_NEAR(pbr_linear_to_srgb(vec3_splat(0.5f)).x, 0.73536f, 1.0e-4f);

    // 中灰附近：sRGB 编码永远不小于线性值（编码是"提亮"的）
    for (u32 i = 0; i <= 20; ++i) {
        const f32 v = (f32)i / 20.0f;
        EXPECT_TRUE(pbr_linear_to_srgb(vec3_splat(v)).x >= v - 1.0e-6f);
    }
}

// 往返一致性：linear -> sRGB -> linear 必须回到原值。
// 这条测试会抓住"线性段的分段阈值写错"这类错误 ——
// 阈值写错时，暗部的往返误差会明显变大（出现色带）。
static void pbr_srgb_round_trip(void)
{
    f32 worst = 0.0f;
    for (u32 i = 0; i <= 1000; ++i) {
        const f32    v = (f32)i / 1000.0f;
        const vec3_t original = vec3_splat(v);

        const vec3_t round_trip = pbr_srgb_to_linear(pbr_linear_to_srgb(original));
        const f32    error = fabsf(round_trip.x - v);
        if (error > worst) {
            worst = error;
        }
    }
    // 1001 个采样点的最大往返误差应该非常小
    EXPECT_NEAR(worst, 0.0f, 1.0e-5f);
}

static void pbr_srgb_round_trip_from_srgb(void)
{
    f32 worst = 0.0f;
    for (u32 i = 0; i <= 1000; ++i) {
        const f32    v = (f32)i / 1000.0f;
        const vec3_t round_trip = pbr_linear_to_srgb(pbr_srgb_to_linear(vec3_splat(v)));
        const f32    error = fabsf(round_trip.x - v);
        if (error > worst) {
            worst = error;
        }
    }
    EXPECT_NEAR(worst, 0.0f, 1.0e-5f);
}

// sRGB 编码必须是单调的 —— 非单调会让渐变出现反相的色带。
static void pbr_srgb_is_monotonic(void)
{
    f32 prev_enc = -1.0f;
    f32 prev_dec = -1.0f;
    for (u32 i = 0; i <= 500; ++i) {
        const f32 v = (f32)i / 500.0f;

        const f32 enc = pbr_linear_to_srgb(vec3_splat(v)).x;
        const f32 dec = pbr_srgb_to_linear(vec3_splat(v)).x;

        EXPECT_TRUE(enc >= prev_enc - 1.0e-7f);
        EXPECT_TRUE(dec >= prev_dec - 1.0e-7f);
        prev_enc = enc;
        prev_dec = dec;
    }
}

// 分段函数的两段在接缝处必须连续。
// 接缝不连续的话，暗部会出现一条可见的跳变（就是"色带"）。
static void pbr_srgb_segments_join_smoothly(void)
{
    const f32 a = pbr_linear_to_srgb(vec3_splat(0.0031308f)).x;
    const f32 b = pbr_linear_to_srgb(vec3_splat(0.0031309f)).x;
    EXPECT_NEAR(a, b, 1.0e-5f);

    const f32 c = pbr_srgb_to_linear(vec3_splat(0.04045f)).x;
    const f32 d = pbr_srgb_to_linear(vec3_splat(0.04046f)).x;
    EXPECT_NEAR(c, d, 1.0e-5f);
}

void test_pbr_register(void)
{
    test_register("pbr", "material_default", pbr_material_default_is_sane);
    test_register("pbr", "material_sanitize", pbr_material_sanitize_clamps);
    test_register("pbr", "f0_metal_interpolation", pbr_f0_interpolates_metal);

    test_register("pbr", "ggx_uniform_at_roughness_one", pbr_ggx_is_uniform_at_roughness_one);
    test_register("pbr", "ggx_peak_grows", pbr_ggx_peak_grows_as_roughness_drops);
    test_register("pbr", "ggx_decreasing", pbr_ggx_is_decreasing_in_ndoth);
    test_register("pbr", "ggx_energy_conservation", pbr_ggx_energy_conservation);

    test_register("pbr", "geometry_boundaries", pbr_geometry_boundary_values);
    test_register("pbr", "geometry_monotonic", pbr_geometry_is_monotonic_in_ndotv);
    test_register("pbr", "geometry_smith_product", pbr_geometry_smith_is_product);

    test_register("pbr", "fresnel_boundaries", pbr_fresnel_boundary_values);
    test_register("pbr", "fresnel_monotonic", pbr_fresnel_is_monotonic);
    test_register("pbr", "fresnel_roughness_grazing", pbr_fresnel_roughness_limits_grazing);

    test_register("pbr", "direct_light_backface", pbr_direct_light_backface_is_black);
    test_register("pbr", "direct_light_reciprocity", pbr_direct_light_is_reciprocal);
    test_register("pbr", "direct_light_invariants", pbr_direct_light_invariants);
    test_register("pbr", "roughness_specular_peak", pbr_roughness_controls_specular_peak);
    test_register("pbr", "directional_albedo", pbr_directional_albedo_is_bounded);
    test_register("pbr", "metal_has_no_diffuse", pbr_metal_has_no_diffuse);

    test_register("pbr", "distance_attenuation_boundaries",
                  pbr_distance_attenuation_boundaries);
    test_register("pbr", "distance_attenuation_decreases",
                  pbr_distance_attenuation_decreases);
    test_register("pbr", "distance_attenuation_smooth",
                  pbr_distance_attenuation_is_smooth_near_range);

    test_register("pbr", "spot_attenuation_axis", pbr_spot_attenuation_axis_is_bright);
    test_register("pbr", "spot_attenuation_outside", pbr_spot_attenuation_outside_cone_is_dark);
    test_register("pbr", "spot_attenuation_monotonic",
                  pbr_spot_attenuation_is_monotonic_in_angle);
    test_register("pbr", "spot_attenuation_inner", pbr_spot_attenuation_inside_inner_angle_is_full);
    test_register("pbr", "spot_attenuation_degenerate",
                  pbr_spot_attenuation_degenerate_is_hard_edged);

    test_register("pbr", "ambient_emissive", pbr_ambient_includes_emissive);
    test_register("pbr", "ambient_occlusion", pbr_ambient_scales_with_occlusion);

    test_register("pbr", "tonemap_range", pbr_tonemap_maps_to_display_range);
    test_register("pbr", "tonemap_monotonic", pbr_tonemap_is_monotonic);

    test_register("pbr", "srgb_known_values", pbr_srgb_known_values);
    test_register("pbr", "srgb_round_trip_linear", pbr_srgb_round_trip);
    test_register("pbr", "srgb_round_trip_srgb", pbr_srgb_round_trip_from_srgb);
    test_register("pbr", "srgb_monotonic", pbr_srgb_is_monotonic);
    test_register("pbr", "srgb_segments_join", pbr_srgb_segments_join_smoothly);
}
