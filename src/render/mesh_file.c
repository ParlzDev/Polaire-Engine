// =============================================================================
//  src/render/mesh_file.c —— 自研二进制网格格式（.mesh）
//
//  头部字段逐个读写，不做结构体整体拷贝 —— 理由见头文件。
// =============================================================================
#include "engine/render/mesh_file.h"

#include <stdint.h> // SIZE_MAX
#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/byte_io.h"
#include "engine/core/log.h"

// 头部字段顺序即文件布局（偏移表见 mesh_file.h）：
//   magic | version | vertex_count | index_count | vertex_stride | name_length
//   | bounds_min f32[3] | bounds_max f32[3] | reserved 16 字节
// 写入端与读取端按同一顺序逐个读写，字节序与对齐由 core/byte_io 负责。
// 两边一旦漂移，tests/test_mesh_file.c 的 header_bytes_match_spec 会逐字节抓出来。
//
// reserved 区（48..63）写入恒为 0，读取时**不校验** ——
// 校验 reserved 会把"将来拿它当 flags"的路堵死，而版本不兼容该由 version 负责。
#define MF_RESERVED_BYTES 16u

// 数量 * 元素大小，溢出时返回 false。
// 不用"返回 0 表示失败"：index_count 为 0 时字节数本来就是 0，
// 那是合法值（非索引图元），拿它当错误信号会把这种网格全拒掉。
static bool mf_elem_bytes(u32 count, usize elem_size, usize *out_bytes)
{
    if ((usize)count > (SIZE_MAX - elem_size) / elem_size) {
        return false;
    }
    *out_bytes = (usize)count * elem_size;
    return true;
}

// =============================================================================
//  序列化
// =============================================================================
bool mesh_file_build(arena_t *arena, const mesh_vertex_t *vertices, u32 vertex_count,
                     const u32 *indices, u32 index_count, const vec3_t *bounds_min,
                     const vec3_t *bounds_max, const char *name, void **out_bytes,
                     usize *out_size)
{
    ASSERT(arena != NULL);

    if (vertices == NULL || vertex_count == 0u) {
        LOG_ERROR(".mesh 序列化失败：顶点为空");
        return false;
    }
    if (indices == NULL && index_count > 0u) {
        LOG_ERROR(".mesh 序列化失败：声明了 %u 个索引但没有索引数据", index_count);
        return false;
    }

    usize vertex_bytes = 0u;
    usize index_bytes = 0u;
    if (!mf_elem_bytes(vertex_count, sizeof(mesh_vertex_t), &vertex_bytes) ||
        !mf_elem_bytes(index_count, sizeof(u32), &index_bytes)) {
        LOG_ERROR(".mesh 序列化失败：顶点 %u / 索引 %u 的数量乘字节数溢出",
                  (unsigned)vertex_count, (unsigned)index_count);
        return false;
    }

    usize name_length = 1u; // 至少一个 NUL
    if (name != NULL && name[0] != '\0') {
        name_length = strlen(name) + 1u;
        if (name_length > MESH_FILE_NAME_MAX) {
            LOG_ERROR(".mesh 名字过长（%llu > %u）: %s", (unsigned long long)name_length,
                      (unsigned)MESH_FILE_NAME_MAX, name);
            return false;
        }
    }

    const usize total = MESH_FILE_HEADER_SIZE + vertex_bytes + index_bytes + name_length;

    u8 *buffer = (u8 *)arena_alloc(arena, total);
    if (buffer == NULL) {
        LOG_ERROR(".mesh 序列化失败：arena 无法分配 %llu 字节", (unsigned long long)total);
        return false;
    }

    bio_writer_t w;
    bio_writer_init(&w, buffer, total);

    const vec3_t zero = vec3_zero();
    const vec3_t *bmin = (bounds_min != NULL) ? bounds_min : &zero;
    const vec3_t *bmax = (bounds_max != NULL) ? bounds_max : &zero;

    bool ok = bio_write_u32(&w, MESH_FILE_MAGIC) && bio_write_u32(&w, MESH_FILE_VERSION) &&
              bio_write_u32(&w, vertex_count) && bio_write_u32(&w, index_count) &&
              bio_write_u32(&w, (u32)sizeof(mesh_vertex_t)) &&
              bio_write_u32(&w, (u32)name_length) &&
              bio_write_f32(&w, bmin->x) && bio_write_f32(&w, bmin->y) && bio_write_f32(&w, bmin->z) &&
              bio_write_f32(&w, bmax->x) && bio_write_f32(&w, bmax->y) && bio_write_f32(&w, bmax->z) &&
              bio_write_zeros(&w, MF_RESERVED_BYTES) && bio_write_bytes(&w, vertices, vertex_bytes) &&
              bio_write_bytes(&w, indices, index_bytes) &&
              (name_length > 1u ? bio_write_bytes(&w, name, name_length)
                                : bio_write_u8(&w, 0u));

    // 写入端是"往自己算出来的缓冲区里写"，理论上不会失败；
    // 真失败了说明上面的长度算错了，必须报出来而不是交出一个坏文件。
    if (!ok || !bio_writer_ok(&w) || bio_writer_used(&w) != total) {
        LOG_ERROR(".mesh 序列化内部不一致：期望 %llu 字节，实际写入 %llu",
                  (unsigned long long)total, (unsigned long long)bio_writer_used(&w));
        return false;
    }

    if (out_bytes != NULL) {
        *out_bytes = buffer;
    }
    if (out_size != NULL) {
        *out_size = total;
    }
    return true;
}

