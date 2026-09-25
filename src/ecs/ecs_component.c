// =============================================================================
//  src/ecs/ecs_component.c —— 组件类型注册、字段描述、增删查
// =============================================================================
#include "engine/ecs/component.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "ecs_internal.h"

// ---------------------------------------------------------------- 字段种类

usize ecs_field_kind_size(ecs_field_kind_t kind)
{
    switch (kind) {
    case ECS_FIELD_F32: return sizeof(f32);
    case ECS_FIELD_F64: return sizeof(f64);
    case ECS_FIELD_U32: return sizeof(u32);
    case ECS_FIELD_I32: return sizeof(i32);
    case ECS_FIELD_U8: return sizeof(u8);
    case ECS_FIELD_BOOL: return sizeof(bool);
    case ECS_FIELD_VEC2: return sizeof(vec2_t);
    case ECS_FIELD_VEC3: return sizeof(vec3_t);
    case ECS_FIELD_VEC4: return sizeof(vec4_t);
    case ECS_FIELD_QUAT: return sizeof(quat_t);
    case ECS_FIELD_ENTITY: return sizeof(entity_t);
    case ECS_FIELD_DENSE_ID: return sizeof(dense_id_t);
    case ECS_FIELD_STRING: return 1u; // count/容量 以字节计
    case ECS_FIELD_KIND_COUNT: break;
    }
    return 0u;
}

const char *ecs_field_kind_name(ecs_field_kind_t kind)
{
    switch (kind) {
    case ECS_FIELD_F32: return "f32";
    case ECS_FIELD_F64: return "f64";
    case ECS_FIELD_U32: return "u32";
    case ECS_FIELD_I32: return "i32";
    case ECS_FIELD_U8: return "u8";
    case ECS_FIELD_BOOL: return "bool";
    case ECS_FIELD_VEC2: return "vec2";
    case ECS_FIELD_VEC3: return "vec3";
    case ECS_FIELD_VEC4: return "vec4";
    case ECS_FIELD_QUAT: return "quat";
    case ECS_FIELD_ENTITY: return "entity";
    case ECS_FIELD_DENSE_ID: return "dense_id";
    case ECS_FIELD_STRING: return "string";
    case ECS_FIELD_KIND_COUNT: break;
    }
    return "?";
}

u32 ecs_component_schema_hash(const ecs_component_desc_t *desc)
{
    if (desc == NULL) {
        return 0u;
    }
    // FNV-1a。这里不要求密码学强度，只要"结构变了哈希就变"。
    u32 hash = 2166136261u;
    const u8 *p = (const u8 *)desc->name;
    usize     n = 0u;
    while (p[n] != '\0' && n < ECS_COMPONENT_NAME_MAX) {
        hash = (hash ^ p[n++]) * 16777619u;
    }
    hash = (hash ^ (u8)desc->size) * 16777619u;
    hash = (hash ^ (u8)(desc->size >> 8)) * 16777619u;

    for (u32 i = 0; i < desc->field_count; ++i) {
        const ecs_field_desc_t *f = &desc->fields[i];
        const u8               *fn = (const u8 *)f->name;
        usize                    k = 0u;
        while (fn != NULL && k < 64u && fn[k] != '\0') {
            hash = (hash ^ fn[k++]) * 16777619u;
        }
        hash = (hash ^ (u8)f->offset) * 16777619u;
        hash = (hash ^ (u8)(f->offset >> 8)) * 16777619u;
        hash = (hash ^ (u8)f->kind) * 16777619u;
        hash = (hash ^ (u8)f->count) * 16777619u;
        hash = (hash ^ (u8)(f->count >> 8)) * 16777619u;
    }
    return hash;
}

// ---------------------------------------------------------------- 注册校验

