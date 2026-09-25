// =============================================================================
//  tests/ecs_test_components.h —— 测试专用组件
//
//  单独一个头是因为 ecs_system.c 与 ecs_transform.c 的测试都要用同一批
//  描述表；放 .c 里会让两个测试文件各复制一份，然后各自漂移。
//
//  这些描述表同时也是"字段表怎么手写"的样例，M11 的属性面板会照这个形状长。
// =============================================================================
#ifndef ECS_TEST_COMPONENTS_H
#define ECS_TEST_COMPONENTS_H

#include "engine/core/types.h"
#include "engine/ecs/component.h"
#include "engine/ecs/query.h"

typedef struct test_vel_t {
    f32 x;
    f32 y;
} test_vel_t;

typedef struct test_health_t {
    u32  hp;
    bool invulnerable;
} test_health_t;

// 只有身份没有数据的"标记组件"：字段数为 0，但**必须**能序列化 ——
// 存档里"它有这个标记"这件事本身就是信息。
// 这正是不能拿 field_count==0 同时表示"派生数据"的原因（见 component.h）。
typedef struct test_agent_t {
    u32 unused;
} test_agent_t;

typedef struct test_marker_t {
    char tag[8];
} test_marker_t;

static const ecs_field_desc_t k_test_vel_fields[] = {
    { "x", (u16)ENGINE_OFFSETOF(test_vel_t, x), ECS_FIELD_F32, 1u, 0u, 0u },
    { "y", (u16)ENGINE_OFFSETOF(test_vel_t, y), ECS_FIELD_F32, 1u, 0u, 0u },
};
_Static_assert(sizeof(test_vel_t) == 8, "test_vel_t 布局变了");

static const ecs_field_desc_t k_test_health_fields[] = {
    { "hp", (u16)ENGINE_OFFSETOF(test_health_t, hp), ECS_FIELD_U32, 1u, 0u, 0u },
    { "invulnerable", (u16)ENGINE_OFFSETOF(test_health_t, invulnerable), ECS_FIELD_BOOL, 1u, 0u,
      0u },
};

static const ecs_field_desc_t k_test_marker_fields[] = {
    { "tag", 0u, ECS_FIELD_STRING, 0u, 8u, 0u },
};

// construct：故意写一个非零默认值，用来证明 construct 真的跑了
static void test_vel_construct(void *component)
{
    test_vel_t *v = (test_vel_t *)component;
    v->x = 7.0f;
    v->y = 0.0f;
}

static void test_health_construct(void *component)
{
    test_health_t *h = (test_health_t *)component;
    h->hp = 100u;
    h->invulnerable = false;
}

static const ecs_component_desc_t ecs_test_vel_desc = {
    .name = "TestVel",
    .size = sizeof(test_vel_t),
    .align = 16u,
    .capacity = 0u,
    .fields = k_test_vel_fields,
    .field_count = (u32)(sizeof(k_test_vel_fields) / sizeof(k_test_vel_fields[0])),
    .skip_serialize = false,
    .construct = test_vel_construct,
    .post_load = NULL,
};

static const ecs_component_desc_t ecs_test_health_desc = {
    .name = "TestHealth",
    .size = sizeof(test_health_t),
    .align = 16u,
    .capacity = 0u,
    .fields = k_test_health_fields,
    .field_count = (u32)(sizeof(k_test_health_fields) / sizeof(k_test_health_fields[0])),
    .skip_serialize = false,
    .construct = test_health_construct,
    .post_load = NULL,
};

static const ecs_component_desc_t ecs_test_agent_desc = {
    .name = "TestAgent",
    .size = sizeof(test_agent_t),
    .align = 16u,
    .capacity = 0u,
    .fields = NULL,
    .field_count = 0u,
    .skip_serialize = false, // 标记组件：零字段但仍要进存档
    .construct = NULL,
    .post_load = NULL,
};

static const ecs_component_desc_t ecs_test_marker_desc = {
    .name = "TestMarker",
    .size = sizeof(test_marker_t),
    .align = 16u,
    .capacity = 0u,
    .fields = k_test_marker_fields,
    .field_count = (u32)(sizeof(k_test_marker_fields) / sizeof(k_test_marker_fields[0])),
    .skip_serialize = false,
    .construct = NULL,
    .post_load = NULL,
};

// 系统测试用的探针：把查询与目标组件打包传给 update 函数。
typedef struct ecs_component_t_probe {
    ecs_query_t     *query;
    ecs_component_id health;
    u32              iterations_seen;
    entity_t         last_entity;
} ecs_component_t_probe;

#endif // ECS_TEST_COMPONENTS_H
