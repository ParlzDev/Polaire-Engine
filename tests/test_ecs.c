// =============================================================================
//  tests/test_ecs.c —— 实体、组件存储、查询、阶段调度、父子层级
//
//  这组测试里最值得看的是三类：
//   1. **交换式移除后的行索引一致性**。移除会把最后一行搬到被删的位置，
//      忘了同步"被搬那一行"的反查表，症状是"改了 A 的结果 B 变了" ——
//      一个几乎无法归因的错误，所以必须有专门的用例。
//   2. **迭代中改结构会被拒绝**（而不是悄悄跑飞）。这是内存安全边界。
//   3. **父子环与深度上限被拒**。编辑器里手滑就会造出环，
//      拒不掉的话下一次求值就是无限递归直到栈溢出。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <string.h>

#include "engine/core/memory.h"
#include "engine/ecs/system.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"
#include "ecs_test_components.h"

// ---------------------------------------------------------------- 夹具

typedef struct ecs_fixture_t {
    allocator_tracking_t tracker;
    ecs_world_t         *world;
    ecs_builtin_components_t builtin;
    ecs_component_id     vel;
    ecs_component_id     health;
    ecs_component_id     agent;
    ecs_component_id     marker;
} ecs_fixture_t;

static void ecs_fixture_begin_cfg(ecs_fixture_t *fx, const char *name, u32 entity_capacity,
                                  u32 component_capacity, u32 command_capacity,
                                  u32 hierarchy_depth_max)
{
    memset(fx, 0, sizeof(*fx));
    allocator_tracking_init(&fx->tracker, allocator_heap());

    ecs_world_config_t config = ecs_world_config_default(&fx->tracker.base);
    config.entity_capacity = entity_capacity;
    config.default_component_capacity = component_capacity;
    config.command_capacity = command_capacity;
    config.hierarchy_depth_max = hierarchy_depth_max;
    config.name = name;

    fx->world = ecs_world_create(&fx->tracker.base, &config);
    EXPECT_NOT_NULL(fx->world);
    if (fx->world == NULL) {
        return;
    }
    EXPECT_TRUE(ecs_register_builtin_components(fx->world, &fx->builtin));

    fx->vel = ecs_register_component(fx->world, &ecs_test_vel_desc);
    fx->health = ecs_register_component(fx->world, &ecs_test_health_desc);
    fx->agent = ecs_register_component(fx->world, &ecs_test_agent_desc);
    fx->marker = ecs_register_component(fx->world, &ecs_test_marker_desc);
    EXPECT_TRUE(fx->vel != ECS_COMPONENT_NONE);
    EXPECT_TRUE(fx->health != ECS_COMPONENT_NONE);
    EXPECT_TRUE(fx->agent != ECS_COMPONENT_NONE);
    EXPECT_TRUE(fx->marker != ECS_COMPONENT_NONE);
}

static void ecs_fixture_begin(ecs_fixture_t *fx, const char *name, u32 entity_capacity)
{
    ecs_fixture_begin_cfg(fx, name, entity_capacity, 64u, 64u, 8u);
}

static void ecs_fixture_begin_with_capacity(ecs_fixture_t *fx, const char *name,
                                            u32 entity_capacity, u32 component_capacity)
{
    ecs_fixture_begin_cfg(fx, name, entity_capacity, component_capacity, 64u, 8u);
}

static void ecs_fixture_end(ecs_fixture_t *fx)
{
    if (fx->world != NULL) {
        ecs_world_destroy(fx->world);
        fx->world = NULL;
    }
    EXPECT_FALSE(allocator_tracking_has_leak(&fx->tracker));
}

// ---------------------------------------------------------------- 实体

static void ecs_entity_lifecycle(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "ent", 32u);

    const entity_t a = ecs_create_entity(fx.world);
    const entity_t b = ecs_create_entity(fx.world);
    EXPECT_TRUE(entity_is_valid(a));
    EXPECT_TRUE(entity_is_valid(b));
    EXPECT_TRUE(!entity_equal(a, b));
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 2u);
    EXPECT_TRUE(ecs_entity_is_alive(fx.world, a));

    EXPECT_TRUE(ecs_destroy_entity(fx.world, a));
    EXPECT_FALSE(ecs_entity_is_alive(fx.world, a));
    EXPECT_TRUE(ecs_entity_is_alive(fx.world, b));
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 1u);

    // 重复销毁同一个实体：第二次必须失败，不能把别人的槽位还掉
    EXPECT_FALSE(ecs_destroy_entity(fx.world, a));

    ecs_fixture_end(&fx);
}

// 销毁后句柄必须永久失效 —— 这是句柄制相对裸指针的全部价值。
static void ecs_stale_handle_is_rejected(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "stale", 32u);

    const entity_t a = ecs_create_entity(fx.world);
    test_vel_t v = { 1.0f, 2.0f };
    EXPECT_NOT_NULL(ecs_add(fx.world, a, fx.vel, &v));
    EXPECT_TRUE(ecs_destroy_entity(fx.world, a));

    EXPECT_NULL(ecs_get(fx.world, a, fx.vel));
    EXPECT_FALSE(ecs_has(fx.world, a, fx.vel));
    EXPECT_FALSE(ecs_entity_is_alive(fx.world, a));

    // 用陈旧句柄加组件必须失败，而不是把组件挂到"将来复用这个槽位的人"身上
    EXPECT_NULL(ecs_add(fx.world, a, fx.vel, &v));

    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 组件

static void ecs_add_get_remove(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "crud", 32u);

    const entity_t e = ecs_create_entity(fx.world);
    EXPECT_FALSE(ecs_has(fx.world, e, fx.vel));

    test_vel_t v = { 3.5f, -2.25f };
    test_vel_t *stored = (test_vel_t *)ecs_add(fx.world, e, fx.vel, &v);
    EXPECT_NOT_NULL(stored);
    EXPECT_NEAR((double)stored->x, 3.5, 1e-9);
    EXPECT_TRUE(ecs_has(fx.world, e, fx.vel));

    // 重复加同一个类型不产生第二份，而是覆盖并返回同一块内存
    test_vel_t v2 = { 9.0f, 8.0f };
    test_vel_t *again = (test_vel_t *)ecs_add(fx.world, e, fx.vel, &v2);
    EXPECT_TRUE(again == stored);
    EXPECT_NEAR((double)again->x, 9.0, 1e-9);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 1u);

    EXPECT_TRUE(ecs_remove(fx.world, e, fx.vel));
    EXPECT_FALSE(ecs_has(fx.world, e, fx.vel));
    EXPECT_FALSE(ecs_remove(fx.world, e, fx.vel)); // 再删一次是 no-op

    // get_or_add
    test_vel_t *added = (test_vel_t *)ecs_get_or_add(fx.world, e, fx.vel, NULL);
    EXPECT_NOT_NULL(added);
    // construct 会把 x 写成 7：证明默认值真的跑了，而不是留了上一次的残值
    EXPECT_NEAR((double)added->x, 7.0, 1e-9);

    ecs_fixture_end(&fx);
}

