// =============================================================================
//  src/render/mesh.c —— 网格创建与程序化几何
//
//  程序化网格的意义：示例与单元测试不需要任何外部资源文件就能跑。
//  M2 的验收标准里"能画三角形、贴图 quad"靠它们就能满足，
//  资源管线（异步加载、打包）是 M7 的事。
// =============================================================================
#include "engine/render/mesh.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/mesh_file.h"
#include "engine/render/obj_loader.h"
#include "engine/render/renderer.h"

// =============================================================================
//  包围盒
// =============================================================================
void mesh_compute_bounds(const mesh_desc_t *desc, vec3_t *out_min, vec3_t *out_max)
{
    ASSERT(out_min != NULL);
    ASSERT(out_max != NULL);

    *out_min = vec3_zero();
    *out_max = vec3_zero();

    if (desc == NULL) {
        return;
    }

    const i32 attribute_index =
        vertex_layout_find_attribute(&desc->layout, VERTEX_SEMANTIC_POSITION);
    if (attribute_index < 0) {
        return;
    }

    const vertex_attribute_t *attr = &desc->layout.attributes[attribute_index];
    if (attr->format != VERTEX_FORMAT_FLOAT3 || attr->binding >= desc->layout.binding_count) {
        // 只处理最常见的 float3 位置。其它格式（压缩位置）在 M2 用不到，
        // 返回零包围盒不会崩，只是排序深度不准。
        return;
    }

    const mesh_binding_data_t *binding = &desc->bindings[attr->binding];
    if (binding->data == NULL || binding->size == 0) {
        return;
    }

    const u8  *base = (const u8 *)binding->data;
    const u32  stride = desc->layout.bindings[attr->binding].stride;
    const u32  count = (stride > 0) ? (u32)(binding->size / stride) : 0u;

    if (count == 0) {
        return;
    }

    vec3_t min_value = vec3_splat(F32_MAX);
    vec3_t max_value = vec3_splat(-F32_MAX);

    for (u32 i = 0; i < count; ++i) {
        const f32 *position = (const f32 *)(const void *)(base + (usize)i * stride + attr->offset);
        const vec3_t p = vec3_make(position[0], position[1], position[2]);
        min_value = vec3_min(min_value, p);
        max_value = vec3_max(max_value, p);
    }

    *out_min = min_value;
    *out_max = max_value;
}

// =============================================================================
//  创建
// =============================================================================
mesh_handle_t mesh_create(renderer_t *renderer, const mesh_desc_t *desc)
{
    ASSERT(renderer != NULL);

    if (desc == NULL || desc->bindings[0].data == NULL) {
        LOG_ERROR("网格描述不完整");
        return MESH_HANDLE_INVALID;
    }

    // 自动补全：调用方常常忘了填 vertex_count。
    // 从 binding 大小与 stride 推出来，比让用户手工填更不容易错。
    mesh_desc_t filled = *desc;
    if (filled.vertex_count == 0) {
        const u32 stride = filled.layout.bindings[0].stride;
        if (stride > 0) {
            filled.vertex_count = (u32)(filled.bindings[0].size / stride);
        }
    }
    if (filled.index_count == 0 && filled.index_data != NULL) {
        const u32 index_size = index_format_byte_size(filled.index_format);
        if (index_size > 0) {
            filled.index_count = (u32)(filled.index_data_size / index_size);
        }
    }

    return renderer_create_mesh(renderer, &filled);
}

mesh_handle_t mesh_create_from_obj_file(renderer_t *renderer, const char *path)
{
    ASSERT(renderer != NULL);

    // 解析用的临时内存走帧 arena：加载完成后 GPU 已经持有数据副本，
    // CPU 侧的解析结果不需要长期保留。
    // （M7 的资源层会换成独立的加载 arena，并支持异步加载。）
    arena_t *arena = renderer_frame_arena(renderer);

    obj_mesh_t obj;
    if (!obj_load_from_file(arena, path, &obj)) {
        return MESH_HANDLE_INVALID;
    }

    const mesh_desc_t desc = obj_mesh_to_desc(&obj);
    const mesh_handle_t handle = renderer_create_mesh(renderer, &desc);

    LOG_INFO("OBJ 已加载: %s（%u 顶点 / %u 索引）", obj.source_name, obj.vertex_count,
             obj.index_count);
    return handle;
}

