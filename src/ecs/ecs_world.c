// =============================================================================
//  src/ecs/ecs_world.c —— 世界生命周期、实体身份、dense id、统计
// =============================================================================
#include "engine/ecs/world.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "ecs_internal.h"

// 每类存储的字节数，用于"容量是量出来的"这句话能被核对。
static usize ecs_storage_bytes(const ecs_storage_t *s)
{
    if (!s->registered) {
        return 0u;
    }
    return (usize)s->capacity * (usize)s->stride + (usize)s->capacity * sizeof(entity_t) +
           (usize)s->index_space * sizeof(u32) + s->presence_words * sizeof(u64);
}

ecs_world_config_t ecs_world_config_default(allocator_t *allocator)
{
    ecs_world_config_t config;
    memset(&config, 0, sizeof(config));
    config.allocator = (allocator != NULL) ? allocator : allocator_heap();
    config.entity_capacity = 16384u;
    config.default_component_capacity = 4096u;
    config.command_capacity = 4096u;
    config.hierarchy_depth_max = 32u;
    config.name = "ecs_world";
    return config;
}

static bool ecs_world_init(ecs_world_t *world, const ecs_world_config_t *config);
static void ecs_world_shutdown(ecs_world_t *world);

ecs_world_t *ecs_world_create(allocator_t *allocator, const ecs_world_config_t *config)
{
    if (config == NULL) {
        LOG_ERROR("ecs_world_create 缺少 config");
        return NULL;
    }
    allocator_t *alloc = (allocator != NULL) ? allocator : config->allocator;
    if (alloc == NULL) {
        LOG_ERROR("ecs_world_create 缺少分配器");
        return NULL;
    }

    ecs_world_t *world = (ecs_world_t *)alloc->alloc(alloc, sizeof(ecs_world_t), 16);
    if (world == NULL) {
        LOG_ERROR("世界结构体分配失败（%llu 字节）", (unsigned long long)sizeof(ecs_world_t));
        return NULL;
    }
    if (!ecs_world_init(world, config)) {
        alloc->free(alloc, world, sizeof(ecs_world_t));
        return NULL;
    }
    return world;
}

void ecs_world_destroy(ecs_world_t *world)
{
    if (world == NULL) {
        return;
    }
    allocator_t *alloc = world->config.allocator;
    ecs_world_shutdown(world);
    alloc->free(alloc, world, sizeof(ecs_world_t));
}

