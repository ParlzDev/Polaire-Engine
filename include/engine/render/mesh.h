// =============================================================================
//  engine/render/mesh.h —— 网格
// =============================================================================
#ifndef ENGINE_RENDER_MESH_H
#define ENGINE_RENDER_MESH_H

#include "engine/core/math.h"
#include "engine/render/render_types.h"

typedef struct renderer_t renderer_t;

// 从描述创建（通用入口）
mesh_handle_t mesh_create(renderer_t *renderer, const mesh_desc_t *desc);

// 扫描顶点数据里的 POSITION 属性，算出包围盒。
// 渲染队列用它算排序深度（用包围盒中心而不是模型矩阵的平移分量 ——
// 对大网格来说后者可能离物体实际位置很远，排序结果会明显错误）。
void mesh_compute_bounds(const mesh_desc_t *desc, vec3_t *out_min, vec3_t *out_max);

// 从 OBJ 文件创建。失败返回 MESH_HANDLE_INVALID 并记日志。
mesh_handle_t mesh_create_from_obj_file(renderer_t *renderer, const char *path);
mesh_handle_t mesh_create_from_obj_memory(renderer_t *renderer, const void *data, usize size,
                                          const char *debug_name);

// 从自研二进制格式（.mesh）创建 —— 见 render/mesh_file.h。
// 与 OBJ 的分工：OBJ 是源格式（要解析），.mesh 是运行格式（一次 memcpy）。
mesh_handle_t mesh_create_from_mesh_file(renderer_t *renderer, const char *path);
mesh_handle_t mesh_create_from_mesh_memory(renderer_t *renderer, const void *data, usize size);

// ---------------------------------------------------------------- 程序化网格
//
// 有这些函数，示例与测试就不需要任何外部资源文件。
// 顶点布局统一用 mesh_vertex_t（与内置着色器匹配）。
//
// quad     : XY 平面上的矩形，法线朝 +Z，UV (0,0)-(1,1)
// cube     : 以原点为中心、边长为 size 的立方体，每个面独立法线与 UV
// plane    : XZ 平面上的网格（subdivisions 段），用于 3D 地形/地面
// grid     : 线框网格（XZ 平面），调试参考线
mesh_handle_t mesh_create_quad(renderer_t *renderer, f32 width, f32 height);
mesh_handle_t mesh_create_cube(renderer_t *renderer, f32 size);
mesh_handle_t mesh_create_plane(renderer_t *renderer, f32 size, u32 subdivisions);
mesh_handle_t mesh_create_grid(renderer_t *renderer, f32 size, u32 divisions);

void mesh_destroy(renderer_t *renderer, mesh_handle_t mesh);

bool        mesh_is_valid(const renderer_t *renderer, mesh_handle_t mesh);
u32         mesh_vertex_count(const renderer_t *renderer, mesh_handle_t mesh);
u32         mesh_index_count(const renderer_t *renderer, mesh_handle_t mesh);
const char *mesh_debug_name(const renderer_t *renderer, mesh_handle_t mesh);

// 网格的局部包围盒（用于视锥剔除与自动取景）
bool  mesh_bounds(const renderer_t *renderer, mesh_handle_t mesh, vec3_t *out_min,
                  vec3_t *out_max);

#endif // ENGINE_RENDER_MESH_H