// construct 的作用在这里被验证：不给初值时必须是确定性的默认，
// 否则"新实体看到上一个对象残留的数据"会变成幽灵字段。
static void ecs_construct_fills_defaults(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "ctor", 32u);

    const entity_t a = ecs_create_entity(fx.world);
    test_health_t *ha = (test_health_t *)ecs_add(fx.world, a, fx.health, NULL);
    EXPECT_NOT_NULL(ha);
    EXPECT_EQ_U64(ha->hp, 100u);

    // 先占掉一个槽并写入脏数据，释放后复用同一个槽位必须看到干净的默认值。
    const entity_t b = ecs_create_entity(fx.world);
    test_health_t *hb = (test_health_t *)ecs_add(fx.world, b, fx.health, NULL);
    hb->hp = 3u;
    EXPECT_TRUE(ecs_remove(fx.world, b, fx.health));

    const entity_t c = ecs_create_entity(fx.world);
    test_health_t *hc = (test_health_t *)ecs_add(fx.world, c, fx.health, NULL);
    EXPECT_EQ_U64(hc->hp, 100u);

    ecs_fixture_end(&fx);
}

// 交换式移除最容易错的地方：把最后一行搬过来之后，
// 必须同步更新"被搬那一行"的反查表。
static void ecs_swap_remove_keeps_indices_consistent(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "swap", 64u);

    enum { N = 12 };
    entity_t ents[N];
    for (u32 i = 0; i < N; ++i) {
        ents[i] = ecs_create_entity(fx.world);
        test_vel_t v = { (f32)i, (f32)(i * 2) };
        EXPECT_NOT_NULL(ecs_add(fx.world, ents[i], fx.vel, &v));
    }
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), (usize)N);

    // 删掉中间的几个，每次都全量核对一遍"每个实体读回自己的值"
    const u32 doomed[] = { 0u, 5u, 11u, 1u, 7u };
    for (usize d = 0; d < ENGINE_ARRAY_COUNT(doomed); ++d) {
        EXPECT_TRUE(ecs_remove(fx.world, ents[doomed[d]], fx.vel));
        for (u32 i = 0; i < N; ++i) {
            const test_vel_t *v = (const test_vel_t *)ecs_peek(fx.world, ents[i], fx.vel);

            bool removed = false;
            for (usize k = 0; k <= d; ++k) {
                if (doomed[k] == i) {
                    removed = true;
                }
            }
            if (removed) {
                EXPECT_TRUE(v == NULL);
            } else {
                EXPECT_NOT_NULL(v);
                if (v != NULL) {
                    EXPECT_NEAR((double)v->x, (double)i, 1e-6);
                    EXPECT_NEAR((double)v->y, (double)(i * 2u), 1e-6);
                }
            }
        }
    }

    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), (usize)(N - 5u));
    ecs_fixture_end(&fx);
}

// 销毁实体要把它身上所有组件都摘干净，否则别人的行会指向已归还的身份。
static void ecs_destroy_removes_all_components(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "destroy_all", 32u);

    const entity_t keep = ecs_create_entity(fx.world);
    test_vel_t kv = { 111.0f, 222.0f };
    EXPECT_NOT_NULL(ecs_add(fx.world, keep, fx.vel, &kv));
    test_health_t kh = { 42u, true };
    EXPECT_NOT_NULL(ecs_add(fx.world, keep, fx.health, &kh));

    const entity_t gone = ecs_create_entity(fx.world);
    EXPECT_NOT_NULL(ecs_add(fx.world, gone, fx.vel, NULL));
    EXPECT_NOT_NULL(ecs_add(fx.world, gone, fx.health, NULL));
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 2u);

    EXPECT_TRUE(ecs_destroy_entity(fx.world, gone));
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 1u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.health), 1u);

    // 幸存者的数据一个字都不能变
    const test_vel_t *v = (const test_vel_t *)ecs_peek(fx.world, keep, fx.vel);
    EXPECT_NOT_NULL(v);
    if (v != NULL) {
        EXPECT_NEAR((double)v->x, 111.0, 1e-6);
    }
    const test_health_t *h = (const test_health_t *)ecs_peek(fx.world, keep, fx.health);
    EXPECT_NOT_NULL(h);
    if (h != NULL) {
        EXPECT_EQ_U64(h->hp, 42u);
    }

    ecs_fixture_end(&fx);
}

// 注册校验：字段越界的描述表必须在注册时就被拒 ——
// 越界 offset 会让序列化字节跑到相邻组件身上。
static void ecs_register_validates_field_table(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "validate", 32u);

    static const ecs_field_desc_t bad_fields[] = {
        { "past_end", (u16)(sizeof(test_vel_t) + 4u), ECS_FIELD_F32, 1u, 0u, 0u },
    };
    const ecs_component_desc_t bad = {
        .name = "BadLayout",
        .size = sizeof(test_vel_t),
        .align = 16u,
        .capacity = 8u,
        .fields = bad_fields,
        .field_count = 1u,
    };
    EXPECT_EQ_U64(ecs_register_component(fx.world, &bad), (usize)ECS_COMPONENT_NONE);

    // 同名不同布局也必须拒（两处代码对同一类型有两种理解）
    const ecs_component_desc_t conflicting = {
        .name = "TestVel",
        .size = 64u,
        .align = 16u,
        .capacity = 8u,
    };
    EXPECT_EQ_U64(ecs_register_component(fx.world, &conflicting), (usize)ECS_COMPONENT_NONE);

    // 同名同布局则是幂等取回原 id
    EXPECT_EQ_U64(ecs_register_component(fx.world, &ecs_test_vel_desc), (usize)fx.vel);

    ecs_fixture_end(&fx);
}

// 容量到顶时必须拒绝并计数，而不是写越界。
static void ecs_component_capacity_is_enforced(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "capacity", 64u);
    // 世界对外是不透明指针，测试也不该去掏内部数组 —— 用"注册一个显式小容量
    // 的同结构类型"来构造容量边界，走的是和真实使用完全相同的路。
    ecs_component_desc_t small_agent = ecs_test_agent_desc;
    small_agent.name = "TestAgentSmall";
    small_agent.capacity = 4u;
    const ecs_component_id small = ecs_register_component(fx.world, &small_agent);
    EXPECT_TRUE(small != ECS_COMPONENT_NONE);

    entity_t ents[8];
    u32 added = 0u;
    for (u32 i = 0; i < 8u; ++i) {
        ents[i] = ecs_create_entity(fx.world);
        if (ecs_add(fx.world, ents[i], small, NULL) != NULL) {
            added++;
        }
    }
    EXPECT_EQ_U64(added, 4u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, small), 4u);

    ecs_world_stats_t stats;
    ecs_world_get_stats(fx.world, &stats);
    EXPECT_GT_U64(stats.component_add_failures, 0u); // 失败要可观测，不能静默

    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 查询

