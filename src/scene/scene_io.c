// =============================================================================
//  src/scene/scene_io.c —— 场景存读（二进制）
//
//  设计、布局、"哪些情况必须失败"都在 scene_io.h 的文件头。
//  这个文件里只有一套字段遍历 + 两段实体读写。
// =============================================================================
#include "engine/scene/scene_io.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/byte_io.h"
#include "engine/core/log.h"
#include "engine/ecs/component.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"
#include "engine/render/material.h"
#include "engine/render/mesh.h"
#include "engine/render/texture.h"

// ============================================================ 上下文与尺寸

typedef struct scene_io_ctx_t {
    scene_t         *scene;
    ecs_world_t     *world;
    const entity_t  *dense_map; // 下标 = 文件里的 dense id；写侧为 NULL
    u32              dense_count;
    scene_io_stats_t *stats;

    // 写侧专用：非 NULL 表示这份存档只覆盖这一段实体（预制体），
    // 数组下标就是它们在文件里的 dense id。
    const entity_t *scope;
    u32            scope_count;
} scene_io_ctx_t;

// 一个实体在这份存档里的 dense id。
static u32 scene_io_write_dense(const scene_io_ctx_t *ctx, entity_t e)
{
    if (ctx->scope == NULL) {
        return (u32)ecs_entity_to_dense(ctx->world, e);
    }
    // 预制体必须**重新编号**：原场景里的 dense id 到了目标场景毫无意义，
    // 而且会把"这份存档只属于那个场景"这件事写进格式里。
    // 找不到 = 引用了子树外的实体，一律变 NONE —— 预制体不许偷偷依赖子树外
    // 的东西，否则实例化后那个引用指向的是目标场景里**别人的**实体，
    // 那是最难查的一类错（表现是"这盏灯跟着别的物体走"）。
    for (u32 i = 0u; i < ctx->scope_count; ++i) {
        if (ctx->scope[i] == e) {
            return i;
        }
    }
    return (u32)ECS_DENSE_NONE;
}

// 一种字段在**文件里**占多少字节。
//
// 和内存大小不等的只有 vec3：内存里 16（含对齐用的 pad），文件里 12。
// 这两个数必须分清 —— 组件的"每段长度前缀"是从这个函数算的，
// 要是拿内存大小去算，声明的长度比实际写出的多 4，
// 读侧就会以为下个组件从那里开始，于是全体错位。
static usize scene_field_elem_wire_size(ecs_field_kind_t kind)
{
    return (kind == ECS_FIELD_VEC3) ? 3u * sizeof(f32) : ecs_field_kind_size(kind);
}

static usize scene_field_wire_size(const ecs_field_desc_t *f)
{
    if (f->kind == ECS_FIELD_STRING) {
        return (usize)f->capacity;
    }
    if (f->kind == ECS_FIELD_MESH_REF || f->kind == ECS_FIELD_MATERIAL_REF ||
        f->kind == ECS_FIELD_ATLAS_REF) {
        return (usize)SCENE_RESOURCE_NAME_MAX;
    }
    const usize elem = scene_field_elem_wire_size(f->kind);
    const usize count = (f->count > 0u) ? (usize)f->count : 1u;
    return elem * count;
}

static usize scene_desc_wire_size(const ecs_component_desc_t *desc)
{
    usize total = 0u;
    for (u32 i = 0; i < desc->field_count; ++i) {
        total += scene_field_wire_size(&desc->fields[i]);
    }
    return total;
}

// ============================================================ 名字与引用翻译

static bool scene_write_fixed_name(bio_writer_t *w, const char *name)
{
    char pad[SCENE_RESOURCE_NAME_MAX];
    memset(pad, 0, sizeof(pad));
    if (name != NULL && name[0] != '\0') {
        snprintf(pad, sizeof(pad), "%s", name);
    }
    return bio_write_bytes(w, pad, sizeof(pad));
}

static bool scene_read_fixed_name(bio_reader_t *r, char *out, usize cap)
{
    char pad[SCENE_RESOURCE_NAME_MAX];
    if (!bio_read_bytes(r, pad, sizeof(pad))) {
        return false;
    }
    pad[SCENE_RESOURCE_NAME_MAX - 1u] = '\0'; // 坏文件里可能压根没有 NUL
    snprintf(out, cap, "%s", pad);
    return true;
}

static const char *scene_ref_name(const scene_io_ctx_t *ctx, ecs_field_kind_t kind, u32 index)
{
    switch (kind) {
    case ECS_FIELD_MESH_REF: return scene_mesh_name(ctx->scene, index);
    case ECS_FIELD_MATERIAL_REF: return scene_material_name(ctx->scene, index);
    case ECS_FIELD_ATLAS_REF: return scene_atlas_name(ctx->scene, index);
    default: return NULL;
    }
}

static u32 scene_ref_index(const scene_io_ctx_t *ctx, ecs_field_kind_t kind, const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return SCENE_RESOURCE_NONE;
    }
    u32 id = SCENE_RESOURCE_NONE;
    switch (kind) {
    case ECS_FIELD_MESH_REF: id = scene_find_mesh(ctx->scene, name); break;
    case ECS_FIELD_MATERIAL_REF: id = scene_find_material(ctx->scene, name); break;
    case ECS_FIELD_ATLAS_REF: id = scene_find_atlas(ctx->scene, name); break;
    default: break;
    }
    if (id == SCENE_RESOURCE_NONE && ctx->stats != NULL) {
        ctx->stats->dangling_refs++; // 引用不到照样建实体，只是这个字段留空
    }
    return id;
}

static entity_t scene_ref_entity(const scene_io_ctx_t *ctx, u32 dense)
{
    if (dense == (u32)ECS_DENSE_NONE) {
        return ENTITY_NONE;
    }
    if (ctx->dense_map == NULL || dense >= ctx->dense_count ||
        !entity_is_valid(ctx->dense_map[dense])) {
        if (ctx->stats != NULL) {
            ctx->stats->dangling_refs++;
        }
        return ENTITY_NONE;
    }
    return ctx->dense_map[dense];
}

// 写侧遇到"下标越界到已不在表里"的引用：写空名字并计数，不让整个存档失败。
// 理由是这类坏引用已经存在了，拒绝存档会让人拿不到一份可以排查的现场。
static bool scene_write_ref(bio_writer_t *w, scene_io_ctx_t *ctx, ecs_field_kind_t kind, u32 index)
{
    if (index == SCENE_RESOURCE_NONE) {
        return scene_write_fixed_name(w, "");
    }
    const char *name = scene_ref_name(ctx, kind, index);
    if (name == NULL && ctx->stats != NULL) {
        ctx->stats->dangling_refs++;
    }
    return scene_write_fixed_name(w, name);
}

// ============================================================ 字段遍历

static bool bin_write_field(bio_writer_t *w, ecs_field_kind_t kind, const void *p,
                            scene_io_ctx_t *ctx)
{
    switch (kind) {
    case ECS_FIELD_F32: return bio_write_f32(w, *(const f32 *)p);
    case ECS_FIELD_F64: {
        u64 bits = 0u;
        memcpy(&bits, p, sizeof(bits));
        return bio_write_u64(w, bits);
    }
    case ECS_FIELD_U32:
    case ECS_FIELD_DENSE_ID: return bio_write_u32(w, *(const u32 *)p);
    case ECS_FIELD_I32: return bio_write_i32(w, *(const i32 *)p);
    case ECS_FIELD_U8: return bio_write_u8(w, *(const u8 *)p);
    case ECS_FIELD_BOOL: return bio_write_u8(w, *(const bool *)p ? 1u : 0u);
    case ECS_FIELD_ENTITY:
        return bio_write_u32(w, scene_io_write_dense(ctx, *(const entity_t *)p));
    case ECS_FIELD_VEC2: {
        const vec2_t *v = (const vec2_t *)p;
        return bio_write_f32(w, v->x) && bio_write_f32(w, v->y);
    }
    case ECS_FIELD_VEC3: {
        const vec3_t *v = (const vec3_t *)p;
        // 只写 x/y/z：pad 不是数据（理由见文件头）
        return bio_write_f32(w, v->x) && bio_write_f32(w, v->y) && bio_write_f32(w, v->z);
    }
    case ECS_FIELD_VEC4: {
        const vec4_t *v = (const vec4_t *)p;
        return bio_write_f32(w, v->x) && bio_write_f32(w, v->y) && bio_write_f32(w, v->z) &&
               bio_write_f32(w, v->w);
    }
    case ECS_FIELD_QUAT: {
        const quat_t *q = (const quat_t *)p;
        return bio_write_f32(w, q->x) && bio_write_f32(w, q->y) && bio_write_f32(w, q->z) &&
               bio_write_f32(w, q->w);
    }
    case ECS_FIELD_MESH_REF:
    case ECS_FIELD_MATERIAL_REF:
    case ECS_FIELD_ATLAS_REF: return scene_write_ref(w, ctx, kind, *(const u32 *)p);
    case ECS_FIELD_STRING:
    case ECS_FIELD_KIND_COUNT: break; // 定长字符串由调用处按 capacity 写
    }
    return false;
}

