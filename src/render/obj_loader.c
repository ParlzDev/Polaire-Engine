// =============================================================================
//  src/render/obj_loader.c —— Wavefront OBJ 解析
//
//  ---- 为什么要手写而不是用第三方 ----
//
//  OBJ 的格式简单到"手写 300 行比集成一个库更省事"，而且我们能精确控制：
//  索引去重策略、多边形三角化方式、内存来源（arena）。
//
//  ---- 两遍扫描 ----
//
//  第一遍只数数量（顶点/法线/UV/面角），第二遍才填数据。
//  好处是输出数组能一次分配到位，不需要"扩容 + 拷贝"，
//  而 arena 没有 realloc —— 边解析边扩容会浪费大量内存。
//
//  ---- 索引去重 ----
//
//  OBJ 的索引是 (v, vt, vn) 三元组。如果按"每个面角一个顶点"直接展开，
//  一个 1 万面的模型会产生 3 万个顶点，而实际唯一顶点可能只有几千个。
//  所以这里用一个哈希表按三元组去重，产出的顶点数与索引数与
//  专业工具导出的结果一致。
// =============================================================================
#include "engine/render/obj_loader.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/hash.h"
#include "engine/core/log.h"
#include "engine/core/memory.h"
#include "engine/render/render_types.h"

// 三元组索引最多支持 2^21 - 2 个位置/UV/法线。
// OBJ 极少超过这个规模；真超过了会明确报错而不是静默出错。
#define OBJ_MAX_INDEX ((1 << 21) - 2)

typedef struct obj_scan_t {
    u32 position_count;
    u32 normal_count;
    u32 texcoord_count;
    u32 face_count;
    u32 face_corner_count; // 三角化之后的总顶点引用数
    u32 degenerate_face_count;
} obj_scan_t;

// =============================================================================
//  词法辅助
//
//  不用 strtof / atoi：它们要求输入以 NUL 结尾，而我们的输入是内存块，
//  直接改内存（写个 NUL 进去）会破坏只读数据。
// =============================================================================
static const char *skip_blanks(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) {
        ++cursor;
    }
    return cursor;
}

static const char *skip_line(const char *cursor, const char *end)
{
    while (cursor < end && *cursor != '\n') {
        ++cursor;
    }
    if (cursor < end) {
        ++cursor; // 吃掉换行符
    }
    return cursor;
}

static bool parse_f32(const char **cursor, const char *end, f32 *out_value)
{
    const char *p = skip_blanks(*cursor, end);
    if (p >= end) {
        return false;
    }

    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    f64  value = 0.0;
    bool any_digit = false;

    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10.0 + (f64)(*p - '0');
        ++p;
        any_digit = true;
    }

    if (p < end && *p == '.') {
        ++p;
        f64 scale = 0.1;
        while (p < end && *p >= '0' && *p <= '9') {
            value += (f64)(*p - '0') * scale;
            scale *= 0.1;
            ++p;
            any_digit = true;
        }
    }

    if (!any_digit) {
        return false;
    }

    // 指数部分（OBJ 里少见，但有些导出器会写 1e-5）
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        ++p;
        bool exponent_negative = false;
        if (p < end && (*p == '-' || *p == '+')) {
            exponent_negative = (*p == '-');
            ++p;
        }
        int  exponent = 0;
        bool any_exponent = false;
        while (p < end && *p >= '0' && *p <= '9') {
            exponent = exponent * 10 + (*p - '0');
            ++p;
            any_exponent = true;
        }
        if (any_exponent && exponent < 64) {
            f64 scale = 1.0;
            for (int i = 0; i < exponent; ++i) {
                scale *= 10.0;
            }
            value *= exponent_negative ? (1.0 / scale) : scale;
        } else {
            p = save; // 指数格式不合法，回退
        }
    }

    *cursor = p;
    *out_value = (f32)(negative ? -value : value);
    return true;
}

static bool parse_i32(const char **cursor, const char *end, i32 *out_value)
{
    const char *p = skip_blanks(*cursor, end);
    if (p >= end) {
        return false;
    }

    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    } else if (*p == '+') {
        ++p;
    }

    if (p >= end || *p < '0' || *p > '9') {
        return false;
    }

    i64 value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        ++p;
    }

    *cursor = p;
    *out_value = (i32)(negative ? -value : value);
    return true;
}

