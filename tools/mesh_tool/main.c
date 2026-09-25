// =============================================================================
//  tools/mesh_tool/main.c —— OBJ -> .mesh 离线转换器
//
//  用途：
//      mesh_tool <input.obj> <output.mesh> [--name NAME]
//
//  为什么要有这个工具（而不是运行时直接读 OBJ）：
//  OBJ 是文本源格式，每次加载都要重做一遍词法解析和面角重建；
//  .mesh 是"解析结果"的落盘，运行时只需要一次 memcpy。
//  转换是离线动作，代价只在美术导入时付一次。
//
//  顺带把两边的耗时和体积都打出来 —— 这样"要不要预烘焙"是个能看数据的决定，
//  而不是信仰问题。
// =============================================================================
#include <stdio.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/memory.h"
#include "engine/platform/time.h"
#include "engine/render/mesh_file.h"
#include "engine/render/obj_loader.h"

#define TOOL_ARENA_BLOCK (8u * 1024u * 1024u)

static void print_usage(void)
{
    printf("用法: mesh_tool <input.obj> <output.mesh> [--name NAME]\n");
    printf("\n");
    printf("  --name NAME   写进 .mesh 的调试名（默认用输出文件名）\n");
    printf("  -h, --help    显示本帮助\n");
}

// 读整个文件到 arena，返回字节数（失败返回 0）。
static bool read_whole_file(arena_t *arena, const char *path, void **out_data, usize *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开文件: %s", path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    rewind(file);
    if (length <= 0) {
        LOG_ERROR("文件为空: %s", path);
        fclose(file);
        return false;
    }

    char *buffer = (char *)arena_alloc(arena, (usize)length + 1u);
    if (buffer == NULL) {
        LOG_ERROR("arena 无法容纳 %s（%ld 字节）", path, length);
        fclose(file);
        return false;
    }
    if (fread(buffer, 1u, (usize)length, file) != (usize)length) {
        LOG_ERROR("读取不完整: %s", path);
        fclose(file);
        return false;
    }
    fclose(file);

    buffer[length] = '\0';
    *out_data = buffer;
    *out_size = (usize)length;
    return true;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *name = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (input_path == NULL) {
            input_path = argv[i];
        } else if (output_path == NULL) {
            output_path = argv[i];
        } else {
            fprintf(stderr, "多余的参数: %s\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    if (input_path == NULL || output_path == NULL) {
        print_usage();
        return 2;
    }

    log_init(NULL, LOG_LEVEL_DEBUG);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    arena_t arena;
    if (!arena_init(&arena, &tracker.base, TOOL_ARENA_BLOCK, "mesh_tool")) {
        LOG_ERROR("arena 初始化失败");
        return 1;
    }

    platform_time_init(false);

    // ---- 路径 A：读盘 + 解析 OBJ ----
    const platform_time_point_t obj_begin = platform_time_now();
    void                       *obj_data = NULL;
    usize                       obj_size = 0u;
    if (!read_whole_file(&arena, input_path, &obj_data, &obj_size)) {
        return 1;
    }
    obj_mesh_t mesh;
    if (!obj_load_from_memory(&arena, obj_data, obj_size, input_path, &mesh)) {
        return 1;
    }
    const f64 obj_seconds = platform_time_delta_seconds(obj_begin, platform_time_now());

    // ---- 写出 .mesh ----
    char fallback_name[128];
    if (name == NULL) {
        snprintf(fallback_name, sizeof(fallback_name), "%s", output_path);
        name = fallback_name;
    }

    if (!mesh_file_save(&arena, output_path, mesh.vertices, mesh.vertex_count, mesh.indices,
                        mesh.index_count, &mesh.bounds_min, &mesh.bounds_max, name)) {
        return 1;
    }

    // ---- 路径 B：读盘 + 解析 .mesh ----
    // 回读同时充当校验：只验"写得出去"不够，读不回来的格式等于没有格式。
    const platform_time_point_t mesh_begin = platform_time_now();
    mesh_file_t                 parsed;
    if (!mesh_file_load(&arena, output_path, &parsed)) {
        LOG_ERROR("回读 .mesh 失败，转换结果不可用: %s", output_path);
        return 1;
    }
    const f64 mesh_seconds = platform_time_delta_seconds(mesh_begin, platform_time_now());

    bool identical = (parsed.vertex_count == mesh.vertex_count) &&
                     (parsed.index_count == mesh.index_count) &&
                     (memcmp(parsed.vertices, mesh.vertices,
                             (usize)mesh.vertex_count * sizeof(mesh_vertex_t)) == 0) &&
                     (memcmp(parsed.indices, mesh.indices,
                             (usize)mesh.index_count * sizeof(u32)) == 0);

    // .mesh 的大小直接问文件系统：比让保存函数返回长度更实在。
    usize mesh_size = 0u;
    void *mesh_bytes = NULL;
    if (read_whole_file(&arena, output_path, &mesh_bytes, &mesh_size)) {
        // 已经读进 arena，这里只用长度
    }

    printf("\n========== 转换结果 ==========\n");
    printf("  输入      : %s（%llu 字节，文本）\n", input_path, (unsigned long long)obj_size);
    printf("  输出      : %s（%llu 字节，二进制）\n", output_path, (unsigned long long)mesh_size);
    printf("  顶点/索引 : %u / %u\n", mesh.vertex_count, mesh.index_count);
    printf("  包围盒 min: (%.3f, %.3f, %.3f)\n", (double)mesh.bounds_min.x,
           (double)mesh.bounds_min.y, (double)mesh.bounds_min.z);
    printf("  包围盒 max: (%.3f, %.3f, %.3f)\n", (double)mesh.bounds_max.x,
           (double)mesh.bounds_max.y, (double)mesh.bounds_max.z);
    // 两侧都是"读盘 + 解析"的端到端时间，可直接比较。
    // 小模型上读盘占主导，差距看不出来；解析成本随顶点数线性增长，
    // 所以这个收益要到几万顶点才明显 —— 别拿一个立方体的数字下结论。
    printf("  OBJ  加载 : %.3f ms（读盘 + 文本解析）\n", (double)(obj_seconds * 1000.0));
    printf("  .mesh加载 : %.3f ms（读盘 + 一次 memcpy）", (double)(mesh_seconds * 1000.0));
    if (mesh_seconds > 0.0 && obj_seconds > mesh_seconds) {
        printf("   （快 %.1f 倍）", (double)(obj_seconds / mesh_seconds));
    } else if (mesh_seconds > 0.0) {
        printf("   （本例规模太小，差异被读盘耗时淹没）");
    }
    printf("\n");
    printf("  回读一致  : %s\n", identical ? "是" : "否（数据不匹配！）");
    printf("==============================\n");

    arena_destroy(&arena);
    platform_time_shutdown();

    if (allocator_tracking_has_leak(&tracker)) {
        LOG_ERROR("内存泄漏: 仍有 %llu 块 / %llu 字节未归还",
                  (unsigned long long)tracker.live_blocks,
                  (unsigned long long)tracker.bytes_in_use);
        return 1;
    }
    LOG_INFO("零泄漏：%llu 字节全部归还", (unsigned long long)tracker.total_bytes);
    return identical ? 0 : 1;
}