mesh_handle_t mesh_create_from_obj_memory(renderer_t *renderer, const void *data, usize size,
                                          const char *debug_name)
{
    ASSERT(renderer != NULL);

    arena_t *arena = renderer_frame_arena(renderer);

    obj_mesh_t obj;
    if (!obj_load_from_memory(arena, data, size, debug_name, &obj)) {
        return MESH_HANDLE_INVALID;
    }

    const mesh_desc_t desc = obj_mesh_to_desc(&obj);
    return renderer_create_mesh(renderer, &desc);
}

mesh_handle_t mesh_create_from_mesh_file(renderer_t *renderer, const char *path)
{
    ASSERT(renderer != NULL);

    arena_t *arena = renderer_frame_arena(renderer);

    mesh_file_t file;
    if (!mesh_file_load(arena, path, &file)) {
        return MESH_HANDLE_INVALID;
    }

    const mesh_desc_t desc = mesh_file_to_desc(&file);
    const mesh_handle_t handle = renderer_create_mesh(renderer, &desc);

    LOG_INFO(".mesh 已加载: %s（%u 顶点 / %u 索引）", file.name, file.vertex_count,
             file.index_count);
    return handle;
}

mesh_handle_t mesh_create_from_mesh_memory(renderer_t *renderer, const void *data, usize size)
{
    ASSERT(renderer != NULL);

    arena_t *arena = renderer_frame_arena(renderer);

    mesh_file_t file;
    if (!mesh_file_parse(arena, data, size, &file)) {
        return MESH_HANDLE_INVALID;
    }

    const mesh_desc_t desc = mesh_file_to_desc(&file);
    return renderer_create_mesh(renderer, &desc);
}

// =============================================================================
//  程序化几何
// =============================================================================
static mesh_vertex_t make_vertex(vec3_t position, vec3_t normal, vec2_t texcoord)
{
    mesh_vertex_t vertex;
    vertex.position[0] = position.x;
    vertex.position[1] = position.y;
    vertex.position[2] = position.z;
    vertex.normal[0] = normal.x;
    vertex.normal[1] = normal.y;
    vertex.normal[2] = normal.z;
    vertex.texcoord[0] = texcoord.x;
    vertex.texcoord[1] = texcoord.y;
    return vertex;
}

mesh_handle_t mesh_create_quad(renderer_t *renderer, f32 width, f32 height)
{
    ASSERT(renderer != NULL);
    ASSERT(width > 0.0f && height > 0.0f);

    const f32 half_w = width * 0.5f;
    const f32 half_h = height * 0.5f;
    const vec3_t normal = vec3_make(0.0f, 0.0f, 1.0f);

    mesh_vertex_t vertices[4];
    vertices[0] = make_vertex(vec3_make(-half_w, -half_h, 0.0f), normal, vec2_make(0.0f, 0.0f));
    vertices[1] = make_vertex(vec3_make(half_w, -half_h, 0.0f), normal, vec2_make(1.0f, 0.0f));
    vertices[2] = make_vertex(vec3_make(half_w, half_h, 0.0f), normal, vec2_make(1.0f, 1.0f));
    vertices[3] = make_vertex(vec3_make(-half_w, half_h, 0.0f), normal, vec2_make(0.0f, 1.0f));

    // 逆时针绕序（从 +Z 方向看），与引擎默认的 FRONT_FACE_COUNTER_CLOCKWISE 一致
    const u16 indices[6] = {0, 1, 2, 0, 2, 3};

    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = vertices;
    desc.bindings[0].size = sizeof(vertices);
    desc.index_data = indices;
    desc.index_data_size = sizeof(indices);
    desc.index_format = INDEX_FORMAT_U16;
    desc.vertex_count = 4;
    desc.index_count = 6;
    desc.debug_name = "quad";

    return mesh_create(renderer, &desc);
}