static bool bin_read_field(bio_reader_t *r, ecs_field_kind_t kind, void *p, scene_io_ctx_t *ctx)
{
    switch (kind) {
    case ECS_FIELD_F32: return bio_read_f32(r, (f32 *)p);
    case ECS_FIELD_F64: {
        u64 bits = 0u;
        if (!bio_read_u64(r, &bits)) {
            return false;
        }
        memcpy(p, &bits, sizeof(bits));
        return true;
    }
    case ECS_FIELD_U32:
    case ECS_FIELD_DENSE_ID: return bio_read_u32(r, (u32 *)p);
    case ECS_FIELD_I32: return bio_read_i32(r, (i32 *)p);
    case ECS_FIELD_U8: return bio_read_u8(r, (u8 *)p);
    case ECS_FIELD_BOOL: {
        u8 raw = 0u;
        // 只认 0/1：读到别的值说明已经错位了，放宽就是把格式问题伪装成数据问题
        if (!bio_read_u8(r, &raw) || raw > 1u) {
            return false;
        }
        *(bool *)p = (raw == 1u);
        return true;
    }
    case ECS_FIELD_ENTITY: {
        u32 dense = 0u;
        if (!bio_read_u32(r, &dense)) {
            return false;
        }
        *(entity_t *)p = scene_ref_entity(ctx, dense);
        return true;
    }
    case ECS_FIELD_VEC2: {
        vec2_t *v = (vec2_t *)p;
        return bio_read_f32(r, &v->x) && bio_read_f32(r, &v->y);
    }
    case ECS_FIELD_VEC3: {
        vec3_t *v = (vec3_t *)p;
        const bool ok = bio_read_f32(r, &v->x) && bio_read_f32(r, &v->y) && bio_read_f32(r, &v->z);
        v->pad = 0.0f;
        return ok;
    }
    case ECS_FIELD_VEC4: {
        vec4_t *v = (vec4_t *)p;
        return bio_read_f32(r, &v->x) && bio_read_f32(r, &v->y) && bio_read_f32(r, &v->z) &&
               bio_read_f32(r, &v->w);
    }
    case ECS_FIELD_QUAT: {
        quat_t *q = (quat_t *)p;
        return bio_read_f32(r, &q->x) && bio_read_f32(r, &q->y) && bio_read_f32(r, &q->z) &&
               bio_read_f32(r, &q->w);
    }
    case ECS_FIELD_MESH_REF:
    case ECS_FIELD_MATERIAL_REF:
    case ECS_FIELD_ATLAS_REF: {
        char name[SCENE_RESOURCE_NAME_MAX];
        if (!scene_read_fixed_name(r, name, sizeof(name))) {
            return false;
        }
        *(u32 *)p = scene_ref_index(ctx, kind, name);
        return true;
    }
    case ECS_FIELD_STRING:
    case ECS_FIELD_KIND_COUNT: break;
    }
    return false;
}

static bool bin_write_fields(bio_writer_t *w, const ecs_component_desc_t *desc, const void *comp,
                             scene_io_ctx_t *ctx)
{
    for (u32 i = 0; i < desc->field_count; ++i) {
        const ecs_field_desc_t *f = &desc->fields[i];
        const u8 *base = (const u8 *)comp + f->offset;
        if (f->kind == ECS_FIELD_STRING) {
            if (!bio_write_bytes(w, base, f->capacity)) {
                return false;
            }
            continue;
        }
        const usize count = (f->count > 0u) ? (usize)f->count : 1u;
        const usize stride = ecs_field_kind_size(f->kind);
        for (usize e = 0; e < count; ++e) {
            if (!bin_write_field(w, f->kind, base + e * stride, ctx)) {
                return false;
            }
        }
    }
    return true;
}

static bool bin_read_fields(bio_reader_t *r, const ecs_component_desc_t *desc, void *comp,
                            scene_io_ctx_t *ctx)
{
    for (u32 i = 0; i < desc->field_count; ++i) {
        const ecs_field_desc_t *f = &desc->fields[i];
        u8 *base = (u8 *)comp + f->offset;
        if (f->kind == ECS_FIELD_STRING) {
            if (!bio_read_bytes(r, base, f->capacity)) {
                return false;
            }
            base[f->capacity - 1u] = '\0'; // 定长字段兜一个结尾，坏文件里可能没有
            continue;
        }
        const usize count = (f->count > 0u) ? (usize)f->count : 1u;
        const usize stride = ecs_field_kind_size(f->kind);
        for (usize e = 0; e < count; ++e) {
            if (!bin_read_field(r, f->kind, base + e * stride, ctx)) {
                return false;
            }
        }
    }
    return true;
}

// 写一条定长记录，并当场核对"字段表算出的长度 == 真写出去的字节数"。
//
// 这条不变量是整个格式的地基：读侧按**声明的长度**决定下一条从哪开始，
// 遇到不认识的类型还要靠它整块跳过。表和实际写出的字节一旦分叉，
// 后果不是"这一个组件坏了"而是"后面全体错位且不报错"。
// 所以每个写记录的地方都核对一遍，并在报错信息里点名是哪张表 ——
// 只在函数末尾比对总长的话，能得到的只是"差了几个字节"，差在哪没人知道。
static bool bin_write_record(bio_writer_t *w, const ecs_component_desc_t *desc, const void *data,
                             scene_io_ctx_t *ctx)
{
    const usize before = bio_writer_used(w);
    if (!bin_write_fields(w, desc, data, ctx)) {
        return false;
    }
    const usize emitted = bio_writer_used(w) - before;
    const usize want = scene_desc_wire_size(desc);
    if (emitted != want) {
        LOG_ERROR("%s 的长度不符：字段表算出 %llu 字节，实际写出 %llu 字节", desc->name,
                  (unsigned long long)want, (unsigned long long)emitted);
        return false;
    }
    return true;
}

// ============================================================ 三张描述表
//
// 环境、资源条目、材质都不是组件，但形状一样，所以共用上面那套编解码。

#define IO_FIELD(name_str, type, field, kind_val)                                                  \
    {                                                                                              \
        name_str, (u16)ENGINE_OFFSETOF(type, field), kind_val, 1u, 0u, 0u                          \
    }
#define IO_FIELD_STR(name_str, type, field)                                                        \
    {                                                                                              \
        name_str, (u16)ENGINE_OFFSETOF(type, field), ECS_FIELD_STRING, 0u,                          \
            (u16)sizeof(((type *)0)->field), 0u                                                    \
    }

static const ecs_field_desc_t k_env_fields[] = {
    IO_FIELD("clear_color", scene_env_t, clear_color, ECS_FIELD_VEC4),
    IO_FIELD("clear_color_enabled", scene_env_t, clear_color_enabled, ECS_FIELD_BOOL),
    IO_FIELD("clear_depth_enabled", scene_env_t, clear_depth_enabled, ECS_FIELD_BOOL),
    IO_FIELD("ambient_color", scene_env_t, ambient_color, ECS_FIELD_VEC3),
    IO_FIELD("ambient_intensity", scene_env_t, ambient_intensity, ECS_FIELD_F32),
    IO_FIELD("fog_enabled", scene_env_t, fog_enabled, ECS_FIELD_BOOL),
    IO_FIELD("fog_color", scene_env_t, fog_color, ECS_FIELD_VEC3),
    IO_FIELD("fog_start", scene_env_t, fog_start, ECS_FIELD_F32),
    IO_FIELD("fog_end", scene_env_t, fog_end, ECS_FIELD_F32),
    IO_FIELD("fog_density", scene_env_t, fog_density, ECS_FIELD_F32),
    IO_FIELD("skybox_enabled", scene_env_t, skybox_enabled, ECS_FIELD_BOOL),
    IO_FIELD("sky_horizon", scene_env_t, sky_horizon, ECS_FIELD_VEC3),
    IO_FIELD("sky_zenith", scene_env_t, sky_zenith, ECS_FIELD_VEC3),
    IO_FIELD("sky_ground", scene_env_t, sky_ground, ECS_FIELD_VEC3),
    IO_FIELD("tonemap_mode", scene_env_t, tonemap_mode, ECS_FIELD_U32),
    IO_FIELD("exposure", scene_env_t, exposure, ECS_FIELD_F32),
    IO_FIELD("msaa_samples", scene_env_t, msaa_samples, ECS_FIELD_U32),
    IO_FIELD("shadows_enabled", scene_env_t, shadows_enabled, ECS_FIELD_BOOL),
    IO_FIELD("shadow_strength", scene_env_t, shadow_strength, ECS_FIELD_F32),
    IO_FIELD("shadow_map_size", scene_env_t, shadow_map_size, ECS_FIELD_U32),
    IO_FIELD("culling_enabled", scene_env_t, culling_enabled, ECS_FIELD_BOOL),
    IO_FIELD("use_scene_target", scene_env_t, use_scene_target, ECS_FIELD_BOOL),
};

