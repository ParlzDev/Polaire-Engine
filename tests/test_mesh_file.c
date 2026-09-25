// =============================================================================
//  tests/test_mesh_file.c —— 自研二进制网格格式
//
//  这组测试的判据思路：
//
//  1. **往返一致**只证明 encode/decode 自洽，不证明字节布局是对的 ——
//     一个把字段顺序写反的实现同样能自洽地往返。所以字节布局单独用
//     "直接核对文件里的原始字节"来验，判据是头文件里那张偏移表，
//     而不是 mesh_file_build 自己。
//
//  2. 坏输入必须**拒绝**而不是返回半成品。这个格式是给 M7 的热重载和
//     打包用的，那时读到的字节可能来自被截断/被改坏的包。
//     索引越界尤其要拦：驱动不会报错，只会随机花屏。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <string.h>

#include "engine/core/memory.h"
#include "engine/render/mesh_file.h"

#define TEST_ARENA_BLOCK (256u * 1024u)

// 一个最小的三角形：3 顶点 / 3 索引，数值都取"能一眼看出错位"的值。
static void make_triangle(mesh_vertex_t vertices[3], u32 indices[3])
{
    for (u32 i = 0; i < 3u; ++i) {
        vertices[i].position[0] = (f32)i + 0.5f;
        vertices[i].position[1] = (f32)i - 1.25f;
        vertices[i].position[2] = (f32)i * 2.0f;
        vertices[i].normal[0] = 0.0f;
        vertices[i].normal[1] = 1.0f;
        vertices[i].normal[2] = (f32)i;
        vertices[i].texcoord[0] = (f32)i * 0.25f;
        vertices[i].texcoord[1] = 1.0f - (f32)i * 0.25f;
        indices[i] = i;
    }
}

static arena_t test_arena_begin(allocator_tracking_t *tracker, const char *name)
{
    arena_t arena;
    allocator_tracking_init(tracker, allocator_heap());
    const bool ok = arena_init(&arena, &tracker->base, TEST_ARENA_BLOCK, name);
    EXPECT_TRUE(ok);
    return arena;
}

// ---------------------------------------------------------------- 往返

static void mesh_file_roundtrip_preserves_geometry(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_roundtrip");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    const vec3_t min = vec3_make(-1.5f, -2.5f, -3.5f);
    const vec3_t max = vec3_make(1.5f, 2.5f, 3.5f);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, &min, &max, "triangle", &bytes,
                                &size));
    EXPECT_NOT_NULL(bytes);
    EXPECT_GT_U64(size, MESH_FILE_HEADER_SIZE);

    mesh_file_t parsed;
    EXPECT_TRUE(mesh_file_parse(&arena, bytes, size, &parsed));

    EXPECT_EQ_U64(parsed.vertex_count, 3u);
    EXPECT_EQ_U64(parsed.index_count, 3u);
    EXPECT_STR_EQ(parsed.name, "triangle");
    EXPECT_VEC3_NEAR(parsed.bounds_min, min, 1e-6f);
    EXPECT_VEC3_NEAR(parsed.bounds_max, max, 1e-6f);
    EXPECT_TRUE(memcmp(parsed.vertices, vertices, sizeof(vertices)) == 0);
    EXPECT_TRUE(memcmp(parsed.indices, indices, sizeof(indices)) == 0);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void mesh_file_roundtrip_without_indices(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_no_index");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    // 非索引图元（glDrawArrays 那类）也要能存
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, NULL, 0u, NULL, NULL, NULL, &bytes, &size));

    mesh_file_t parsed;
    EXPECT_TRUE(mesh_file_parse(&arena, bytes, size, &parsed));
    EXPECT_EQ_U64(parsed.index_count, 0u);
    EXPECT_NULL(parsed.indices);
    EXPECT_TRUE(memcmp(parsed.vertices, vertices, sizeof(vertices)) == 0);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// ---------------------------------------------------------------- 字节布局
//
// 判据来自头文件里那张偏移表，独立于 mesh_file_build 的实现。