mesh_handle_t mesh_create_cube(renderer_t *renderer, f32 size)
{
    ASSERT(renderer != NULL);
    ASSERT(size > 0.0f);

    const f32 h = size * 0.5f;

    // 每个面独立顶点：立方体需要硬边法线，共享顶点会把法线平均掉，
    // 表现为"立方体看起来像个球"。
    mesh_vertex_t vertices[24];
    u16           indices[36];

    // 六个面：+X, -X, +Y, -Y, +Z, -Z
    const vec3_t normals[6] = {
        {1.0f, 0.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, -1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, -1.0f, 0.0f},
    };

    for (u32 face = 0; face < 6; ++face) {
        const vec3_t n = normals[face];

        // 用两个切向量构造面。选轴的方式保证绕序是逆时针（从面外侧看）。
        vec3_t tangent;
        vec3_t bitangent;
        if (face < 2) {
            tangent = vec3_make(0.0f, 0.0f, (face == 0) ? -1.0f : 1.0f);
            bitangent = vec3_make(0.0f, 1.0f, 0.0f);
        } else if (face < 4) {
            tangent = vec3_make(1.0f, 0.0f, 0.0f);
            bitangent = vec3_make(0.0f, 0.0f, (face == 2) ? 1.0f : -1.0f);
        } else {
            tangent = vec3_make((face == 4) ? 1.0f : -1.0f, 0.0f, 0.0f);
            bitangent = vec3_make(0.0f, 1.0f, 0.0f);
        }

        const vec3_t center = vec3_scale(n, h);
        const u32    base = face * 4u;

        vertices[base + 0] = make_vertex(
            vec3_add(center, vec3_add(vec3_scale(tangent, -h), vec3_scale(bitangent, -h))), n,
            vec2_make(0.0f, 0.0f));
        vertices[base + 1] = make_vertex(
            vec3_add(center, vec3_add(vec3_scale(tangent, h), vec3_scale(bitangent, -h))), n,
            vec2_make(1.0f, 0.0f));
        vertices[base + 2] = make_vertex(
            vec3_add(center, vec3_add(vec3_scale(tangent, h), vec3_scale(bitangent, h))), n,
            vec2_make(1.0f, 1.0f));
        vertices[base + 3] = make_vertex(
            vec3_add(center, vec3_add(vec3_scale(tangent, -h), vec3_scale(bitangent, h))), n,
            vec2_make(0.0f, 1.0f));

        const u32 index_base = face * 6u;
        indices[index_base + 0] = (u16)(base + 0);
        indices[index_base + 1] = (u16)(base + 1);
        indices[index_base + 2] = (u16)(base + 2);
        indices[index_base + 3] = (u16)(base + 0);
        indices[index_base + 4] = (u16)(base + 2);
        indices[index_base + 5] = (u16)(base + 3);
    }

    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = vertices;
    desc.bindings[0].size = sizeof(vertices);
    desc.index_data = indices;
    desc.index_data_size = sizeof(indices);
    desc.index_format = INDEX_FORMAT_U16;
    desc.vertex_count = 24;
    desc.index_count = 36;
    desc.debug_name = "cube";

    return mesh_create(renderer, &desc);
}

mesh_handle_t mesh_create_plane(renderer_t *renderer, f32 size, u32 subdivisions)
{
    ASSERT(renderer != NULL);
    ASSERT(size > 0.0f);

    if (subdivisions == 0) {
        subdivisions = 1;
    }
    if (subdivisions > 512) {
        subdivisions = 512; // 上限保护：512 段已经是 52 万三角面
    }

    const u32 grid = subdivisions + 1u;
    const u32 vertex_count = grid * grid;
    const u32 index_count = subdivisions * subdivisions * 6u;

    // 临时数据走帧 arena，不占用堆
    arena_t *arena = renderer_frame_arena(renderer);
    mesh_vertex_t *vertices = ARENA_NEW_ARRAY(arena, mesh_vertex_t, vertex_count);
    u32           *indices = ARENA_NEW_ARRAY(arena, u32, index_count);
    if (vertices == NULL || indices == NULL) {
        LOG_ERROR("地面网格临时内存不足（%u 顶点）", vertex_count);
        return MESH_HANDLE_INVALID;
    }

    const f32 half = size * 0.5f;

    for (u32 z = 0; z < grid; ++z) {
        for (u32 x = 0; x < grid; ++x) {
            const f32 fx = (f32)x / (f32)subdivisions;
            const f32 fz = (f32)z / (f32)subdivisions;
            vertices[z * grid + x] =
                make_vertex(vec3_make(-half + fx * size, 0.0f, -half + fz * size),
                            vec3_make(0.0f, 1.0f, 0.0f), vec2_make(fx, fz));
        }
    }

    u32 cursor = 0;
    for (u32 z = 0; z < subdivisions; ++z) {
        for (u32 x = 0; x < subdivisions; ++x) {
            const u32 top_left = z * grid + x;
            const u32 top_right = top_left + 1u;
            const u32 bottom_left = top_left + grid;
            const u32 bottom_right = bottom_left + 1u;

            // 逆时针（从 +Y 往下看）
            indices[cursor++] = top_left;
            indices[cursor++] = bottom_left;
            indices[cursor++] = bottom_right;
            indices[cursor++] = top_left;
            indices[cursor++] = bottom_right;
            indices[cursor++] = top_right;
        }
    }

    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = vertices;
    desc.bindings[0].size = (usize)vertex_count * sizeof(mesh_vertex_t);
    desc.index_data = indices;
    desc.index_data_size = (usize)index_count * sizeof(u32);
    desc.index_format = INDEX_FORMAT_U32;
    desc.vertex_count = vertex_count;
    desc.index_count = index_count;
    desc.debug_name = "plane";

    return mesh_create(renderer, &desc);
}