static const ecs_component_desc_t k_env_desc = {
    .name = "Env",
    .size = sizeof(scene_env_t),
    .align = 16u,
    .fields = k_env_fields,
    .field_count = (u32)(sizeof(k_env_fields) / sizeof(k_env_fields[0])),
};

typedef struct scene_io_resource_t {
    char name[SCENE_RESOURCE_NAME_MAX];
    char path[SCENE_RESOURCE_NAME_MAX * 2u]; // 空 = 由应用提供的程序化几何
} scene_io_resource_t;

static const ecs_field_desc_t k_resource_fields[] = {
    IO_FIELD_STR("name", scene_io_resource_t, name),
    IO_FIELD_STR("path", scene_io_resource_t, path),
};

static const ecs_component_desc_t k_resource_desc = {
    .name = "Resource",
    .size = sizeof(scene_io_resource_t),
    .align = 16u,
    .fields = k_resource_fields,
    .field_count = (u32)(sizeof(k_resource_fields) / sizeof(k_resource_fields[0])),
};

typedef struct scene_io_material_t {
    char name[SCENE_RESOURCE_NAME_MAX];
    char pipeline[SCENE_RESOURCE_NAME_MAX];
    // 每个槽位一个图集名（空串 = 该槽位没挂）。
    //
    // 8 槽 × 64 字节 = 512 字节/材质，材质表上限 64 → 最坏多占 32KB。
    // 这里选"定长、整块读写"而不是"槽数 + 变长"，是为了让这一条记录仍然
    // 能被那**一套**定长遍历处理（变长就要给格式加第二套读法，
    // 而"跳过不认识的组件"依赖的正是尺寸可纯从表算出）。
    // 真要瘦身，等 M11 有实测数据再换，不在这里先设计后需求。
    char atlases[RENDER_MAX_TEXTURE_SLOTS][SCENE_RESOURCE_NAME_MAX];
    vec4_t tint;
    vec4_t pbr;
    vec4_t emissive;
} scene_io_material_t;

static const ecs_field_desc_t k_material_fields[] = {
    IO_FIELD_STR("name", scene_io_material_t, name),
    IO_FIELD_STR("pipeline", scene_io_material_t, pipeline),
    IO_FIELD_STR("atlases", scene_io_material_t, atlases),
    IO_FIELD("tint", scene_io_material_t, tint, ECS_FIELD_VEC4),
    IO_FIELD("pbr", scene_io_material_t, pbr, ECS_FIELD_VEC4),
    IO_FIELD("emissive", scene_io_material_t, emissive, ECS_FIELD_VEC4),
};

static const ecs_component_desc_t k_material_desc = {
    .name = "Material",
    .size = sizeof(scene_io_material_t),
    .align = 16u,
    .fields = k_material_fields,
    .field_count = (u32)(sizeof(k_material_fields) / sizeof(k_material_fields[0])),
};

// ============================================================ 注册表与类型表

u32 scene_registry_hash(scene_t *scene)
{
    ecs_world_t *world = scene_world(scene);
    u32 hash = 2166136261u;
    const u32 types = ecs_component_type_count(world);
    for (u32 id = 0u; id < types; ++id) {
        const ecs_component_desc_t *d = ecs_component_desc(world, id);
        if (d == NULL || d->skip_serialize) {
            continue;
        }
        hash ^= ecs_component_schema_hash(d);
        hash *= 16777619u;
    }
    return hash;
}

static u32 scene_serializable_types(const scene_io_ctx_t *ctx, ecs_component_id *out, u32 max)
{
    const u32 types = ecs_component_type_count(ctx->world);
    u32 n = 0u;
    for (u32 id = 0u; id < types; ++id) {
        const ecs_component_desc_t *d = ecs_component_desc(ctx->world, id);
        if (d == NULL || d->skip_serialize) {
            continue;
        }
        if (n < max) {
            out[n++] = (ecs_component_id)id;
        }
    }
    return n;
}

// ============================================================ 记录构造

static scene_io_resource_t scene_io_resource_record(const char *name, const char *path)
{
    scene_io_resource_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "%s", (name != NULL) ? name : "");
    snprintf(r.path, sizeof(r.path), "%s", (path != NULL) ? path : "");
    return r;
}

static scene_io_material_t scene_io_material_record(scene_t *scene, u32 id)
{
    scene_io_material_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.name, sizeof(m.name), "%s", scene_material_name(scene, id));
    snprintf(m.pipeline, sizeof(m.pipeline), "%s",
             (scene_material_pipeline(scene, id) != NULL)
                 ? scene_material_pipeline(scene, id)
                 : "");
    for (u32 slot = 0u; slot < RENDER_MAX_TEXTURE_SLOTS; ++slot) {
        const u32 atlas = scene_material_atlas(scene, id, slot);
        const char *atlas_name = (atlas != (u32)SCENE_RESOURCE_NONE)
                                     ? scene_atlas_name(scene, atlas)
                                     : NULL;
        snprintf(m.atlases[slot], SCENE_RESOURCE_NAME_MAX, "%s", (atlas_name != NULL) ? atlas_name : "");
    }
    const material_t *mat = scene_material(scene, id);
    if (mat != NULL) {
        m.tint = mat->tint;
        m.pbr = mat->pbr_params;
        m.emissive = mat->emissive;
    }
    return m;
}

// 一次写出的实体上限。超过就是要换分块存档 —— 报错而不是静默截断。
#define SCENE_IO_MAX_ENTITIES_PER_FILE 65536u

bool scene_archive_peek(const void *bytes, usize size, u32 *out_entities, u32 *out_kind)
{
    if (out_entities != NULL) {
        *out_entities = 0u;
    }
    if (out_kind != NULL) {
        *out_kind = 0u;
    }
    if (bytes == NULL || size < SCENE_FILE_HEADER_SIZE) {
        return false;
    }

    // 按 [下标] 走，不写偏移常量：偏移只在这一个文件里有意义（见头文件里的布局表）。
    bio_reader_t r;
    bio_reader_init(&r, bytes, size);
    u32 word = 0u;
    if (!bio_read_u32(&r, &word) || word != SCENE_FILE_MAGIC) {
        return false;
    }
    if (!bio_read_u32(&r, &word) || word != SCENE_FILE_VERSION) {
        return false;
    }
    if (!bio_skip(&r, 4u)) { // [2] 注册表哈希
        return false;
    }
    u32 entities = 0u;
    if (!bio_read_u32(&r, &entities)) { // [3] 实体数
        return false;
    }
    if (!bio_skip(&r, 16u)) { // [4]..[7] 四个计数
        return false;
    }
    u32 kind = 0u;
    if (!bio_read_u32(&r, &kind)) { // [8] 存档种类
        return false;
    }
    if (out_entities != NULL) {
        *out_entities = entities;
    }
    if (out_kind != NULL) {
        *out_kind = kind;
    }
    return true;
}

// ============================================================ 存档范围（预制体）
// 一份预制体只需要"它引用到的那些资源"，不是源场景的整张资源表。
// 三张位图按资源类型分开存（256 字节，栈上放得下，也不引入全局状态）。
typedef struct scene_io_ref_mark_t {
    bool mesh[SCENE_MAX_MESHES];
    bool atlas[SCENE_MAX_ATLASES];
    bool material[SCENE_MAX_MATERIALS];
} scene_io_ref_mark_t;