static bool ecs_world_init(ecs_world_t *world, const ecs_world_config_t *config)
{
    if (world == NULL || config == NULL || config->allocator == NULL) {
        LOG_ERROR("ecs_world_init 参数不完整");
        return false;
    }
    if (config->entity_capacity < 2u) {
        LOG_ERROR("entity_capacity 至少要是 2（0 号槽永久保留）：给了 %u",
                  config->entity_capacity);
        return false;
    }

    memset(world, 0, sizeof(*world));
    world->config = *config;

    if (!pool_init(&world->entities, config->allocator, sizeof(ecs_entity_record_t),
                   config->entity_capacity, "ecs_entities")) {
        LOG_ERROR("实体池初始化失败（容量 %u）", config->entity_capacity);
        return false;
    }

    // row_of / presence 都以 handle_index 为下标，而 handle 的 index 位宽是 24 位。
    // 容量若超过这个上限，index 会被截断成另一个槽位 —— 那是静默的错乱，
    // 所以在这里直接拒绝。
    if (config->entity_capacity > (1u << 24)) {
        LOG_ERROR("entity_capacity %u 超过句柄 index 位宽上限", config->entity_capacity);
        pool_destroy(&world->entities);
        return false;
    }

    const usize dense_bytes = (usize)config->entity_capacity * sizeof(entity_t);
    world->dense_to_entity = (entity_t *)config->allocator->alloc(config->allocator, dense_bytes,
                                                                  16);
    if (world->dense_to_entity == NULL) {
        pool_destroy(&world->entities);
        return false;
    }
    memset(world->dense_to_entity, 0, dense_bytes);
    world->memory_bytes += dense_bytes;

    const usize topo_bytes = (usize)config->entity_capacity * sizeof(entity_t);
    world->topo = (entity_t *)config->allocator->alloc(config->allocator, topo_bytes, 16);
    if (world->topo == NULL) {
        config->allocator->free(config->allocator, world->dense_to_entity, dense_bytes);
        world->dense_to_entity = NULL;
        pool_destroy(&world->entities);
        return false;
    }
    memset(world->topo, 0, topo_bytes);
    world->topo_capacity = config->entity_capacity;
    world->memory_bytes += topo_bytes;

    // 深度缓存按 handle_index 的取值空间开（ecs_handle_index_space 里写明了
    // 为什么要 +1）。少一项不会"最后一个实体查不到"，而是越界读一个从未初始化
    // 的槽位 —— 那个垃圾值会被当成实体的深度，把按层重排的循环放大上亿倍。
    const u32 index_space = ecs_handle_index_space(world);
    const usize depth_bytes = (usize)index_space * sizeof(u32);
    world->depth_cache = (u32 *)config->allocator->alloc(config->allocator, depth_bytes, 16);
    if (world->depth_cache == NULL) {
        // 走到这里 ready 还是 false，而 shutdown() 一进门就查 ready ——
        // 所以失败路径必须自己把已经拿到的内存还回去，否则会漏。
        config->allocator->free(config->allocator, world->topo, topo_bytes);
        world->topo = NULL;
        config->allocator->free(config->allocator, world->dense_to_entity, dense_bytes);
        world->dense_to_entity = NULL;
        pool_destroy(&world->entities);
        return false;
    }
    for (u32 i = 0; i < index_space; ++i) {
        world->depth_cache[i] = ECS_DEPTH_UNKNOWN;
    }
    world->memory_bytes += depth_bytes;

    // 空 dense id 栈：上限就是实体容量（同时存活的实体数不会超过它）。
    const usize dense_free_bytes = (usize)config->entity_capacity * sizeof(dense_id_t);
    world->dense_free =
        (dense_id_t *)config->allocator->alloc(config->allocator, dense_free_bytes, 16);
    if (world->dense_free == NULL) {
        config->allocator->free(config->allocator, world->depth_cache, depth_bytes);
        world->depth_cache = NULL;
        config->allocator->free(config->allocator, world->topo, topo_bytes);
        world->topo = NULL;
        config->allocator->free(config->allocator, world->dense_to_entity, dense_bytes);
        world->dense_to_entity = NULL;
        pool_destroy(&world->entities);
        return false;
    }
    world->dense_free_count = 0u;
    world->memory_bytes += dense_free_bytes;

    if (!array_init(&world->commands, config->allocator, sizeof(ecs_command_t), 256u)) {
        LOG_ERROR("命令队列初始化失败");
        // 这条错误路径以前只 return false，把前面拿到的四块内存全丢下了。
        // 分配失败的清理要一份就够，别等到泄漏检查才想起来。
        config->allocator->free(config->allocator, world->dense_free, dense_free_bytes);
        world->dense_free = NULL;
        config->allocator->free(config->allocator, world->depth_cache, depth_bytes);
        world->depth_cache = NULL;
        config->allocator->free(config->allocator, world->topo, topo_bytes);
        world->topo = NULL;
        config->allocator->free(config->allocator, world->dense_to_entity, dense_bytes);
        world->dense_to_entity = NULL;
        pool_destroy(&world->entities);
        return false;
    }

    world->hierarchy_dirty = true;
    world->dense_dirty = true;
    world->ready = true;
    return true;
}

