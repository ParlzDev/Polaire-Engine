// =============================================================================
//  engine/render/uniforms.h —— uniform 块（std140）
//
//  ============================================================================
//   为什么不做 set_uniform_xxx(name, value)
//  ============================================================================
//
//  OpenGL 允许按名字设 uniform（glGetUniformLocation + glUniformMatrix4fv）。
//  Vulkan **没有**这个概念：它只有"把一块内存绑定到某个 descriptor 槽位"。
//
//  如果接口做成"按名字设 uniform"，每个后端都要实现一套 GLSL 反射
//  （解析 SPIR-V 找 uniform 位置），而且每次 draw 前的 uniform 上传会变成
//  十几次 API 调用 —— 在 Vulkan 上这些调用要写进命令缓冲，开销完全不可接受。
//
//  所以本引擎采用 **uniform 块 + std140 布局**：
//    * C 结构体直接就是显存里的字节布局，一次 memcpy / 一次 glBufferSubData；
//    * GL 后端用真正的 UBO（glBindBufferBase + glUniformBlockBinding），
//      而不是 glUniform* —— 强制 GL 走和 Vulkan 一样的路径；
//    * 每帧只有 2 次上传（frame + object），而不是每 draw 十几次。
//
//  ============================================================================
//   为什么 GLSL 声明和 C 结构体放在同一个文件里
//  ============================================================================
//
//  std140 的布局规则很细（mat4 按 4 个 vec4 对齐、结构体大小要对齐到 16、
//  数组元素跨度向上取整到 16）。如果 C 结构体和 GLSL 声明分处两个文件，
//  某天有人给 C 结构体加一个字段、忘了改 GLSL，症状是"某个 uniform
//  莫名其妙变成 0"—— 极难定位。
//
//  放在一起 + 一堆 _Static_assert，能把这类错误变成编译期失败。
// =============================================================================
#ifndef ENGINE_RENDER_UNIFORMS_H
#define ENGINE_RENDER_UNIFORMS_H

#include "engine/core/mat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

// ---------------------------------------------------------------- 绑定点
//
// 这些数字必须与 GLSL 里的 `binding = N` 一致，也与 GL 后端的
// glUniformBlockBinding / glBindBufferBase 使用的一致。
#define RENDER_UNIFORM_BINDING_FRAME 0
#define RENDER_UNIFORM_BINDING_OBJECT 1
#define RENDER_UNIFORM_BINDING_COUNT 2

// 纹理槽位。每个槽位对应 GLSL 里的一个 sampler2D uniform。
#define RENDER_TEXTURE_SLOT_ALBEDO 0
#define RENDER_TEXTURE_SLOT_NORMAL 1
#define RENDER_TEXTURE_SLOT_METALLIC_ROUGHNESS 2
#define RENDER_TEXTURE_SLOT_EMISSIVE 3
#define RENDER_TEXTURE_SLOT_OCCLUSION 4
#define RENDER_TEXTURE_SLOT_SHADOW 5
#define RENDER_TEXTURE_SLOT_COUNT RENDER_MAX_TEXTURE_SLOTS

// 单帧能参与光照的光源上限。
//
// 为什么是"上限"而不是动态数组：每帧要整块上传到 UBO，
// 动态长度会让布局不确定。真实场景里超过 8 盏同时照亮同一像素的灯
// 本来就该做"重要光源排序 + 剔除"，而不是无脑全传。
#define MAX_LIGHTS 8

// =============================================================================
//  光源的 GPU 表示
//
//  **std140 下数组元素的跨度必须向上取整到 16 的倍数**，
//  所以这个结构体大小必须是 16 的倍数（这里是 64）。
//  字段全是 vec4，天然满足对齐要求。
//
//  不用 C 侧的 light_t 直接当 GPU 表示：light_t 里 bool / f32 混排，
//  在 std140 下会被塞进不同的对齐位置，两边的布局必然对不上。
// =============================================================================
typedef struct gpu_light_t {
    vec4_t position_type;   // xyz = 位置，w = light_type_t
    vec4_t direction_range; // xyz = 方向（已归一化），w = range
    vec4_t color_intensity; // rgb = 颜色，w = 强度
    vec4_t spot_shadow;     // x = cos(inner)，y = cos(outer)，z = cast_shadow，w = bias
} gpu_light_t;