static bool ecs_validate_fields(const ecs_component_desc_t *desc, const char **out_reason)
{
    for (u32 i = 0; i < desc->field_count; ++i) {
        const ecs_field_desc_t *f = &desc->fields[i];
        if (f->name == NULL || f->name[0] == '\0') {
            *out_reason = "字段没有名字";
            return false;
        }
        if (f->kind >= ECS_FIELD_KIND_COUNT) {
            *out_reason = "字段种类未知";
            return false;
        }
        const usize elem = ecs_field_kind_size(f->kind);
        if (elem == 0u) {
            *out_reason = "字段种类尺寸为 0";
            return false;
        }
        const usize bytes = ecs_field_byte_size(f);
        if (bytes == 0u) {
            *out_reason = "字符串字段没有 capacity（STRING 的容量必须显式给出）";
            return false;
        }
        // 字段必须完整落在结构体里。**这是序列化的内存安全边界**：
        // 越界的 offset 会让读写字节跑到相邻组件身上，症状是"另一个实体的
        // 某个值莫名变了"，比崩溃还难查。
        if ((usize)f->offset > desc->size || (usize)f->offset + bytes > desc->size) {
            *out_reason = "字段越出结构体范围";
            return false;
        }
        // 字段必须落在自身类型的对齐上：否则序列化按 offset 拷贝时，
        // 结构体里的 vec3/quat 会处在未对齐地址上。
        if ((usize)f->offset % elem != 0u && f->kind != ECS_FIELD_STRING) {
            *out_reason = "字段偏移与其元素尺寸不对齐";
            return false;
        }
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(desc->fields[j].name, f->name) == 0) {
                *out_reason = "字段名重复";
                return false;
            }
        }
    }
    return true;
}

usize ecs_field_byte_size(const ecs_field_desc_t *field)
{
    if (field == NULL) {
        return 0u;
    }
    const usize elem = ecs_field_kind_size(field->kind);
    if (field->kind == ECS_FIELD_STRING) {
        // 字符串用 capacity 表达总字节数（含结尾 NUL），count 不参与。
        return (usize)field->capacity;
    }
    // count==0 是"1 个元素"的简写，只在读取侧解释。
    const usize count = (field->count > 0u) ? (usize)field->count : 1u;
    return elem * count;
}