static void ecs_query_matches_all_terms(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "query", 64u);

    entity_t both[3], vel_only[2];
    for (u32 i = 0; i < 3u; ++i) {
        both[i] = ecs_create_entity(fx.world);
        test_vel_t v = { (f32)i, 0.0f };
        EXPECT_NOT_NULL(ecs_add(fx.world, both[i], fx.vel, &v));
        EXPECT_NOT_NULL(ecs_add(fx.world, both[i], fx.health, NULL));
    }
    for (u32 i = 0; i < 2u; ++i) {
        vel_only[i] = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, vel_only[i], fx.vel, NULL));
    }
    for (u32 i = 0; i < 2u; ++i) {
        (void)ecs_create_entity(fx.world); // 光有身份、什么组件都没有的实体
    }
    EXPECT_NEAR((double)((const test_vel_t *)ecs_peek(fx.world, both[2], fx.vel))->x, 2.0, 1e-6);
    EXPECT_NEAR((double)((const test_vel_t *)ecs_peek(fx.world, vel_only[1], fx.vel))->y, 0.0,
                1e-6);

    ecs_query_desc_t d = ecs_query_desc_2(fx.vel, fx.health, "vel+health");
    ecs_query_t *q = ecs_query_create(fx.world, &d);
    EXPECT_NOT_NULL(q);
    EXPECT_EQ_U64(ecs_query_match_count(q), 3u);

    ecs_iter_t it;
    u32 seen = 0u;
    ecs_query_begin(q, &it);
    while (ecs_query_next(&it)) {
        const test_vel_t *v = ECS_COLUMN(&it, 0, const test_vel_t);
        const test_health_t *h = ECS_COLUMN(&it, 1, const test_health_t);
        EXPECT_NOT_NULL(v);
        EXPECT_NOT_NULL(h);
        EXPECT_EQ_U64(h->hp, 100u);
        seen++;
    }
    ecs_query_end(&it);
    EXPECT_EQ_U64(seen, 3u);

    // 再验一遍 end 之后计数归零（漏 end 会被世界在 update 时抓到）
    EXPECT_EQ_U64(ecs_active_iterator_count(fx.world), 0u);

    ecs_query_destroy(fx.world, q);
    ecs_fixture_end(&fx);
}

// none 条件：系统之间分工靠它（"有 Agent 且没有 Inactive 的我来管"）。
static void ecs_query_none_excludes(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "query_none", 64u);

    for (u32 i = 0; i < 5u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
        if (i < 2u) {
            EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.agent, NULL));
        }
    }

    ecs_query_desc_t with_agent = ecs_query_desc_2(fx.vel, fx.agent, "vel+agent");
    ecs_query_t *q1 = ecs_query_create(fx.world, &with_agent);
    EXPECT_EQ_U64(ecs_query_match_count(q1), 2u);

    ecs_query_desc_t desc2 = ecs_query_desc_1(fx.vel, "vel without agent");
    desc2.none[0] = fx.agent;
    desc2.none_count = 1u;
    ecs_query_t *q2 = ecs_query_create(fx.world, &desc2);
    EXPECT_EQ_U64(ecs_query_match_count(q2), 3u);

    // all 与 none 有交集的查询恒不命中 —— 一定是笔误，创建时就得报
    ecs_query_desc_t bogus = ecs_query_desc_1(fx.vel, "bogus");
    bogus.none[0] = fx.vel;
    bogus.none_count = 1u;
    EXPECT_NULL(ecs_query_create(fx.world, &bogus));

    ecs_query_destroy(fx.world, q1);
    ecs_query_destroy(fx.world, q2);
    ecs_fixture_end(&fx);
}

// 驱动类型必须选基数最小的那个，否则查询从 O(命中) 退化成 O(全部实体)。
static void ecs_query_picks_smallest_driver(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "driver", 128u);

    for (u32 i = 0; i < 40u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL)); // 多
    }
    for (u32 i = 0; i < 3u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.agent, NULL)); // 少
    }
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 43u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.agent), 3u);

    ecs_query_desc_t d = ecs_query_desc_2(fx.vel, fx.agent, "join");
    ecs_query_t *q = ecs_query_create(fx.world, &d);
    EXPECT_EQ_U64(ecs_query_driving_type(q), (usize)fx.agent);
    EXPECT_EQ_U64(ecs_query_match_count(q), 3u);

    // 扫过的行数应该等于驱动类型的行数（3），不是 43 —— 这就是选驱动的意义
    ecs_iter_t it;
    ecs_query_begin(q, &it);
    u32 hits = 0u;
    while (ecs_query_next(&it)) {
        hits++;
    }
    const u32 probes = it.skipped + hits;
    ecs_query_end(&it);
    EXPECT_EQ_U64(hits, 3u);
    EXPECT_EQ_U64(probes, 3u);

    ecs_query_destroy(fx.world, q);
    ecs_fixture_end(&fx);
}

// 迭代中改结构必须被拒绝：这是内存安全边界，不是风格问题。
static void ecs_mutation_during_iteration_is_rejected(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "iter_guard", 64u);

    for (u32 i = 0; i < 6u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
    }

    ecs_query_desc_t d = ecs_query_desc_1(fx.vel, "all vel");
    ecs_query_t *q = ecs_query_create(fx.world, &d);

    const entity_t extra = ecs_create_entity(fx.world);

    ecs_iter_t it;
    ecs_query_begin(q, &it);
    EXPECT_EQ_U64(ecs_active_iterator_count(fx.world), 1u);

    EXPECT_TRUE(ecs_query_next(&it));
    // 移除会移动数组元素，让迭代器的行号指错 —— 必须被挡
    EXPECT_FALSE(ecs_remove(fx.world, it.entity, fx.health));
    EXPECT_NULL(ecs_add(fx.world, extra, fx.vel, NULL));

    // 销毁的语义和上面两个**不一样**，这是刻意的（见 world.h 的契约）：
    // 它在迭代中自动转成延后命令，所以返回 true 表示"已排队"，
    // 而实体本帧仍然活着 —— 这样"死人的尸体当帧凭空消失"就不会发生。
    const entity_t victim = it.entity;
    EXPECT_TRUE(ecs_destroy_entity(fx.world, victim));
    EXPECT_EQ_U64(ecs_command_backlog(fx.world), 1u);
    EXPECT_TRUE(ecs_entity_is_alive(fx.world, victim));
    // 而且排队不等于已生效：组件数在本帧不变
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 6u);

    // 只读操作照常
    EXPECT_NOT_NULL(ecs_get(fx.world, it.entity, fx.vel));

    while (ecs_query_next(&it)) { /* 走完 */ }
    ecs_query_end(&it);
    EXPECT_EQ_U64(ecs_active_iterator_count(fx.world), 0u);

    // 迭代结束后同样的写操作就该成功
    EXPECT_NOT_NULL(ecs_add(fx.world, extra, fx.vel, NULL));
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 7u);

    // 摘掉迭代器之后 flush，延后的销毁才真正落地
    EXPECT_EQ_U64(ecs_flush_commands(fx.world), 1u);
    EXPECT_FALSE(ecs_entity_is_alive(fx.world, victim));
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 6u);

    ecs_query_destroy(fx.world, q);
    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 调度