// 解析一个面顶点引用：支持 "1"、"1/2"、"1//3"、"1/2/3" 四种写法
static bool parse_face_reference(const char **cursor, const char *end, i32 *out_position,
                                 i32 *out_texcoord, i32 *out_normal)
{
    *out_position = 0;
    *out_texcoord = 0;
    *out_normal = 0;

    if (!parse_i32(cursor, end, out_position)) {
        return false;
    }

    const char *p = *cursor;
    if (p < end && *p == '/') {
        ++p;
        if (p < end && *p != '/') {
            if (!parse_i32(&p, end, out_texcoord)) {
                return false;
            }
        }
        if (p < end && *p == '/') {
            ++p;
            if (!parse_i32(&p, end, out_normal)) {
                return false;
            }
        }
    }

    *cursor = p;
    return true;
}

// OBJ 的索引是 1-based；负数表示"从末尾往前数"（-1 = 最后一个）
static i32 resolve_index(i32 raw, u32 count)
{
    if (raw > 0) {
        return raw - 1;
    }
    if (raw < 0) {
        return (i32)count + raw;
    }
    return -1; // 0 表示未指定
}

// =============================================================================
//  第一遍：统计数量
// =============================================================================
static bool obj_scan_counts(const char *cursor, const char *end, obj_scan_t *scan)
{
    memset(scan, 0, sizeof(*scan));

    while (cursor < end) {
        cursor = skip_blanks(cursor, end);
        if (cursor >= end) {
            break;
        }
        if (*cursor == '\n' || *cursor == '#') {
            cursor = skip_line(cursor, end);
            continue;
        }

        // 关键字是两个字符（v / vn / vt / f），后面可能跟空格或直接换行
        const char keyword0 = cursor[0];
        const char keyword1 = (cursor + 1 < end) ? cursor[1] : '\0';

        if (keyword0 == 'v' && keyword1 == ' ') {
            scan->position_count++;
        } else if (keyword0 == 'v' && keyword1 == 't') {
            scan->texcoord_count++;
        } else if (keyword0 == 'v' && keyword1 == 'n') {
            scan->normal_count++;
        } else if (keyword0 == 'f' && (keyword1 == ' ' || keyword1 == '\t')) {
            scan->face_count++;

            // 数这一行有多少个顶点引用：三角化后贡献 (n - 2) 个三角形
            const char *p = cursor + 1;
            u32         corners = 0;
            while (p < end && *p != '\n') {
                p = skip_blanks(p, end);
                if (p >= end || *p == '\n') {
                    break;
                }
                i32 dummy_v = 0;
                i32 dummy_vt = 0;
                i32 dummy_vn = 0;
                if (!parse_face_reference(&p, end, &dummy_v, &dummy_vt, &dummy_vn)) {
                    break;
                }
                corners++;
            }

            if (corners >= 3) {
                scan->face_corner_count += (corners - 2u) * 3u;
            } else {
                scan->degenerate_face_count++;
            }
        }

        cursor = skip_line(cursor, end);
    }

    return scan->position_count > 0;
}

// =============================================================================
//  第二遍：填充数据
// =============================================================================
typedef struct obj_build_context_t {
    // 注意：这些指针指向的是 arena 上刚分配出来的可写内存。
    // 不用 const 是为了避免在填充时做 const 去除转换
    // （那会触发 -Wcast-qual，而且确实是"写只读内存"的坏味道）。
    vec3_t *positions;
    vec2_t *texcoords;
    vec3_t *normals;
    u32     position_count;
    u32     texcoord_count;
    u32     normal_count;

    mesh_vertex_t *vertices;
    u32           *indices;
    u32            vertex_capacity;
    u32            vertex_count;
    u32            index_count;

    hash_t *dedup; // (v,vt,vn) 三元组 -> 输出顶点下标
} obj_build_context_t;

// 把 (v, vt, vn) 打包成一个 u64 键。
// 每个字段加 1 后占 21 位（0 表示"未指定"，避免与合法的下标 0 冲突）。
static u64 obj_make_vertex_key(i32 v, i32 vt, i32 vn)
{
    const u64 vv = (u64)(u32)(v + 1);
    const u64 tt = (u64)(u32)(vt + 1);
    const u64 nn = (u64)(u32)(vn + 1);
    return (vv << 42) | (tt << 21) | nn;
}