static void scene_io_mark_component(const ecs_component_desc_t *d, const void *comp,
                                    scene_io_ref_mark_t *mark)
{
    for (u32 i = 0; i < d->field_count; ++i) {
        const ecs_field_desc_t *f = &d->fields[i];
        if (f->kind != ECS_FIELD_MESH_REF && f->kind != ECS_FIELD_ATLAS_REF &&
            f->kind != ECS_FIELD_MATERIAL_REF) {
            continue;
        }
        const u32 index = *(const u32 *)((const u8 *)comp + f->offset);
        if (index == (u32)SCENE_RESOURCE_NONE) {
            continue;
        }
        // 越界的下标不标记也不报错：写出的名字会是空串，
        // 那条路径上 dangling_refs 已经计过一次了，这里再吵就是重复。
        switch (f->kind) {
        case ECS_FIELD_MESH_REF:
            if (index < SCENE_MAX_MESHES) {
                mark->mesh[index] = true;
            }
            break;
        case ECS_FIELD_ATLAS_REF:
            if (index < SCENE_MAX_ATLASES) {
                mark->atlas[index] = true;
            }
            break;
        default:
            if (index < SCENE_MAX_MATERIALS) {
                mark->material[index] = true;
            }
            break;
        }
    }
}

// 材质 → 图集是第二层依赖，必须跟着标：漏了它的话，
// "实例化一个带贴图的炮塔"会写出一个引用了不存在图集的存档。
static void scene_io_mark_material_deps(const scene_t *scene, scene_io_ref_mark_t *mark)
{
    for (u32 m = 0u; m < SCENE_MAX_MATERIALS; ++m) {
        if (!mark->material[m]) {
            continue;
        }
        for (u32 slot = 0u; slot < RENDER_MAX_TEXTURE_SLOTS; ++slot) {
            const u32 atlas = scene_material_atlas(scene, m, slot);
            if (atlas != (u32)SCENE_RESOURCE_NONE && atlas < SCENE_MAX_ATLASES) {
                mark->atlas[atlas] = true;
            }
        }
    }
}

// root 及其所有后代，按"父在子前、兄弟按 dense 升序"输出（out[0] 一定是 root）。
//
// 不用递归：递归深度由数据决定，编辑器里手滑拖出一条十万层的链就是把栈打穿 ——
// 和 ecs_rebuild_hierarchy 同一个理由，这里同样改成显式栈。
// 也不用"每层扫一遍全部实体"：那是 O(实体数 × 深度)，一条长链就能让存盘变成分钟级。
// 这里先做一次计数排序建立 children 索引（3 趟 O(n)），再走一遍子树。
static u32 collect_subtree(ecs_world_t *world, const entity_t *all, u32 all_count, u32 capacity,
                           entity_t root, entity_t *out, u32 out_max, arena_t *arena)
{
    if (!entity_is_valid(root) || out == NULL || out_max == 0u) {
        return 0u;
    }

    u32 *child_start = ARENA_NEW_ARRAY(arena, u32, capacity + 2u);
    u32 *child_cursor = ARENA_NEW_ARRAY(arena, u32, capacity + 1u);
    u32 *child_list = ARENA_NEW_ARRAY(arena, u32, all_count + 1u);
    u32 *stack = ARENA_NEW_ARRAY(arena, u32, all_count + 1u);
    if (child_start == NULL || child_cursor == NULL || child_list == NULL || stack == NULL) {
        LOG_ERROR("子树遍历的临时数组分配失败（%u 个实体）", (unsigned)all_count);
        return 0u;
    }

    // dense id -> 在 all[] 里的下标（+1，0 表示"这个 dense 没有实体"）
    u32 *pos_of_dense = ARENA_NEW_ARRAY(arena, u32, capacity + 1u);
    if (pos_of_dense == NULL) {
        LOG_ERROR("子树遍历的 dense 索引分配失败");
        return 0u;
    }
    for (u32 i = 0u; i < all_count; ++i) {
        const dense_id_t d = ecs_entity_to_dense(world, all[i]);
        if (d < capacity) {
            pos_of_dense[d] = i + 1u;
        }
    }

    // 1) 数每个父有几个孩子
    for (u32 i = 0u; i < all_count; ++i) {
        const entity_t p = ecs_get_parent(world, all[i]);
        if (!entity_is_valid(p)) {
            continue;
        }
        const dense_id_t pd = ecs_entity_to_dense(world, p);
        if (pd < capacity) {
            child_start[pd + 1u]++;
        }
    }
    // 2) 前缀和 -> 每个父的孩子区间起点
    for (u32 i = 0u; i < capacity; ++i) {
        child_start[i + 1u] += child_start[i];
    }
    // 3) 按 dense 升序投放，于是每个区间内部天然有序（确定性 = 存档可逐字节比对）
    memcpy(child_cursor, child_start, (usize)(capacity + 1u) * sizeof(u32));
    for (u32 i = 0u; i < all_count; ++i) {
        const entity_t p = ecs_get_parent(world, all[i]);
        if (!entity_is_valid(p)) {
            continue;
        }
        const dense_id_t pd = ecs_entity_to_dense(world, p);
        if (pd < capacity) {
            child_list[child_cursor[pd]++] = i;
        }
    }

    const dense_id_t root_dense = ecs_entity_to_dense(world, root);
    if (root_dense >= capacity) {
        LOG_ERROR("预制体根实体不在这个世界里");
        return 0u;
    }

    u32 emitted = 0u;
    u32 top = 0u;
    stack[top++] = root_dense;
    while (top > 0u) {
        const u32 dense = stack[--top];
        const u32 pos = pos_of_dense[dense];
        if (pos == 0u) {
            continue; // 这个 dense 上没有活实体（正常不该发生，发生就是索引错了）
        }
        if (emitted >= out_max) {
            LOG_ERROR("子树超过 %u 个实体，容量不足", (unsigned)out_max);
            return 0u;
        }
        out[emitted++] = all[pos - 1u];

        // 逆序压栈，弹出来就是 dense 升序
        const u32 begin = child_start[dense];
        const u32 end = child_start[dense + 1u];
        for (u32 c = end; c > begin; --c) {
            const u32 child_pos = child_list[c - 1u];
            const dense_id_t cd = ecs_entity_to_dense(world, all[child_pos]);
            if (cd < capacity) {
                stack[top++] = cd;
            }
        }
    }
    return emitted;
}

// ============================================================ 写出