typedef struct order_probe_t {
    char log[64];
    u32  count;
    u32  dt_seen;
} order_probe_t;

static order_probe_t g_order_probe;

// user 指向系统自己的标签字节，于是 log 记的是"谁跑了"。
// 探针若只记调用序号，任何执行顺序都会打出 "ABCDE"，这条测试就成了摆设 ——
// 我第一版正是那么写的，留在这儿当反面例子。
static void probe_system(ecs_world_t *world, void *user, f32 delta_seconds)
{
    ENGINE_UNUSED(world);
    const char *tag = (const char *)user;
    if (g_order_probe.count < sizeof(g_order_probe.log) - 1u) {
        g_order_probe.log[g_order_probe.count] = (tag != NULL) ? *tag : '?';
    }
    g_order_probe.count++;
    if (delta_seconds > 0.0f) {
        g_order_probe.dt_seen++;
    }
}

// 注册顺序刻意打乱：执行顺序必须是 stage -> priority -> name。
static void systems_run_in_deterministic_order(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "order", 32u);

    memset(&g_order_probe, 0, sizeof(g_order_probe));

    static struct {
        const char *name;
        ecs_stage_t stage;
        i32         priority;
        char        tag;
    } specs[] = {
        { "z_last", ECS_STAGE_CLEANUP, 0, 'z' },
        { "b_mid", ECS_STAGE_LOGIC, 5, 'b' },
        { "a_mid", ECS_STAGE_LOGIC, 5, 'a' },
        { "c_first", ECS_STAGE_INPUT, 0, 'c' },
        { "d_late_logic", ECS_STAGE_LOGIC, 9, 'd' },
    };

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(specs); ++i) {
        ecs_system_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.name = specs[i].name;
        desc.stage = specs[i].stage;
        desc.priority = specs[i].priority;
        desc.update = probe_system;
        desc.user = &specs[i].tag; // 可写字节：把 const 字符串塞进 void* 会剥掉 const
        EXPECT_NOT_NULL(ecs_add_system(fx.world, &desc));
    }

    EXPECT_EQ_U64(ecs_system_count(fx.world), 5u);
    // 按名字定序：LOGIC 阶段里 a_mid 在 b_mid 前（priority 相同），d_late_logic 最后
    EXPECT_STR_EQ(ecs_system_name(ecs_system_at(fx.world, 0)), "c_first");
    EXPECT_STR_EQ(ecs_system_name(ecs_system_at(fx.world, 1)), "a_mid");
    EXPECT_STR_EQ(ecs_system_name(ecs_system_at(fx.world, 2)), "b_mid");
    EXPECT_STR_EQ(ecs_system_name(ecs_system_at(fx.world, 3)), "d_late_logic");
    EXPECT_STR_EQ(ecs_system_name(ecs_system_at(fx.world, 4)), "z_last");

    g_order_probe.count = 0u;
    g_order_probe.log[0] = '\0';
    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.016f), 5u);
    EXPECT_STR_EQ(g_order_probe.log, "cabdz");
    EXPECT_EQ_U64(g_order_probe.dt_seen, 5u);

    // 同名系统必须被拒：同名会让"按名字定序"退化成随机
    ecs_system_desc_t dup;
    memset(&dup, 0, sizeof(dup));
    dup.name = "a_mid";
    dup.stage = ECS_STAGE_MOVE;
    dup.update = probe_system;
    dup.user = &specs[0].tag;
    EXPECT_NULL(ecs_add_system(fx.world, &dup));

    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 延后命令

static void add_health_system(ecs_world_t *world, void *user, f32 dt)
{
    ENGINE_UNUSED(dt);
    ecs_component_t_probe *p = (ecs_component_t_probe *)user;
    ecs_iter_t it;
    ecs_query_begin(p->query, &it);
    u32 seen = 0u;
    while (ecs_query_next(&it)) {
        seen++;
        // 在迭代里加组件：立即版会被拒，延后版必须成功
        EXPECT_FALSE(ecs_add(world, it.entity, p->health, NULL) != NULL);
        EXPECT_TRUE(ecs_defer_add(world, it.entity, p->health, NULL));
    }
    ecs_query_end(&it);
    p->iterations_seen = seen;
}

static void commands_apply_at_stage_boundary(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "defer", 64u);

    for (u32 i = 0; i < 4u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
    }
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.health), 0u);

    static ecs_component_t_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.health = fx.health;
    ecs_query_desc_t d = ecs_query_desc_1(fx.vel, "vel");
    probe.query = ecs_query_create(fx.world, &d);
    EXPECT_NOT_NULL(probe.query);

    ecs_system_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = "adder";
    desc.stage = ECS_STAGE_LOGIC;
    desc.priority = 0;
    desc.update = add_health_system;
    desc.user = &probe;
    EXPECT_NOT_NULL(ecs_add_system(fx.world, &desc));

    // TRANSFORM 阶段的世界矩阵求值需要拓扑序；这里不注册 transform 组件，
    // 所以重建会给出 0 个节点 —— 也要能安全跑过。
    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.016f), 1u);
    EXPECT_EQ_U64(probe.iterations_seen, 4u);
    // 命令在 stage 边界落地：这一帧结束时 4 个实体都有 health
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.health), 4u);
    EXPECT_EQ_U64(ecs_command_backlog(fx.world), 0u);

    // 再跑一帧：已经有 health 的实体不重复加（计数不变）
    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.016f), 1u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.health), 4u);

    ecs_query_destroy(fx.world, probe.query);
    ecs_fixture_end(&fx);
}

// 延后销毁：本帧仍然参与，下一帧才消失。
static void destroy_system(ecs_world_t *world, void *user, f32 dt)
{
    ENGINE_UNUSED(dt);
    ecs_component_t_probe *p = (ecs_component_t_probe *)user;
    ecs_iter_t it;
    ecs_query_begin(p->query, &it);
    u32 seen = 0u;
    while (ecs_query_next(&it)) {
        seen++;
        EXPECT_TRUE(ecs_defer_destroy(world, it.entity));
    }
    ecs_query_end(&it);
    p->iterations_seen = seen;
}

