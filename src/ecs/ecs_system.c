// =============================================================================
//  src/ecs/ecs_system.c —— 阶段调度与延后命令
// =============================================================================
#include "engine/ecs/system.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "ecs_internal.h"

const char *ecs_stage_name(ecs_stage_t stage)
{
    static const char *const k_names[ECS_STAGE_COUNT] = {
        "INPUT", "LOGIC", "MOVE", "RELATION", "TRANSFORM", "RENDER_SUBMIT", "CLEANUP",
    };
    if ((unsigned)stage >= (unsigned)ECS_STAGE_COUNT) {
        return "?";
    }
    return k_names[stage];
}

// ---------------------------------------------------------------- 注册表

// 同 (stage, priority) 内按名字定序 —— 名字必须唯一，否则这一层全序就不成立，
// 执行顺序会退化成"注册顺序"，也就是我们想摆脱的那个东西。
static int ecs_system_compare(const void *a, const void *b)
{
    const ecs_system_t *sa = (const ecs_system_t *)a;
    const ecs_system_t *sb = (const ecs_system_t *)b;

    if (sa->desc.stage != sb->desc.stage) {
        return ((int)sa->desc.stage < (int)sb->desc.stage) ? -1 : 1;
    }
    if (sa->desc.priority != sb->desc.priority) {
        return (sa->desc.priority < sb->desc.priority) ? -1 : 1;
    }
    const int cmp = strcmp(sa->desc.name ? sa->desc.name : "", sb->desc.name ? sb->desc.name : "");
    if (cmp != 0) {
        return (cmp < 0) ? -1 : 1;
    }
    // 名字也相同的话用指针定序，保证 qsort 的输入是**全序**的。
    // qsort 不是稳定排序，比较函数一旦构成偏序，同样的输入可能排出不同顺序 ——
    // 这条原则和 M2 的渲染队列排序完全一致。
    return (sa < sb) ? -1 : 1;
}

static void ecs_sort_systems(ecs_world_t *world)
{
    if (world->system_count < 2u) {
        return;
    }
    qsort(world->systems, world->system_count, sizeof(ecs_system_t), ecs_system_compare);
}

ecs_system_t *ecs_add_system(ecs_world_t *world, const ecs_system_desc_t *desc)
{
    ASSERT(world != NULL);

    if (!world->ready) {
        LOG_ERROR("世界未初始化，无法注册系统");
        return NULL;
    }
    if (desc == NULL || desc->update == NULL || desc->name == NULL || desc->name[0] == '\0') {
        LOG_ERROR("系统描述不完整（name 与 update 必填）");
        return NULL;
    }
    if ((unsigned)desc->stage >= (unsigned)ECS_STAGE_COUNT) {
        LOG_ERROR("系统 %s 的 stage 非法", desc->name);
        return NULL;
    }
    if (world->system_count >= ECS_MAX_SYSTEMS) {
        LOG_ERROR("系统表已满（%u）", ECS_MAX_SYSTEMS);
        return NULL;
    }
    for (u32 i = 0; i < world->system_count; ++i) {
        if (world->systems[i].registered && strcmp(world->systems[i].desc.name, desc->name) == 0) {
            LOG_ERROR("系统名字重复：%s —— 同名会让按名字定序变成随机的", desc->name);
            return NULL;
        }
    }

    ecs_system_t *s = &world->systems[world->system_count];
    memset(s, 0, sizeof(*s));
    s->desc = *desc;
    s->registered = true;
    s->enabled = true;
    world->system_count++;
    ecs_sort_systems(world);

    // 重新排序后指针可能移动，所以要**排序完成之后**再找回来。
    // （qsort 移动的是数组元素本身，所以这里按名字重新定位。）
    ecs_system_t *placed = NULL;
    for (u32 i = 0; i < world->system_count; ++i) {
        if (strcmp(world->systems[i].desc.name, desc->name) == 0) {
            placed = &world->systems[i];
            break;
        }
    }
    if (placed == NULL) {
        LOG_ERROR("内部错误：注册后找不到系统 %s", desc->name);
        return NULL;
    }
    if (placed->desc.startup != NULL && !placed->desc.startup(world, placed->desc.user)) {
        LOG_ERROR("系统 %s startup 失败，取消注册", placed->desc.name);
        memset(placed, 0, sizeof(*placed));
        world->system_count--;
        ecs_sort_systems(world);
        return NULL;
    }
    return placed;
}

bool ecs_remove_system(ecs_world_t *world, ecs_system_t *system)
{
    if (world == NULL || system == NULL || !system->registered) {
        return false;
    }
    if (system->desc.shutdown != NULL) {
        system->desc.shutdown(world, system->desc.user);
    }
    // 用最后一个元素填坑，保持数组紧凑（顺序由 sort 决定，与位置无关）
    const usize pos = (usize)(system - world->systems);
    if (pos >= world->system_count) {
        return false;
    }
    memcpy(&world->systems[pos], &world->systems[world->system_count - 1u], sizeof(ecs_system_t));
    memset(&world->systems[world->system_count - 1u], 0, sizeof(ecs_system_t));
    world->system_count--;
    ecs_sort_systems(world);
    return true;
}