// =============================================================================
//  解析
// =============================================================================
bool mesh_file_parse(arena_t *arena, const void *data, usize size, mesh_file_t *out_file)
{
    ASSERT(arena != NULL);

    if (data == NULL || out_file == NULL) {
        return false;
    }
    memset(out_file, 0, sizeof(*out_file));

    if (size < MESH_FILE_HEADER_SIZE) {
        LOG_ERROR(".mesh 文件太小：%llu 字节（光头部就要 %u）", (unsigned long long)size,
                  (unsigned)MESH_FILE_HEADER_SIZE);
        return false;
    }

    bio_reader_t r;
    bio_reader_init(&r, data, size);

    u32 magic = 0u, version = 0u, vertex_count = 0u, index_count = 0u;
    u32 vertex_stride = 0u, name_length = 0u;
    if (!bio_read_u32(&r, &magic) || !bio_read_u32(&r, &version) ||
        !bio_read_u32(&r, &vertex_count) || !bio_read_u32(&r, &index_count) ||
        !bio_read_u32(&r, &vertex_stride) || !bio_read_u32(&r, &name_length)) {
        LOG_ERROR(".mesh 头部读取失败（%llu 字节）", (unsigned long long)size);
        return false;
    }

    if (magic != MESH_FILE_MAGIC) {
        LOG_ERROR(".mesh 魔数不符：期望 0x%08X，实际 0x%08X（这不是 .mesh 文件）",
                  (unsigned)MESH_FILE_MAGIC, (unsigned)magic);
        return false;
    }
    if (version != MESH_FILE_VERSION) {
        LOG_ERROR(".mesh 版本不支持：文件 %u，引擎认 %u", (unsigned)version,
                  (unsigned)MESH_FILE_VERSION);
        return false;
    }
    if (vertex_stride != (u32)sizeof(mesh_vertex_t)) {
        // 顶点布局变了就是格式变了。硬读会读到错位的位置/法线，
        // 症状是"模型歪了"这种极难定位的问题，所以宁可拒绝。
        LOG_ERROR(".mesh 顶点步长不符：文件 %u 字节，引擎 %u 字节", (unsigned)vertex_stride,
                  (unsigned)sizeof(mesh_vertex_t));
        return false;
    }
    if (vertex_count == 0u) {
        LOG_ERROR(".mesh 文件不含顶点");
        return false;
    }
    if (name_length == 0u || name_length > MESH_FILE_NAME_MAX) {
        LOG_ERROR(".mesh 名字长度非法：%u", (unsigned)name_length);
        return false;
    }

    usize vertex_bytes = 0u;
    usize index_bytes = 0u;
    if (!mf_elem_bytes(vertex_count, sizeof(mesh_vertex_t), &vertex_bytes) ||
        !mf_elem_bytes(index_count, sizeof(u32), &index_bytes)) {
        LOG_ERROR(".mesh 数量异常：顶点 %u / 索引 %u（乘字节数溢出）", (unsigned)vertex_count,
                  (unsigned)index_count);
        return false;
    }

    const usize total = MESH_FILE_HEADER_SIZE + vertex_bytes + index_bytes + (usize)name_length;
    if (size < total) {
        LOG_ERROR(".mesh 被截断：需要 %llu 字节，文件只有 %llu 字节", (unsigned long long)total,
                  (unsigned long long)size);
        return false;
    }

    // 头部剩下的字段：包围盒 + reserved。顺序必须与写入端一致。
    vec3_t bounds_min, bounds_max;
    if (!bio_read_f32(&r, &bounds_min.x) || !bio_read_f32(&r, &bounds_min.y) ||
        !bio_read_f32(&r, &bounds_min.z) || !bio_read_f32(&r, &bounds_max.x) ||
        !bio_read_f32(&r, &bounds_max.y) || !bio_read_f32(&r, &bounds_max.z) ||
        !bio_skip(&r, MF_RESERVED_BYTES)) {
        LOG_ERROR(".mesh 头部包围盒读取失败");
        return false;
    }
    bounds_min.pad = 0.0f;
    bounds_max.pad = 0.0f;

    mesh_vertex_t *vertices = (mesh_vertex_t *)arena_alloc(arena, vertex_bytes);
    u32           *indices =
        (index_bytes > 0u) ? (u32 *)arena_alloc(arena, index_bytes) : NULL;
    // +1：文件里本应含 NUL，多给一字节兜住"作者忘了写 NUL"的坏文件。
    char *name = (char *)arena_alloc(arena, (usize)name_length + 1u);
    if (vertices == NULL || name == NULL || (index_bytes > 0u && indices == NULL)) {
        LOG_ERROR(".mesh 解析失败：arena 内存不足");
        return false;
    }

    if (!bio_read_bytes(&r, vertices, vertex_bytes) ||
        !bio_read_bytes(&r, indices, index_bytes) ||
        !bio_read_bytes(&r, name, name_length)) {
        LOG_ERROR(".mesh 数据区不完整");
        return false;
    }
    name[name_length] = '\0';

    // 索引越界必须在交给 GPU 之前查出来：驱动越界读不会报错，
    // 只会随机花屏或丢设备上下文，那种问题查起来是没有尽头的。
    for (u32 i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_count) {
            LOG_ERROR(".mesh 索引越界：第 %u 个索引 = %u，但顶点只有 %u 个", (unsigned)i,
                      (unsigned)indices[i], (unsigned)vertex_count);
            return false;
        }
    }

    out_file->bounds_min = bounds_min;
    out_file->bounds_max = bounds_max;
    out_file->vertices = vertices;
    out_file->vertex_count = vertex_count;
    out_file->indices = indices;
    out_file->index_count = index_count;
    out_file->name = name;
    return true;
}