// 一份存档 = 环境块 + 资源表 + 一段实体。
// "一段实体"可以是整个世界（scope == NULL），也可以是场景里的一个子树（预制体）——
// 同一套代码，因为两者需要的翻译只有一处不同：dense id 从哪来。
static bool write_archive(scene_t *scene, const entity_t *scope, u32 scope_count, arena_t *arena,
                          void **out_bytes, usize *out_size, scene_io_stats_t *out_stats)
{
    ASSERT(scene != NULL && arena != NULL);
    if (out_bytes != NULL) {
        *out_bytes = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0u;
    }

    scene_io_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    scene_io_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scene = scene;
    ctx.world = scene_world(scene);
    ctx.stats = &stats;
    ctx.scope = scope;
    ctx.scope_count = scope_count;

    ecs_component_id types[ECS_MAX_COMPONENTS];
    const u32 type_count = scene_serializable_types(&ctx, types, ECS_MAX_COMPONENTS);

    // 整场景写**整张**资源表；预制体只写它引用到的那几条。
    // 少了这个过滤，"实例化一个炮塔"会把源场景的 200 个网格名一并灌进目标场景，
    // 而资源表上限是 128 —— 第三次实例化就直接失败，且失败原因看着像"表满了"。
    const u32 mesh_total = scene_mesh_count(scene);
    const u32 atlas_total = scene_atlas_count(scene);
    const u32 material_total = scene_material_count(scene);
    scene_io_ref_mark_t mark;
    memset(&mark, 0, sizeof(mark));
    u32 mesh_count = mesh_total, atlas_count = atlas_total, material_count = material_total;
    if (scope != NULL) {
        for (u32 i = 0u; i < scope_count; ++i) {
            for (u32 t = 0u; t < type_count; ++t) {
                const void *comp = ecs_peek(ctx.world, scope[i], types[t]);
                if (comp != NULL) {
                    scene_io_mark_component(ecs_component_desc(ctx.world, types[t]), comp, &mark);
                }
            }
        }
        scene_io_mark_material_deps(scene, &mark);
        mesh_count = 0u;
        atlas_count = 0u;
        material_count = 0u;
        for (u32 i = 0u; i < mesh_total; ++i) {
            mesh_count += mark.mesh[i] ? 1u : 0u;
        }
        for (u32 i = 0u; i < atlas_total; ++i) {
            atlas_count += mark.atlas[i] ? 1u : 0u;
        }
        for (u32 i = 0u; i < material_total; ++i) {
            material_count += mark.material[i] ? 1u : 0u;
        }
    }

    // 实体列表从 arena 拿，不用 static 数组：
    // static 就是可变全局状态（违反"引擎可多实例、无全局状态"），
    // 而且上限 65536 时那是白给的 256KB 常驻内存。
    const entity_t *ents = scope;
    u32 ent_count = scope_count;
    if (scope == NULL) {
        const u32 want = ecs_collect_entities(ctx.world, NULL, 0u);
        if (want > SCENE_IO_MAX_ENTITIES_PER_FILE) {
            LOG_ERROR("实体数 %u 超过单文件上限 %u", (unsigned)want,
                      (unsigned)SCENE_IO_MAX_ENTITIES_PER_FILE);
            return false;
        }
        entity_t *collected = ARENA_NEW_ARRAY(arena, entity_t, want + 1u);
        if (collected == NULL) {
            LOG_ERROR("实体列表分配失败（%u 个）", (unsigned)want);
            return false;
        }
        ent_count = ecs_collect_entities(ctx.world, collected, want);
        ents = collected;
    } else if (ent_count > SCENE_IO_MAX_ENTITIES_PER_FILE) {
        LOG_ERROR("子树 %u 个实体超过单文件上限 %u", (unsigned)ent_count,
                  (unsigned)SCENE_IO_MAX_ENTITIES_PER_FILE);
        return false;
    }

    // 先量一遍总长度：一次分配、一次写入，不给"边写边扩容"留机会，
    // 也让末尾那句"长度是否吻合"成为一次真正的自检。
    usize each_comp = 8u; // type_index + payload_size
    usize entity_bytes = 0u;
    u32 comps_counted = 0u; // 与下面 stats.components_written 对账用

    for (u32 i = 0; i < ent_count; ++i) {
        // 实体数据段里**每条实体没有自己的头**：它的组件数在索引段里已经写过了。
        // 所以这里只有"每个组件 8 字节前缀 + 负载"。
        // 这个 0 看着是废话，但它是这条格式里最容易写错的一处 ——
        // 上一版这里是 12（照抄了索引段的记录大小），于是长度自检差了 48 字节。
        usize record = 0u;
        for (u32 t = 0; t < type_count; ++t) {
            const void *comp = ecs_peek(ctx.world, ents[i], types[t]);
            if (comp == NULL) {
                continue;
            }
            const usize payload = scene_desc_wire_size(ecs_component_desc(ctx.world, types[t]));
            record += each_comp + payload;
            comps_counted++;
        }
        entity_bytes += record;
    }

    usize total = SCENE_FILE_HEADER_SIZE;
    total += scene_desc_wire_size(&k_env_desc);
    total += 4u + (usize)type_count * (SCENE_RESOURCE_NAME_MAX + 4u);
    // 资源三段**没有**自己的计数前缀（长度就是头部的三个计数），所以这里不能
    // 顺手各加 4：加了就是"算出来的总长比写出去的多"，而这类自我不一致
    // 只有末尾那条长度自检会抓到 —— 它抓到了，这就是它存在的理由。
    total += (usize)mesh_count * scene_desc_wire_size(&k_resource_desc);
    total += (usize)atlas_count * scene_desc_wire_size(&k_resource_desc);
    total += (usize)material_count * scene_desc_wire_size(&k_material_desc);
    total += 4u + (usize)ent_count * 12u; // 实体索引段
    total += 4u + entity_bytes;           // 实体数据段

    u8 *buffer = (u8 *)arena_alloc(arena, total);
    if (buffer == NULL) {
        LOG_ERROR("存档缓冲区分配失败（%llu 字节）", (unsigned long long)total);
        return false;
    }

    // 头部的 kind/root_dense：预制体存档写了子树，[0] 就是它的根，读侧要能把这个
    // 实体交回给调用方（否则实例化完不知道该把这一坨挂到哪个节点下）。
    // 整场景没有"根"这个概念，写 NONE。
    const bool is_prefab = (scope != NULL);
    const u32 root_dense = is_prefab ? 0u : (u32)ECS_DENSE_NONE;
    bio_writer_t w;
    bio_writer_init(&w, buffer, total);
    bool ok = bio_write_u32(&w, SCENE_FILE_MAGIC) && bio_write_u32(&w, SCENE_FILE_VERSION) &&
              bio_write_u32(&w, scene_registry_hash(scene)) && bio_write_u32(&w, ent_count) &&
              bio_write_u32(&w, mesh_count) && bio_write_u32(&w, atlas_count) &&
              bio_write_u32(&w, material_count) && bio_write_u32(&w, type_count) &&
              bio_write_u32(&w, is_prefab ? (u32)SCENE_ARCHIVE_PREFAB : (u32)SCENE_ARCHIVE_SCENE) &&
              bio_write_u32(&w, root_dense) &&
              bio_write_zeros(&w, SCENE_FILE_HEADER_SIZE - bio_writer_used(&w));
    if (!ok) {
        return false;
    }

    // 环境块：预制体没有"环境"这个概念，但这一段字节必须还在（段偏移只有一套）。
    // 写默认值而不是源场景的实际环境 —— 读侧本来就不应用它，
    // 万一哪天有人手工拼这份存档，看到默认值也不会以为"这个炮塔自带一套雾效"。
    const scene_env_t prefab_env = scene_env_default();
    ok = bin_write_record(&w, &k_env_desc, is_prefab ? &prefab_env : scene_environment(scene),
                          &ctx);
    if (!ok) {
        return false;
    }

    if (!bio_write_u32(&w, type_count)) {
        return false;
    }
    for (u32 t = 0; t < type_count; ++t) {
        const ecs_component_desc_t *d = ecs_component_desc(ctx.world, types[t]);
        if (!scene_write_fixed_name(&w, d->name) ||
            !bio_write_u32(&w, ecs_component_schema_hash(d))) {
            return false;
        }
    }

    // 三段资源：整场景按表顺序全写；预制体只写被标到的那几条（顺序仍然是表顺序，
    // 于是同一份场景导出两次必然得到相同字节 —— 逐字节比对的测试靠这一点）。
    for (u32 i = 0u; i < mesh_total; ++i) {
        if (is_prefab && !mark.mesh[i]) {
            continue;
        }
        scene_io_resource_t r = scene_io_resource_record(scene_mesh_name(scene, i),
                                                        scene_mesh_path(scene, i));
        if (!bin_write_record(&w, &k_resource_desc, &r, &ctx)) {
            return false;
        }
    }
    for (u32 i = 0u; i < atlas_total; ++i) {
        if (is_prefab && !mark.atlas[i]) {
            continue;
        }
        scene_io_resource_t r = scene_io_resource_record(scene_atlas_name(scene, i),
                                                        scene_atlas_path(scene, i));
        if (!bin_write_record(&w, &k_resource_desc, &r, &ctx)) {
            return false;
        }
    }
    for (u32 i = 0u; i < material_total; ++i) {
        if (is_prefab && !mark.material[i]) {
            continue;
        }
        scene_io_material_t m = scene_io_material_record(scene, i);
        if (!bin_write_record(&w, &k_material_desc, &m, &ctx)) {
            return false;
        }
    }

    // 实体索引段：先 dense/parent/组件数，后组件数据。
    // 这个"两段式"就是为了能一遍线性读完成 dense id 重映射（见头文件）。
    if (!bio_write_u32(&w, ent_count)) {
        return false;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        const entity_t e = ents[i];
        const entity_t parent = ecs_get_parent(ctx.world, e);
        u32 present = 0u;
        for (u32 t = 0; t < type_count; ++t) {
            if (ecs_peek(ctx.world, e, types[t]) != NULL) {
                present++;
            }
        }
        // 两处 dense 都要过 scene_io_write_dense：预制体模式下它们是"子树内重新编号"，
        // 直接拿世界的 dense 就把"这份存档属于那个场景"写进格式里了。
        const u32 self_dense = scene_io_write_dense(&ctx, e);
        const u32 pd = scene_io_write_dense(&ctx, parent);
        if (!bio_write_u32(&w, self_dense) || !bio_write_u32(&w, pd) ||
            !bio_write_u32(&w, present)) {
            return false;
        }
    }

    if (!bio_write_u32(&w, ent_count)) {
        return false;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        const entity_t e = ents[i];
        for (u32 t = 0; t < type_count; ++t) {
            const void *comp = ecs_peek(ctx.world, e, types[t]);
            if (comp == NULL) {
                continue;
            }
            const ecs_component_desc_t *d = ecs_component_desc(ctx.world, types[t]);
            const usize payload = scene_desc_wire_size(d);
            if (!bio_write_u32(&w, t) || !bio_write_u32(&w, (u32)payload)) {
                return false;
            }
            // 声明的长度必须等于真写出去的字节数：读侧是照着声明长度决定下一个组件
            // 从哪开始的（不认识的类型还要靠它整块跳过）。两者不等就是字段表在
            // **这个组件上**算错了，bin_write_record 会点名是哪张表 ——
            // 等到末尾的总长检查再发现，得到的只是"差了几个字节"，差在谁身上没人知道。
            if (!bin_write_record(&w, d, comp, &ctx)) {
                return false;
            }
            stats.components_written++;
        }
        stats.entities_written++;
    }

    // 写进去的长度必须正好等于算出来的：不等说明上面两处长度计算有一处错了。
    // 这条断言把"格式实现自我不一致"变成一次明确的失败，而不是一个坏文件。
    if (!bio_writer_ok(&w) || bio_writer_used(&w) != total) {
        // 只在失败路径上打各段数字：这两条不一致时，"期望/实际"两个总数
        // 不够用 —— 得知道是哪一段的两次算法分歧，否则只能靠猜。
        LOG_ERROR("存档长度不符：期望 %llu，实际 %llu", (unsigned long long)total,
                  (unsigned long long)bio_writer_used(&w));
        LOG_ERROR("  分段：env %llu，类型表 %u 条，网格 %u，图集 %u，材质 %u，实体 %u 个 / "
                  "数据 %llu 字节（量出 %u 个组件，写出 %u 个）",
                  (unsigned long long)scene_desc_wire_size(&k_env_desc), (unsigned)type_count,
                  (unsigned)mesh_count, (unsigned)atlas_count, (unsigned)material_count,
                  (unsigned)ent_count, (unsigned long long)entity_bytes,
                  (unsigned)comps_counted, (unsigned)stats.components_written);
        return false;
    }
    if (out_bytes != NULL) {
        *out_bytes = buffer;
    }
    if (out_size != NULL) {
        *out_size = total;
    }
    if (out_stats != NULL) {
        *out_stats = stats; // 坏引用在**写出侧**就已经发生了，必须当场看得见
    }
    return true;
}