static void deferred_destroy_reaps_entities(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "defer_destroy", 64u);

    for (u32 i = 0; i < 5u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
    }

    static ecs_component_t_probe probe;
    memset(&probe, 0, sizeof(probe));
    ecs_query_desc_t d = ecs_query_desc_1(fx.vel, "vel");
    probe.query = ecs_query_create(fx.world, &d);
    ecs_system_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = "reaper";
    desc.stage = ECS_STAGE_CLEANUP;
    desc.update = destroy_system;
    desc.user = &probe;
    EXPECT_NOT_NULL(ecs_add_system(fx.world, &desc));

    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.016f), 1u);
    EXPECT_EQ_U64(probe.iterations_seen, 5u);
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 0u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 0u);

    // 空世界再跑一帧必须安全（0 命中不能变成未定义行为）
    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.016f), 1u);

    ecs_query_destroy(fx.world, probe.query);
    ecs_fixture_end(&fx);
}

// 命令队列溢出要"丢命令 + 报数"，而不是悄悄扩容或写越界。
static void command_overflow_is_reported(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin_cfg(&fx, "overflow", 64u, 64u, 4u, 8u);

    entity_t ents[8];
    for (u32 i = 0; i < 8u; ++i) {
        ents[i] = ecs_create_entity(fx.world);
    }
    u32 accepted = 0u;
    for (u32 i = 0; i < 8u; ++i) {
        if (ecs_defer_add(fx.world, ents[i], fx.vel, NULL)) {
            accepted++;
        }
    }
    EXPECT_EQ_U64(accepted, 4u);

    ecs_world_stats_t stats;
    ecs_world_get_stats(fx.world, &stats);
    EXPECT_GT_U64(stats.command_overflows, 0u);

    EXPECT_EQ_U64(ecs_flush_commands(fx.world), 4u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 4u);

    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 层级

static void hierarchy_parent_moves_children(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "hierarchy", 32u);

    const entity_t root = ecs_create_entity(fx.world);
    const entity_t child = ecs_create_entity(fx.world);
    const entity_t grand = ecs_create_entity(fx.world);

    EXPECT_TRUE(ecs_transform_set_local(fx.world, root, vec3_make(10.0f, 0.0f, 0.0f),
                                            quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_transform_set_local(fx.world, child, vec3_make(0.0f, 5.0f, 0.0f),
                                            quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_transform_set_local(fx.world, grand, vec3_make(0.0f, 0.0f, 2.0f),
                                            quat_identity(), vec3_one()));

    EXPECT_TRUE(ecs_set_parent(fx.world, child, root));
    EXPECT_TRUE(ecs_set_parent(fx.world, grand, child));
    EXPECT_EQ_U64(ecs_hierarchy_depth(fx.world, grand), 2u);
    EXPECT_TRUE(entity_equal(ecs_get_parent(fx.world, child), root));

    ecs_update_world_transforms(fx.world);

    ecs_world_transform_t wt;
    EXPECT_TRUE(ecs_transform_get_world(fx.world, grand, &wt));
    // 世界位置 = 三层平移累加
    EXPECT_NEAR((double)wt.model.col[3].x, 10.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 5.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].z, 2.0, 1e-4);
    EXPECT_EQ_U64(ecs_hierarchy_node_count(fx.world), 3u);

    // 动父节点，子的世界位置必须跟着动
    ecs_transform_set_local(fx.world, root, vec3_make(20.0f, 1.0f, 0.0f), quat_identity(),
                            vec3_one());
    ecs_update_world_transforms(fx.world);
    EXPECT_TRUE(ecs_transform_get_world(fx.world, grand, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 20.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 6.0, 1e-4);

    // 解除父子后，子的世界等于它自己的本地值
    EXPECT_TRUE(ecs_set_parent(fx.world, child, ENTITY_NONE));
    EXPECT_FALSE(ecs_has(fx.world, child, fx.builtin.parent));
    ecs_update_world_transforms(fx.world);
    EXPECT_TRUE(ecs_transform_get_world(fx.world, child, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 0.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 5.0, 1e-4);

    ecs_fixture_end(&fx);
}

// 走**每帧入口**而不是直接调 ecs_update_world_transforms：上面那条测的是求值数学，
// 这一条测的是"什么时候会去求值"。区别很重要 —— 老实现里世界矩阵的求值挂在
// "遇到第一个注册在 TRANSFORM 阶段的系统"上，于是没有任何系统的世界里，
// 改了 transform 画面永远不动。那条路径上所有单测都是绿的，因为它们都直接调了
// 内部函数，绕开了真正会出问题的那一层。
//
// 症状不是崩溃也不是报错，而是"数据对了，屏幕上不动"：父子链、动画、
// 相机挂点全部静默失效，最难归因的一类。
static void hierarchy_world_transform_updates_via_world_update(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "hierarchy_entry", 32u);

    // 一个系统都不注册 —— 这正是要复现的那个配置
    EXPECT_EQ_U64(ecs_system_count(fx.world), 0u);

    const entity_t root = ecs_create_entity(fx.world);
    const entity_t child = ecs_create_entity(fx.world);
    const entity_t leaf = ecs_create_entity(fx.world);
    EXPECT_TRUE(ecs_transform_set_local(fx.world, root, vec3_make(1.0f, 0.0f, 0.0f),
                                        quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_transform_set_local(fx.world, child, vec3_make(0.0f, 2.0f, 0.0f),
                                        quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_transform_set_local(fx.world, leaf, vec3_make(0.0f, 0.0f, 3.0f),
                                        quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_set_parent(fx.world, child, root));
    EXPECT_TRUE(ecs_set_parent(fx.world, leaf, child));

    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.0f), 0u); // 没有系统，但阶段仍然要走完
    ecs_world_transform_t wt;
    EXPECT_TRUE(ecs_transform_get_world(fx.world, leaf, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 1.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 2.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].z, 3.0, 1e-4);

    // 只改本地变换（不碰结构、不碰任何系统），一次 update 之后叶子必须跟上。
    // 三层链要"每帧传播一层"的话，这里读到的还是旧值 —— 老实现正是这样。
    EXPECT_TRUE(ecs_transform_set_local(fx.world, root, vec3_make(9.0f, 8.0f, 7.0f),
                                        quat_identity(), vec3_one()));
    EXPECT_EQ_U64(ecs_world_update(fx.world, 0.0f), 0u);
    EXPECT_TRUE(ecs_transform_get_world(fx.world, leaf, &wt));
    // 累加：root(9,8,7) + child 本地(0,2,0) + leaf 本地(0,0,3)
    EXPECT_NEAR((double)wt.model.col[3].x, 9.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 10.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].z, 10.0, 1e-4);

    // 同一次 update 里，中间层也要是新的（防"只更新了根和叶"这种巧合通过）
    EXPECT_TRUE(ecs_transform_get_world(fx.world, child, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 9.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 10.0, 1e-4);

    ecs_fixture_end(&fx);
}

// 在世界更新**之后**才创建的实体，也必须在下一次更新里拿到世界变换。
//
// 这条抓的是拓扑序的标脏：重建只在 hierarchy_dirty 时发生，而
// ecs_create_entity 原来只标 dense_dirty 不标 hierarchy_dirty。
// 结果不是崩溃也不是报错 —— 新实体进不了那次拓扑快照，于是永远算不出世界矩阵，
// 症状是"实体建成功了、组件也都在、就是看不见"。
// 为什么 06_scene 里看不出来：整场景加载发生在第一次更新之前（世界初始化时标过脏），
// 而带子节点的预制体在 set_parent 时才标脏。漏出来的形态是"单实体预制体第二次
// 实例化就不出现"，见 tests/test_prefab.c 的 is_a_snapshot_not_a_window。
static void ecs_entity_created_after_update_gets_world_transform(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "late_entity", 32u);

    // 先跑一次更新：这一次会把"当前存在的实体"收进拓扑序并把脏位清掉
    const entity_t early = ecs_create_entity(fx.world);
    EXPECT_TRUE(ecs_transform_set_local(fx.world, early, vec3_make(1.0f, 0.0f, 0.0f),
                                        quat_identity(), vec3_one()));
    (void)ecs_world_update(fx.world, 0.0f);
    ecs_world_transform_t wt;
    EXPECT_TRUE(ecs_transform_get_world(fx.world, early, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 1.0, 1e-4);

    // 之后再建的实体：没有 set_parent、没有别的结构事件，只有"它存在了"
    ecs_world_stats_t stats_before;
    ecs_world_get_stats(fx.world, &stats_before);
    const entity_t late = ecs_create_entity(fx.world);
    EXPECT_TRUE(ecs_transform_set_local(fx.world, late, vec3_make(5.0f, 2.0f, 0.0f),
                                        quat_identity(), vec3_one()));
    (void)ecs_world_update(fx.world, 0.0f);

    EXPECT_TRUE(ecs_transform_get_world(fx.world, late, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 5.0, 1e-4);
    EXPECT_NEAR((double)wt.model.col[3].y, 2.0, 1e-4);
    ecs_world_stats_t stats_after;
    ecs_world_get_stats(fx.world, &stats_after);
    EXPECT_GT_U64((usize)stats_after.hierarchy_rebuilds, (usize)stats_before.hierarchy_rebuilds);
    EXPECT_GE_U64((usize)ecs_hierarchy_node_count(fx.world), 2u);

    // 给一个早就存在的实体补上 Transform，同样要进得了拓扑序
    const entity_t bare = ecs_create_entity(fx.world);
    (void)ecs_world_update(fx.world, 0.0f); // 它还没有 Transform
    EXPECT_NULL(ecs_peek(fx.world, bare, fx.builtin.world_transform));
    (void)ecs_transform_get_or_add(fx.world, bare);
    (void)ecs_transform_set_local(fx.world, bare, vec3_make(0.0f, 7.0f, 0.0f), quat_identity(),
                                  vec3_one());
    (void)ecs_world_update(fx.world, 0.0f);
    EXPECT_TRUE(ecs_transform_get_world(fx.world, bare, &wt));
    EXPECT_NEAR((double)wt.model.col[3].y, 7.0, 1e-4);

    ecs_fixture_end(&fx);
}

// 尺度必须沿链累乘：只看父矩阵不看缩放，是非均匀缩放下最常见的错。
static void hierarchy_scale_composes(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "hierarchy_scale", 32u);

    const entity_t parent = ecs_create_entity(fx.world);
    const entity_t child = ecs_create_entity(fx.world);
    ecs_transform_set_local(fx.world, parent, vec3_zero(), quat_identity(),
                            vec3_make(2.0f, 2.0f, 2.0f));
    ecs_transform_set_local(fx.world, child, vec3_make(3.0f, 0.0f, 0.0f), quat_identity(),
                            vec3_one());
    EXPECT_TRUE(ecs_set_parent(fx.world, child, parent));
    ecs_update_world_transforms(fx.world);

    ecs_world_transform_t wt;
    EXPECT_TRUE(ecs_transform_get_world(fx.world, child, &wt));
    EXPECT_NEAR((double)wt.model.col[3].x, 6.0, 1e-4); // 子的 x=3 被父的 2 倍缩放放大
    // 法线矩阵是 (M^-1)^T 的 3x3：均匀缩放 2 倍时它应是 1/2，
    // 也就是说"缩放会让法线变短"——着色器里不重新归一化就会算错光。
    EXPECT_NEAR((double)wt.normal_matrix.col[1].y, 0.5, 1e-4);

    ecs_fixture_end(&fx);
}

static void hierarchy_rejects_cycles(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "cycle", 32u);

    const entity_t a = ecs_create_entity(fx.world);
    const entity_t b = ecs_create_entity(fx.world);
    const entity_t c = ecs_create_entity(fx.world);
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, a));
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, b));
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, c));

    EXPECT_TRUE(ecs_set_parent(fx.world, b, a));
    EXPECT_TRUE(ecs_set_parent(fx.world, c, b));

    // a 已经是 c 的祖先：c -> a 会闭环
    EXPECT_FALSE(ecs_set_parent(fx.world, a, c));
    // 自己当自己的父
    EXPECT_FALSE(ecs_set_parent(fx.world, a, a));

    ecs_world_stats_t stats;
    ecs_world_get_stats(fx.world, &stats);
    EXPECT_GT_U64(stats.hierarchy_rejected_cycles, 0u);

    // 拒绝之后链没有被破坏：再跑一次层级重建与求值必须正常收敛
    EXPECT_TRUE(ecs_rebuild_hierarchy(fx.world));
    ecs_update_world_transforms(fx.world);
    EXPECT_EQ_U64(ecs_hierarchy_depth(fx.world, c), 2u);

    ecs_fixture_end(&fx);
}