_Static_assert(sizeof(gpu_light_t) == 64,
               "gpu_light_t 必须是 64 字节（std140 数组元素跨度要求 16 的倍数）");
_Static_assert(alignof(gpu_light_t) == 16, "gpu_light_t 必须 16 字节对齐");
_Static_assert(ENGINE_OFFSETOF(gpu_light_t, position_type) == 0, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(gpu_light_t, direction_range) == 16, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(gpu_light_t, color_intensity) == 32, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(gpu_light_t, spot_shadow) == 48, "std140 布局不符");

// =============================================================================
//  每帧常量
//
//  一次上传，供本帧所有 draw 使用。
// =============================================================================
typedef struct frame_uniforms_t {
    mat4_t view;                // 世界 -> 相机
    mat4_t projection;          // 相机 -> 裁剪
    mat4_t view_projection;     // 上面两者的乘积（省掉每个顶点一次矩阵乘）
    mat4_t inv_view_projection; // 逆矩阵，天空盒用它从 NDC 反推世界空间射线

    vec4_t camera_position; // xyz = 世界坐标，w 未使用
    vec4_t ambient;         // rgb = 环境光颜色，a = 强度
    vec4_t light_direction; // 主平行光方向（xyz，指向光源）+ 是否启用（w）
    vec4_t light_color;     // 主平行光颜色（rgb）+ 强度（a）

    // x = 参与光照的光源数，y = 阴影强度（0 = 关闭），
    // z = 阴影贴图尺寸（texel），w = 曝光（色调映射前的乘数）
    vec4_t light_params;

    // x = 雾启用，y = 起始距离，z = 结束距离，w = 密度
    vec4_t fog_params;
    vec4_t fog_color; // rgb = 雾颜色

    // x = 天空盒启用，y = 色调映射模式（0 = 关闭，1 = Reinhard，2 = ACES），
    // z = 环境光强度，w = 保留
    vec4_t sky_params;
    vec4_t sky_horizon; // 地平线颜色
    vec4_t sky_zenith;  // 天顶颜色
    vec4_t sky_ground;  // 地面颜色

    mat4_t light_view_projection; // 光源视角的视图投影（阴影用）

    // x = 阴影采样的法线偏移（世界单位），y = PCF 半径（texel），z/w 保留
    //
    // 为什么偏移量要进 uniform 而不是在着色器里写死：
    // 它是"阴影痤疮 <-> 彼得潘现象"之间唯一的旋钮，不同场景（角色特写 vs 大地形）
    // 需要的值差一个数量级。写死的话每次调都得重编译着色器。
    vec4_t shadow_params;

    gpu_light_t lights[MAX_LIGHTS];
} frame_uniforms_t;

_Static_assert(sizeof(frame_uniforms_t) == 1024, "frame_uniforms_t 必须是 1024 字节");
_Static_assert(alignof(frame_uniforms_t) == 16, "frame_uniforms_t 必须 16 字节对齐");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, view) == 0, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, projection) == 64, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, view_projection) == 128, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, inv_view_projection) == 192, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, camera_position) == 256, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, ambient) == 272, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, light_direction) == 288, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, light_color) == 304, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, light_params) == 320, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, fog_params) == 336, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, fog_color) == 352, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, sky_params) == 368, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, sky_horizon) == 384, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, sky_zenith) == 400, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, sky_ground) == 416, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, light_view_projection) == 432,
               "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, shadow_params) == 496, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(frame_uniforms_t, lights) == 512, "std140 布局不符");

// =============================================================================
//  每对象常量
//
//  每个 draw 之前上传一次。
//
//  normal_matrix 用 mat4 而不是 mat3：std140 下 mat3 会被当成 3 个 vec4
//  （48 字节），C 侧的 mat3_t 也是 48 字节，本来能对上；但用 mat4 可以
//  让着色器里的写法更统一，而且多出来的 16 字节在每对象数据里可以忽略。
// =============================================================================
typedef struct object_uniforms_t {
    mat4_t model;
    mat4_t normal_matrix; // (model^-1)^T，非均匀缩放下必须用它变换法线
    vec4_t tint;          // 颜色调制；a = 不透明度
    // M4 新增：PBR 材质参数
    vec4_t pbr_params; // x = metallic，y = roughness，z = reflectance，w = ambient_occlusion
    vec4_t emissive;   // rgb = 自发光，a = 保留
} object_uniforms_t;