ecs_component_id ecs_register_component(ecs_world_t *world, const ecs_component_desc_t *desc)
{
    ASSERT(world != NULL);

    if (!world->ready) {
        LOG_ERROR("世界未初始化，无法注册组件");
        return ECS_COMPONENT_NONE;
    }
    if (desc == NULL || desc->name == NULL || desc->name[0] == '\0') {
        LOG_ERROR("组件描述不完整");
        return ECS_COMPONENT_NONE;
    }
    if (strlen(desc->name) >= ECS_COMPONENT_NAME_MAX) {
        LOG_ERROR("组件名过长（>= %u）: %s", ECS_COMPONENT_NAME_MAX, desc->name);
        return ECS_COMPONENT_NONE;
    }
    if (desc->size == 0u || !ENGINE_IS_POW2(desc->align) || desc->align < 16u) {
        LOG_ERROR("组件 %s 的 size/align 非法（size=%u align=%u；vec3 类成员要求 16 对齐）",
                  desc->name, desc->size, desc->align);
        return ECS_COMPONENT_NONE;
    }

    // 同名重复注册直接返回已有 id：多个模块各自 ensure 同一个内置组件是正常用法。
    // 但**同名不同布局**必须拒绝 —— 那意味着两处代码对同一个类型有两种理解，
    // 存档会彻底错位。
    for (u32 i = 0; i < world->type_count; ++i) {
        const ecs_storage_t *s = &world->storage[i];
        if (!s->registered || s->desc->name == NULL || desc->name == NULL) {
            continue;
        }
        if (strcmp(s->desc->name, desc->name) == 0) {
            if (s->desc->size != desc->size || s->desc->field_count != desc->field_count ||
                ecs_component_schema_hash(s->desc) != ecs_component_schema_hash(desc)) {
                LOG_ERROR("组件 %s 重复注册但布局不同 —— 两处代码对同一类型理解不一致",
                          desc->name);
                return ECS_COMPONENT_NONE;
            }
            return i;
        }
    }

    if (world->type_count >= ECS_MAX_COMPONENTS) {
        LOG_ERROR("组件类型表已满（%u）。提高 ECS_MAX_COMPONENTS。", ECS_MAX_COMPONENTS);
        return ECS_COMPONENT_NONE;
    }

    const char *reason = "?";
    if (!ecs_validate_fields(desc, &reason)) {
        LOG_ERROR("组件 %s 描述非法：%s", desc->name, reason);
        return ECS_COMPONENT_NONE;
    }

    const u32 capacity = (desc->capacity > 0u) ? desc->capacity
                                               : world->config.default_component_capacity;
    const u32 stride = (u32)ENGINE_ALIGN_UP(desc->size, 16u); // 行距至少 16 对齐，
    // 否则结构体里的 vec3 成员会落在非 16 对齐的地址上（未对齐访问在部分平台直接崩）
    const u32 index_space = pool_capacity(&world->entities); // handle_index 的取值空间
    const usize presence_words = ((usize)index_space + 63u) / 64u;

    allocator_t *alloc = world->config.allocator;

    ecs_storage_t *s = &world->storage[world->type_count];
    memset(s, 0, sizeof(*s));

    s->data = alloc->alloc(alloc, (usize)capacity * stride, 16);
    s->owners = (entity_t *)alloc->alloc(alloc, (usize)capacity * sizeof(entity_t), 16);
    s->row_of = (u32 *)alloc->alloc(alloc, (usize)index_space * sizeof(u32), 16);
    s->presence = (u64 *)alloc->alloc(alloc, presence_words * sizeof(u64), 16);
    if (s->data == NULL || s->owners == NULL || s->row_of == NULL || s->presence == NULL) {
        LOG_ERROR("组件 %s 存储分配失败（容量 %u）", desc->name, capacity);
        if (s->data != NULL) {
            alloc->free(alloc, s->data, (usize)capacity * stride);
        }
        if (s->owners != NULL) {
            alloc->free(alloc, s->owners, (usize)capacity * sizeof(entity_t));
        }
        if (s->row_of != NULL) {
            alloc->free(alloc, s->row_of, (usize)index_space * sizeof(u32));
        }
        if (s->presence != NULL) {
            alloc->free(alloc, s->presence, presence_words * sizeof(u64));
        }
        memset(s, 0, sizeof(*s));
        return ECS_COMPONENT_NONE;
    }

    memset(s->owners, 0, (usize)capacity * sizeof(entity_t));
    memset(s->row_of, 0, (usize)index_space * sizeof(u32));
    memset(s->presence, 0, presence_words * sizeof(u64));

    // 存**副本**而不是调用方的指针 —— 见 ecs_storage_t 里记的悬垂事故：
    // 注册内置组件时那个为了覆盖 construct 而造的栈局部一旦返回，
    // 借来的指针就让后续每次 strcmp 读到死栈帧上。
    // 表里 count==0 表示"1 个元素"的简写，统一由 ecs_field_byte_size() 在
    // 读取侧解释 —— 不改写任何一张表（对只读段做写是未定义行为）。
    s->desc_storage = *desc;
    s->desc = &s->desc_storage;

    s->capacity = capacity;
    s->stride = stride;
    s->index_space = index_space;
    s->presence_words = presence_words;
    s->registered = true;
    s->count = 0u;

    world->memory_bytes += (usize)capacity * stride + (usize)capacity * sizeof(entity_t) +
                           (usize)index_space * sizeof(u32) + presence_words * sizeof(u64);

    const ecs_component_id id = world->type_count;
    world->type_count++;
    return id;
}

ecs_component_id ecs_find_component(const ecs_world_t *world, const char *name)
{
    if (world == NULL || name == NULL) {
        return ECS_COMPONENT_NONE;
    }
    for (u32 i = 0; i < world->type_count; ++i) {
        const ecs_storage_t *s = &world->storage[i];
        if (s->registered && s->desc->name != NULL && strcmp(s->desc->name, name) == 0) {
            return i;
        }
    }
    return ECS_COMPONENT_NONE;
}

bool ecs_component_is_registered(const ecs_world_t *world, ecs_component_id id)
{
    return world != NULL && id < world->type_count && world->storage[id].registered;
}

const ecs_component_desc_t *ecs_component_desc(const ecs_world_t *world, ecs_component_id id)
{
    const ecs_storage_t *s = ecs_storage_of(world, id);
    return (s != NULL) ? s->desc : NULL;
}

u32 ecs_component_type_count(const ecs_world_t *world)
{
    return (world != NULL) ? world->type_count : 0u;
}

// ---------------------------------------------------------------- 增删查