static void hierarchy_rejects_excessive_depth(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin_cfg(&fx, "depth_cap", 64u, 64u, 64u, 4u);

    entity_t chain[10];
    chain[0] = ecs_create_entity(fx.world);
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, chain[0]));
    bool built_all = true;
    for (u32 i = 1; i < 10u; ++i) {
        chain[i] = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, chain[i]));
        const bool ok = ecs_set_parent(fx.world, chain[i], chain[i - 1u]);
        if (!ok) {
            built_all = false;
            break;
        }
    }
    EXPECT_FALSE(built_all); // 第 5 层（深度 4）之后必须开始拒绝

    // 前 4 条挂接成功
    EXPECT_EQ_U64(ecs_hierarchy_depth(fx.world, chain[4]), 4u);

    ecs_fixture_end(&fx);
}

// 脏标记：没改层级就不该重算拓扑序
static void hierarchy_rebuild_is_lazy_and_counted(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "lazy", 32u);

    const entity_t a = ecs_create_entity(fx.world);
    const entity_t b = ecs_create_entity(fx.world);
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, a));
    EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, b));

    ecs_world_stats_t before;
    ecs_world_get_stats(fx.world, &before);

    ecs_update_world_transforms(fx.world);
    ecs_world_stats_t mid;
    ecs_world_get_stats(fx.world, &mid);
    EXPECT_GT_U64(mid.hierarchy_rebuilds, before.hierarchy_rebuilds);

    ecs_update_world_transforms(fx.world);
    ecs_world_stats_t after;
    ecs_world_get_stats(fx.world, &after);
    EXPECT_EQ_U64(after.hierarchy_rebuilds, mid.hierarchy_rebuilds); // 没脏，不重建

    EXPECT_TRUE(ecs_set_parent(fx.world, b, a));
    ecs_update_world_transforms(fx.world);
    ecs_world_stats_t dirty;
    ecs_world_get_stats(fx.world, &dirty);
    EXPECT_GT_U64(dirty.hierarchy_rebuilds, after.hierarchy_rebuilds);

    ecs_fixture_end(&fx);
}

