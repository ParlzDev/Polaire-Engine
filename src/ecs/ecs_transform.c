// =============================================================================
//  src/ecs/ecs_transform.c —— 变换组件、父子关系、世界矩阵求值
// =============================================================================
#include "engine/ecs/transform.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "ecs_internal.h"

// ---------------------------------------------------------------- 字段描述表
//
// 表和结构体之间的对应靠下面的 _Static_assert 钉住 ——
// C 没有真反射，不钉就是"改了结构体忘了改表"→ 存档错位且不报错。

static const ecs_field_desc_t k_transform_fields[] = {
    { "position", (u16)ENGINE_OFFSETOF(ecs_transform_t, position), ECS_FIELD_VEC3, 1u, 0u, 0u },
    { "rotation", (u16)ENGINE_OFFSETOF(ecs_transform_t, rotation), ECS_FIELD_QUAT, 1u, 0u, 0u },
    { "scale", (u16)ENGINE_OFFSETOF(ecs_transform_t, scale), ECS_FIELD_VEC3, 1u, 0u, 0u },
};

_Static_assert(ENGINE_OFFSETOF(ecs_transform_t, position) == 0, "transform.position 偏移变了");
_Static_assert(ENGINE_OFFSETOF(ecs_transform_t, rotation) == 16, "transform.rotation 偏移变了");
_Static_assert(ENGINE_OFFSETOF(ecs_transform_t, scale) == 32, "transform.scale 偏移变了");
_Static_assert(sizeof(ecs_transform_t) == 48, "transform 尺寸变了，字段表要同步");

static const ecs_field_desc_t k_parent_fields[] = {
    { "parent", (u16)ENGINE_OFFSETOF(ecs_parent_t, parent), ECS_FIELD_ENTITY, 1u, 0u, 0u },
};
_Static_assert(sizeof(ecs_parent_t) == sizeof(entity_t), "parent 只应含一个句柄");

// world_transform 是**派生数据**：每帧由 transform + 父子链算出来。
// 存进存档的话，下次加载会被重算覆盖，只会造成"存档值和屏幕上不一样"。
// 所以零字段 + skip_serialize。
static const ecs_component_desc_t k_transform_desc = {
    .name = "Transform",
    .size = sizeof(ecs_transform_t),
    .align = 16u,
    .capacity = 0u, // 用世界的默认容量
    .fields = k_transform_fields,
    .field_count = ENGINE_ARRAY_COUNT(k_transform_fields),
    .skip_serialize = false,
    .construct = NULL, // 下面单独赋值：需要一个非 static 的函数指针
    .post_load = NULL,
};

static const ecs_component_desc_t k_world_transform_desc = {
    .name = "WorldTransform",
    .size = sizeof(ecs_world_transform_t),
    .align = 16u,
    .capacity = 0u,
    .fields = NULL,
    .field_count = 0u,
    .skip_serialize = true,
    .construct = NULL,
    .post_load = NULL,
};

static const ecs_component_desc_t k_parent_desc = {
    .name = "Parent",
    .size = sizeof(ecs_parent_t),
    .align = 16u,
    .capacity = 0u,
    .fields = k_parent_fields,
    .field_count = ENGINE_ARRAY_COUNT(k_parent_fields),
    .skip_serialize = false,
    .construct = NULL,
    .post_load = NULL,
};

static void ecs_transform_construct(void *component)
{
    ecs_transform_t *t = (ecs_transform_t *)component;
    t->position = vec3_zero();
    t->rotation = quat_identity();
    // scale 必须是 1 而不是 0：全零缩放让法线矩阵求逆失败，
    // 表现是整个模型黑掉，看起来完全像光照 bug。
    t->scale = vec3_one();
}

// ---------------------------------------------------------------- 注册