static void ecs_world_shutdown(ecs_world_t *world)
{
    if (world == NULL || !world->ready) {
        return;
    }

    allocator_t *alloc = world->config.allocator;

    for (u32 i = 0; i < ECS_MAX_COMPONENTS; ++i) {
        ecs_storage_t *s = &world->storage[i];
        if (!s->registered) {
            continue;
        }
        const usize bytes = ecs_storage_bytes(s);
        if (s->data != NULL) {
            alloc->free(alloc, s->data, (usize)s->capacity * (usize)s->stride);
        }
        if (s->owners != NULL) {
            alloc->free(alloc, s->owners, (usize)s->capacity * sizeof(entity_t));
        }
        if (s->row_of != NULL) {
            alloc->free(alloc, s->row_of, (usize)s->index_space * sizeof(u32));
        }
        if (s->presence != NULL) {
            alloc->free(alloc, s->presence, s->presence_words * sizeof(u64));
        }
        world->memory_bytes -= bytes;
        memset(s, 0, sizeof(*s));
    }

    if (world->dense_to_entity != NULL) {
        alloc->free(alloc, world->dense_to_entity,
                    (usize)world->config.entity_capacity * sizeof(entity_t));
        world->dense_to_entity = NULL;
    }
    if (world->topo != NULL) {
        alloc->free(alloc, world->topo, (usize)world->topo_capacity * sizeof(entity_t));
        world->topo = NULL;
    }
    if (world->depth_cache != NULL) {
        // 释放必须用**分配时同一个式子**：这里曾经是 pool_capacity()，
        // 而分配用的是 +1 的取值空间，于是跟踪分配器会在关闭时叫"尺寸不符"。
        alloc->free(alloc, world->depth_cache,
                    (usize)ecs_handle_index_space(world) * sizeof(u32));
        world->depth_cache = NULL;
    }
    if (world->dense_free != NULL) {
        alloc->free(alloc, world->dense_free,
                    (usize)world->config.entity_capacity * sizeof(dense_id_t));
        world->dense_free = NULL;
        world->dense_free_count = 0u;
    }

    array_destroy(&world->commands);
    pool_destroy(&world->entities);

    world->ready = false;
}

bool ecs_world_is_valid(const ecs_world_t *world)
{
    return world != NULL && world->ready;
}

const ecs_storage_t *ecs_storage_of(const ecs_world_t *world, ecs_component_id id)
{
    if (world == NULL || id >= ECS_MAX_COMPONENTS) {
        return NULL;
    }
    const ecs_storage_t *s = &world->storage[id];
    return s->registered ? s : NULL;
}

ecs_storage_t *ecs_storage_of_mut(ecs_world_t *world, ecs_component_id id)
{
    if (world == NULL || id >= ECS_MAX_COMPONENTS) {
        return NULL;
    }
    ecs_storage_t *s = &world->storage[id];
    return s->registered ? s : NULL;
}

// ---------------------------------------------------------------- 实体

entity_t ecs_create_entity(ecs_world_t *world)
{
    ASSERT(world != NULL);
    ASSERT(world->ready);

    const handle_t handle = pool_alloc(&world->entities);
    if (!handle_is_valid(handle)) {
        LOG_ERROR("实体池已满（上限 %u）。提高 ecs_world_config_t.entity_capacity。",
                  world->config.entity_capacity);
        return ENTITY_NONE;
    }

    ecs_entity_record_t *record =
        POOL_GET_AS(&world->entities, handle, ecs_entity_record_t);

    // dense id 优先复用销毁后空出来的号，其次才推进水位线。
    //
    // 只推进不回收的语义是"这个世界总共创建过多少实体"，而不是"同时存在多少"。
    // 编辑器里删一个建一个，几百次之后水位线顶到 entity_capacity，
    // 而 dense_to_entity 只有 capacity 项 —— 新实体进不了那张表，
    // 于是它**不出现在 ecs_collect_entities 里，也就永远不会被存盘**。
    // 那种"实体建成功了但世界看不见它"的状态必须当场失败，不能留。
    dense_id_t dense;
    if (world->dense_free_count > 0u) {
        dense = world->dense_free[--world->dense_free_count];
    } else {
        dense = world->next_dense_id;
        if (dense >= world->config.entity_capacity) {
            LOG_ERROR("dense id 空间已满（容量 %u，已用 %u）—— 同时存活的实体没这么多，"
                      "说明空闲号丢失了",
                      (unsigned)world->config.entity_capacity, (unsigned)world->next_dense_id);
            pool_free(&world->entities, handle);
            return ENTITY_NONE;
        }
        world->next_dense_id++;
    }
    record->dense_id = dense;
    record->reserved = 0u;

    const u32 live = pool_count(&world->entities);
    if (live > world->entity_peak) {
        world->entity_peak = live;
    }
    world->dense_dirty = true;
    return handle;
}

