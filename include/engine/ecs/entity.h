// =============================================================================
//  engine/ecs/entity.h —— 实体
//
//  ---- 实体就是一个代际句柄 ----
//
//  直接复用 M0 的 handle_t（24 位索引 + 8 位代际），不另发明一套 id。
//  理由：pool 已经把"槽位复用 + 陈旧句柄检测"实现并测过了，
//  再造一套只会得到两套会各自出 bug 的机制。
//
//  实体本身**不含任何组件**。它是"一行身份"，组件按类型存在各自的池里
//  （见 component.h）。所以：
//      ecs_create_entity()   只占一个身份槽 —— 立刻生效是安全的
//      ecs_destroy_entity()  要摘掉它身上的所有组件 —— 必须延后
//
//  ---- dense id（给序列化用）----
//
//  handle_t 的 index 依赖"池的当前状态"：存档里写着 index=17，
//  下次启动时 17 号槽可能属于完全不同的实体。所以**句柄不进存档**，
//  改用 dense id：加载顺序里第几个实体就是几。
//  world 维护 entity ↔ dense id 双向映射，仅在建/存场景时使用；
//  运行时的热路径一律用 handle_t。
// =============================================================================
#ifndef ENGINE_ECS_ENTITY_H
#define ENGINE_ECS_ENTITY_H

#include "engine/core/types.h"

typedef handle_t entity_t;

#define ENTITY_NONE ((entity_t)0) // 与 HANDLE_INVALID 同值：0 号槽永久保留

static inline bool entity_is_valid(entity_t e)
{
    return handle_is_valid(e);
}

static inline bool entity_equal(entity_t a, entity_t b)
{
    return a == b;
}

// 序列化用的紧凑编号。ECS_DENSE_NONE 表示"不属于本场景"（比如指向场景外的引用）。
typedef u32 dense_id_t;
#define ECS_DENSE_NONE ((dense_id_t)0xFFFFFFFFu)

#endif // ENGINE_ECS_ENTITY_H
