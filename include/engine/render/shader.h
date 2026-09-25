// =============================================================================
//  engine/render/shader.h —— 着色器程序
//
//  ---- 用户源码里不要写 #version，也不要写 uniform 块 ----
//
//  后端会在编译前自动前置两样东西：
//    1. `#version <后端实际支持的版本> core`
//       写死版本的话，驱动降级（例如只支持 4.3）时会编译失败；
//       由后端按上下文实际版本生成才不会。
//    2. uniform 块声明（FrameUniforms / ObjectUniforms / sampler 绑定）
//       见 uniforms.h。这样 C 结构体与 GLSL 布局不可能漂移。
//
//  用户只需要写 `layout(location = N) in ...` 与 `main()`。
//
//  ---- 顶点属性 location 的约定 ----
//
//  属性 location 直接取 vertex_semantic_t 的枚举值：
//    POSITION   = 0
//    NORMAL     = 1
//    TANGENT    = 2
//    TEXCOORD0  = 4
//    TEXCOORD1  = 5
//    COLOR0     = 6
//
//  这个约定让"顶点布局描述"和"着色器里的 location"不用再维护一张对照表。
// =============================================================================
#ifndef ENGINE_RENDER_SHADER_H
#define ENGINE_RENDER_SHADER_H

#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

// 属性 location = 语义枚举值
#define RENDER_ATTRIBUTE_LOCATION(semantic) ((u32)(semantic))

// 从源码创建
shader_handle_t shader_create(renderer_t *renderer, const shader_desc_t *desc);

void        shader_destroy(renderer_t *renderer, shader_handle_t shader);
bool        shader_is_valid(const renderer_t *renderer, shader_handle_t shader);
const char *shader_debug_name(const renderer_t *renderer, shader_handle_t shader);

// ---------------------------------------------------------------- 内置着色器
//
// 源码是静态字符串，不额外分配。传 false 的字段表示"这个内置着色器不需要"。
//
// unlit_textured : 采样 u_albedo，乘 u_tint。2D 精灵与无光照 3D 用。
// lit_textured   : 平行光 + 环境光的 Lambert。M4 会换成 PBR。
// solid_color    : 不使用纹理，直接用 u_tint 上色。调试线框、碰撞体可视化用。
shader_desc_t shader_builtin_unlit_textured(void);
shader_desc_t shader_builtin_lit_textured(void);
shader_desc_t shader_builtin_solid_color(void);

// M4：PBR（金属度-粗糙度）+ 多光源 + 阴影 + 雾。
// 顶点格式与其它内置着色器相同（mesh_vertex_t），但需要法线。
shader_desc_t shader_builtin_pbr_textured(void);

// ---------------------------------------------------------------- 顶点格式约定
//
// 内置着色器假定顶点布局是 mesh_vertex_t（见 render_types.h）：
//   offset 0  : POSITION   FLOAT3
//   offset 12 : NORMAL     FLOAT3
//   offset 24 : TEXCOORD0  FLOAT2
//
// obj_loader 与 mesh_create_quad 等程序化网格产出的都是这种布局，
// 对应的布局描述由 vertex_layout_mesh_vertex() 提供。

#endif // ENGINE_RENDER_SHADER_H