bool ecs_add_immediate(ecs_world_t *world, entity_t entity, ecs_component_id id,
                       const void *data)
{
    return ecs_add(world, entity, id, data) != NULL;
}

void *ecs_add(ecs_world_t *world, entity_t entity, ecs_component_id id, const void *data)
{
    ASSERT(world != NULL);

    if (!ecs_entity_is_alive(world, entity)) {
        LOG_ERROR("给一个不存在的实体加组件 %s",
                  (id < world->type_count && world->storage[id].desc != NULL)
                      ? world->storage[id].desc->name
                      : "?");
        return NULL;
    }
    ecs_storage_t *s = ecs_storage_of_mut(world, id);
    if (s == NULL) {
        LOG_ERROR("组件类型 %u 未注册", (unsigned)id);
        return NULL;
    }

    const u32 index = handle_index(entity);

    // 迭代中加组件同样是危险的：count 增加会让别的迭代器多扫到行，
    // 更麻烦的是"换存储"（本实现不会换，但交换式移除会移动）。
    // 加只写尾部不会毁掉别人的行号，但会让"这一帧到底看见几个"变得不确定，
    // 所以和移除一样挡掉，并要求走 ecs_defer_add。
    if (world->active_iterators > 0u) {
        LOG_ERROR("迭代中加组件（%s）。系统里请改用 ecs_defer_add。", s->desc->name);
        return NULL;
    }

    if (ecs_presence_test(s, index)) {
        void *existing = (u8 *)s->data + (usize)s->row_of[index] * (usize)s->stride;
        if (data != NULL) {
            memcpy(existing, data, s->desc->size);
        }
        return existing;
    }

    if (s->count >= s->capacity) {
        s->add_failures++;
        LOG_ERROR("组件 %s 已达容量 %u，加不上。提高 ecs_component_desc_t.capacity。",
                  s->desc->name, s->capacity);
        return NULL;
    }

    const u32 row = s->count;
    u8       *slot = (u8 *)s->data + (usize)row * (usize)s->stride;
    memset(slot, 0, s->stride);

    if (s->desc->construct != NULL) {
        s->desc->construct(slot); // 默认值优先：有的组件必须有非零默认（如 scale=1）
    }
    if (data != NULL) {
        memcpy(slot, data, s->desc->size);
    }

    s->owners[row] = entity;
    s->row_of[index] = row;
    ecs_presence_set(s, index, true);
    s->count++;
    return slot;
}

// 校验 + 定位，返回行号。两个公开入口共用，避免"一个改了另一个忘改"
// 这种成对函数最经典的漂移。
static bool ecs_resolve(const ecs_world_t *world, entity_t entity, ecs_component_id id,
                        const ecs_storage_t **out_storage, u32 *out_row)
{
    const ecs_storage_t *s = ecs_storage_of(world, id);
    if (s == NULL || !entity_is_valid(entity)) {
        return false;
    }
    if (!pool_valid(&world->entities, entity)) {
        return false; // 陈旧句柄：绝不能因为 index 相同就把别人的组件递出去
    }
    const u32 index = handle_index(entity);
    if (!ecs_presence_test(s, index)) {
        return false;
    }
    *out_storage = s;
    *out_row = s->row_of[index];
    return true;
}

const void *ecs_peek(const ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    const ecs_storage_t *s = NULL;
    u32                  row = 0u;
    if (!ecs_resolve(world, entity, id, &s, &row)) {
        return NULL;
    }
    return (const u8 *)s->data + (usize)row * (usize)s->stride;
}

void *ecs_get(ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    const ecs_storage_t *s = NULL;
    u32                  row = 0u;
    if (!ecs_resolve((const ecs_world_t *)world, entity, id, &s, &row)) {
        return NULL;
    }
    // 这里不需要任何强转：C 的 const 是"浅"的，s->data 的成员类型仍是
    // void *（const 只修饰到 storage 结构体本身，不传染到它指向的组件内存）。
    // 所以可写性由**入参 world 是不是 const**决定，正是我们想要的规则。
    return (u8 *)s->data + (usize)row * (usize)s->stride;
}

bool ecs_has(const ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    return ecs_peek(world, entity, id) != NULL;
}