static bool obj_emit_vertex(obj_build_context_t *context, i32 v, i32 vt, i32 vn, u32 *out_index)
{
    const u64 key = obj_make_vertex_key(v, vt, vn);

    u64 existing = 0;
    if (hash_get(context->dedup, key, &existing)) {
        *out_index = (u32)existing;
        return true;
    }

    if (context->vertex_count >= context->vertex_capacity) {
        return false; // 不该发生：容量是按第一遍的统计精确分配的
    }

    mesh_vertex_t vertex;
    memset(&vertex, 0, sizeof(vertex));

    if (v >= 0 && (u32)v < context->position_count) {
        const vec3_t p = context->positions[v];
        vertex.position[0] = p.x;
        vertex.position[1] = p.y;
        vertex.position[2] = p.z;
    }
    if (vn >= 0 && (u32)vn < context->normal_count) {
        const vec3_t n = context->normals[vn];
        vertex.normal[0] = n.x;
        vertex.normal[1] = n.y;
        vertex.normal[2] = n.z;
    }
    if (vt >= 0 && (u32)vt < context->texcoord_count) {
        const vec2_t t = context->texcoords[vt];
        vertex.texcoord[0] = t.x;
        vertex.texcoord[1] = t.y;
    }

    const u32 index = context->vertex_count;
    context->vertices[index] = vertex;
    context->vertex_count++;

    hash_set(context->dedup, key, (u64)index);
    *out_index = index;
    return true;
}

static bool obj_build_data(const char *cursor, const char *end, obj_build_context_t *context)
{
    u32 position_cursor = 0;
    u32 texcoord_cursor = 0;
    u32 normal_cursor = 0;

    while (cursor < end) {
        cursor = skip_blanks(cursor, end);
        if (cursor >= end) {
            break;
        }
        if (*cursor == '\n' || *cursor == '#') {
            cursor = skip_line(cursor, end);
            continue;
        }

        const char keyword0 = cursor[0];
        const char keyword1 = (cursor + 1 < end) ? cursor[1] : '\0';

        if (keyword0 == 'v' && keyword1 == ' ') {
            const char *p = cursor + 1;
            f32         x = 0.0f;
            f32         y = 0.0f;
            f32         z = 0.0f;
            if (parse_f32(&p, end, &x) && parse_f32(&p, end, &y) && parse_f32(&p, end, &z) &&
                position_cursor < context->position_count) {
                context->positions[position_cursor] = vec3_make(x, y, z);
            }
            position_cursor++;
        } else if (keyword0 == 'v' && keyword1 == 't') {
            const char *p = cursor + 2;
            f32         u = 0.0f;
            f32         v = 0.0f;
            if (parse_f32(&p, end, &u) && parse_f32(&p, end, &v) &&
                texcoord_cursor < context->texcoord_count) {
                context->texcoords[texcoord_cursor] = vec2_make(u, v);
            }
            texcoord_cursor++;
        } else if (keyword0 == 'v' && keyword1 == 'n') {
            const char *p = cursor + 2;
            f32         x = 0.0f;
            f32         y = 0.0f;
            f32         z = 0.0f;
            if (parse_f32(&p, end, &x) && parse_f32(&p, end, &y) && parse_f32(&p, end, &z) &&
                normal_cursor < context->normal_count) {
                context->normals[normal_cursor] = vec3_make(x, y, z);
            }
            normal_cursor++;
        } else if (keyword0 == 'f' && (keyword1 == ' ' || keyword1 == '\t')) {
            const char *p = cursor + 1;

            // 多边形面最多 64 个角；超过的按扇形三角化处理前 64 个。
            // 真实的 OBJ 极少有超过 64 边的面。
            enum { MAX_FACE_CORNERS = 64 };
            u32 corners[MAX_FACE_CORNERS];
            u32 corner_count = 0;

            while (p < end && *p != '\n' && corner_count < MAX_FACE_CORNERS) {
                p = skip_blanks(p, end);
                if (p >= end || *p == '\n') {
                    break;
                }

                i32 raw_v = 0;
                i32 raw_vt = 0;
                i32 raw_vn = 0;
                if (!parse_face_reference(&p, end, &raw_v, &raw_vt, &raw_vn)) {
                    break;
                }

                const i32 v = resolve_index(raw_v, context->position_count);
                const i32 vt = resolve_index(raw_vt, context->texcoord_count);
                const i32 vn = resolve_index(raw_vn, context->normal_count);

                u32 emitted = 0;
                if (!obj_emit_vertex(context, v, vt, vn, &emitted)) {
                    return false;
                }
                corners[corner_count++] = emitted;
            }

            // 扇形三角化：n 边形 -> (n-2) 个三角形。
            // 只对凸多边形正确；凹多边形需要更复杂的算法（M7 再说）。
            for (u32 i = 1; i + 1u < corner_count; ++i) {
                context->indices[context->index_count++] = corners[0];
                context->indices[context->index_count++] = corners[i];
                context->indices[context->index_count++] = corners[i + 1u];
            }
        }

        cursor = skip_line(cursor, end);
    }

    return true;
}

