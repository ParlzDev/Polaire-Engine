// =============================================================================
//  engine/ecs/system.h —— 系统注册、固定阶段调度、延后命令
//
//  ---- 为什么是"固定阶段"而不是"注册顺序就是执行顺序" ----
//
//  执行顺序随注册顺序漂移，等于把"这一帧读到的是不是上一帧的数据"
//  交给初始化代码的书写顺序 —— 那类 bug 表现为"偶发抖动"，
//  而且换台机器就"好了"。固定 stage + stage 内全序，才让同一份场景
//  在同样输入下产生同样的画面（M2 的渲染队列排序用的是同一条原则）。
//
//  stage 的顺序是照数据流排的：谁生产谁消费一目了然。
//      INPUT -> LOGIC -> MOVE -> RELATION -> TRANSFORM -> RENDER_SUBMIT -> CLEANUP
//  其中 RELATION 在 TRANSFORM 之前：世界矩阵依赖父子链已经稳定。
//  CLEANUP 最后，负责落地延后命令，这样"这一帧被干掉的实体"仍然参与了
//  本帧的变换与提交，不会出现"死人的尸体凭空消失"的观感跳变。
//
//  ---- 延后命令（决策 D5）----
//
//  在系统里加/删组件、销毁实体、改父子，都排进命令队列，
//  在 stage 边界统一执行。原因很具体：交换式移除会移动数组元素，
//  而迭代器手上只有行号。
//
//  注意 **ecs_create_entity 不在这里** —— 它只占一个身份槽，
//  不碰任何组件数组，所以迭代中可以立即创建，没必要绕一圈。
//  能把"哪些操作必须延后"说清楚，比"全都延后"更好用也更好查。
//
//  ---- 命令队列溢出怎么办 ----
//
//  计数 + 丢弃 + 记错误，**不偷偷扩容**。表现是"某个实体没被销毁"
//  （可复现、可归因），而不是"某处内存被写坏"（不可归因）。
// =============================================================================
#ifndef ENGINE_ECS_SYSTEM_H
#define ENGINE_ECS_SYSTEM_H

#include "engine/core/types.h"
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/world.h"

typedef enum ecs_stage_t {
    ECS_STAGE_INPUT = 0,    // 采样输入、写意图组件
    ECS_STAGE_LOGIC,        // 玩法决策：生成/销毁/状态机
    ECS_STAGE_MOVE,         // 速度积分到本地 transform
    ECS_STAGE_RELATION,     // 父子、约束：必须在 TRANSFORM 之前
    ECS_STAGE_TRANSFORM,    // 算世界矩阵与法线矩阵
    ECS_STAGE_RENDER_SUBMIT,// 往渲染队列提交（M5 的桥接系统）
    ECS_STAGE_CLEANUP,      // 生命周期回收 + 落地延后命令
    ECS_STAGE_COUNT
} ecs_stage_t;

const char *ecs_stage_name(ecs_stage_t stage);

// 延后命令里能内联存下的组件初始数据上限。
// 超过就走"先加默认值、之后系统里再改"的路子 —— 而不是在命令里塞指针：
// 指针的有效期到不了 flush 那一刻，那种 API 设计是在制造悬垂。
#define ECS_COMMAND_PAYLOAD_CAPACITY 64u

typedef struct ecs_system_desc_t {
    const char *name; // 必填：全序的最后一环，也是日志里唯一能认的东西
    ecs_stage_t stage;
    i32         priority; // 同 stage 内小者先；相同则按 name 字典序

    void (*update)(ecs_world_t *world, void *user, f32 delta_seconds);
    void *user;

    // 可选：系统自己的查询在 world 里创建前的准备/清理。
    // 用得很少，但没它就得让每个系统自己维护一个全局状态。
    bool (*startup)(ecs_world_t *world, void *user);
    void (*shutdown)(ecs_world_t *world, void *user);
} ecs_system_desc_t;

typedef struct ecs_system_t ecs_system_t;

#define ECS_MAX_SYSTEMS 64u

// 注册系统。startup 会立即调用；返回 NULL 表示失败（表满 / 名字重复 / startup 失败）。
// 名字重复会被拒：两个同名系统会让"按名字定序"这条规则变成随机的。
ecs_system_t *ecs_add_system(ecs_world_t *world, const ecs_system_desc_t *desc);
bool          ecs_remove_system(ecs_world_t *world, ecs_system_t *system);
u32           ecs_system_count(const ecs_world_t *world);
// 按注册顺序（= 执行顺序）取第 i 个系统，用于调试面板。
const ecs_system_t *ecs_system_at(const ecs_world_t *world, u32 index);
const char         *ecs_system_name(const ecs_system_t *system);
ecs_stage_t         ecs_system_stage(const ecs_system_t *system);

// 跑一帧：按 stage 顺序执行系统，并在每个 stage 结束后落地延后命令。
// 返回本次执行的系统数（0 说明一个系统都没注册 —— 通常是忘了加系统）。
u32 ecs_world_update(ecs_world_t *world, f32 delta_seconds);

// ---------------------------------------------------------------- 延后操作
//
// 这些是**给系统用的**。在系统外（没有迭代器在跑）应当直接用 component.h
// 里的立即版，那样错误会当场暴露而不是隔一个 stage 才出现。

bool ecs_defer_destroy(ecs_world_t *world, entity_t entity);
// data 为 NULL 时按 construct 初始化；非 NULL 时要求组件尺寸 <= 64 字节。
bool ecs_defer_add(ecs_world_t *world, entity_t entity, ecs_component_id id, const void *data);
bool ecs_defer_remove(ecs_world_t *world, entity_t entity, ecs_component_id id);
// parent 传 ENTITY_NONE 表示解除父子关系。
bool ecs_defer_set_parent(ecs_world_t *world, entity_t child, entity_t parent);

// 立刻落地命令队列。ecs_world_update 内部会调；
// 公开出来是为了"加载完场景就要马上看到组件"这种时候不必假装跑一帧。
u32 ecs_flush_commands(ecs_world_t *world);
u32 ecs_command_backlog(const ecs_world_t *world);

#endif // ENGINE_ECS_SYSTEM_H