bool scene_write_binary(scene_t *scene, arena_t *arena, void **out_bytes, usize *out_size,
                        scene_io_stats_t *out_stats)
{
    return write_archive(scene, NULL, 0u, arena, out_bytes, out_size, out_stats);
}

bool scene_write_prefab(scene_t *scene, entity_t root, arena_t *arena, void **out_bytes,
                        usize *out_size, scene_io_stats_t *out_stats)
{
    ASSERT(scene != NULL && arena != NULL);
    if (out_bytes != NULL) {
        *out_bytes = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0u;
    }
    if (!entity_is_valid(root)) {
        LOG_ERROR("预制体的根实体无效");
        return false;
    }

    // 临时内存全走调用方给的 arena，用完由调用方整体丢弃。
    // 量级要说清楚：建 children 索引要 5 个按 dense 编号的 u32 数组，
    // 于是 65536 实体的场景导出一次约需 1.3MB。这是**加载期**开销，
    // 不进帧内 —— 但如果哪天要在帧内导出，就得换成按 dense_count 收紧的索引。
    ecs_world_t *world = scene_world(scene);
    ecs_world_stats_t wstats;
    ecs_world_get_stats(world, &wstats);

    const u32 want = ecs_collect_entities(world, NULL, 0u);
    if (want > SCENE_IO_MAX_ENTITIES_PER_FILE) {
        LOG_ERROR("实体数 %u 超过单文件上限 %u", (unsigned)want,
                  (unsigned)SCENE_IO_MAX_ENTITIES_PER_FILE);
        return false;
    }
    entity_t *all = ARENA_NEW_ARRAY(arena, entity_t, want + 1u);
    entity_t *sub = ARENA_NEW_ARRAY(arena, entity_t, want + 1u);
    if (all == NULL || sub == NULL) {
        LOG_ERROR("预制体的实体列表分配失败（%u 个）", (unsigned)want);
        return false;
    }
    const u32 all_count = ecs_collect_entities(world, all, want);
    const u32 sub_count = collect_subtree(world, all, all_count, wstats.entity_capacity, root, sub,
                                          want, arena);
    if (sub_count == 0u) {
        return false; // collect_subtree 已经说明为什么是 0 了
    }
    return write_archive(scene, sub, sub_count, arena, out_bytes, out_size, out_stats);
}

// ============================================================ 读入

// 读一段"结构数组"：每条都是一张字段表描述的定长记录。
static bool bin_read_resource_section(bio_reader_t *r, scene_t *scene, u32 count, bool is_mesh,
                                      scene_io_ctx_t *ctx)
{
    for (u32 i = 0; i < count; ++i) {
        scene_io_resource_t rec;
        memset(&rec, 0, sizeof(rec));
        if (!bin_read_fields(r, &k_resource_desc, &rec, ctx)) {
            return false;
        }
        u32 id = SCENE_RESOURCE_NONE;
        if (rec.path[0] != '\0') {
            const bool ok = is_mesh ? scene_add_mesh_from_file(scene, rec.name, rec.path, &id)
                                    : scene_add_atlas_from_file(scene, rec.name, rec.path, &id);
            if (!ok) {
                LOG_ERROR("%s %s 从 %s 加载失败", is_mesh ? "网格" : "图集", rec.name, rec.path);
                return false;
            }
        } else {
            // 没有路径 = 程序化几何：这里先按名字占位，等应用用同名把真实句柄填进来
            // （scene_add_*_handle 同名时覆盖句柄，见 scene.c）。
            const bool ok = is_mesh
                                ? scene_add_mesh_handle(scene, rec.name, MESH_HANDLE_INVALID, &id)
                                : scene_add_atlas_handle(scene, rec.name, TEXTURE_HANDLE_INVALID,
                                                         &id);
            if (!ok) {
                return false;
            }
        }
        ctx->stats->resources_loaded++;
    }
    return true;
}

static bool bin_read_material_section(bio_reader_t *r, scene_t *scene, u32 count,
                                     scene_io_ctx_t *ctx)
{
    for (u32 i = 0; i < count; ++i) {
        scene_io_material_t m;
        memset(&m, 0, sizeof(m));
        if (!bin_read_fields(r, &k_material_desc, &m, ctx)) {
            return false;
        }
        material_t mat = material_default();
        mat.tint = m.tint;
        mat.pbr_params = m.pbr;
        mat.emissive = m.emissive;
        u32 id = SCENE_RESOURCE_NONE;
        if (!scene_add_material(scene, m.name, m.pipeline, &mat, &id)) {
            LOG_ERROR("材质 %s 加载失败（管线 %s 未登记？）", m.name, m.pipeline);
            return false;
        }
        // 贴图槽位要在材质进表之后再挂：挂的是"表下标 → 表下标"的关系。
        // 图集排在材质前面读，所以这里查得到；查不到就是存档里引用了一个
        // 不存在的图集名 —— 计数并跳过这个槽位，不毁掉整个材质。
        for (u32 slot = 0u; slot < RENDER_MAX_TEXTURE_SLOTS; ++slot) {
            if (m.atlases[slot][0] == '\0') {
                continue;
            }
            const u32 atlas = scene_find_atlas(scene, m.atlases[slot]);
            if (atlas == (u32)SCENE_RESOURCE_NONE) {
                ctx->stats->dangling_refs++;
                continue;
            }
            if (!scene_material_set_atlas(scene, id, slot, atlas)) {
                ctx->stats->dangling_refs++;
            }
        }
        ctx->stats->resources_loaded++;
    }
    return true;
}