// =============================================================================
//  公开接口
// =============================================================================
bool obj_load_from_memory(arena_t *arena, const void *data, usize size, const char *source_name,
                          obj_mesh_t *out_mesh)
{
    ASSERT(arena != NULL);
    ASSERT(out_mesh != NULL);

    memset(out_mesh, 0, sizeof(*out_mesh));
    out_mesh->source_name = (source_name != NULL) ? source_name : "<memory>";
    out_mesh->bounds_min = vec3_splat(F32_MAX);
    out_mesh->bounds_max = vec3_splat(-F32_MAX);

    if (data == NULL || size == 0) {
        LOG_ERROR("OBJ 数据为空 (%s)", out_mesh->source_name);
        return false;
    }

    const char *cursor = (const char *)data;
    const char *end = cursor + size;

    obj_scan_t scan;
    if (!obj_scan_counts(cursor, end, &scan)) {
        LOG_ERROR("OBJ 解析失败：没有找到任何顶点 (%s)", out_mesh->source_name);
        return false;
    }

    if (scan.position_count > OBJ_MAX_INDEX || scan.normal_count > OBJ_MAX_INDEX ||
        scan.texcoord_count > OBJ_MAX_INDEX) {
        LOG_ERROR("OBJ 规模超出支持上限（位置 %u / 法线 %u / UV %u，上限 %d）",
                  scan.position_count, scan.normal_count, scan.texcoord_count, OBJ_MAX_INDEX);
        return false;
    }

    LOG_DEBUG("OBJ 扫描 (%s): 位置 %u, 法线 %u, UV %u, 面 %u, 面角 %u, 退化面 %u",
              out_mesh->source_name, scan.position_count, scan.normal_count, scan.texcoord_count,
              scan.face_count, scan.face_corner_count, scan.degenerate_face_count);

    // 一次分配到位。顶点数最多等于面角数（每个面角一个独立顶点），
    // 去重只会让它更少。
    vec3_t *positions = ARENA_NEW_ARRAY(arena, vec3_t, scan.position_count);
    vec3_t *normals = ARENA_NEW_ARRAY(arena, vec3_t, scan.normal_count > 0 ? scan.normal_count : 1);
    vec2_t *texcoords =
        ARENA_NEW_ARRAY(arena, vec2_t, scan.texcoord_count > 0 ? scan.texcoord_count : 1);
    mesh_vertex_t *vertices = ARENA_NEW_ARRAY(arena, mesh_vertex_t, scan.face_corner_count);
    u32           *indices = ARENA_NEW_ARRAY(arena, u32, scan.face_corner_count);

    if (positions == NULL || normals == NULL || texcoords == NULL || vertices == NULL ||
        indices == NULL) {
        LOG_ERROR("OBJ 解析内存不足 (%s)", out_mesh->source_name);
        return false;
    }

    // 去重表跑在 arena 上。它的内存随 arena reset 一起回收，
    // 不需要单独管理（见 memory.h 里 arena_allocator 的说明）。
    arena_allocator_t arena_alloc;
    arena_allocator_init(&arena_alloc, arena);

    hash_t dedup;
    if (!hash_init(&dedup, &arena_alloc.base, scan.face_corner_count / 2u + 16u)) {
        LOG_ERROR("OBJ 去重表创建失败 (%s)", out_mesh->source_name);
        return false;
    }

    obj_build_context_t context;
    memset(&context, 0, sizeof(context));
    context.positions = positions;
    context.normals = normals;
    context.texcoords = texcoords;
    context.position_count = scan.position_count;
    context.normal_count = scan.normal_count;
    context.texcoord_count = scan.texcoord_count;
    context.vertices = vertices;
    context.indices = indices;
    context.vertex_capacity = scan.face_corner_count;
    context.dedup = &dedup;

    if (!obj_build_data(cursor, end, &context)) {
        LOG_ERROR("OBJ 数据填充失败 (%s)", out_mesh->source_name);
        return false;
    }

    // 法线缺失时补一个默认值，避免着色器拿到 (0,0,0) 之后 normalize 出 NaN
    // （表现为整个模型全黑）。
    for (u32 i = 0; i < context.vertex_count; ++i) {
        if (vec3_length_sq(vec3_make(vertices[i].normal[0], vertices[i].normal[1],
                                     vertices[i].normal[2])) < 1.0e-12f) {
            vertices[i].normal[1] = 1.0f;
        }
    }

    // 包围盒
    vec3_t bounds_min = vec3_splat(F32_MAX);
    vec3_t bounds_max = vec3_splat(-F32_MAX);
    for (u32 i = 0; i < context.vertex_count; ++i) {
        const vec3_t p =
            vec3_make(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]);
        bounds_min = vec3_min(bounds_min, p);
        bounds_max = vec3_max(bounds_max, p);
    }
    if (context.vertex_count == 0) {
        bounds_min = vec3_zero();
        bounds_max = vec3_zero();
    }

    out_mesh->vertices = vertices;
    out_mesh->vertex_count = context.vertex_count;
    out_mesh->indices = indices;
    out_mesh->index_count = context.index_count;
    out_mesh->bounds_min = bounds_min;
    out_mesh->bounds_max = bounds_max;
    out_mesh->source_position_count = scan.position_count;
    out_mesh->source_normal_count = scan.normal_count;
    out_mesh->source_texcoord_count = scan.texcoord_count;
    out_mesh->source_face_count = scan.face_count;
    out_mesh->skipped_face_count = scan.degenerate_face_count;

    LOG_INFO("OBJ 解析完成 (%s): %u 顶点 / %u 三角面（源 %u 位置，去重率 %.0f%%）",
             out_mesh->source_name, out_mesh->vertex_count, out_mesh->index_count / 3u,
             scan.position_count,
             (scan.face_corner_count > 0)
                 ? (100.0 - 100.0 * (f64)out_mesh->vertex_count / (f64)scan.face_corner_count)
                 : 0.0);

    return true;
}