static void mesh_file_header_bytes_match_spec(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_bytes");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, NULL, NULL, "ab", &bytes, &size));

    const u8 *raw = (const u8 *)bytes;

    // 魔数按字节是 'M','E','S','H'
    EXPECT_EQ_U64(raw[0], (usize)'M');
    EXPECT_EQ_U64(raw[1], (usize)'E');
    EXPECT_EQ_U64(raw[2], (usize)'S');
    EXPECT_EQ_U64(raw[3], (usize)'H');

    // 小端 u32：值 3 写成 03 00 00 00
    EXPECT_EQ_U64(raw[8], 3u);
    EXPECT_EQ_U64(raw[9], 0u);
    EXPECT_EQ_U64(raw[10], 0u);
    EXPECT_EQ_U64(raw[11], 0u);
    EXPECT_EQ_U64(raw[12], 3u);

    // 顶点步长 = sizeof(mesh_vertex_t) = 32
    EXPECT_EQ_U64(raw[16], 32u);

    // 名字长度含 NUL："ab" -> 3
    EXPECT_EQ_U64(raw[20], 3u);

    // reserved 区必须为 0，否则将来的 flags 语义会踩到旧文件的垃圾数据
    for (u32 i = 48u; i < MESH_FILE_HEADER_SIZE; ++i) {
        EXPECT_EQ_U64(raw[i], 0u);
    }

    // 段落顺序：头部之后立刻是顶点数据（第一个 f32 = 0.5f）
    f32 first_x = 0.0f;
    memcpy(&first_x, raw + MESH_FILE_HEADER_SIZE, sizeof(f32));
    EXPECT_NEAR(first_x, 0.5f, 1e-6f);

    // 名字在索引之后
    EXPECT_EQ_U64(raw[MESH_FILE_HEADER_SIZE + 3u * 32u + 3u * 4u], (usize)'a');

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// ---------------------------------------------------------------- 坏输入拒绝

// 把合法文件按长度截断，解析必须失败（而不是读到越界内存或返回半份数据）。
static void mesh_file_rejects_truncated(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_truncated");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, NULL, NULL, "tri", &bytes, &size));

    mesh_file_t parsed;
    // 少一个字节：整个顶点/索引/名字区都可能在边界上
    EXPECT_FALSE(mesh_file_parse(&arena, bytes, size - 1u, &parsed));
    // 只剩头部：连顶点区都是空的
    EXPECT_FALSE(mesh_file_parse(&arena, bytes, MESH_FILE_HEADER_SIZE, &parsed));
    // 比头部还小
    EXPECT_FALSE(mesh_file_parse(&arena, bytes, 8u, &parsed));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void mesh_file_rejects_bad_header_fields(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_bad_header");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, NULL, NULL, "tri", &bytes, &size));

    u8 *raw = (u8 *)bytes;
    mesh_file_t parsed;

    // 魔数不对 —— 八成是把 OBJ 当 .mesh 喂进来了
    raw[0] = 'X';
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));
    raw[0] = 'M';

    // 版本不认识
    raw[4] = 99u;
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));
    raw[4] = 1u;

    // 顶点步长变了：说明 mesh_vertex_t 与文件不是一回事，硬读会得到错位的法线
    raw[16] = 24u;
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));
    raw[16] = 32u;

    // 顶点数 0：合法文件不会长这样
    raw[8] = 0u;
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));
    raw[8] = 3u;

    // 名字长度超过上限
    raw[20] = 250u;
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));
    raw[20] = 3u;

    // 改完之后必须还能正常解析 —— 否则上面的"失败"可能只是碰巧
    EXPECT_TRUE(mesh_file_parse(&arena, raw, size, &parsed));
    EXPECT_EQ_U64(parsed.vertex_count, 3u);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void mesh_file_rejects_out_of_range_index(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_bad_index");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, NULL, NULL, "tri", &bytes, &size));

    u8 *raw = (u8 *)bytes;
    // 索引区起点 = 头部 + 3 * 32；把最后一个索引改成 999（越界）
    const usize last_index = MESH_FILE_HEADER_SIZE + 3u * 32u + 2u * 4u;
    raw[last_index] = 0xE7u; // 999 = 0x3E7
    raw[last_index + 1u] = 0x03u;

    mesh_file_t parsed;
    EXPECT_FALSE(mesh_file_parse(&arena, raw, size, &parsed));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void mesh_file_rejects_absurd_counts(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_absurd");

    // 头部写一个巨大的顶点数：数量 * 32 会溢出，必须在分配之前就被拦住。
    // 不拦的话就是"读一个坏文件 → 申请天文数字内存 → 分配失败 → 解引用 NULL"。
    u8 header[MESH_FILE_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    header[0] = 'M';
    header[1] = 'E';
    header[2] = 'S';
    header[3] = 'H';
    header[4] = 1u;                        // version
    header[8] = 0xFFu;                     // vertex_count = 0xFFFFFFFF
    header[9] = 0xFFu;
    header[10] = 0xFFu;
    header[11] = 0xFFu;
    header[16] = 32u;                      // stride
    header[20] = 1u;                       // name_length

    mesh_file_t parsed;
    EXPECT_FALSE(mesh_file_parse(&arena, header, sizeof(header), &parsed));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// ---------------------------------------------------------------- desc 转换

static void mesh_file_desc_matches_geometry(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_desc");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(mesh_file_build(&arena, vertices, 3u, indices, 3u, NULL, NULL, "tri", &bytes, &size));

    mesh_file_t parsed;
    EXPECT_TRUE(mesh_file_parse(&arena, bytes, size, &parsed));

    const mesh_desc_t desc = mesh_file_to_desc(&parsed);
    EXPECT_EQ_U64(desc.vertex_count, 3u);
    EXPECT_EQ_U64(desc.index_count, 3u);
    EXPECT_EQ_U64(desc.index_data_size, 12u);
    EXPECT_EQ_U64(desc.bindings[0].size, 3u * 32u);
    EXPECT_TRUE(desc.index_format == INDEX_FORMAT_U32);
    EXPECT_STR_EQ(desc.debug_name, "tri");

    // 布局得是引擎的标准"位置+法线+UV"，否则内置着色器会读到错位属性
    const vertex_layout_t expected = vertex_layout_mesh_vertex();
    EXPECT_EQ_U64(desc.layout.attribute_count, expected.attribute_count);
    EXPECT_EQ_U64(desc.layout.bindings[0].stride, expected.bindings[0].stride);

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// ---------------------------------------------------------------- 文件往返

static void mesh_file_save_then_load(void)
{
    allocator_tracking_t tracker;
    arena_t              arena = test_arena_begin(&tracker, "test_mesh_file_io");

    mesh_vertex_t vertices[3];
    u32           indices[3];
    make_triangle(vertices, indices);

    const vec3_t min = vec3_make(-0.5f, -0.5f, 0.0f);
    const vec3_t max = vec3_make(2.5f, 2.5f, 4.0f);

    const char *path = "mesh_file_test.tmp.mesh";
    EXPECT_TRUE(mesh_file_save(&arena, path, vertices, 3u, indices, 3u, &min, &max, "on_disk"));

    mesh_file_t parsed;
    EXPECT_TRUE(mesh_file_load(&arena, path, &parsed));
    EXPECT_EQ_U64(parsed.vertex_count, 3u);
    EXPECT_STR_EQ(parsed.name, "on_disk");
    EXPECT_VEC3_NEAR(parsed.bounds_min, min, 1e-6f);
    EXPECT_TRUE(memcmp(parsed.vertices, vertices, sizeof(vertices)) == 0);

    // 临时文件用完就删：留着会让"仓库干净"变成一件要人记着做的事
    remove(path);

    // 不存在的文件必须失败而不是崩溃
    mesh_file_t missing;
    EXPECT_FALSE(mesh_file_load(&arena, "mesh_file_test.definitely-absent.mesh", &missing));

    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// =============================================================================
void test_mesh_file_register(void)
{
    test_register("mesh_file", "roundtrip_preserves_geometry", mesh_file_roundtrip_preserves_geometry);
    test_register("mesh_file", "roundtrip_without_indices", mesh_file_roundtrip_without_indices);
    test_register("mesh_file", "header_bytes_match_spec", mesh_file_header_bytes_match_spec);
    test_register("mesh_file", "rejects_truncated", mesh_file_rejects_truncated);
    test_register("mesh_file", "rejects_bad_header_fields", mesh_file_rejects_bad_header_fields);
    test_register("mesh_file", "rejects_out_of_range_index", mesh_file_rejects_out_of_range_index);
    test_register("mesh_file", "rejects_absurd_counts", mesh_file_rejects_absurd_counts);
    test_register("mesh_file", "desc_matches_geometry", mesh_file_desc_matches_geometry);
    test_register("mesh_file", "save_then_load", mesh_file_save_then_load);
}