// 存档的读实现。公开的两个入口只在这上面套了一层 scratch arena 的 mark/rewind。
static bool read_archive(scene_t *scene, const void *bytes, usize size,
                         scene_io_stats_t *out_stats, entity_t *out_root)
{
    ASSERT(scene != NULL);

    scene_io_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (out_root != NULL) {
        *out_root = ENTITY_NONE;
    }

    if (bytes == NULL || size < SCENE_FILE_HEADER_SIZE) {
        LOG_ERROR("场景存档太小：%llu 字节", (unsigned long long)size);
        return false;
    }

    bio_reader_t r;
    bio_reader_init(&r, bytes, size);

    u32 magic = 0u, version = 0u, hash = 0u;
    u32 ent_count = 0u, mesh_count = 0u, atlas_count = 0u, material_count = 0u, type_count = 0u;
    u32 kind = 0u, root_dense = 0u;
    if (!bio_read_u32(&r, &magic) || !bio_read_u32(&r, &version) || !bio_read_u32(&r, &hash) ||
        !bio_read_u32(&r, &ent_count) || !bio_read_u32(&r, &mesh_count) ||
        !bio_read_u32(&r, &atlas_count) || !bio_read_u32(&r, &material_count) ||
        !bio_read_u32(&r, &type_count) || !bio_read_u32(&r, &kind) ||
        !bio_read_u32(&r, &root_dense)) {
        LOG_ERROR("场景存档头部不完整");
        return false;
    }
    if (magic != SCENE_FILE_MAGIC) {
        LOG_ERROR("不是场景存档：魔数 0x%08X", (unsigned)magic);
        return false;
    }
    if (version != SCENE_FILE_VERSION) {
        LOG_ERROR("场景存档版本不支持：文件 %u，引擎 %u", (unsigned)version,
                  (unsigned)SCENE_FILE_VERSION);
        return false;
    }
    if (kind >= (u32)SCENE_ARCHIVE_KIND_COUNT) {
        LOG_ERROR("存档种类未知：%u（这个构建最多认到 %u）", (unsigned)kind,
                  (unsigned)(SCENE_ARCHIVE_KIND_COUNT - 1u));
        return false;
    }
    const bool is_prefab = (kind == (u32)SCENE_ARCHIVE_PREFAB);
    // kind 和 root_dense 必须自洽：说自己是预制体却没有有效的根，或者
    // 说自己是整场景却指向一个根，都是"这份文件被改坏了一半"。
    // 这时后面每个决定（要不要应用环境、要不要重映射 dense）都建立在
    // 一个自相矛盾的输入上，所以在这里就拒掉。
    if (is_prefab ? (root_dense >= ent_count) : (root_dense != (u32)ECS_DENSE_NONE)) {
        LOG_ERROR("存档的 kind=%u 与根实体 dense %u 不自洽（实体数 %u）", (unsigned)kind,
                  (unsigned)root_dense, (unsigned)ent_count);
        return false;
    }
    if (!bio_skip(&r, SCENE_FILE_HEADER_SIZE - bio_reader_pos(&r))) {
        LOG_ERROR("场景存档头部不完整（保留段读不到末尾）");
        return false;
    }

    ecs_world_t *world = scene_world(scene);
    if (hash != scene_registry_hash(scene)) {
        LOG_ERROR("组件注册表与这份存档不一致（存档 0x%08X，当前 0x%08X）—— 拒绝加载",
                  (unsigned)hash, (unsigned)scene_registry_hash(scene));
        return false;
    }
    if (ent_count > SCENE_IO_MAX_ENTITIES_PER_FILE) {
        LOG_ERROR("存档声明 %u 个实体，超过单文件上限 %u", (unsigned)ent_count,
                  (unsigned)SCENE_IO_MAX_ENTITIES_PER_FILE);
        return false;
    }
    // 资源表上限也要在读任何记录之前拒掉。
    // 不拒的话症状是"第 129 个网格 add 失败"，日志看着像表满了的偶发问题；
    // 而真实情况是这份存档根本不该被接受 —— 提前一步，报错才说得上话。
    if (mesh_count > SCENE_MAX_MESHES || atlas_count > SCENE_MAX_ATLASES ||
        material_count > SCENE_MAX_MATERIALS) {
        LOG_ERROR("存档声明的资源数超过表上限：网格 %u/%u，图集 %u/%u，材质 %u/%u",
                  (unsigned)mesh_count, (unsigned)SCENE_MAX_MESHES, (unsigned)atlas_count,
                  (unsigned)SCENE_MAX_ATLASES, (unsigned)material_count,
                  (unsigned)SCENE_MAX_MATERIALS);
        return false;
    }

    // 环境块**总是**要读（它在流的中间，跳不过去），但预制体不应用它：
    // 预制体里的环境是导出那一刻源场景的状态，不是这份资产的内容。
    scene_env_t env = *scene_environment(scene);
    scene_io_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scene = scene;
    ctx.world = world;
    ctx.stats = &stats;
    if (!bin_read_fields(&r, &k_env_desc, &env, &ctx)) {
        LOG_ERROR("场景存档的环境块读取失败");
        return false;
    }

    // 类型表：文件下标 -> 运行期 id。
    //
    // 两个计数器的单位是**类型**，不是组件实例。理由：一个类型不认识，
    // 后果是"这类组件的所有实例都没了"，数量随场景变化，报成数字反而误导；
    // 而"存档里声明了 3 个这个构建不认识的类型"是一条能直接行动的信息。
    // 每跳过一次实例都再 ++ 会让同一个原因被计几十次，那是噪声不是观测。
    ecs_component_id type_map[ECS_MAX_COMPONENTS];
    for (u32 i = 0u; i < ECS_MAX_COMPONENTS; ++i) {
        type_map[i] = ECS_COMPONENT_NONE;
    }
    u32 declared = 0u;
    if (!bio_read_u32(&r, &declared) || declared != type_count || declared > ECS_MAX_COMPONENTS) {
        LOG_ERROR("场景存档的类型表长度非法");
        return false;
    }
    for (u32 i = 0u; i < declared; ++i) {
        char name[SCENE_RESOURCE_NAME_MAX];
        u32 schema = 0u;
        if (!scene_read_fixed_name(&r, name, sizeof(name)) || !bio_read_u32(&r, &schema)) {
            return false;
        }
        const ecs_component_id id = ecs_find_component(world, name);
        if (id == ECS_COMPONENT_NONE) {
            stats.unknown_component_types++;
            LOG_WARN("存档里的组件类型 %s 本构建未注册，其实例将被跳过", name);
            continue;
        }
        if (ecs_component_schema_hash(ecs_component_desc(world, id)) != schema) {
            stats.schema_mismatches++;
            LOG_WARN("组件类型 %s 的字段布局与存档不符，其实例将被跳过", name);
            continue;
        }
        type_map[i] = id;
    }

    if (!bin_read_resource_section(&r, scene, mesh_count, true, &ctx) ||
        !bin_read_resource_section(&r, scene, atlas_count, false, &ctx) ||
        !bin_read_material_section(&r, scene, material_count, &ctx)) {
        return false;
    }

    // 实体索引段 → 先把所有实体建出来并填 dense 映射
    u32 indexed = 0u;
    if (!bio_read_u32(&r, &indexed) || indexed != ent_count) {
        LOG_ERROR("实体索引段长度不符");
        return false;
    }
    ecs_world_stats_t wstats;
    ecs_world_get_stats(world, &wstats);
    entity_t *dense_map = (entity_t *)arena_alloc(scene_scratch_arena(scene),
                                                 (usize)wstats.entity_capacity * sizeof(entity_t));
    if (dense_map == NULL) {
        LOG_ERROR("dense 映射表分配失败");
        return false;
    }
    for (u32 i = 0; i < wstats.entity_capacity; ++i) {
        dense_map[i] = ENTITY_NONE;
    }

    u32 *parent_dense = (u32 *)arena_alloc(scene_scratch_arena(scene),
                                           (usize)(ent_count + 1u) * sizeof(u32));
    u32 *comp_counts = (u32 *)arena_alloc(scene_scratch_arena(scene),
                                          (usize)(ent_count + 1u) * sizeof(u32));
    u32 *record_dense = (u32 *)arena_alloc(scene_scratch_arena(scene),
                                           (usize)(ent_count + 1u) * sizeof(u32));
    if (parent_dense == NULL || comp_counts == NULL || record_dense == NULL) {
        return false;
    }

    dense_id_t max_dense = 0u;
    for (u32 i = 0; i < ent_count; ++i) {
        u32 dense = 0u, parent = 0u, comps = 0u;
        if (!bio_read_u32(&r, &dense) || !bio_read_u32(&r, &parent) || !bio_read_u32(&r, &comps)) {
            return false;
        }
        if (dense >= wstats.entity_capacity) {
            LOG_ERROR("存档里的 dense id %u 超出实体容量 %u", (unsigned)dense,
                      (unsigned)wstats.entity_capacity);
            return false;
        }
        if (entity_is_valid(dense_map[dense])) {
            // 两条记录声称自己是同一个 dense id：后面那条会把前面那条的组件
            // 挂到一个说不清的实体上。这种文件不是"旧版本"，是被改坏了。
            LOG_ERROR("存档里 dense id %u 出现了两次 —— 文件被改坏了", (unsigned)dense);
            return false;
        }
        const entity_t e = ecs_create_entity(world);
        if (!entity_is_valid(e)) {
            LOG_ERROR("实体数超过世界容量 %u（存档需要 %u）", (unsigned)wstats.entity_capacity,
                      (unsigned)ent_count);
            return false;
        }
        dense_map[dense] = e;
        parent_dense[i] = parent;
        comp_counts[i] = comps;
        record_dense[i] = dense;
        if (dense + 1u > max_dense) {
            max_dense = (dense_id_t)(dense + 1u);
        }
    }
    ctx.dense_map = dense_map;
    ctx.dense_count = (u32)max_dense;

    // 实体数据段 → 组件直接读进 ecs_add 交回来的那块内存。
    // 先 add 再读有两个实在的好处：construct 先把默认值填好（存档里缺字段
    // 就用默认，这是特性不是将就），以及不需要一个"够大"的临时缓冲区 ——
    // 临时缓冲区就要给上限，超限就得写"组件太大读不了"的分支。
    u32 bodies = 0u;
    if (!bio_read_u32(&r, &bodies) || bodies != ent_count) {
        LOG_ERROR("实体数据段长度不符");
        return false;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        // 注意这里过的是**记录→dense→实体**，不是直接用 i 当 dense：
        // 一个跑过销毁实体再存盘的场景，dense 空间是有洞的（不连续），
        // 拿循环下标去查就会把第 i 条记录挂到错的实体上 —— 表现是
        // "组件串门"，而且不报错。
        const entity_t self = dense_map[record_dense[i]];
        if (!entity_is_valid(self)) {
            LOG_ERROR("第 %u 条实体记录找不到对应的实体（dense %u）", (unsigned)i,
                      (unsigned)record_dense[i]);
            return false;
        }
        for (u32 c = 0; c < comp_counts[i]; ++c) {
            u32 type_index = 0u, payload = 0u;
            if (!bio_read_u32(&r, &type_index) || !bio_read_u32(&r, &payload)) {
                return false;
            }
            const ecs_component_id id =
                (type_index < ECS_MAX_COMPONENTS) ? type_map[type_index] : ECS_COMPONENT_NONE;
            if (id == ECS_COMPONENT_NONE) {
                // 已经在类型表里计过数了，这里只负责整块跳过。
                // 跳过之后后面的实体仍然读得出来 —— 一个坏类型不该毁掉整个场景。
                if (!bio_skip(&r, payload)) {
                    return false;
                }
                continue;
            }
            // payload 与表算出来的尺寸不符 = 这份存档自己前后不一致。
            // 这时"按长度跳过"的算术也不可信，继续读只会把后面的实体全读歪，
            // 所以这里必须整份拒绝，而不是宽容地往下走。
            const ecs_component_desc_t *d = ecs_component_desc(world, id);
            if (payload != (u32)scene_desc_wire_size(d)) {
                LOG_ERROR("组件 %s 的存档长度 %u 与字段表算出的 %llu 不符", d->name,
                          (unsigned)payload, (unsigned long long)scene_desc_wire_size(d));
                return false;
            }
            void *comp = ecs_add(world, self, id, NULL);
            if (comp == NULL) {
                LOG_ERROR("组件 %s 创建失败（容量不足？）", d->name);
                return false;
            }
            if (!bin_read_fields(&r, d, comp, &ctx)) {
                LOG_ERROR("组件 %s 读取失败", d->name);
                return false;
            }
            stats.components_loaded++;
        }

        if (parent_dense[i] != (u32)ECS_DENSE_NONE) {
            const entity_t parent = scene_ref_entity(&ctx, parent_dense[i]);
            if (entity_is_valid(parent) && !ecs_set_parent(world, self, parent)) {
                stats.dangling_refs++; // 成环或超深：不毁掉整次加载
            }
        }
    }

    if (!is_prefab) {
        *scene_environment(scene) = env; // 一次读完整块再覆盖：中途失败不会留下半个场景
    }
    stats.entities_loaded = ent_count;

    // 根实体：预制体实例化后要拿它去挂到目标场景的节点上。
    // 走到这里 dense_map 已经填好，root_dense 也在校验时确认过在范围内。
    if (out_root != NULL) {
        *out_root = is_prefab ? dense_map[root_dense] : ENTITY_NONE;
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    return true;
}

// 读档的两个公开入口负责 scratch arena 的 mark/rewind。
//
// 为什么在**读**这边做而不在写那边：写出的字节要交回给调用方（测试会拿它做
// 逐字节比对），所以那块缓冲区归调用方给的 arena；读档的三张临时表
// （dense 映射、父 dense、组件计数）纯属实现细节，用完就该整体丢掉。
// 少了这一步，"实例化 100 次预制体"就是 100 × 实体容量 × 4 字节的净增长 ——
// 前 99 次都不会失败，所以它只能靠"根本不留"来防。
bool scene_read_binary_root(scene_t *scene, const void *bytes, usize size,
                            scene_io_stats_t *out_stats, entity_t *out_root)
{
    arena_t *scratch = scene_scratch_arena(scene);
    if (scratch == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法读档");
        return false;
    }
    const arena_mark_t mark = arena_mark(scratch);
    const bool ok = read_archive(scene, bytes, size, out_stats, out_root);
    arena_rewind(scratch, mark);
    return ok;
}

bool scene_read_binary(scene_t *scene, const void *bytes, usize size, scene_io_stats_t *out_stats)
{
    return scene_read_binary_root(scene, bytes, size, out_stats, NULL);
}

// ============================================================ 文件读写

// 把一份已经生成好的存档落盘。单独一个函数是因为存场景和存预制体
// 除了"字节从哪来"以外完全一样，而错误处理（短写、fclose 失败）抄两遍
// 迟早会有一遍是错的。
static bool scene_write_file(const void *bytes, usize size, const char *path, const char *what)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        LOG_ERROR("无法写入%s存档: %s", what, path);
        return false;
    }
    const bool wrote = (fwrite(bytes, 1u, size, file) == size);
    const bool closed = (fclose(file) == 0);
    if (!wrote || !closed) {
        LOG_ERROR("写入%s存档失败: %s（%s）", what, path, wrote ? "关闭失败" : "写入不完整");
        return false;
    }
    LOG_INFO("%s已存盘: %s（%llu 字节）", what, path, (unsigned long long)size);
    return true;
}

