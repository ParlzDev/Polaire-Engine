// =============================================================================
//  engine/render/obj_loader.h —— Wavefront OBJ 加载
//
//  ---- 为什么 M2 就先做 OBJ 而不是等 M7 的资源层 ----
//
//  M2 的验收标准里就有"加载 OBJ 模型"。OBJ 是纯文本、格式简单、
//  能手工写测试用例，很适合在还没有资源管线的时候先用起来。
//  真正的资源管线（异步加载、打包、热重载）在 M7。
//
//  ---- M2 支持的范围 ----
//
//  支持：v / vn / vt / f（含 a、a/b、a//c、a/b/c 四种索引写法）、
//        负索引、多边形面的扇形三角化、以 # 开头的注释、CRLF 换行。
//  忽略：o / g / s / usemtl / mtllib（M7 再接材质）、
//        曲线与曲面（curv/curv2/surf）、点与线图元。
//
//  ---- 关于"OBJ 没有统一单位" ----
//
//  不同来源的 OBJ 尺寸可能差几个数量级（有的 1 单位 = 1 米，有的 1 单位 = 1 厘米）。
//  加载后请用 obj_mesh_t::bounds_min/max 算出实际尺寸，
//  在应用层决定缩放，而不是在加载器里猜。
// =============================================================================
#ifndef ENGINE_RENDER_OBJ_LOADER_H
#define ENGINE_RENDER_OBJ_LOADER_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/render_types.h" // mesh_vertex_t

// 加载结果。所有数据都分配在传入的 arena 上，整体释放即可。
typedef struct obj_mesh_t {
    mesh_vertex_t *vertices;
    u32            vertex_count;
    u32           *indices; // 三角形列表
    u32            index_count;

    vec3_t bounds_min;
    vec3_t bounds_max;

    // 统计信息，便于发现"文件解析了一半就停了"这类问题
    u32 source_position_count;
    u32 source_normal_count;
    u32 source_texcoord_count;
    u32 source_face_count;
    u32 skipped_face_count; // 顶点数少于 3 的面

    const char *source_name;
} obj_mesh_t;

// 从内存中的 OBJ 文本解析。
// 这是可测试的入口 —— 单元测试直接用字符串构造模型，不依赖文件系统。
bool obj_load_from_memory(arena_t *arena, const void *data, usize size, const char *source_name,
                          obj_mesh_t *out_mesh);

// 从文件读取并解析。内部会把整个文件读进 arena。
bool obj_load_from_file(arena_t *arena, const char *path, obj_mesh_t *out_mesh);

// 把解析结果转成 mesh_desc_t（顶点缓冲 + 索引缓冲），供 renderer_create_mesh 使用。
mesh_desc_t obj_mesh_to_desc(const obj_mesh_t *mesh);

#endif // ENGINE_RENDER_OBJ_LOADER_H
