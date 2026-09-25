// =============================================================================
//  src/ecs/ecs_query.c —— 查询构造与迭代
// =============================================================================
#include "engine/ecs/query.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "ecs_internal.h"

// ---------------------------------------------------------------- 描述构造

static bool query_desc_add_all(ecs_query_desc_t *d, ecs_component_id id)
{
    if (d->all_count >= ECS_MAX_QUERY_TERMS) {
        return false;
    }
    d->all[d->all_count++] = id;
    return true;
}

ecs_query_desc_t ecs_query_desc_1(ecs_component_id a, const char *name)
{
    ecs_query_desc_t d;
    memset(&d, 0, sizeof(d));
    d.debug_name = name;
    (void)query_desc_add_all(&d, a);
    return d;
}

ecs_query_desc_t ecs_query_desc_2(ecs_component_id a, ecs_component_id b, const char *name)
{
    ecs_query_desc_t d;
    memset(&d, 0, sizeof(d));
    d.debug_name = name;
    (void)query_desc_add_all(&d, a);
    (void)query_desc_add_all(&d, b);
    return d;
}

ecs_query_desc_t ecs_query_desc_3(ecs_component_id a, ecs_component_id b, ecs_component_id c,
                                  const char *name)
{
    ecs_query_desc_t d;
    memset(&d, 0, sizeof(d));
    d.debug_name = name;
    (void)query_desc_add_all(&d, a);
    (void)query_desc_add_all(&d, b);
    (void)query_desc_add_all(&d, c);
    return d;
}

// ---------------------------------------------------------------- 创建

static void ecs_query_pick_driving(ecs_query_t *q)
{
    // 驱动类型 = all 里当前基数最小的。
    // 这一步直接决定查询是 O(命中数) 还是 O(场景实体数)：
    // "1 万个 Transform 里只有 3 个 Agent"若拿 Transform 驱动，就白扫 1 万次。
    const ecs_storage_t *best = NULL;
    ecs_component_id     best_id = q->desc.all[0];
    u32                  best_count = 0xFFFFFFFFu;

    for (u32 i = 0; i < q->term_count; ++i) {
        const ecs_storage_t *s = ecs_storage_of(q->world, q->desc.all[i]);
        if (s == NULL) {
            continue;
        }
        if (s->count < best_count) {
            best_count = s->count;
            best = s;
            best_id = q->desc.all[i];
        }
    }
    ENGINE_UNUSED(best);
    q->driving = best_id;
    q->estimate = (best_count == 0xFFFFFFFFu) ? 0u : best_count;
}

