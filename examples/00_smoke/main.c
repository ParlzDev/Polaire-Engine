// =============================================================================
//  examples/00_smoke/main.c —— M0 冒烟示例
//
//  这个程序的作用不是"演示某个 API"，而是回答一个问题：
//  M0 的四块地基（内存、容器、数学、日志）能不能真的拼起来用？
//
//  它做一件完整的小事：模拟 10 帧的"场景更新 + 相机投影 + 统计输出"，
//  全过程只使用引擎自身提供的内存设施，最后用追踪分配器证明零泄漏。
//
//  构建并运行：
//      cmake --preset debug && cmake --build --preset debug
//      ./build/debug/examples/00_smoke/00_smoke.exe
// =============================================================================
#include <stdio.h>

#include "engine/engine.h"

#define MAX_ENTITIES 64
#define FRAMES 10

typedef struct entity_t {
    vec3_t position;
    vec3_t velocity;
    f32    radius;
    u32    name_hash;
} entity_t;

typedef struct scene_t {
    pool_t   entities;
    arena_t  frame_arena;
    array_t  visible;    // 本帧可见实体（handle_t）
    hash_t   name_lookup; // 名字哈希 -> 实体索引
    mat4_t   view;
    mat4_t   projection;
} scene_t;

static const char *const k_entity_names[] = {
    "player", "enemy_a", "enemy_b", "coin", "platform", "camera_target",
};

// 名字表的键 = 名字哈希的低 32 位 << 16 | 序号低 16 位。
//
// 为什么必须包成函数：之前构建和查询两处各写了一遍同样的表达式，
// 但一处先把哈希截断成 u32、另一处没有，结果键根本对不上 ——
// 表现为"数据明明插进去了，查却查不到"。键的拼法只应该有一个出处。
static u64 make_name_key(const char *name, u32 index)
{
    const u32 name_hash = (u32)hash_str(name);
    return ((u64)name_hash << 16) | (u64)(index & 0xFFFFu);
}

static void scene_build(scene_t *scene, allocator_t *alloc)
{
    if (!pool_init(&scene->entities, alloc, sizeof(entity_t), MAX_ENTITIES, "entities")) {
        LOG_FATAL("实体池创建失败");
        return;
    }
    if (!arena_init(&scene->frame_arena, alloc, ENGINE_KB(32), "frame")) {
        LOG_FATAL("帧 arena 创建失败");
        return;
    }
    if (!array_init(&scene->visible, alloc, sizeof(handle_t), 16)) {
        LOG_FATAL("可见列表创建失败");
        return;
    }
    if (!hash_init(&scene->name_lookup, alloc, 0)) {
        LOG_FATAL("名字表创建失败");
        return;
    }

    // 相机固定在 (0, 3, 12)，看向 (0, 1, 0)
    scene->view = mat4_look_at(vec3_make(0.0f, 3.0f, 12.0f), vec3_make(0.0f, 1.0f, 0.0f),
                               vec3_unit_y());
    scene->projection = mat4_perspective(math_radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);

    // 创建一批实体
    for (u32 i = 0; i < MAX_ENTITIES; ++i) {
        const handle_t handle = pool_alloc(&scene->entities);
        if (!handle_is_valid(handle)) {
            break;
        }

        entity_t *entity = POOL_GET_AS(&scene->entities, handle, entity_t);
        entity->position = vec3_make((f32)(i % 8) - 3.5f, 0.0f, (f32)(i / 8) * 2.0f - 6.0f);
        entity->velocity = vec3_make(0.0f, 0.0f, 0.35f);
        entity->radius = 0.25f + (f32)(i % 4) * 0.1f;

        const char *name = k_entity_names[i % ENGINE_ARRAY_COUNT(k_entity_names)];
        entity->name_hash = (u32)hash_str(name);

        // 键里带上序号，避免同名实体互相覆盖
        hash_set(&scene->name_lookup, make_name_key(name, i), (u64)i);
    }
}

static void scene_destroy(scene_t *scene)
{
    hash_destroy(&scene->name_lookup);
    array_destroy(&scene->visible);
    arena_destroy(&scene->frame_arena);
    pool_destroy(&scene->entities);
}

static void scene_update(scene_t *scene, f32 delta_time)
{
    // 帧临时数据走 arena：reset 是 O(块数)，不产生任何系统分配
    arena_reset(&scene->frame_arena);
    array_clear(&scene->visible);

    vec3_t *scratch_positions = ARENA_NEW_ARRAY(&scene->frame_arena, vec3_t, MAX_ENTITIES);
    if (scratch_positions == NULL) {
        LOG_ERROR("帧 arena 耗尽");
        return;
    }

    const mat4_t view_projection = mat4_mul(scene->projection, scene->view);

    u32 slot = 0;
    pool_iter_t it;
    pool_iter_begin(&it, &scene->entities);

    handle_t handle = HANDLE_INVALID;
    void    *ptr = NULL;
    while (pool_iter_next(&it, &handle, &ptr)) {
        entity_t *entity = (entity_t *)ptr;

        // 半隐式欧拉积分
        entity->position = vec3_mul_add(entity->position, entity->velocity, delta_time);

        // 超出边界就绕回来
        if (entity->position.z > 8.0f) {
            entity->position.z = -8.0f;
        }

        scratch_positions[slot++] = entity->position;

        // 视锥剔除（这里用简化判据：相机前方且在近远平面之间）
        const vec4_t clip = mat4_mul_vec4(view_projection, vec4_make(entity->position.x,
                                                                     entity->position.y,
                                                                     entity->position.z, 1.0f));
        if (clip.w > 0.0f) {
            const vec3_t ndc = vec4_perspective_divide(clip);
            const f32    margin = entity->radius;
            if (ndc.x >= -1.0f - margin && ndc.x <= 1.0f + margin && ndc.y >= -1.0f - margin &&
                ndc.y <= 1.0f + margin && ndc.z >= 0.0f && ndc.z <= 1.0f) {
                ARRAY_PUSH(&scene->visible, handle_t, handle);
            }
        }
    }
}

