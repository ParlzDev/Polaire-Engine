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
#include "engine/core/log.h"

// 头部字段偏移。集中成宏，让"布局"只有一处定义。
#define MF_OFF_MAGIC 0u
#define MF_OFF_VERSION 4u
#define MF_OFF_VERTEX_COUNT 8u
#define MF_OFF_INDEX_COUNT 12u
#define MF_OFF_VERTEX_STRIDE 16u
#define MF_OFF_NAME_LENGTH 20u
#define MF_OFF_BOUNDS_MIN 24u
#define MF_OFF_BOUNDS_MAX 36u
// 48..63 是 reserved。写入时恒为 0，读取时**不校验** ——
// 校验 reserved 会把"将来拿它当 flags"的路堵死，而版本不兼容该由 version 负责。

// ---------------------------------------------------------------- 字节读写
//
// 用 memcpy 而不是 *(u32*)ptr = value：
//   1. 头部是按偏移从缓冲区里切的，编译器有权假设指针不对齐；
//   2. 直接强转还违反严格别名规则。
//   memcpy 让编译器自己决定用什么指令，两个问题都没有。

static void mf_put_u32(u8 *dst, u32 value)
{
    // 显式小端：文件格式的字节序与主机字节序解耦（架构要预留 Linux/macOS）。
    dst[0] = (u8)(value & 0xFFu);
    dst[1] = (u8)((value >> 8) & 0xFFu);
    dst[2] = (u8)((value >> 16) & 0xFFu);
    dst[3] = (u8)((value >> 24) & 0xFFu);
}

static u32 mf_get_u32(const u8 *src)
{
    return (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
}

static void mf_put_f32(u8 *dst, f32 value)
{
    u32 bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    mf_put_u32(dst, bits);
}

static f32 mf_get_f32(const u8 *src)
{
    const u32 bits = mf_get_u32(src);
    f32       value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// vec3_t 是 16 字节（含 pad），文件里每个包围盒只占 12 字节。
static void mf_put_bounds(u8 *dst, const vec3_t *value)
{
    mf_put_f32(dst + 0u, value->x);
    mf_put_f32(dst + 4u, value->y);
    mf_put_f32(dst + 8u, value->z);
}

static void mf_get_bounds(const u8 *src, vec3_t *out)
{
    out->x = mf_get_f32(src + 0u);
    out->y = mf_get_f32(src + 4u);
    out->z = mf_get_f32(src + 8u);
    out->pad = 0.0f; // pad 不是数据，但给个确定值便于调试与 memcmp
}

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

    memset(buffer, 0, MESH_FILE_HEADER_SIZE);
    mf_put_u32(buffer + MF_OFF_MAGIC, MESH_FILE_MAGIC);
    mf_put_u32(buffer + MF_OFF_VERSION, MESH_FILE_VERSION);
    mf_put_u32(buffer + MF_OFF_VERTEX_COUNT, vertex_count);
    mf_put_u32(buffer + MF_OFF_INDEX_COUNT, index_count);
    mf_put_u32(buffer + MF_OFF_VERTEX_STRIDE, (u32)sizeof(mesh_vertex_t));
    mf_put_u32(buffer + MF_OFF_NAME_LENGTH, (u32)name_length);

    const vec3_t zero = vec3_zero();
    mf_put_bounds(buffer + MF_OFF_BOUNDS_MIN, (bounds_min != NULL) ? bounds_min : &zero);
    mf_put_bounds(buffer + MF_OFF_BOUNDS_MAX, (bounds_max != NULL) ? bounds_max : &zero);

    usize offset = MESH_FILE_HEADER_SIZE;
    memcpy(buffer + offset, vertices, vertex_bytes);
    offset += vertex_bytes;
    if (index_bytes > 0u) {
        memcpy(buffer + offset, indices, index_bytes);
        offset += index_bytes;
    }
    if (name_length > 1u) {
        memcpy(buffer + offset, name, name_length);
    } else {
        buffer[offset] = 0u;
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

    const u8 *bytes = (const u8 *)data;

    if (size < MESH_FILE_HEADER_SIZE) {
        LOG_ERROR(".mesh 文件太小：%llu 字节（光头部就要 %u）", (unsigned long long)size,
                  (unsigned)MESH_FILE_HEADER_SIZE);
        return false;
    }

    const u32 magic = mf_get_u32(bytes + MF_OFF_MAGIC);
    const u32 version = mf_get_u32(bytes + MF_OFF_VERSION);
    const u32 vertex_count = mf_get_u32(bytes + MF_OFF_VERTEX_COUNT);
    const u32 index_count = mf_get_u32(bytes + MF_OFF_INDEX_COUNT);
    const u32 vertex_stride = mf_get_u32(bytes + MF_OFF_VERTEX_STRIDE);
    const u32 name_length = mf_get_u32(bytes + MF_OFF_NAME_LENGTH);

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

    mesh_vertex_t *vertices = (mesh_vertex_t *)arena_alloc(arena, vertex_bytes);
    u32           *indices =
        (index_bytes > 0u) ? (u32 *)arena_alloc(arena, index_bytes) : NULL;
    // +1：文件里本应含 NUL，多给一字节兜住"作者忘了写 NUL"的坏文件。
    char *name = (char *)arena_alloc(arena, (usize)name_length + 1u);
    if (vertices == NULL || name == NULL || (index_bytes > 0u && indices == NULL)) {
        LOG_ERROR(".mesh 解析失败：arena 内存不足");
        return false;
    }

    usize offset = MESH_FILE_HEADER_SIZE;
    memcpy(vertices, bytes + offset, vertex_bytes);
    offset += vertex_bytes;
    if (index_bytes > 0u) {
        memcpy(indices, bytes + offset, index_bytes);
        offset += index_bytes;
    }
    memcpy(name, bytes + offset, name_length);
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

    mf_get_bounds(bytes + MF_OFF_BOUNDS_MIN, &out_file->bounds_min);
    mf_get_bounds(bytes + MF_OFF_BOUNDS_MAX, &out_file->bounds_max);
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