mesh_handle_t mesh_create_grid(renderer_t *renderer, f32 size, u32 divisions)
{
    ASSERT(renderer != NULL);
    ASSERT(size > 0.0f);

    if (divisions == 0) {
        divisions = 1;
    }
    if (divisions > 512) {
        divisions = 512;
    }

    const u32 line_count = (divisions + 1u) * 2u;
    const u32 vertex_count = line_count * 2u;

    arena_t *arena = renderer_frame_arena(renderer);
    mesh_vertex_t *vertices = ARENA_NEW_ARRAY(arena, mesh_vertex_t, vertex_count);
    if (vertices == NULL) {
        return MESH_HANDLE_INVALID;
    }

    const f32 half = size * 0.5f;
    const vec3_t normal = vec3_make(0.0f, 1.0f, 0.0f);

    u32 cursor = 0;
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 t = -half + size * ((f32)i / (f32)divisions);

        // 沿 Z 方向的线
        vertices[cursor++] = make_vertex(vec3_make(t, 0.0f, -half), normal, vec2_make(0.0f, 0.0f));
        vertices[cursor++] = make_vertex(vec3_make(t, 0.0f, half), normal, vec2_make(0.0f, 1.0f));

        // 沿 X 方向的线
        vertices[cursor++] = make_vertex(vec3_make(-half, 0.0f, t), normal, vec2_make(0.0f, 0.0f));
        vertices[cursor++] = make_vertex(vec3_make(half, 0.0f, t), normal, vec2_make(1.0f, 0.0f));
    }

    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = vertices;
    desc.bindings[0].size = (usize)vertex_count * sizeof(mesh_vertex_t);
    desc.index_data = NULL;
    desc.index_data_size = 0;
    desc.index_format = INDEX_FORMAT_NONE;
    desc.vertex_count = vertex_count;
    desc.index_count = 0;
    desc.debug_name = "grid";

    // 网格线用 LINE_LIST 拓扑，管线里指定
    return mesh_create(renderer, &desc);
}

// =============================================================================
//  查询
// =============================================================================
void mesh_destroy(renderer_t *renderer, mesh_handle_t mesh)
{
    ASSERT(renderer != NULL);
    renderer_destroy_mesh(renderer, mesh);
}

bool mesh_is_valid(const renderer_t *renderer, mesh_handle_t mesh)
{
    return renderer_mesh_is_valid(renderer, mesh);
}

u32 mesh_vertex_count(const renderer_t *renderer, mesh_handle_t mesh)
{
    return renderer_mesh_vertex_count(renderer, mesh);
}

u32 mesh_index_count(const renderer_t *renderer, mesh_handle_t mesh)
{
    return renderer_mesh_index_count(renderer, mesh);
}

const char *mesh_debug_name(const renderer_t *renderer, mesh_handle_t mesh)
{
    return renderer_mesh_name(renderer, mesh);
}

bool mesh_bounds(const renderer_t *renderer, mesh_handle_t mesh, vec3_t *out_min, vec3_t *out_max)
{
    return renderer_mesh_bounds(renderer, mesh, out_min, out_max);
}