// ---------------------------------------------------------------- 规模与统计

static void ecs_many_entities_component_access(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin_with_capacity(&fx, "many", 1100u, 2048u);

    enum { N = 1000 };
    entity_t ents[N];
    for (u32 i = 0; i < N; ++i) {
        ents[i] = ecs_create_entity(fx.world);
        test_vel_t v = { (f32)i, (f32)(N - i) };
        EXPECT_NOT_NULL(ecs_add(fx.world, ents[i], fx.vel, &v));
    }
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), (usize)N);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), (usize)N);

    // 抽查（全量核对在上面 12 个元素的用例里做过，这里只是确认规模下不错位）
    for (u32 i = 0; i < N; i += 97u) {
        const test_vel_t *v = (const test_vel_t *)ecs_peek(fx.world, ents[i], fx.vel);
        EXPECT_NOT_NULL(v);
        if (v != NULL) {
            EXPECT_NEAR((double)v->x, (double)i, 1e-3);
            EXPECT_NEAR((double)v->y, (double)(N - i), 1e-3);
        }
    }

    // 全部销毁后，存储占用应归零
    for (u32 i = 0; i < N; ++i) {
        EXPECT_TRUE(ecs_destroy_entity(fx.world, ents[i]));
    }
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 0u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 0u);

    ecs_world_stats_t stats;
    ecs_world_get_stats(fx.world, &stats);
    EXPECT_EQ_U64(stats.entity_peak, (usize)N);
    EXPECT_GT_U64(stats.memory_bytes, 0u);

    ecs_fixture_end(&fx);
}

// 池满必须报错而不是踩内存
static void ecs_entity_pool_full_is_reported(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "full", 8u);

    u32 created = 0u;
    for (u32 i = 0; i < 32u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        if (!entity_is_valid(e)) {
            break;
        }
        created++;
    }
    EXPECT_EQ_U64(created, 8u);
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 8u);

    ecs_fixture_end(&fx);
}

// world_clear 之后组件类型仍然可用（重载场景的用法）
static void ecs_clear_keeps_registry(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "clear", 32u);

    for (u32 i = 0; i < 5u; ++i) {
        const entity_t e = ecs_create_entity(fx.world);
        EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));
    }
    ecs_world_clear(fx.world);
    EXPECT_EQ_U64(ecs_alive_entity_count(fx.world), 0u);
    EXPECT_EQ_U64(ecs_component_count(fx.world, fx.vel), 0u);
    EXPECT_EQ_U64(ecs_component_type_count(fx.world), 7u); // 3 内置 + 4 测试

    const entity_t e = ecs_create_entity(fx.world);
    EXPECT_NOT_NULL(ecs_add(fx.world, e, fx.vel, NULL));

    ecs_fixture_end(&fx);
}

// dense id 往返
static void ecs_dense_id_mapping(void)
{
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "dense", 32u);

    entity_t ents[5];
    for (u32 i = 0; i < 5u; ++i) {
        ents[i] = ecs_create_entity(fx.world);
    }
    ecs_rebuild_dense_index(fx.world);

    for (u32 i = 0; i < 5u; ++i) {
        const dense_id_t dense = ecs_entity_to_dense(fx.world, ents[i]);
        EXPECT_EQ_U64(dense, (usize)i);
        EXPECT_TRUE(entity_equal(ecs_entity_from_dense(fx.world, dense), ents[i]));
    }
    EXPECT_EQ_U64(ecs_entity_to_dense(fx.world, ENTITY_NONE), (usize)ECS_DENSE_NONE);
    EXPECT_FALSE(entity_is_valid(ecs_entity_from_dense(fx.world, 999u)));

    ecs_fixture_end(&fx);
}

// 按 handle_index 索引的数组（row_of、presence 位图、深度缓存）取值上界是
// entity_capacity，也就是 pool_capacity()。少算一项不是"最后一个实体查不到"，
// 而是越界：深度缓存那一格从没被初始化过，读出来是堆上的垃圾值。
//
// 这条测试把实体池**填满**，于是最后一个实体的 handle_index 正好落在那一格上，
// 然后检查层级重建的结果。没有 +1 的那一版在这里会拿到一个三十亿的"深度"，
// 症状是 ecs_rebuild_hierarchy 事实上再也不返回（示例表现为"卡死"）。
static void ecs_last_slot_of_index_space_is_usable(void)
{
    const u32 cap = 16u;
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "last_slot", cap);

    entity_t ents[16];
    for (u32 i = 0; i < cap; ++i) {
        ents[i] = ecs_create_entity(fx.world);
        EXPECT_TRUE(entity_is_valid(ents[i]));
        EXPECT_NOT_NULL(ecs_transform_get_or_add(fx.world, ents[i]));
    }
    // 星形而不是长链：链会撞上 hierarchy_depth_max 的降级路径，
    // 而这条测试要验的是"最后一个槽位的数据是对的"，不是环检测。
    for (u32 i = 1u; i < cap; ++i) {
        EXPECT_TRUE(ecs_set_parent(fx.world, ents[i], ents[0]));
    }

    EXPECT_TRUE(ecs_rebuild_hierarchy(fx.world));
    EXPECT_EQ_U64(ecs_hierarchy_node_count(fx.world), (usize)cap);
    // 深度只有 0 和 1，所以"实测最大深度"必须是 2。
    // 这一条就是垃圾值的探针：它要是变成一个大数，说明又有一格没被初始化。
    EXPECT_EQ_U64(ecs_hierarchy_depth_max_seen(fx.world), 2u);

    // 关键的一条：按层重排里"深度超过上限就按根节点处理"那道兜底**不该被触发**。
    // 触发了就说明某个实体的深度是从没初始化过的格子里读出来的 ——
    // 也就是按 handle_index 开的数组少算了一项（+1 的那个 off-by-one）。
    // 光看深度值和"不卡死"是抓不到它的：兜底会把垃圾值降级成 0，
    // 于是症状全被盖住，只剩这个计数器会说真话。
    ecs_world_stats_t stats;
    ecs_world_get_stats(fx.world, &stats);
    EXPECT_EQ_U64(stats.hierarchy_rejected_cycles, 0u);

    ecs_update_world_transforms(fx.world);

    // 最后一个实体（handle_index == capacity，正好是过去少算的那一格）
    const ecs_world_transform_t *last_wt =
        (const ecs_world_transform_t *)ecs_peek(fx.world, ents[cap - 1u],
                                                fx.builtin.world_transform);
    EXPECT_NOT_NULL(last_wt);
    if (last_wt != NULL) {
        EXPECT_VEC3_NEAR(mat4_get_translation(last_wt->model), vec3_zero(), 1e-5f);
    }
    EXPECT_TRUE(ecs_has(fx.world, ents[cap - 1u], fx.builtin.world_transform));

    ecs_fixture_end(&fx);
}