bool ecs_entity_is_alive(const ecs_world_t *world, entity_t entity)
{
    return world != NULL && world->ready && pool_valid(&world->entities, entity);
}

bool ecs_destroy_immediate(ecs_world_t *world, entity_t entity)
{
    if (!ecs_entity_is_alive(world, entity)) {
        return false;
    }
    // 迭代中销毁会移动组件数组，必须先挡住（Release 下也挡：
    // 这条不是"调试辅助"，而是内存安全边界）。
    if (world->active_iterators > 0u) {
        LOG_ERROR("迭代中销毁实体（%u 个活跃迭代器）。系统里请改用 ecs_defer_destroy。",
                  world->active_iterators);
        return false;
    }

    for (u32 i = 0; i < world->type_count; ++i) {
        ecs_storage_t *s = &world->storage[i];
        if (s->registered && ecs_presence_test(s, handle_index(entity))) {
            (void)ecs_remove_immediate(world, entity, i);
        }
    }

    // dense id 归还到空闲栈：pool_free 会把槽位清零，所以要在它之前读。
    // 这一步不做，next_dense_id 就会只涨不降，最终把新实体挡在存档之外
    // （理由见 ecs_create_entity 里那段注释）。
    ecs_entity_record_t *record =
        POOL_GET_AS(&world->entities, entity, ecs_entity_record_t);
    if (record != NULL && record->dense_id < world->config.entity_capacity &&
        world->dense_free_count < world->config.entity_capacity) {
        world->dense_free[world->dense_free_count++] = record->dense_id;
    }

    world->dense_dirty = true;
    world->hierarchy_dirty = true;
    return pool_free(&world->entities, entity);
}

void ecs_world_clear(ecs_world_t *world)
{
    ASSERT(world != NULL);

    // 先摘组件再还身份：反过来会让 storage 里留下指向已归还槽位的 owners。
    for (u32 i = 0; i < world->type_count; ++i) {
        ecs_storage_t *s = &world->storage[i];
        if (!s->registered) {
            continue;
        }
        memset(s->presence, 0, s->presence_words * sizeof(u64));
        s->count = 0u;
    }

    pool_iter_t it;
    pool_iter_begin(&it, &world->entities);
    handle_t handle = HANDLE_INVALID;
    while (pool_iter_next(&it, &handle, NULL)) {
        pool_free(&world->entities, handle);
    }

    array_clear(&world->commands);
    world->next_dense_id = 0u;
    // 水位线归零的同时必须清空空闲号栈：留着旧条目会让同一个 dense 发两次，
    // 而重复的 dense 进存档就是"两个实体的组件互相覆盖"。
    world->dense_free_count = 0u;
    world->dense_count = 0u;
    world->dense_dirty = true;
    world->hierarchy_dirty = true;
}

// ---------------------------------------------------------------- 迭代计数

u32 ecs_active_iterator_count(const ecs_world_t *world)
{
    return (world != NULL) ? world->active_iterators : 0u;
}

void ecs_world_acquire_iterator(ecs_world_t *world)
{
    if (world != NULL) {
        world->active_iterators++;
    }
}

void ecs_world_release_iterator(ecs_world_t *world)
{
    if (world != NULL) {
        ASSERT(world->active_iterators > 0u);
        world->active_iterators--;
    }
}

// ---------------------------------------------------------------- dense id

void ecs_rebuild_dense_index(ecs_world_t *world)
{
    ASSERT(world != NULL);

    memset(world->dense_to_entity, 0,
           (usize)world->config.entity_capacity * sizeof(entity_t));
    world->dense_count = 0u;

    pool_iter_t it;
    pool_iter_begin(&it, &world->entities);
    handle_t handle = HANDLE_INVALID;
    ecs_entity_record_t *record = NULL;
    while (pool_iter_next(&it, &handle, (void **)&record)) {
        const dense_id_t dense = record->dense_id;
        if (dense < world->config.entity_capacity) {
            world->dense_to_entity[dense] = handle;
            if (dense + 1u > world->dense_count) {
                world->dense_count = dense + 1u;
            }
        }
    }
    world->dense_dirty = false;
}