bool ecs_register_builtin_components(ecs_world_t *world, ecs_builtin_components_t *out_ids)
{
    ASSERT(world != NULL);
    if (out_ids == NULL) {
        return false;
    }
    memset(out_ids, 0, sizeof(*out_ids));

    // 幂等：多个模块各自 ensure 同一批内置组件是正常用法。
    // 已注册就直接回原 id（ecs_register_component 内部会校验布局一致）。
    ecs_component_desc_t transform_desc = k_transform_desc;
    transform_desc.construct = ecs_transform_construct;

    out_ids->transform = ecs_find_component(world, "Transform");
    if (out_ids->transform == ECS_COMPONENT_NONE) {
        out_ids->transform = ecs_register_component(world, &transform_desc);
    }
    out_ids->world_transform = ecs_find_component(world, "WorldTransform");
    if (out_ids->world_transform == ECS_COMPONENT_NONE) {
        out_ids->world_transform = ecs_register_component(world, &k_world_transform_desc);
    }
    out_ids->parent = ecs_find_component(world, "Parent");
    if (out_ids->parent == ECS_COMPONENT_NONE) {
        out_ids->parent = ecs_register_component(world, &k_parent_desc);
    }

    if (out_ids->transform == ECS_COMPONENT_NONE ||
        out_ids->world_transform == ECS_COMPONENT_NONE || out_ids->parent == ECS_COMPONENT_NONE) {
        LOG_ERROR("内置组件注册失败（类型表满或名字冲突）");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 便捷操作

ecs_transform_t *ecs_transform_get_or_add(ecs_world_t *world, entity_t entity)
{
    ASSERT(world != NULL);
    const ecs_component_id id = ecs_find_component(world, "Transform");
    if (id == ECS_COMPONENT_NONE) {
        LOG_ERROR("Transform 未注册，先调用 ecs_register_builtin_components");
        return NULL;
    }
    return (ecs_transform_t *)ecs_get_or_add(world, entity, id, NULL);
}

bool ecs_transform_set_local(ecs_world_t *world, entity_t entity, vec3_t position, quat_t rotation,
                             vec3_t scale)
{
    ecs_transform_t *t = ecs_transform_get_or_add(world, entity);
    if (t == NULL) {
        return false;
    }
    t->position = position;
    t->rotation = rotation;
    t->scale = scale;
    return true;
}

bool ecs_transform_get_world(const ecs_world_t *world, entity_t entity,
                             ecs_world_transform_t *out)
{
    const ecs_component_id id = ecs_find_component(world, "WorldTransform");
    if (id == ECS_COMPONENT_NONE || out == NULL) {
        return false;
    }
    const ecs_world_transform_t *src =
        (const ecs_world_transform_t *)ecs_peek(world, entity, id);
    if (src == NULL) {
        return false;
    }
    *out = *src;
    return true;
}

// ---------------------------------------------------------------- 父子

entity_t ecs_get_parent(const ecs_world_t *world, entity_t child)
{
    const ecs_component_id id = ecs_find_component(world, "Parent");
    const ecs_parent_t    *p =
        (const ecs_parent_t *)ecs_peek(world, child, id);
    return (p != NULL) ? p->parent : ENTITY_NONE;
}

// 沿父链上溯，返回 start 的深度（根节点为 0）。
// 链在 limit 步之内没有走到头就返回 ECS_DEPTH_UNKNOWN ——
// 不管是成环还是真的深，对调用方来说都是"这条链有问题，拒绝"。
//
// **刻意用迭代而不是递归**：递归版本短十几行，但深度由数据决定 ——
// 编辑器里手滑把父拖到自己的子孙下面，就是无限递归直到栈溢出，
// 崩的位置离真正的错误十万八千里。M4 刚为同类问题查了一整晚
// （见 docs/M4-设计决策.md 第二节）。
static u32 ecs_parent_chain_depth(const ecs_world_t *world, entity_t start, u32 limit)
{
    const ecs_component_id parent_id = ecs_find_component(world, "Parent");
    if (parent_id == ECS_COMPONENT_NONE || !entity_is_valid(start)) {
        return 0u;
    }

    entity_t cursor = start;
    for (u32 step = 0u; step <= limit; ++step) {
        const ecs_parent_t *p = (const ecs_parent_t *)ecs_peek(world, cursor, parent_id);
        if (p == NULL || !entity_is_valid(p->parent)) {
            return step; // step 就是已走过的父数 = 深度
        }
        if (entity_equal(p->parent, start)) {
            return ECS_DEPTH_UNKNOWN; // 直接环回起点
        }
        cursor = p->parent;
    }
    return ECS_DEPTH_UNKNOWN;
}

// maybe_ancestor 是否在 node 的父链上。挂父子之前必须查这个，
// 否则"把父拖到孙子下面"会当场造出一个环。
static bool ecs_is_ancestor(const ecs_world_t *world, entity_t maybe_ancestor, entity_t node)
{
    const ecs_component_id parent_id = ecs_find_component(world, "Parent");
    if (parent_id == ECS_COMPONENT_NONE || !entity_is_valid(maybe_ancestor)) {
        return false;
    }

    entity_t cursor = node;
    u32      guard = 0u;
    while (entity_is_valid(cursor) && guard <= 4096u) { // 兜底：调用前已确保链无环
        const ecs_parent_t *p = (const ecs_parent_t *)ecs_peek(world, cursor, parent_id);
        if (p == NULL || !entity_is_valid(p->parent)) {
            return false;
        }
        if (entity_equal(p->parent, maybe_ancestor)) {
            return true;
        }
        cursor = p->parent;
        guard++;
    }
    return false;
}

u32 ecs_hierarchy_depth(const ecs_world_t *world, entity_t entity)
{
    return ecs_parent_chain_depth(world, entity, 64u);
}

bool ecs_set_parent(ecs_world_t *world, entity_t child, entity_t parent)
{
    ASSERT(world != NULL);

    if (!ecs_entity_is_alive(world, child)) {
        LOG_ERROR("set_parent：子实体不存在");
        return false;
    }
    if (entity_equal(child, parent)) {
        LOG_ERROR("set_parent：实体不能是自己的父节点");
        return false;
    }

    const ecs_component_id parent_id = ecs_find_component(world, "Parent");
    if (parent_id == ECS_COMPONENT_NONE) {
        LOG_ERROR("Parent 组件未注册");
        return false;
    }

    if (!entity_is_valid(parent)) {
        // 解除关系。直接移除组件而不是存一个 NONE：
        // 这样"有没有父"和"父是不是有效"只有一种表示，不会两边各说一套。
        ecs_remove(world, child, parent_id);
        ecs_mark_hierarchy_dirty(world);
        return true;
    }

    if (!ecs_entity_is_alive(world, parent)) {
        LOG_ERROR("set_parent：父实体不存在或已销毁");
        return false;
    }

    // 成环检查：把 child 挂到 parent 下之后，child 就成了 parent 祖先链的
    // 延伸。如果 child 本来就在 parent 的父链上（即 child 是 parent 的祖先），
    // 这一挂就把链闭合成环。
    if (ecs_is_ancestor(world, child, parent)) {
        world->hierarchy_cycles_rejected++;
        LOG_ERROR("set_parent：会形成环（实体 %u 是 %u 的祖先）",
                  (unsigned)handle_index(child), (unsigned)handle_index(parent));
        return false;
    }

    const u32 limit = world->config.hierarchy_depth_max;
    const u32 parent_depth = ecs_parent_chain_depth(world, parent, limit);
    if (parent_depth == ECS_DEPTH_UNKNOWN) {
        world->hierarchy_cycles_rejected++;
        LOG_ERROR("set_parent：父链本身已经超深或成环（上限 %u），拒绝挂接", limit);
        return false;
    }
    if (parent_depth + 1u > limit) {
        world->hierarchy_cycles_rejected++;
        LOG_ERROR("set_parent：挂上后深度 %u 会超过上限 %u", parent_depth + 1u, limit);
        return false;
    }

    ecs_parent_t record;
    record.parent = parent;
    void *added = ecs_add(world, child, parent_id, &record);
    if (added == NULL) {
        return false;
    }
    ecs_mark_hierarchy_dirty(world);
    return true;
}

// ---------------------------------------------------------------- 拓扑序

void ecs_mark_hierarchy_dirty(ecs_world_t *world)
{
    if (world != NULL) {
        world->hierarchy_dirty = true;
    }
}

static void ecs_reset_depth_cache(ecs_world_t *world)
{
    // 界限用 ecs_handle_index_space：它是**取值空间**而不是实体数。
    // 之前这里是 pool_capacity()，恰好漏掉最后一个槽位，于是那个槽里的堆上垃圾
    // 会被当成实体的深度直接用 —— 一个 34 亿的"深度"能把层级重排变成事实上的死循环。
    const u32 space = ecs_handle_index_space(world);
    for (u32 i = 0; i < space; ++i) {
        world->depth_cache[i] = ECS_DEPTH_UNKNOWN;
    }
}

bool ecs_rebuild_hierarchy(ecs_world_t *world)
{
    ASSERT(world != NULL);

    const ecs_component_id transform_id = ecs_find_component(world, "Transform");
    const ecs_storage_t   *tstore = ecs_storage_of(world, transform_id);
    if (tstore == NULL) {
        world->topo_count = 0u;
        world->hierarchy_dirty = false;
        return false;
    }

    // 深度用带缓存的迭代上溯算；然后按深度分层摆放。
    //
    // 分层的实现在这里是"对每个深度扫一遍"，复杂度 O(节点数 * 最大深度)。
    // 之所以不做计数排序：那要额外一块 scratch，而重建只在**层级发生变化**时
    // 发生（加载一次、编辑器拖拽时），不在每帧。最大深度上限是 32，
    // 所以最坏也就是 33 遍顺序扫，且顺序扫对缓存极友好。
    ecs_reset_depth_cache(world);

    u32 max_depth = 0u;
    u32 collected = 0u;

    // 第一遍：算每个节点深度，同时收集句柄。
    // 这里直接用存储数组而不是 ecs_type_iter —— 迭代器会登记"有迭代器在跑"，
    // 而本函数后面要给实体挂 world_transform 组件，那会被自己的保护挡下。
    // 内部实现刻意绕过计数的前提是：全程不移动**当前正在扫的这个数组**
    // （transform 存储在本函数里只读），所以是安全的。
    for (u32 row = 0u; row < tstore->count; ++row) {
        const entity_t e = tstore->owners[row];
        if (collected >= world->topo_capacity) {
            LOG_ERROR("层级节点数超过实体容量 %u", world->topo_capacity);
            break;
        }
        u32 depth = world->depth_cache[handle_index(e)];
        if (depth == ECS_DEPTH_UNKNOWN) {
            depth = ecs_parent_chain_depth(world, e, world->config.hierarchy_depth_max);
            if (depth == ECS_DEPTH_UNKNOWN) {
                // 环或超限：当成根处理，让画面仍然有东西可看，
                // 但已经计进 cycles_rejected，日志里看得见。
                world->hierarchy_cycles_rejected++;
                depth = 0u;
            }
            world->depth_cache[handle_index(e)] = depth;
        }
        // 第二道防线：任何超过配置上限的深度都按无效链处理。
        //
        // 缓存里出现垃圾值说明别处有越界（事实上就有过一次：取值空间少算了一项，
        // 最后一个槽里是堆上垃圾，读出来 34 亿当深度 —— 下面那个
        // `for (d = 0; d <= max_depth; ++d)` 于是变成事实上的死循环，症状是"示例卡死"）。
        // 越界已经修了，但**一个坏数据不该有能力把一帧变成十分钟**，
        // 所以这里宁可把它降级成根节点并计数，也不让循环上界由数据说了算。
        if (depth > world->config.hierarchy_depth_max) {
            LOG_ERROR("实体 %u 的深度 %u 超过上限 %u，按根节点处理",
                      (unsigned)handle_index(e), (unsigned)depth,
                      (unsigned)world->config.hierarchy_depth_max);
            world->hierarchy_cycles_rejected++;
            depth = 0u;
            world->depth_cache[handle_index(e)] = depth;
        }
        if (depth > max_depth) {
            max_depth = depth;
        }
        world->topo[collected++] = e;
    }

    // 第二遍：按深度稳定地重排（用输出计数就地放置需要一个目标数组，
    // 这里用插入的方式：按深度从小到大把匹配的项前移）。
    // 实现选择：对每个深度 d，把数组里深度为 d 的项依次换到 [written, ...) 区间。
    u32 written = 0u;
    for (u32 d = 0u; d <= max_depth; ++d) {
        usize cursor = written;
        while (cursor < collected) {
            const u32 cd = world->depth_cache[handle_index(world->topo[cursor])];
            if (cd == d) {
                cursor++;
                continue;
            }
            // 往后找一个属于当前深度的项换过来
            usize finder = cursor + 1u;
            while (finder < collected &&
                   world->depth_cache[handle_index(world->topo[finder])] != d) {
                finder++;
            }
            if (finder >= collected) {
                break; // 后面没有更浅的了，剩下的深度更大，交给下一轮
            }
            const entity_t tmp = world->topo[cursor];
            world->topo[cursor] = world->topo[finder];
            world->topo[finder] = tmp;
            cursor++;
        }
        written = (u32)cursor;
    }

    world->topo_count = collected;
    world->hierarchy_depth_max_seen = max_depth + 1u;
    world->hierarchy_rebuilds++;
    world->hierarchy_dirty = false;
    return true;
}

u32 ecs_hierarchy_node_count(const ecs_world_t *world)
{
    return (world != NULL) ? world->topo_count : 0u;
}

u32 ecs_hierarchy_depth_max_seen(const ecs_world_t *world)
{
    return (world != NULL) ? world->hierarchy_depth_max_seen : 0u;
}

// ---------------------------------------------------------------- 世界矩阵

void ecs_update_world_transforms(ecs_world_t *world)
{
    ASSERT(world != NULL);

    if (world->hierarchy_dirty) {
        (void)ecs_rebuild_hierarchy(world);
    }

    const ecs_component_id transform_id = ecs_find_component(world, "Transform");
    const ecs_component_id world_id = ecs_find_component(world, "WorldTransform");
    const ecs_component_id parent_id = ecs_find_component(world, "Parent");
    if (transform_id == ECS_COMPONENT_NONE || world_id == ECS_COMPONENT_NONE) {
        return;
    }

    // 拓扑序保证父先于子，所以这里一趟顺序扫即可，不需要递归也不需要多轮。
    //
    // find_component 是线性 strcmp，绝不能放进循环里：这个函数每帧都跑，
    // 10 万实体 x 每次 strcmp 会把它变成可测量的开销（决策：先测再说，
    // 而这一眼就知道是白给的浪费）。
    for (u32 i = 0; i < world->topo_count; ++i) {
        const entity_t e = world->topo[i];

        const ecs_transform_t *t = (const ecs_transform_t *)ecs_get(world, e, transform_id);
        if (t == NULL) {
            continue; // 拓扑数组是快照，期间组件被移除是允许的（本帧跳过它）
        }

        mat4_t model = mat4_from_trs(t->position, t->rotation, t->scale);

        const ecs_parent_t *p = (const ecs_parent_t *)ecs_get(world, e, parent_id);
        if (p != NULL && entity_is_valid(p->parent)) {
            const ecs_world_transform_t *pw =
                (const ecs_world_transform_t *)ecs_get(world, p->parent, world_id);
            if (pw != NULL) {
                model = mat4_mul(pw->model, model);
            }
            // 父节点还没有 world_transform 只可能是它没有 transform 组件：
            // 此时"父只是逻辑容器"，子按自己的本地值当世界值用，是合理语义。
        }

        // 直接写进存储，不经过栈上的 128 字节临时量再整体拷一遍：
        // 少一次 memcpy，也让"派生数据每帧重算"这件事在代码上一目了然。
        ecs_world_transform_t *wt =
            (ecs_world_transform_t *)ecs_add(world, e, world_id, NULL);
        if (wt == NULL) {
            continue; // 容量不足：ecs_add 已经记过错，这里不能继续写
        }
        wt->model = model;
        wt->normal_matrix = mat4_from_mat3(mat4_normal_matrix(model));
    }
}