// =============================================================================
//  文件读写
// =============================================================================
bool mesh_file_save(arena_t *arena, const char *path, const mesh_vertex_t *vertices,
                    u32 vertex_count, const u32 *indices, u32 index_count,
                    const vec3_t *bounds_min, const vec3_t *bounds_max, const char *name)
{
    if (path == NULL) {
        return false;
    }

    void *bytes = NULL;
    usize size = 0u;
    if (!mesh_file_build(arena, vertices, vertex_count, indices, index_count, bounds_min,
                         bounds_max, name, &bytes, &size)) {
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        LOG_ERROR("无法写入 .mesh 文件: %s", path);
        return false;
    }

    const bool wrote_all = (fwrite(bytes, 1u, size, file) == size);
    const bool closed = (fclose(file) == 0);
    if (!wrote_all || !closed) {
        LOG_ERROR("写入 .mesh 文件失败: %s", path);
        return false;
    }

    LOG_INFO(".mesh 已写出: %s（%u 顶点 / %u 索引 / %llu 字节）", path, (unsigned)vertex_count,
             (unsigned)index_count, (unsigned long long)size);
    return true;
}

bool mesh_file_load(arena_t *arena, const char *path, mesh_file_t *out_file)
{
    ASSERT(arena != NULL);

    if (path == NULL || out_file == NULL) {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开 .mesh 文件: %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        LOG_ERROR(".mesh 文件无法定位到末尾: %s", path);
        fclose(file);
        return false;
    }
    const long length = ftell(file);
    if (length <= 0) {
        LOG_ERROR(".mesh 文件为空: %s", path);
        fclose(file);
        return false;
    }
    rewind(file);

    // 整个文件读进 arena 再解析。.mesh 一般几十到几百 KB，
    // 一次顺序读比重复 seek 快，而且 mesh_file_parse 本来就要求"完整缓冲区"。
    u8 *buffer = (u8 *)arena_alloc(arena, (usize)length);
    if (buffer == NULL) {
        LOG_ERROR(".mesh 文件超出 arena 容量: %s（%ld 字节）", path, length);
        fclose(file);
        return false;
    }
    if (fread(buffer, 1u, (usize)length, file) != (usize)length) {
        LOG_ERROR(".mesh 文件读取不完整: %s", path);
        fclose(file);
        return false;
    }
    fclose(file);

    return mesh_file_parse(arena, buffer, (usize)length, out_file);
}

mesh_desc_t mesh_file_to_desc(const mesh_file_t *file)
{
    mesh_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    ASSERT(file != NULL);

    desc.layout = vertex_layout_mesh_vertex();
    desc.bindings[0].data = file->vertices;
    desc.bindings[0].size = (usize)file->vertex_count * sizeof(mesh_vertex_t);
    desc.index_data = file->indices;
    desc.index_data_size = (usize)file->index_count * sizeof(u32);
    desc.index_format = INDEX_FORMAT_U32;
    desc.vertex_count = file->vertex_count;
    desc.index_count = file->index_count;
    desc.debug_name = file->name;

    return desc;
}