u32 ecs_system_count(const ecs_world_t *world)
{
    return (world != NULL) ? world->system_count : 0u;
}

const ecs_system_t *ecs_system_at(const ecs_world_t *world, u32 index)
{
    if (world == NULL || index >= world->system_count) {
        return NULL;
    }
    return &world->systems[index];
}

const char *ecs_system_name(const ecs_system_t *system)
{
    if (system == NULL || system->desc.name == NULL) {
        return "?";
    }
    return system->desc.name;
}

ecs_stage_t ecs_system_stage(const ecs_system_t *system)
{
    return (system != NULL) ? system->desc.stage : ECS_STAGE_COUNT;
}

// ---------------------------------------------------------------- 命令队列

static bool ecs_command_push(ecs_world_t *world, const ecs_command_t *cmd)
{
    if (array_size(&world->commands) >= world->config.command_capacity) {
        world->command_overflows++;
        LOG_ERROR("命令队列已满（%u）。这条命令被丢弃 —— 提高 command_capacity。",
                  world->config.command_capacity);
        return false;
    }
    void *slot = array_push_uninit(&world->commands);
    if (slot == NULL) {
        world->command_overflows++;
        return false;
    }
    memcpy(slot, cmd, sizeof(ecs_command_t));
    return true;
}

static bool ecs_command_common(ecs_world_t *world, ecs_command_t *cmd, u8 kind, entity_t a)
{
    ASSERT(world != NULL);
    if (!world->ready) {
        return false;
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->kind = kind;
    cmd->a = a;
    if (!entity_is_valid(a)) {
        LOG_ERROR("命令的主体实体非法");
        return false;
    }
    return true;
}

bool ecs_defer_destroy(ecs_world_t *world, entity_t entity)
{
    ecs_command_t cmd;
    if (!ecs_command_common(world, &cmd, (u8)ECS_CMD_DESTROY_ENTITY, entity)) {
        return false;
    }
    return ecs_command_push(world, &cmd);
}

bool ecs_defer_add(ecs_world_t *world, entity_t entity, ecs_component_id id, const void *data)
{
    ecs_command_t cmd;
    if (!ecs_command_common(world, &cmd, (u8)ECS_CMD_ADD_COMPONENT, entity)) {
        return false;
    }
    const ecs_storage_t *s = ecs_storage_of(world, id);
    if (s == NULL) {
        LOG_ERROR("defer_add 引用了未注册的组件类型 %u", (unsigned)id);
        return false;
    }
    cmd.type = id;
    if (data != NULL) {
        if (s->desc->size > ECS_COMMAND_PAYLOAD_CAPACITY) {
            // 不在命令里存指针：指针的有效期到不了 flush 那一刻，
            // 那样设计出来的 API 一定会在某个地方变成悬垂读。
            LOG_ERROR("组件 %s 尺寸 %u 超过命令内联上限 %u，defer_add 不接受初始数据",
                      s->desc->name, s->desc->size, (unsigned)ECS_COMMAND_PAYLOAD_CAPACITY);
            return false;
        }
        memcpy(cmd.payload, data, s->desc->size);
        cmd.payload_size = s->desc->size;
        cmd.flags = 1u;
    }
    return ecs_command_push(world, &cmd);
}

bool ecs_defer_remove(ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    ecs_command_t cmd;
    if (!ecs_command_common(world, &cmd, (u8)ECS_CMD_REMOVE_COMPONENT, entity)) {
        return false;
    }
    if (ecs_storage_of(world, id) == NULL) {
        LOG_ERROR("defer_remove 引用了未注册的组件类型 %u", (unsigned)id);
        return false;
    }
    cmd.type = id;
    return ecs_command_push(world, &cmd);
}

bool ecs_defer_set_parent(ecs_world_t *world, entity_t child, entity_t parent)
{
    ecs_command_t cmd;
    if (!ecs_command_common(world, &cmd, (u8)ECS_CMD_SET_PARENT, child)) {
        return false;
    }
    cmd.b = parent;
    return ecs_command_push(world, &cmd);
}

u32 ecs_command_backlog(const ecs_world_t *world)
{
    return (world != NULL) ? array_size(&world->commands) : 0u;
}

// 落地命令时可以再嵌进一层"命令里又产生命令"吗？
// 不允许：先 array_clear 再执行，所以命令里的 ecs_defer_* 会排到**下一轮**。
// 这个取舍写在这里，是因为它的后果很具体：
// 一个销毁实体的系统如果能在同一轮里再触发销毁，销毁顺序就依赖实现细节了。
u32 ecs_flush_commands(ecs_world_t *world)
{
    ASSERT(world != NULL);

    const u32 pending = array_size(&world->commands);
    if (pending == 0u) {
        return 0u;
    }
    if (world->active_iterators > 0u) {
        LOG_ERROR("还有 %u 个迭代器在跑就 flush —— 会在执行命令时移动迭代中的数组",
                  world->active_iterators);
        return 0u;
    }

    // 拷贝一份再清空：命令处理过程中新排进来的命令属于下一轮。
    const usize batch_bytes = (usize)pending * sizeof(ecs_command_t);
    ecs_command_t *batch =
        (ecs_command_t *)world->config.allocator->alloc(world->config.allocator, batch_bytes, 16);
    if (batch == NULL) {
        LOG_ERROR("flush 分配批次内存失败（%u 条命令）", pending);
        return 0u;
    }
    memcpy(batch, array_data(&world->commands), batch_bytes);
    array_clear(&world->commands);

    u32 applied = 0u;
    for (u32 i = 0; i < pending; ++i) {
        ecs_apply_command(&batch[i], world);
        applied++;
    }

    world->config.allocator->free(world->config.allocator, batch, batch_bytes);
    world->commands_flushed += applied;
    return applied;
}

void ecs_apply_command(const ecs_command_t *cmd, ecs_world_t *world)
{
    switch ((ecs_command_kind_t)cmd->kind) {
    case ECS_CMD_DESTROY_ENTITY:
        (void)ecs_destroy_immediate(world, cmd->a);
        break;
    case ECS_CMD_ADD_COMPONENT:
        (void)ecs_add(world, cmd->a, cmd->type,
                      (cmd->flags & 1u) ? (const void *)cmd->payload : NULL);
        break;
    case ECS_CMD_REMOVE_COMPONENT:
        (void)ecs_remove(world, cmd->a, cmd->type);
        break;
    case ECS_CMD_SET_PARENT:
        if (!ecs_set_parent(world, cmd->a, cmd->b)) {
            LOG_ERROR("延后的 set_parent 被拒绝（实体 %u）", (unsigned)handle_index(cmd->a));
        }
        break;
    case ECS_CMD_KIND_COUNT: break;
    }
}

// ---------------------------------------------------------------- 每帧驱动

u32 ecs_world_update(ecs_world_t *world, f32 delta_seconds)
{
    ASSERT(world != NULL);

    if (!world->ready) {
        return 0u;
    }
    if (world->active_iterators > 0u) {
        LOG_ERROR("进入 ecs_world_update 时仍有 %u 个活跃迭代器 —— 有迭代器没被 end",
                  world->active_iterators);
        world->active_iterators = 0u; // 复位，否则之后每一帧都卡在同一个错误上
        return 0u;
    }

    u32 ran = 0u;

    // 按**阶段**走，而不是按"系统列表里下一个是谁"走。
    //
    // 原来这里是遍历系统数组、看到 stage 变了就做一件事：在遇到第一个 TRANSFORM
    // 阶段的系统时顺手求值世界矩阵。后果是"没有任何系统注册在 TRANSFORM 阶段"
    // 的世界里，世界矩阵**永远不重算** —— 表现不是崩溃也不是报错，而是
    // "改了 transform，画面不动"：父子链上动父节点，子节点原地不动。
    // 06_scene 正好就是这个情况（提交发生在 ecs_world_update 之外，
    // 而示例只在 LOGIC 阶段注册了 Spin），所以这条缺陷藏了整整一个阶段。
    //
    // 世界矩阵求值是**阶段本身**的语义，不是某个系统的副作用：几乎所有提交/剔除
    // 都依赖它，让它可被"忘记注册"抹掉只会制造一个静默的空白行为。
    u32 index = 0u;
    for (u32 st = 0u; st < (u32)ECS_STAGE_COUNT; ++st) {
        // 阶段边界：先把上一阶段积压的结构变更落地，
        // 这样新阶段看到的组件状态是完整的。空队列时这个调用是免费的。
        (void)ecs_flush_commands(world);

        if (st == (u32)ECS_STAGE_TRANSFORM) {
            ecs_update_world_transforms(world);
        }

        // 系统数组按 (stage, priority, name) 全序排好，所以这里是一次线性推进
        while (index < world->system_count) {
            ecs_system_t *s = &world->systems[index];
            if ((u32)s->desc.stage != st) {
                break; // 排序保证：不是这一阶段的都在后面
            }
            index++;
            if (!s->registered || !s->enabled) {
                continue;
            }
            s->desc.update(world, s->desc.user, delta_seconds);
            s->run_count++;
            ran++;
        }
    }

    (void)ecs_flush_commands(world);

    // CLEANUP 阶段之后又出现新的脏标记时补一次（提交发生在 world_update 之外的
    // 调用方要看到最新矩阵）。没有脏标记时这只是一次布尔判断。
    if (world->hierarchy_dirty) {
        ecs_update_world_transforms(world);
    }

    world->systems_run_last_update = ran;
    return ran;
}