dense_id_t ecs_entity_to_dense(const ecs_world_t *world, entity_t entity)
{
    if (!ecs_entity_is_alive(world, entity)) {
        return ECS_DENSE_NONE;
    }
    const ecs_entity_record_t *record =
        POOL_GET_AS(&world->entities, entity, const ecs_entity_record_t);
    return (record != NULL) ? record->dense_id : ECS_DENSE_NONE;
}

entity_t ecs_entity_from_dense(ecs_world_t *world, dense_id_t dense)
{
    if (world == NULL || !world->ready) {
        return ENTITY_NONE;
    }
    if (world->dense_dirty) {
        ecs_rebuild_dense_index(world);
    }
    if (dense >= world->config.entity_capacity) {
        return ENTITY_NONE;
    }
    return world->dense_to_entity[dense];
}

u32 ecs_collect_entities(ecs_world_t *world, entity_t *out, u32 max)
{
    if (world == NULL || !world->ready) {
        return 0u;
    }

    // 直接走 dense_to_entity：重建之后它天然按 dense 升序，
    // 于是不需要排序、也不需要临时缓冲。
    if (world->dense_dirty) {
        ecs_rebuild_dense_index(world);
    }

    u32 emitted = 0u;
    for (u32 dense = 0u; dense < world->dense_count; ++dense) {
        const entity_t e = world->dense_to_entity[dense];
        if (!entity_is_valid(e) || !pool_valid(&world->entities, e)) {
            continue;
        }
        if (out != NULL) {
            if (emitted >= max) {
                LOG_ERROR("实体数组只装得下 %u 个 —— 结果被截断", max);
                return emitted;
            }
            out[emitted] = e;
        }
        emitted++;
    }
    return emitted;
}

// ---------------------------------------------------------------- 统计

void ecs_world_get_stats(const ecs_world_t *world, ecs_world_stats_t *out)
{
    if (world == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->entity_capacity = world->config.entity_capacity;
    out->entities_alive = pool_count(&world->entities);
    out->entity_peak = world->entity_peak;
    out->component_types = world->type_count;

    for (u32 i = 0; i < world->type_count; ++i) {
        const ecs_storage_t *s = &world->storage[i];
        if (!s->registered) {
            continue;
        }
        out->component_instances += s->count;
        out->component_add_failures += s->add_failures;
        out->component_instance_capacity += s->capacity;
    }

    out->commands_flushed = world->commands_flushed;
    out->command_overflows = world->command_overflows;
    out->systems_registered = world->system_count;
    out->systems_run_last_update = world->systems_run_last_update;
    out->hierarchy_depth_max_seen = world->hierarchy_depth_max_seen;
    out->hierarchy_rebuilds = world->hierarchy_rebuilds;
    out->hierarchy_rejected_cycles = world->hierarchy_cycles_rejected;
    out->memory_bytes = world->memory_bytes;
}

void ecs_world_log_stats(const ecs_world_t *world)
{
    ecs_world_stats_t stats;
    ecs_world_get_stats(world, &stats);

    LOG_INFO("ECS 世界: 实体 %u（峰值 %u，容量 %u）| 组件类型 %u 实例 %u（容量 %u）",
             stats.entities_alive, stats.entity_peak, stats.entity_capacity,
             stats.component_types, stats.component_instances, stats.component_instance_capacity);
    LOG_INFO("ECS 统计: 加组件失败 %u | 命令落地 %u 溢出 %u | 系统 %u/%u",
             stats.component_add_failures, stats.commands_flushed, stats.command_overflows,
             stats.systems_run_last_update, stats.systems_registered);
    LOG_INFO("ECS 层级: 节点重建 %u 次 | 最大深度 %u | 拒环 %u 次 | 存储 %llu 字节",
             stats.hierarchy_rebuilds, stats.hierarchy_depth_max_seen,
             stats.hierarchy_rejected_cycles, (unsigned long long)stats.memory_bytes);
}