void *ecs_get_or_add(ecs_world_t *world, entity_t entity, ecs_component_id id, const void *data)
{
    void *existing = ecs_get(world, entity, id);
    if (existing != NULL) {
        return existing;
    }
    return ecs_add(world, entity, id, data);
}

void *ecs_edit(ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    void *existing = ecs_get(world, entity, id);
    if (existing == NULL) {
        const ecs_storage_t *s = ecs_storage_of(world, id);
        LOG_ERROR("实体上没有组件 %s，改不了",
                  (s != NULL && s->desc != NULL) ? s->desc->name : "?");
    }
    return existing;
}

// 交换式移除：把最后一行搬到被删的位置。
// 于是**必须**同步更新被搬那一行的主人的 row_of —— 忘了更新的话，
// 那个主人之后 ecs_get 会拿到别人的组件（表现为"改了 A 的结果 B 变了"）。
bool ecs_remove_immediate(ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    ASSERT(world != NULL);

    ecs_storage_t *s = ecs_storage_of_mut(world, id);
    if (s == NULL) {
        return false;
    }
    const u32 index = handle_index(entity);
    if (!ecs_presence_test(s, index)) {
        return false;
    }
    if (world->active_iterators > 0u) {
        LOG_ERROR("迭代中移除组件（%s）。系统里请改用 ecs_defer_remove。", s->desc->name);
        return false;
    }

    const u32 row = s->row_of[index];
    const u32 last = s->count - 1u;

    if (row != last) {
        u8       *dst = (u8 *)s->data + (usize)row * (usize)s->stride;
        const u8 *src = (u8 *)s->data + (usize)last * (usize)s->stride;
        memcpy(dst, src, (usize)s->stride);

        s->owners[row] = s->owners[last];
        s->row_of[handle_index(s->owners[row])] = row;
    }

    s->count = last;
    s->owners[last] = ENTITY_NONE;
    s->row_of[index] = 0u;
    ecs_presence_set(s, index, false);
    return true;
}

bool ecs_remove(ecs_world_t *world, entity_t entity, ecs_component_id id)
{
    return ecs_remove_immediate(world, entity, id);
}

bool ecs_destroy_entity(ecs_world_t *world, entity_t entity)
{
    ASSERT(world != NULL);
    if (world->active_iterators > 0u) {
        return ecs_defer_destroy(world, entity);
    }
    return ecs_destroy_immediate(world, entity);
}

u32 ecs_alive_entity_count(const ecs_world_t *world)
{
    return (world != NULL) ? pool_count(&world->entities) : 0u;
}

u32 ecs_component_count(const ecs_world_t *world, ecs_component_id id)
{
    const ecs_storage_t *s = ecs_storage_of(world, id);
    return (s != NULL) ? s->count : 0u;
}

u32 ecs_component_capacity(const ecs_world_t *world, ecs_component_id id)
{
    const ecs_storage_t *s = ecs_storage_of(world, id);
    return (s != NULL) ? s->capacity : 0u;
}

// ---------------------------------------------------------------- 类型遍历

void ecs_type_iter_begin(const ecs_world_t *world, ecs_component_id id, ecs_type_iter_t *out_it)
{
    ASSERT(out_it != NULL);
    memset(out_it, 0, sizeof(*out_it));

    const ecs_storage_t *s = ecs_storage_of(world, id);
    if (s == NULL) {
        return;
    }
    out_it->world = world;
    out_it->id = id;
    out_it->index = 0u;
    out_it->count = s->count; // 快照：迭代过程中新增的看不到，这是有意的
    out_it->data = s->data;
    out_it->owners = s->owners;
    out_it->stride = s->stride;
}

bool ecs_type_iter_next(ecs_type_iter_t *it, entity_t *out_entity, const void **out_component)
{
    if (it == NULL || it->world == NULL || it->index >= it->count) {
        return false;
    }
    const u32 row = it->index++;
    if (out_entity != NULL) {
        *out_entity = it->owners[row];
    }
    if (out_component != NULL) {
        *out_component = (const u8 *)it->data + (usize)row * (usize)it->stride;
    }
    return true;
}

usize ecs_type_iter_remaining(const ecs_type_iter_t *it)
{
    if (it == NULL || it->world == NULL || it->index >= it->count) {
        return 0u;
    }
    return (usize)(it->count - it->index);
}
