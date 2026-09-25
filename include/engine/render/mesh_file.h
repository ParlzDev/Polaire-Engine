// =============================================================================
//  engine/render/mesh_file.h —— 自研二进制网格格式（.mesh）
//
//  ---- 为什么有 OBJ 还要这个 ----
//
//  OBJ 是文本格式，加载要做两遍开销：先把整个文件读进来做词法解析
//  （逐字符扫 v/vn/vt/f），再按面角去重建顶点表。一个几万顶点的模型
//  光解析就是毫秒级，而这笔开销每次启动、每次热重载都要付一遍。
//
//  .mesh 是"解析结果"的直接落盘：顶点表和索引表按内存布局原样写出，
//  加载时只有一次 memcpy 加一遍头部校验。
//
//  所以两者的分工是：
//      .obj  = 源格式，人可读写，美术导出用
//      .mesh = 运行格式，机器专用，由 tools/mesh_tool 从 OBJ 转换而来
//
//  GLTF 加载留给 M7（资源管线一起做：异步、打包、世代句柄）。
//  到时候 .mesh 就是 GLTF 之后的那道"预烘焙"步骤，不是过渡方案。
//
//  ---- 文件格式 ----
//
//  整个文件是小端（LE），字段读写逐个进行而不是把结构体直接 fwrite：
//      * 不依赖主机的对齐规则（不同编译器对填充的把握不一致）
//      * 不依赖主机的字节序（架构要预留 Linux/macOS）
//      * 头部大小固定 64 字节，将来加字段用 reserved，老文件永远能读
//
//      [0, 64)   头部
//      [64, ...) 顶点表：vertex_count * 32 字节，元素为 mesh_vertex_t
//      [...  )   索引表：index_count * 4 字节，u32 三角形列表
//      [...  )   名字：name_length 字节，含结尾 NUL
//
//  ---- 关于校验 ----
//
//  这是"外部输入"边界，所以校验从严：魔数、版本、步长、数量与文件长度的
//  一致性、索引是否越界，任何一条不过就直接失败。
//  索引越界尤其要查 —— 它会让驱动在显存里越界读，症状是随机的花屏或
//  设备丢失，而不是一个干净的错误。
// =============================================================================
#ifndef ENGINE_RENDER_MESH_FILE_H
#define ENGINE_RENDER_MESH_FILE_H

#include "engine/core/memory.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/render_types.h" // mesh_vertex_t, mesh_desc_t

#define MESH_FILE_MAGIC 0x4853454Du // 按字节写出去是 'M','E','S','H'
#define MESH_FILE_VERSION 1u
#define MESH_FILE_HEADER_SIZE 64u
#define MESH_FILE_NAME_MAX 128u

// 解析结果。顶点、索引、名字都分配在传入的 arena 上，释放 arena 即可全部回收。
typedef struct mesh_file_t {
    mesh_vertex_t *vertices;
    u32            vertex_count;
    u32           *indices; // 三角形列表，索引始终为 u32
    u32            index_count;

    vec3_t bounds_min; // 局部空间包围盒，供视锥剔除与自动取景
    vec3_t bounds_max;

    const char *name; // arena 内的副本，不是调用者的指针
} mesh_file_t;

// 把顶点/索引序列化成 .mesh 字节流。缓冲区从 arena 分配。
// out_bytes/out_size 允许为 NULL（只想知道成败与大小以外的错误）。
bool mesh_file_build(arena_t *arena, const mesh_vertex_t *vertices, u32 vertex_count,
                     const u32 *indices, u32 index_count, const vec3_t *bounds_min,
                     const vec3_t *bounds_max, const char *name, void **out_bytes,
                     usize *out_size);

// 从内存中的 .mesh 字节流解析。可测试入口 —— 单元测试不需要文件系统。
bool mesh_file_parse(arena_t *arena, const void *data, usize size, mesh_file_t *out_file);

// 存盘 / 读盘便捷入口。
bool mesh_file_save(arena_t *arena, const char *path, const mesh_vertex_t *vertices,
                    u32 vertex_count, const u32 *indices, u32 index_count,
                    const vec3_t *bounds_min, const vec3_t *bounds_max, const char *name);
bool mesh_file_load(arena_t *arena, const char *path, mesh_file_t *out_file);

// 转成 mesh_desc_t，供 renderer_create_mesh 使用。
mesh_desc_t mesh_file_to_desc(const mesh_file_t *file);

#endif // ENGINE_RENDER_MESH_FILE_H