int main(void)
{
    // ---- 用追踪分配器包住堆，结束时证明零泄漏 ----
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    if (!log_init("00_smoke.log", LOG_LEVEL_TRACE)) {
        fprintf(stderr, "无法打开日志文件，继续使用控制台输出\n");
    }

    LOG_INFO("MyEngine %s —— M0 冒烟示例", MYENGINE_VERSION_STRING);
    LOG_INFO("日志同时输出到控制台与 00_smoke.log");

    // 打开 FTZ/DAZ：游戏中出现的非规格化数几乎都是"已经衰减到没意义"的残留，
    // 清掉它们可以避免随机的性能尖刺。
    math_set_flush_denormals(true);
    LOG_DEBUG("已启用 FTZ/DAZ（非规格化数清零）");

    scene_t scene;
    scene_build(&scene, &tracker.base);

    LOG_INFO("场景就绪: 实体池容量 %u，名字表 %u 项", pool_capacity(&scene.entities),
             hash_size(&scene.name_lookup));

    const f32 delta_time = 1.0f / 60.0f;

    for (i32 frame = 0; frame < FRAMES; ++frame) {
        scene_update(&scene, delta_time);

        LOGT_TRACE("frame", "第 %d 帧: 可见 %u / %u，帧 arena 已用 %llu 字节", frame,
                   array_size(&scene.visible), pool_count(&scene.entities),
                   (unsigned long long)arena_bytes_used(&scene.frame_arena));
    }

    // ---- 数学链路验证：把一个实体投影到屏幕 ----
    {
        // 取第一个存活实体。用迭代器而不是硬编码句柄：
        // 句柄的代际号在运行期才知道，写死一个索引是拿不到东西的。
        const entity_t *entity = NULL;
        pool_iter_t     it;
        handle_t        h = HANDLE_INVALID;
        void           *p = NULL;
        pool_iter_begin(&it, &scene.entities);
        if (pool_iter_next(&it, &h, &p)) {
            entity = (const entity_t *)p;
        }

        if (entity != NULL) {
            const mat4_t vp = mat4_mul(scene.projection, scene.view);
            const vec3_t ndc = vec4_perspective_divide(
                mat4_mul_vec4(vp, vec4_make(entity->position.x, entity->position.y,
                                            entity->position.z, 1.0f)));

            LOG_INFO("世界坐标 (%.2f, %.2f, %.2f) -> NDC (%.3f, %.3f, %.3f)",
                     (f64)entity->position.x, (f64)entity->position.y, (f64)entity->position.z,
                     (f64)ndc.x, (f64)ndc.y, (f64)ndc.z);

            const bool ndc_ok = ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f &&
                                ndc.y <= 1.0f && ndc.z >= 0.0f && ndc.z <= 1.0f;
            if (ndc_ok) {
                LOG_INFO("NDC 落在裁剪体内部，投影链路自洽");
            } else {
                LOG_WARN("NDC 越界，投影链路有问题");
            }
        }
    }

    // ---- 名字查找 ----
    {
        u64 found = 0;
        if (hash_get(&scene.name_lookup, make_name_key("player", 0), &found)) {
            LOG_INFO("名字表命中: \"player\" #0 -> 实体 #%llu", (unsigned long long)found);
        } else {
            LOG_WARN("名字表未命中");
        }
    }

    // ---- 统计（必须在 destroy 之前取，destroy 会把池结构清零）----
    const u32 pool_peak_value = pool_peak(&scene.entities);

    scene_destroy(&scene);

    LOG_INFO("内存统计: 分配 %llu 次 / 释放 %llu 次 / 峰值 %llu 字节",
             (unsigned long long)tracker.alloc_calls, (unsigned long long)tracker.free_calls,
             (unsigned long long)tracker.peak_bytes);

    // ---- 验收：零泄漏 ----
    const bool leaked = allocator_tracking_has_leak(&tracker);

    if (leaked) {
        LOG_ERROR("检测到内存泄漏: 仍有 %llu 块 / %llu 字节未释放",
                  (unsigned long long)tracker.live_blocks,
                  (unsigned long long)tracker.bytes_in_use);
    } else {
        LOG_INFO("零泄漏：全部 %llu 字节已归还",
                 (unsigned long long)tracker.total_bytes);
    }

    const u64 log_lines = log_lines_written();

    log_shutdown();

    printf("\n");
    printf("========== M0 冒烟结果 ==========\n");
    printf("  实体池峰值    : %u\n", pool_peak_value);
    printf("  累计分配次数  : %llu\n", (unsigned long long)tracker.alloc_calls);
    printf("  累计释放次数  : %llu\n", (unsigned long long)tracker.free_calls);
    printf("  峰值内存      : %llu 字节\n", (unsigned long long)tracker.peak_bytes);
    printf("  内存泄漏      : %s\n", leaked ? "有" : "无");
    printf("  日志行数      : %llu\n", (unsigned long long)log_lines);
    printf("=================================\n");

    return leaked ? 1 : 0;
}