// dense id 要在实体销毁之后能被重新发出去。
//
// 以前的语义是"next_dense_id 只涨不降"，那说的是"总共创建过多少实体"，
// 而 dense_to_entity 只有 entity_capacity 项 —— 于是编辑器里删一个建一个，
// 迟早新实体的 dense 越界：它建成功了，但**不出现在 ecs_collect_entities 里**，
// 也就永远不会被存盘，且不报任何错。
static void ecs_dense_ids_are_reused_after_destroy(void)
{
    const u32 cap = 8u;
    ecs_fixture_t fx;
    ecs_fixture_begin(&fx, "dense_reuse", cap);

    entity_t first[8];
    for (u32 i = 0; i < cap; ++i) {
        first[i] = ecs_create_entity(fx.world);
        EXPECT_TRUE(entity_is_valid(first[i]));
    }
    for (u32 i = cap / 2u; i < cap; ++i) {
        EXPECT_TRUE(ecs_destroy_entity(fx.world, first[i]));
    }

    // 水位线已经在 cap 上了：没有复用机制的话这一步就会失败（或被挡下）
    entity_t second[4];
    for (u32 i = 0; i < cap / 2u; ++i) {
        second[i] = ecs_create_entity(fx.world);
        EXPECT_TRUE(entity_is_valid(second[i]));
    }

    ecs_rebuild_dense_index(fx.world);
    EXPECT_EQ_U64(ecs_collect_entities(fx.world, NULL, 0u), (usize)cap);

    // 每个存活实体都要能被 dense 找回来，且 dense 互不相同
    entity_t alive[8];
    const u32 got = ecs_collect_entities(fx.world, alive, 8u);
    EXPECT_EQ_U64(got, (usize)cap);
    dense_id_t seen[8];
    for (u32 i = 0; i < got; ++i) {
        const dense_id_t d = ecs_entity_to_dense(fx.world, alive[i]);
        EXPECT_TRUE(d < (usize)cap);
        EXPECT_TRUE(entity_equal(ecs_entity_from_dense(fx.world, d), alive[i]));
        seen[i] = d;
    }
    for (u32 i = 0; i < got; ++i) {
        for (u32 j = i + 1u; j < got; ++j) {
            EXPECT_TRUE(seen[i] != seen[j]); // 重复的 dense = 存档里两个实体互相覆盖
        }
    }
    EXPECT_TRUE(entity_is_valid(second[0])); // 新建的那批确实活着（不是只拿到号）

    ecs_fixture_end(&fx);
}

// =============================================================================
void test_ecs_register(void)
{
    test_register("ecs", "entity_lifecycle", ecs_entity_lifecycle);
    test_register("ecs", "stale_handle_is_rejected", ecs_stale_handle_is_rejected);
    test_register("ecs", "add_get_remove", ecs_add_get_remove);
    test_register("ecs", "construct_fills_defaults", ecs_construct_fills_defaults);
    test_register("ecs", "swap_remove_keeps_indices_consistent",
                  ecs_swap_remove_keeps_indices_consistent);
    test_register("ecs", "destroy_removes_all_components", ecs_destroy_removes_all_components);
    test_register("ecs", "register_validates_field_table", ecs_register_validates_field_table);
    test_register("ecs", "component_capacity_is_enforced", ecs_component_capacity_is_enforced);
    test_register("ecs", "query_matches_all_terms", ecs_query_matches_all_terms);
    test_register("ecs", "query_none_excludes", ecs_query_none_excludes);
    test_register("ecs", "query_picks_smallest_driver", ecs_query_picks_smallest_driver);
    test_register("ecs", "mutation_during_iteration_is_rejected",
                  ecs_mutation_during_iteration_is_rejected);
    test_register("ecs", "systems_run_in_deterministic_order",
                  systems_run_in_deterministic_order);
    test_register("ecs", "commands_apply_at_stage_boundary", commands_apply_at_stage_boundary);
    test_register("ecs", "deferred_destroy_reaps_entities", deferred_destroy_reaps_entities);
    test_register("ecs", "command_overflow_is_reported", command_overflow_is_reported);
    test_register("ecs", "parent_moves_children", hierarchy_parent_moves_children);
    test_register("ecs", "scale_composes", hierarchy_scale_composes);
    test_register("ecs", "world_transform_updates_via_world_update",
                  hierarchy_world_transform_updates_via_world_update);
    test_register("ecs", "entity_created_after_update_gets_world_transform",
                  ecs_entity_created_after_update_gets_world_transform);
    test_register("ecs", "rejects_cycles", hierarchy_rejects_cycles);
    test_register("ecs", "rejects_excessive_depth", hierarchy_rejects_excessive_depth);
    test_register("ecs", "rebuild_is_lazy_and_counted", hierarchy_rebuild_is_lazy_and_counted);
    test_register("ecs", "many_entities_component_access", ecs_many_entities_component_access);
    test_register("ecs", "entity_pool_full_is_reported", ecs_entity_pool_full_is_reported);
    test_register("ecs", "clear_keeps_registry", ecs_clear_keeps_registry);
    test_register("ecs", "dense_id_mapping", ecs_dense_id_mapping);
    test_register("ecs", "last_slot_of_index_space_is_usable",
                  ecs_last_slot_of_index_space_is_usable);
    test_register("ecs", "dense_ids_are_reused_after_destroy",
                  ecs_dense_ids_are_reused_after_destroy);
}