ecs_query_t *ecs_query_create(ecs_world_t *world, const ecs_query_desc_t *desc)
{
    ASSERT(world != NULL);

    if (desc == NULL || desc->all_count == 0u) {
        // 没有 all 条件的查询要遍历"所有实体"，而那需要一个实体索引数组
        // （M5 不做：真正需要它的只有编辑器全量列表，那里用 type_iter 更合适）。
        LOG_ERROR("查询至少要有一个 all 条件（%s）",
                  (desc != NULL && desc->debug_name != NULL) ? desc->debug_name : "?");
        return NULL;
    }
    if (desc->all_count > ECS_MAX_QUERY_TERMS || desc->none_count > ECS_MAX_QUERY_TERMS) {
        LOG_ERROR("查询条件数超限（最多 %u）", ECS_MAX_QUERY_TERMS);
        return NULL;
    }

    for (u32 i = 0; i < desc->all_count; ++i) {
        if (ecs_storage_of(world, desc->all[i]) == NULL) {
            LOG_ERROR("查询引用了未注册的组件类型 %u", (unsigned)desc->all[i]);
            return NULL;
        }
        for (u32 j = 0; j < i; ++j) {
            if (desc->all[i] == desc->all[j]) {
                LOG_ERROR("查询的 all 条件里有重复类型");
                return NULL;
            }
        }
    }
    for (u32 i = 0; i < desc->none_count; ++i) {
        if (ecs_storage_of(world, desc->none[i]) == NULL) {
            LOG_ERROR("查询的 none 引用了未注册的组件类型 %u", (unsigned)desc->none[i]);
            return NULL;
        }
        for (u32 j = 0; j < desc->all_count; ++j) {
            // 同一类型既要求又有 —— 那这个查询永远命中 0 个，
            // 而且一定是笔误。当场报，而不是留下一个"系统不工作"的谜。
            if (desc->none[i] == desc->all[j]) {
                LOG_ERROR("组件 %u 同时出现在 all 与 none 里，查询恒不命中",
                          (unsigned)desc->none[i]);
                return NULL;
            }
        }
    }

    ecs_query_t *q = (ecs_query_t *)world->config.allocator->alloc(world->config.allocator,
                                                                   sizeof(ecs_query_t), 16);
    if (q == NULL) {
        LOG_ERROR("查询分配失败");
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    q->world = world;
    q->desc = *desc; // 复制：调用方的 desc 很可能是栈上的临时值
    q->term_count = desc->all_count;
    ecs_query_pick_driving(q);
    return q;
}

void ecs_query_destroy(ecs_world_t *world, ecs_query_t *query)
{
    if (world == NULL || query == NULL) {
        return;
    }
    world->config.allocator->free(world->config.allocator, query, sizeof(ecs_query_t));
}

u32 ecs_query_estimate_count(const ecs_query_t *query)
{
    if (query == NULL) {
        return 0u;
    }
    const ecs_storage_t *s = ecs_storage_of(query->world, query->driving);
    return (s != NULL) ? s->count : 0u;
}

u32 ecs_query_match_count(const ecs_query_t *query)
{
    if (query == NULL) {
        return 0u;
    }
    const ecs_storage_t *drv = ecs_storage_of(query->world, query->driving);
    if (drv == NULL) {
        return 0u;
    }

    u32 hits = 0u;
    for (u32 row = 0u; row < drv->count; ++row) {
        const u32 index = handle_index(drv->owners[row]);
        bool      match = true;
        for (u32 i = 0; i < query->term_count && match; ++i) {
            const ecs_storage_t *s = ecs_storage_of(query->world, query->desc.all[i]);
            if (s == NULL || !ecs_presence_test(s, index)) {
                match = false;
            }
        }
        for (u32 i = 0; i < query->desc.none_count && match; ++i) {
            const ecs_storage_t *s = ecs_storage_of(query->world, query->desc.none[i]);
            if (s != NULL && ecs_presence_test(s, index)) {
                match = false;
            }
        }
        if (match) {
            hits++;
        }
    }
    return hits;
}

ecs_component_id ecs_query_driving_type(const ecs_query_t *query)
{
    return (query != NULL) ? query->driving : ECS_COMPONENT_NONE;
}

const char *ecs_query_debug_name(const ecs_query_t *query)
{
    if (query == NULL || query->desc.debug_name == NULL) {
        return "<unnamed query>";
    }
    return query->desc.debug_name;
}

// ---------------------------------------------------------------- 迭代

void ecs_query_begin(ecs_query_t *query, ecs_iter_t *it)
{
    ASSERT(query != NULL);
    ASSERT(it != NULL);

    memset(it, 0, sizeof(*it));
    it->world = query->world;
    it->query = query;
    it->term_count = query->term_count;

    // 登记"有迭代器在跑"。之后所有会移动组件数组的操作都会被挡住，
    // 所以这里可以放心地按行号一路扫下去。
    ecs_world_acquire_iterator(query->world);
}

bool ecs_query_next(ecs_iter_t *it)
{
    if (it == NULL || it->query == NULL) {
        return false;
    }
    const ecs_query_t *q = it->query;
    const ecs_world_t *world = q->world;

    // drv->count 在迭代期间不会变（加组件被迭代计数挡住），
    // 所以不需要快照行尾。
    const ecs_storage_t *drv = ecs_storage_of(world, q->driving);
    if (drv == NULL) {
        return false;
    }

    while (it->driving_row < drv->count) {
        const u32     row = it->driving_row++;
        const entity_t entity = drv->owners[row];
        const u32      index = handle_index(entity);

        bool match = true;
        for (u32 i = 0; i < q->term_count && match; ++i) {
            const ecs_storage_t *s = ecs_storage_of(world, q->desc.all[i]);
            if (s == NULL || !ecs_presence_test(s, index)) {
                match = false;
                break;
            }
            it->columns[i] = (u8 *)s->data + (usize)s->row_of[index] * (usize)s->stride;
        }
        for (u32 i = 0; i < q->desc.none_count && match; ++i) {
            const ecs_storage_t *s = ecs_storage_of(world, q->desc.none[i]);
            if (s != NULL && ecs_presence_test(s, index)) {
                match = false;
            }
        }

        if (!match) {
            it->skipped++;
            continue;
        }

        // 驱动列在 all 里也可能不在第 0 位，统一再填一次保证列顺序与 desc 一致
        for (u32 i = 0; i < q->term_count; ++i) {
            const ecs_storage_t *s = ecs_storage_of(world, q->desc.all[i]);
            it->columns[i] = (u8 *)s->data + (usize)s->row_of[index] * (usize)s->stride;
        }

        it->entity = entity;
        it->index++;
        return true;
    }

    return false;
}

void ecs_query_end(ecs_iter_t *it)
{
    if (it == NULL || it->world == NULL) {
        return;
    }
    ecs_world_release_iterator((ecs_world_t *)it->world);
    it->world = NULL; // 再调一次 end 不会重复减计数
}

void *ecs_iter_column(const ecs_iter_t *it, ecs_component_id id)
{
    if (it == NULL || it->query == NULL) {
        return NULL;
    }
    for (u32 i = 0; i < it->query->term_count; ++i) {
        if (it->query->desc.all[i] == id) {
            return it->columns[i];
        }
    }
    return NULL;
}