// 存盘的两个入口负责 scratch arena 的 mark/rewind：那块缓冲区只活着到 fwrite 结束。
// 不丢掉的话，编辑器里每按一次 Ctrl+S 就净增长一份场景大小 ——
// 第一次不会失败，所以这种泄漏只能靠纪律而不是靠运气。
bool scene_save_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats)
{
    if (path == NULL) {
        return false;
    }
    arena_t *scratch = scene_scratch_arena(scene);
    if (scratch == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法存盘");
        return false;
    }
    const arena_mark_t mark = arena_mark(scratch);
    void *bytes = NULL;
    usize size = 0u;
    const bool ok = scene_write_binary(scene, scratch, &bytes, &size, out_stats) &&
                    scene_write_file(bytes, size, path, "场景");
    arena_rewind(scratch, mark);
    return ok;
}

bool scene_save_prefab(scene_t *scene, entity_t root, const char *path, scene_io_stats_t *out_stats)
{
    if (path == NULL) {
        return false;
    }
    arena_t *scratch = scene_scratch_arena(scene);
    if (scratch == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法导出预制体");
        return false;
    }
    const arena_mark_t mark = arena_mark(scratch);
    void *bytes = NULL;
    usize size = 0u;
    const bool ok = scene_write_prefab(scene, root, scratch, &bytes, &size, out_stats) &&
                    scene_write_file(bytes, size, path, "预制体");
    arena_rewind(scratch, mark);
    return ok;
}

bool scene_load_binary(scene_t *scene, const char *path, scene_io_stats_t *out_stats)
{
    if (scene == NULL || path == NULL) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开场景存档: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        LOG_ERROR("场景存档无法定位到末尾: %s", path);
        fclose(file);
        return false;
    }
    const long length = ftell(file);
    if (length <= 0) {
        LOG_ERROR("场景存档为空: %s", path);
        fclose(file);
        return false;
    }
    rewind(file);

    arena_t *arena = scene_scratch_arena(scene);
    if (arena == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法读档");
        fclose(file);
        return false;
    }
    // 临时文件缓冲走 scratch，且由 scene_read_binary_root 自己的 mark 管不到它 ——
    // 所以这里也要 mark/rewind，否则"加载 100 个预制体文件"就是 100 份文件大小。
    const arena_mark_t mark = arena_mark(arena);
    void *buffer = arena_alloc(arena, (usize)length);
    if (buffer == NULL) {
        LOG_ERROR("场景存档超出 scratch arena 容量: %s（%ld 字节）", path, length);
        fclose(file);
        return false;
    }
    if (fread(buffer, 1u, (usize)length, file) != (usize)length) {
        LOG_ERROR("场景存档读取不完整: %s", path);
        fclose(file);
        arena_rewind(arena, mark);
        return false;
    }
    fclose(file);

    const bool ok = scene_read_binary_root(scene, buffer, (usize)length, out_stats, NULL);
    arena_rewind(arena, mark);
    return ok;
}