_Static_assert(sizeof(object_uniforms_t) == 176, "object_uniforms_t 必须是 176 字节");
_Static_assert(alignof(object_uniforms_t) == 16, "object_uniforms_t 必须 16 字节对齐");
_Static_assert(ENGINE_OFFSETOF(object_uniforms_t, model) == 0, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(object_uniforms_t, normal_matrix) == 64, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(object_uniforms_t, tint) == 128, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(object_uniforms_t, pbr_params) == 144, "std140 布局不符");
_Static_assert(ENGINE_OFFSETOF(object_uniforms_t, emissive) == 160, "std140 布局不符");

// =============================================================================
//  GLSL 声明
//
//  由后端在编译着色器时自动前置到用户源码后面（紧跟 #version 行）。
//  用户着色器只写 main()，不写 uniform 块声明 —— 从源头上杜绝两边不一致。
//
//  ⚠ GpuLight 的 struct 定义必须在 uniform block **外面**。
//    GLSL 规范不允许在 uniform/buffer block 内部定义结构体类型。
//    写进去的话 NVIDIA 驱动会报：
//        error C1321: structure type 'GpuLight' may not be defined in a uniform
//        or buffer block
//    而且报错信息指向的是**被前置之后的用户着色器**，一眼看不出是这里的问题。
// =============================================================================
#define RENDER_UNIFORM_BLOCK_GLSL                                                                  \
    "const int MAX_LIGHTS = 8;\n"                                                                  \
    "struct GpuLight {\n"                                                                          \
    "    vec4 position_type;\n"                                                                    \
    "    vec4 direction_range;\n"                                                                  \
    "    vec4 color_intensity;\n"                                                                  \
    "    vec4 spot_shadow;\n"                                                                      \
    "};\n"                                                                                         \
    "layout(std140, binding = 0) uniform FrameUniforms {\n"                                        \
    "    mat4 u_view;\n"                                                                           \
    "    mat4 u_projection;\n"                                                                     \
    "    mat4 u_view_projection;\n"                                                                \
    "    mat4 u_inv_view_projection;\n"                                                            \
    "    vec4 u_camera_position;\n"                                                                \
    "    vec4 u_ambient;\n"                                                                        \
    "    vec4 u_light_direction;\n"                                                                \
    "    vec4 u_light_color;\n"                                                                    \
    "    vec4 u_light_params;\n"                                                                   \
    "    vec4 u_fog_params;\n"                                                                     \
    "    vec4 u_fog_color;\n"                                                                      \
    "    vec4 u_sky_params;\n"                                                                     \
    "    vec4 u_sky_horizon;\n"                                                                    \
    "    vec4 u_sky_zenith;\n"                                                                     \
    "    vec4 u_sky_ground;\n"                                                                     \
    "    mat4 u_light_view_projection;\n"                                                          \
    "    vec4 u_shadow_params;\n"                                                                  \
    "    GpuLight u_lights[MAX_LIGHTS];\n"                                                         \
    "};\n"                                                                                         \
    "layout(std140, binding = 1) uniform ObjectUniforms {\n"                                       \
    "    mat4 u_model;\n"                                                                          \
    "    mat4 u_normal_matrix;\n"                                                                  \
    "    vec4 u_tint;\n"                                                                           \
    "    vec4 u_pbr_params;\n"                                                                     \
    "    vec4 u_emissive;\n"                                                                       \
    "};\n"                                                                                         \
    "layout(binding = 0) uniform sampler2D u_albedo;\n"                                            \
    "layout(binding = 1) uniform sampler2D u_normal_map;\n"                                        \
    "layout(binding = 2) uniform sampler2D u_metallic_roughness;\n"                                \
    "layout(binding = 5) uniform sampler2D u_shadow_map;\n"

// 默认的帧常量（无光照、白色环境光）。
frame_uniforms_t frame_uniforms_default(void);

// 默认的对象常量（单位变换、白色、不透明）。
object_uniforms_t object_uniforms_default(void);

// 由模型矩阵算出正确的对象常量（自动填好 normal_matrix）。
object_uniforms_t object_uniforms_from_model(mat4_t model);

// 写入 PBR 材质参数
void object_uniforms_set_pbr(object_uniforms_t *uniforms, f32 metallic, f32 roughness,
                             f32 reflectance, f32 ambient_occlusion);

#endif // ENGINE_RENDER_UNIFORMS_H