bool obj_load_from_file(arena_t *arena, const char *path, obj_mesh_t *out_mesh)
{
    ASSERT(arena != NULL);

    if (path == NULL) {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开 OBJ 文件: %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        LOG_ERROR("OBJ 文件无法定位到末尾: %s", path);
        fclose(file);
        return false;
    }

    const long length = ftell(file);
    if (length <= 0) {
        LOG_ERROR("OBJ 文件为空: %s", path);
        fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    char *buffer = (char *)arena_alloc(arena, (usize)length + 1u);
    if (buffer == NULL) {
        LOG_ERROR("OBJ 文件读取缓冲分配失败 (%ld 字节): %s", length, path);
        fclose(file);
        return false;
    }

    const usize read = fread(buffer, 1, (usize)length, file);
    fclose(file);

    if (read != (usize)length) {
        LOG_ERROR("OBJ 文件读取不完整 (%llu / %ld): %s", (unsigned long long)read, length, path);
        return false;
    }
    buffer[length] = '\0';

    return obj_load_from_memory(arena, buffer, (usize)length, path, out_mesh);
}

mesh_desc_t obj_mesh_to_desc(const obj_mesh_t *mesh)
{
    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    ASSERT(mesh != NULL);

    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = mesh->vertices;
    desc.bindings[0].size = (usize)mesh->vertex_count * sizeof(mesh_vertex_t);
    desc.index_data = mesh->indices;
    desc.index_data_size = (usize)mesh->index_count * sizeof(u32);
    desc.index_format = INDEX_FORMAT_U32;
    desc.vertex_count = mesh->vertex_count;
    desc.index_count = mesh->index_count;
    desc.debug_name = mesh->source_name;

    return desc;
}
