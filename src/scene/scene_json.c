// =============================================================================
//  src/scene/scene_json.c —— 场景存档的 JSON 形式
//
//  ---- 为什么这一份是"转码"而不是第二套序列化器 ----
//
//  M5 的验收是"二进制与 JSON 两条路往返一致"、"两者可互相转换"。
//  最直觉的实现是往 scene 上再挂一套遍历（写 JSON、读 JSON）。那意味着
//  下面这些问题各自有**两份**答案：
//    * 哪些实体进存档（整个世界，还是 root 的子树）
//    * 按什么顺序（顺序不定 → "逐字节比对"这条判据直接失效）
//    * 引用子树外的实体怎么算、dense id 从哪重新编号
//    * 预制体该带哪几条资源（漏了过滤就是第三次实例化把资源表撑爆）
//  这两份答案会一直一致到有人改了其中一边为止 —— 而 M5 已经查过三次
//  "同一个长度算两遍、两遍不一样"，形状完全相同，只是这次范围大得多。
//
//  所以这个文件只做一件事：**存档字节 ↔ JSON 文本**。
//    导出：scene_io_write_archive() 出二进制 → 本文件展开成文本
//    导入：本文件把文本编回二进制 → scene_io_read_archive()
//  于是"哪些实体、什么顺序、带哪些资源"全工程只有一处实现，两种编码不可能
//  在它上面分叉；校验也仍然只有 read_archive 一个守门人。
//
//  ---- 为什么转码不需要活的场景状态 ----
//
//  二进制格式自己就是自描述的：资源引用落**名字**、实体引用落 **dense id**、
//  每条记录的长度能从字段表算出来。所以"字节 → 文本"不需要句柄→索引→名字
//  这类必须问 renderer 的翻译，"文本 → 字节"也不需要名字→索引。
//  两边都只需要那张字段表 —— 也就是 scene_io_internal.h 里唯一的一份。
//
//  ---- JSON 的形状 ----
//
//      { "format", "version", "kind", "registry_hash", "root",
//        "environment": { 字段表里的名字: 值 },
//        "meshes": [ {name, path}, ... ],      "atlases": [ ... ],
//        "materials": [ {name, pipeline, atlases: [8 个名字], tint, pbr, emissive} ],
//        "entities": [ { "dense": 0, "parent": null,
//                        "components": [ { "type": "Transform",
//                                          "fields": { "position": [0,0,0], ... } } ] } ] }
//
//  向量选数组而不是对象：`"position": [0,0,0]` 比 `{"x":0,"y":0,"z":0}` 短一半，
//  而 vec3 在场景文件里出现的次数远超其他字段。代价是元素没有名字、读错了
//  没有线索 —— 所以数组长度是**严格**校验的（见下面"两类失败"的第 1 类）。
//
//  实体在 JSON 里合成**一段**：索引/数据两段是二进制为了"一遍线性读完成 dense
//  重映射"才分的（见 scene_io.h 的布局说明），那是读实现的细节，
//  不该让一个人手编辑的文件去承担。dense 与 parent 因此写在每条实体上。
//
//  ---- 两类失败，处理方式不同（这条区分是整个文件的设计核心）----
//
//  1. **文档写坏了** → **整份拒绝**：向量分量不齐全、数组元素类型不对、
//     名字超出容量、顶层冒出未知的键、一个实体里同一组件出现两次。
//     理由和 json.c 一样："能读出一半"是最糟的结果，它会让人以为文件没坏。
//  2. **版本漂移** → 跳过那个类型并计数（unknown_component_types /
//     schema_mismatches），其余照常读出来：这个构建不认识的组件类型名、
//     字段名不在该类型的表里。
//     理由：这种输入不是打字错误，而是"文件比引擎新（或来自别的分支）",
//     二进制形式对它的处理就是整块跳过 —— 两种编码必须给同一个答案。
//
//  为什么第 2 类按**类型**丢而不是按字段忽略：字段表算出的那条字节块里,
//  字段的顺序与偏移就是格式本身。认不出字段名意味着"这份文件的表和这个引擎的
//  不是一套"，剩下的字段该按哪套摆没人知道。二进制的 schema 哈希不符时
//  同样是整个类型作废 —— 同一条判断，只是 JSON 里靠字段名比对来得到它。
//
//  一处**有意**的不对称要写清：二进制遇到 schema 哈希不符会拒绝整个类型，
//  即使文件里少的字段本可以用默认值补；JSON 却允许"只写关心的那几个字段"。
//  因为二进制是引擎写给引擎的（要么完全认识要么作废），
//  而 JSON 是写给人的（"我只改曝光，别的保持原样"必须是合法诉求）。
//
//  ---- registry_hash 在 JSON 里只是信息 ----
//
//  人手编辑的文件不可能算出这个哈希，所以导入时**一律填当前注册表的值**，
//  文档里写了不一致的值只报 WARN。真正的防线是逐类型的字段名比对（第 2 类）
//  以及 read_archive 自己的类型表校验。
//
//  ---- 二进制有、JSON 没有的一条：非有限浮点 ----
//
//  二进制走小端 f32 位模式，NaN 也存得下。JSON 没有 NaN/Infinity 的字面量，
//  所以**含非有限浮点的场景存不成 JSON**（json_write_f32 会拒绝并报出来）。
//  这不是可以商量的余地：写出 "NaN" 会产出不合法的文档，
//  而"能存但读不回来"比"存不了"糟得多。
// =============================================================================
#include "engine/scene/scene_io.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/byte_io.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/memory.h" // 直接用 arena_alloc / ARENA_NEW_ARRAY，不靠 json.h 传递
#include "engine/ecs/component.h"
#include "engine/ecs/entity.h"
#include "engine/ecs/world.h"

#include "scene_io_internal.h"

// 缩进 2 空格：场景文件要贴进 PR、要在 diff 里看得清，4 空格会让
// 一层 components 数组就把行推到 80 列以外。
#define SCENE_JSON_INDENT 2u

#define SCENE_JSON_FORMAT "Polaire-Scene"

// 字符串字段的中转缓冲区。宽度取自**定义它的地方**（资源路径 = 名字上限 ×2），
// 而不是另拍一个数：再放宽一点就会让"容量算错"这类 bug 藏在一个没人碰到的余量里。
#define SCENE_JSON_STRING_MAX (SCENE_RESOURCE_NAME_MAX * 2u)

static const char *scene_json_kind_name(u32 kind)
{
    switch (kind) {
    case (u32)SCENE_ARCHIVE_SCENE: return "scene";
    case (u32)SCENE_ARCHIVE_PREFAB: return "prefab";
    default: return NULL;
    }
}

// ============================================================ 导出：字节 → 文本

// 读存档里一个定长名字块（SCENE_RESOURCE_NAME_MAX 字节）到 C 字符串。
static bool json_in_fixed_name(bio_reader_t *r, char *out, usize cap)
{
    char pad[SCENE_RESOURCE_NAME_MAX];
    if (!bio_read_bytes(r, pad, sizeof(pad))) {
        return false;
    }
    pad[SCENE_RESOURCE_NAME_MAX - 1u] = '\0'; // 坏字节里可能压根没有 NUL
    snprintf(out, cap, "%s", pad);
    return true;
}

// 从字节流读一个标量、写成 JSON 值。只写值本身，key 与分隔符由调用处负责。
static bool json_out_scalar(bio_reader_t *r, ecs_field_kind_t kind, json_writer_t *jw)
{
    switch (kind) {
    case ECS_FIELD_F32: {
        f32 v = 0.0f;
        return bio_read_f32(r, &v) && json_write_f32(jw, v);
    }
    case ECS_FIELD_F64: {
        u64 bits = 0u;
        f64 v = 0.0;
        if (!bio_read_u64(r, &bits)) {
            return false;
        }
        memcpy(&v, &bits, sizeof(v));
        return json_write_f64(jw, v);
    }
    case ECS_FIELD_U32: {
        u32 v = 0u;
        return bio_read_u32(r, &v) && json_write_u32(jw, v);
    }
    case ECS_FIELD_DENSE_ID: {
        u32 v = 0u;
        if (!bio_read_u32(r, &v)) {
            return false;
        }
        // 两个"指向场景内别的东西"的种类都用 null 表示"没有"：
        // 4294967295 在文本里长得很像"某个巨大的下标"，null 才不会被读错。
        return (v == (u32)ECS_DENSE_NONE) ? json_write_null(jw) : json_write_u32(jw, v);
    }
    case ECS_FIELD_I32: {
        i32 v = 0;
        return bio_read_i32(r, &v) && json_write_i32(jw, v);
    }
    case ECS_FIELD_U8: {
        u8 v = 0u;
        return bio_read_u8(r, &v) && json_write_u32(jw, (u32)v);
    }
    case ECS_FIELD_BOOL: {
        u8 raw = 0u;
        // 只认 0/1，和二进制读侧同一条规矩：放宽就是把格式问题伪装成数据问题。
        if (!bio_read_u8(r, &raw) || raw > 1u) {
            return false;
        }
        return json_write_bool(jw, raw == 1u);
    }
    case ECS_FIELD_ENTITY: {
        u32 dense = 0u;
        if (!bio_read_u32(r, &dense)) {
            return false;
        }
        return (dense == (u32)ECS_DENSE_NONE) ? json_write_null(jw) : json_write_u32(jw, dense);
    }
    case ECS_FIELD_VEC2:
    case ECS_FIELD_VEC3:
    case ECS_FIELD_VEC4:
    case ECS_FIELD_QUAT: {
        const usize lanes = (kind == ECS_FIELD_VEC2) ? 2u
                            : (kind == ECS_FIELD_VEC3) ? 3u
                                                       : 4u;
        // 向量写成一行：`"position": [0, -0.3, 0]`。
        // 一行一个分量的话这份文件的行数要翻三倍，而它存在的理由就是给人扫一眼
        // 再改掉其中那个 -0.3 —— 阅读密度不是排版洁癖，是这个格式的全部用途。
        if (!json_begin_array_inline(jw)) {
            return false;
        }
        for (usize i = 0; i < lanes; ++i) {
            f32 v = 0.0f;
            if (!bio_read_f32(r, &v) || !json_write_f32(jw, v)) {
                return false;
            }
        }
        return json_end_array(jw);
    }
    case ECS_FIELD_STRING:
    case ECS_FIELD_MESH_REF:
    case ECS_FIELD_MATERIAL_REF:
    case ECS_FIELD_ATLAS_REF:
    case ECS_FIELD_KIND_COUNT: break; // 定长名字块由 json_out_field 处理
    }
    return false;
}

// 定长字符串的一个元素：capacity 字节，可能没有结尾 NUL。
static bool json_out_fixed_string(bio_reader_t *r, usize capacity, json_writer_t *jw)
{
    char buf[SCENE_JSON_STRING_MAX];
    if (capacity == 0u || capacity > sizeof(buf)) {
        LOG_ERROR("字符串字段的容量 %llu 超出转码缓冲区 %u", (unsigned long long)capacity,
                  (unsigned)sizeof(buf));
        return false;
    }
    if (!bio_read_bytes(r, buf, capacity)) {
        return false;
    }
    buf[capacity - 1u] = '\0';
    return json_write_string(jw, buf);
}

// 一个"若干个同样元素"的字段：count==1 打成标量，count>1 打成数组。
// 三处（STRING / 资源引用 / 标量数组）共用这个形状判断，否则"什么时候用数组"
// 会在同一个文件里有两套答案。
typedef struct json_out_elem_ctx_t {
    ecs_field_desc_t field;
    json_writer_t *jw;
} json_out_elem_ctx_t;

static bool json_out_one_string(bio_reader_t *r, void *ud)
{
    json_out_elem_ctx_t *c = (json_out_elem_ctx_t *)ud;
    return json_out_fixed_string(r, c->field.capacity, c->jw);
}

static bool json_out_one_ref(bio_reader_t *r, void *ud)
{
    json_out_elem_ctx_t *c = (json_out_elem_ctx_t *)ud;
    char name[SCENE_RESOURCE_NAME_MAX];
    return json_in_fixed_name(r, name, sizeof(name)) && json_write_string(c->jw, name);
}

static bool json_out_one_scalar(bio_reader_t *r, void *ud)
{
    json_out_elem_ctx_t *c = (json_out_elem_ctx_t *)ud;
    return json_out_scalar(r, c->field.kind, c->jw);
}

static bool json_out_elements(bio_reader_t *r, usize count, json_writer_t *jw,
                              bool (*one)(bio_reader_t *, void *), json_out_elem_ctx_t *ctx)
{
    if (count <= 1u) {
        return one(r, ctx);
    }
    if (!json_begin_array(jw)) {
        return false;
    }
    for (usize i = 0; i < count; ++i) {
        if (!one(r, ctx)) {
            return false;
        }
    }
    return json_end_array(jw);
}

static bool json_out_field(bio_reader_t *r, const ecs_field_desc_t *f, json_writer_t *jw)
{
    const usize count = (f->count > 0u) ? (usize)f->count : 1u;
    json_out_elem_ctx_t ctx;
    ctx.field = *f;
    ctx.jw = jw;
    if (!json_write_key(jw, f->name)) {
        return false;
    }
    if (f->kind == ECS_FIELD_STRING) {
        // 成组的定长字符串（材质那 8 个图集槽位）打成数组，并且**尾部空串照样写出去**：
        // 省略它们会让"这里有 8 个槽位"只剩数组长度一个依据，
        // 而数组长度正是导入侧唯一的依据。
        return json_out_elements(r, count, jw, json_out_one_string, &ctx);
    }
    if (f->kind == ECS_FIELD_MESH_REF || f->kind == ECS_FIELD_MATERIAL_REF ||
        f->kind == ECS_FIELD_ATLAS_REF) {
        return json_out_elements(r, count, jw, json_out_one_ref, &ctx);
    }
    return json_out_elements(r, count, jw, json_out_one_scalar, &ctx);
}

// 按字段表把一整条定长记录展开成一个 JSON 对象。
static bool json_out_record(bio_reader_t *r, const ecs_component_desc_t *desc, json_writer_t *jw)
{
    if (!json_begin_object(jw)) {
        return false;
    }
    for (u32 i = 0; i < desc->field_count; ++i) {
        if (!json_out_field(r, &desc->fields[i], jw)) {
            LOG_ERROR("%s 展开成 JSON 时字节流提前读空了", desc->name);
            return false;
        }
    }
    return json_end_object(jw);
}

// dense / parent 这类"可以没有"的整数：没有就写 null，而不是 4294967295。
static bool json_out_maybe_u32(json_writer_t *jw, u32 value)
{
    return (value == (u32)ECS_DENSE_NONE) ? json_write_null(jw) : json_write_u32(jw, value);
}

static bool scene_json_archive_to_text(scene_t *scene, bio_reader_t *r, json_writer_t *jw)
{
    ecs_world_t *world = scene_world(scene);

    u32 magic = 0u, version = 0u, hash = 0u;
    u32 ent_count = 0u, mesh_count = 0u, atlas_count = 0u, material_count = 0u, type_count = 0u;
    u32 kind = 0u, root_dense = 0u;
    if (!bio_read_u32(r, &magic) || !bio_read_u32(r, &version) || !bio_read_u32(r, &hash) ||
        !bio_read_u32(r, &ent_count) || !bio_read_u32(r, &mesh_count) ||
        !bio_read_u32(r, &atlas_count) || !bio_read_u32(r, &material_count) ||
        !bio_read_u32(r, &type_count) || !bio_read_u32(r, &kind) ||
        !bio_read_u32(r, &root_dense)) {
        LOG_ERROR("转码失败：存档头部不完整");
        return false;
    }
    if (magic != SCENE_FILE_MAGIC) {
        LOG_ERROR("转码失败：不是场景存档（魔数 0x%08X）", (unsigned)magic);
        return false;
    }
    if (version != SCENE_FILE_VERSION) {
        LOG_ERROR("转码失败：存档版本 %u，引擎 %u", (unsigned)version,
                  (unsigned)SCENE_FILE_VERSION);
        return false;
    }
    if (scene_json_kind_name(kind) == NULL) {
        LOG_ERROR("转码失败：存档种类 %u 未知", (unsigned)kind);
        return false;
    }
    // 类型表要靠**这个构建**的字段表才能解释，所以注册表必须一致。
    // 这不是把 read_archive 的校验抄一遍：那条路在转码**之后**才走，
    // 而这里如果放行，症状会是"JSON 少了一段而没人报错"。
    if (hash != scene_registry_hash(scene)) {
        LOG_ERROR("转码失败：存档注册表 0x%08X 与当前 0x%08X 不一致", (unsigned)hash,
                  (unsigned)scene_registry_hash(scene));
        return false;
    }
    if (!bio_skip(r, SCENE_FILE_HEADER_SIZE - bio_reader_pos(r))) {
        LOG_ERROR("转码失败：头部保留段读不到末尾");
        return false;
    }
    // 这些计数直接驱动循环，所以必须先卡上限：一个被改坏的巨大计数会变成
    // "转码跑几十亿次"，而那看起来像卡死而不是报错。
    if (ent_count > SCENE_IO_MAX_ENTITIES_PER_FILE || mesh_count > SCENE_MAX_MESHES ||
        atlas_count > SCENE_MAX_ATLASES || material_count > SCENE_MAX_MATERIALS ||
        type_count > ECS_MAX_COMPONENTS) {
        LOG_ERROR("转码失败：存档计数越界（实体 %u，网格 %u，图集 %u，材质 %u，类型 %u）",
                  (unsigned)ent_count, (unsigned)mesh_count, (unsigned)atlas_count,
                  (unsigned)material_count, (unsigned)type_count);
        return false;
    }

    if (!json_begin_object(jw) || !json_write_key(jw, "format") ||
        !json_write_string(jw, SCENE_JSON_FORMAT) || !json_write_key(jw, "version") ||
        !json_write_u32(jw, SCENE_FILE_VERSION) || !json_write_key(jw, "kind") ||
        !json_write_string(jw, scene_json_kind_name(kind)) ||
        !json_write_key(jw, "registry_hash") || !json_write_u32(jw, hash) ||
        !json_write_key(jw, "root") || !json_out_maybe_u32(jw, root_dense)) {
        return false;
    }

    if (!json_write_key(jw, "environment") || !json_out_record(r, &k_env_desc, jw)) {
        return false;
    }

    // 类型表不单独打出来：entities 段里每个组件都写着自己的类型名，
    // 那份信息已经在人看得见的地方了。这里只把名字解成本构建的 desc。
    const ecs_component_desc_t *type_desc[ECS_MAX_COMPONENTS];
    u32 declared = 0u;
    if (!bio_read_u32(r, &declared) || declared != type_count) {
        LOG_ERROR("转码失败：类型表长度 %u 与头部 %u 不符", (unsigned)declared,
                  (unsigned)type_count);
        return false;
    }
    for (u32 t = 0; t < type_count; ++t) {
        char name[SCENE_RESOURCE_NAME_MAX];
        u32 schema = 0u;
        if (!json_in_fixed_name(r, name, sizeof(name)) || !bio_read_u32(r, &schema)) {
            return false;
        }
        // 转码**不能**像读档那样跳过不认识的名字：读档跳过是在填一个还没建起来的
        // 场景，转码跳过是在重写一份自称"完整导出结果"的文件，那是静默丢内容。
        const ecs_component_id id = ecs_find_component(world, name);
        const ecs_component_desc_t *d =
            (id == ECS_COMPONENT_NONE) ? NULL : ecs_component_desc(world, id);
        if (d == NULL) {
            LOG_ERROR("转码失败：这个构建不认识组件类型 %s，无法解释它的字节", name);
            return false;
        }
        if (ecs_component_schema_hash(d) != schema) {
            LOG_ERROR("转码失败：组件类型 %s 的字段布局与这份存档不符", d->name);
            return false;
        }
        type_desc[t] = d;
    }

    if (!json_write_key(jw, "meshes") || !json_begin_array(jw)) {
        return false;
    }
    for (u32 i = 0; i < mesh_count; ++i) {
        if (!json_out_record(r, &k_resource_desc, jw)) {
            return false;
        }
    }
    if (!json_end_array(jw) || !json_write_key(jw, "atlases") || !json_begin_array(jw)) {
        return false;
    }
    for (u32 i = 0; i < atlas_count; ++i) {
        if (!json_out_record(r, &k_resource_desc, jw)) {
            return false;
        }
    }
    if (!json_end_array(jw) || !json_write_key(jw, "materials") || !json_begin_array(jw)) {
        return false;
    }
    for (u32 i = 0; i < material_count; ++i) {
        if (!json_out_record(r, &k_material_desc, jw)) {
            return false;
        }
    }
    if (!json_end_array(jw)) {
        return false;
    }

    // 实体的"索引 + 数据"两段在 JSON 里合成一段（理由见文件头），
    // 所以先把索引收下来，再边读数据边写文本。
    u32 indexed = 0u;
    if (!bio_read_u32(r, &indexed) || indexed != ent_count) {
        LOG_ERROR("转码失败：实体索引段计数 %u 与头部 %u 不符", (unsigned)indexed,
                  (unsigned)ent_count);
        return false;
    }
    arena_t *scratch = scene_scratch_arena(scene);
    u32 *dense = ARENA_NEW_ARRAY(scratch, u32, ent_count + 1u);
    u32 *parent = ARENA_NEW_ARRAY(scratch, u32, ent_count + 1u);
    u32 *comps = ARENA_NEW_ARRAY(scratch, u32, ent_count + 1u);
    if (dense == NULL || parent == NULL || comps == NULL) {
        LOG_ERROR("转码失败：实体索引的临时数组分配失败（%u 个实体）", (unsigned)ent_count);
        return false;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        if (!bio_read_u32(r, &dense[i]) || !bio_read_u32(r, &parent[i]) ||
            !bio_read_u32(r, &comps[i])) {
            return false;
        }
    }

    u32 bodies = 0u;
    if (!bio_read_u32(r, &bodies) || bodies != ent_count) {
        LOG_ERROR("转码失败：实体数据段计数 %u 与头部 %u 不符", (unsigned)bodies,
                  (unsigned)ent_count);
        return false;
    }
    if (!json_write_key(jw, "entities") || !json_begin_array(jw)) {
        return false;
    }
    for (u32 i = 0; i < ent_count; ++i) {
        if (!json_begin_object(jw) || !json_write_key(jw, "dense") ||
            !json_out_maybe_u32(jw, dense[i]) || !json_write_key(jw, "parent") ||
            !json_out_maybe_u32(jw, parent[i]) || !json_write_key(jw, "components") ||
            !json_begin_array(jw)) {
            return false;
        }
        for (u32 c = 0; c < comps[i]; ++c) {
            u32 type_index = 0u, payload = 0u;
            if (!bio_read_u32(r, &type_index) || !bio_read_u32(r, &payload)) {
                return false;
            }
            if (type_index >= type_count) {
                LOG_ERROR("转码失败：第 %u 个实体引用了类型下标 %u，类型表只有 %u 项",
                          (unsigned)i, (unsigned)type_index, (unsigned)type_count);
                return false;
            }
            const ecs_component_desc_t *d = type_desc[type_index];
            const usize want = scene_io_desc_wire_size(d);
            if (payload != (u32)want) {
                LOG_ERROR("转码失败：组件 %s 声明的长度 %u 与字段表算出的 %llu 不符", d->name,
                          (unsigned)payload, (unsigned long long)want);
                return false;
            }
            if (!json_begin_object(jw) || !json_write_key(jw, "type") ||
                !json_write_string(jw, d->name) || !json_write_key(jw, "fields") ||
                !json_out_record(r, d, jw) || !json_end_object(jw)) {
                return false;
            }
        }
        if (!json_end_array(jw) || !json_end_object(jw)) {
            return false;
        }
    }
    if (!json_end_array(jw) || !json_end_object(jw)) {
        return false;
    }

    // 走到这里必须刚好读空。还剩字节说明有一段没被解释 ——
    // 要么是新加的段忘了转码，要么是文件被截断/拼接过，两种都不能算成功。
    if (bio_reader_remaining(r) != 0u || !bio_reader_ok(r)) {
        LOG_ERROR("转码后仍有 %llu 字节没被解释（存档被改坏，或它的格式比这个构建新）",
                  (unsigned long long)bio_reader_remaining(r));
        return false;
    }
    return true;
}

// 把二进制存档展开成 JSON 文本。缓冲区末尾会多写一个 NUL（方便直接当 C 字符串
// 打印），但 *out_length 不含它。
//
// 装不下就**翻倍重试**。为什么不手算一个精确上限：每种字段的文本宽度取决于
// 实际值（0.1 与 0.10000000001 差一个数量级），手推的上限算小了就是"存盘失败"、
// 算大了就是白占内存；而重试把这个数从"正确性问题"降级成"性能问题"，
// 几何级数最多浪费一倍。
static bool scene_json_encode_text(scene_t *scene, const void *bytes, usize size, arena_t *arena,
                                   void **out_text, usize *out_length)
{
    usize cap = size * 4u + 1024u;
    // 重试的起点。arena 不会把内存还给分配器，所以每次试大一号之前要 rewind 回去 ——
    // 不 rewind 的话一次存盘最多会白占八份文本，而"白占八份"和"白占一份"
    // 在日志里都写不出问题，只有内存曲线会替你说。
    const arena_mark_t start = arena_mark(arena);
    for (u32 attempt = 0u; attempt < 8u; ++attempt) {
        arena_rewind(arena, start);
        u8 *buffer = (u8 *)arena_alloc(arena, cap + 1u);
        if (buffer == NULL) {
            LOG_ERROR("JSON 文本缓冲区分配失败（%llu 字节）", (unsigned long long)(cap + 1u));
            return false;
        }
        bio_writer_t raw;
        bio_writer_init(&raw, buffer, cap);
        json_writer_t jw;
        json_writer_init(&jw, &raw, SCENE_JSON_INDENT);
        bio_reader_t r;
        bio_reader_init(&r, bytes, size);

        const bool walked = scene_json_archive_to_text(scene, &r, &jw);
        if (!bio_writer_ok(&raw)) {
            // 只有"装不下"才重试；语义错误重试多少次都是同一个结果。
            // 第一次翻倍时说一句：这既是"估算偏小了"的证据，也是文本规模的实测点。
            if (attempt == 0u) {
                LOG_DEBUG("JSON 文本超过了 %llu 字节的预估，按两倍重试", (unsigned long long)cap);
            }
            cap *= 2u;
            continue;
        }
        if (!walked || !json_writer_ok(&jw)) {
            arena_rewind(arena, start); // 失败不留临时内存（调用方的 mark 也在同一层）
            return false;
        }
        const usize used = bio_writer_used(&raw);
        buffer[used] = '\0';
        if (out_text != NULL) {
            *out_text = buffer;
        }
        if (out_length != NULL) {
            *out_length = used;
        }
        return true; // **不** rewind：文本是要交回调用方的
    }
    LOG_ERROR("JSON 文本超过 %llu 字节，重试 8 次仍然装不下", (unsigned long long)cap);
    arena_rewind(arena, start);
    return false;
}

// ============================================================ 导入：文本 → 字节

typedef struct scene_json_enc_t {
    scene_t *scene;
    const ecs_world_t *world;

    // 本层的计数。单位是**类型**，和 scene_io_stats_t 里那两条一致。
    scene_io_stats_t stats;

    // 文件类型表：按文档里首次出现的顺序编号。二进制那边按注册表顺序，
    // 但那个顺序对读侧没有意义 —— read_archive 是拿名字查 id 的。
    const ecs_component_desc_t *types[ECS_MAX_COMPONENTS];
    u32 type_count;

    // 已经因为"不认识/布局不符"被丢掉的类型名。这张表存在的唯一理由是计数单位：
    // 一个类型有 37 个实例被丢，报成 37 会让人以为坏了 37 处，
    // 而该做的只有一件事 —— 去补那个类型。
    const char *dropped[ECS_MAX_COMPONENTS];
    u32 drop_count;
} scene_json_enc_t;

static bool scene_json_dropped(const scene_json_enc_t *e, const char *name)
{
    for (u32 i = 0u; i < e->drop_count; ++i) {
        if (strcmp(e->dropped[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void scene_json_drop_type(scene_json_enc_t *e, const char *name, bool by_schema)
{
    if (scene_json_dropped(e, name) || e->drop_count >= ECS_MAX_COMPONENTS) {
        return;
    }
    e->dropped[e->drop_count++] = name; // 名字活在 DOM 的 arena 上，比这次转码活得久
    if (by_schema) {
        e->stats.schema_mismatches++;
    } else {
        e->stats.unknown_component_types++;
    }
}

// 名字 → 文件类型下标。false = 这个构建没有这个类型（或它是派生数据），
// 于是这条记录丢掉并已计数。
//
// 类型表在解码开始时**整张照抄注册表**（scene_serializable_types 的顺序），
// 所以这里只查不加。为什么不给文档自己编号：那样"二进制 → JSON → 二进制"
// 的类型下标会按文档顺序重排，两份字节就不可能相同 ——
// 而那份相同是"两种编码描述同一份数据"唯一的直接证据。
static bool scene_json_type_index(const scene_json_enc_t *e, const char *name, u32 *out_index)
{
    for (u32 i = 0u; i < e->type_count; ++i) {
        if (strcmp(e->types[i]->name, name) == 0) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

// obj 的每个键都必须在 desc 的字段表里。返回第一个不认识的名字（NULL = 全认识）。
static const char *scene_json_unknown_key(const ecs_component_desc_t *desc,
                                         const json_value_t *obj)
{
    for (usize i = 0u; i < obj->item_count; ++i) {
        bool known = false;
        for (u32 f = 0u; f < desc->field_count; ++f) {
            if (strcmp(desc->fields[f].name, obj->keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return obj->keys[i];
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- 取值
//
// 一个字段某元素的取值来源，合成一个"来源"而不是每处各判一次 NULL，是为了让
// **省略字段**这条规则只有一处实现：没写就取默认记录的内存。
// 默认记录由调用处备好 —— 组件走 desc->construct、环境走 scene_env_default()、
// 材质走 material_default()（scene_io_material_record_default）。
typedef struct scene_json_src_t {
    const json_value_t *node; // 该元素对应的节点；NULL = 文档里没写
    const u8 *mem;            // 默认记录里**这个元素**的基址
    bool bad;                 // 写坏了：类型不符、长度不符、字符串超长
} scene_json_src_t;

static bool src_f32(scene_json_src_t *s, f32 *out)
{
    if (s->node == NULL) {
        memcpy(out, s->mem, sizeof(f32));
        return true;
    }
    if (!json_as_f32(s->node, out)) {
        s->bad = true;
        return false;
    }
    return true;
}

static bool src_f64(scene_json_src_t *s, f64 *out)
{
    if (s->node == NULL) {
        memcpy(out, s->mem, sizeof(f64));
        return true;
    }
    if (!json_as_f64(s->node, out)) {
        s->bad = true;
        return false;
    }
    return true;
}

static bool src_u32(scene_json_src_t *s, u32 *out)
{
    if (s->node == NULL) {
        memcpy(out, s->mem, sizeof(u32));
        return true;
    }
    if (!json_as_u32(s->node, out)) {
        s->bad = true;
        return false;
    }
    return true;
}

static bool src_u8(scene_json_src_t *s, u8 *out)
{
    if (s->node == NULL) {
        *out = *s->mem;
        return true;
    }
    u32 v = 0u;
    if (!json_as_u32(s->node, &v) || v > 255u) {
        s->bad = true;
        return false;
    }
    *out = (u8)v;
    return true;
}

static bool src_bool(scene_json_src_t *s, bool *out)
{
    if (s->node == NULL) {
        memcpy(out, s->mem, sizeof(bool));
        return true;
    }
    if (!json_as_bool(s->node, out)) {
        s->bad = true;
        return false;
    }
    return true;
}

// dense id / 实体引用：文档里是一个数字或 null（省略也算 null）。
// 不读默认内存：那里躺的是 entity_t，而句柄不跨存档 ——
// 把它的 index 当成 dense 写出去就是把"这个引用指向别的实体"写进格式。
// 组件的 construct 只会把引用留成"无"，所以按 NONE 出去正是默认值本身。
static bool src_dense(scene_json_src_t *s, u32 *out)
{
    if (s->node == NULL || s->node->kind == JSON_NULL) {
        *out = (u32)ECS_DENSE_NONE;
        return true;
    }
    if (!json_as_u32(s->node, out)) {
        s->bad = true;
        return false;
    }
    return true;
}

// 定长名字：写进 dst（capacity 字节，保证有结尾 NUL）。
// 超长必须拒绝而不是截断 —— 两个只在第 63 个字符之后才不同的名字，
// 截断之后是同一个名字，读档按名字查表就会把资源挂到别的物体上。
static bool src_string(scene_json_src_t *s, u8 *dst, usize capacity)
{
    memset(dst, 0, capacity);
    if (s->node == NULL) {
        memcpy(dst, s->mem, capacity); // 默认记录里本来就是 capacity 字节
        dst[capacity - 1u] = '\0';     // 全零也是合法的"空名字"，这一句是给坏默认值兜底
        return true;
    }
    const char *text = "";
    usize length = 0u;
    if (!json_as_string(s->node, &text, &length)) {
        s->bad = true;
        return false;
    }
    if (length + 1u > capacity) {
        s->bad = true;
        LOG_ERROR("字符串 \"%s\"（%llu 字节）超过字段容量 %llu", text, (unsigned long long)length,
                  (unsigned long long)capacity);
        return false;
    }
    memcpy(dst, text, length);
    return true;
}

// 资源引用：文档里是名字。
// 为什么不像 STRING 那样抄默认内存：那里的默认是 char[N]（本身就有意义），
// 而这里的默认是一个 u32 表下标 —— 下标属于上一次运行，在这份文档里毫无意义，
// 唯一能传递的信息是"没有引用"。
static bool src_ref(scene_json_src_t *s, u8 *dst, scene_json_enc_t *e)
{
    if (s->node == NULL) {
        u32 index = SCENE_RESOURCE_NONE;
        memcpy(&index, s->mem, sizeof(index));
        if (index != SCENE_RESOURCE_NONE) {
            LOG_WARN("某个组件的默认值里带着非空的资源下标 %u，按\"无\"写出", (unsigned)index);
            e->stats.dangling_refs++;
        }
        dst[0] = '\0';
        return true;
    }
    return src_string(s, dst, SCENE_RESOURCE_NAME_MAX);
}

// ---------------------------------------------------------------- 字段与记录

// 一个标量元素（含向量的整族分量）→ 字节流。
static bool enc_scalar(bio_writer_t *w, ecs_field_kind_t kind, scene_json_src_t *s)
{
    switch (kind) {
    case ECS_FIELD_F32: {
        f32 v = 0.0f;
        return src_f32(s, &v) ? bio_write_f32(w, v) : false;
    }
    case ECS_FIELD_F64: {
        f64 v = 0.0;
        if (!src_f64(s, &v)) {
            return false;
        }
        u64 bits = 0u;
        memcpy(&bits, &v, sizeof(bits));
        return bio_write_u64(w, bits);
    }
    case ECS_FIELD_U32: {
        // 省略的 u32 取**默认记录**的值，不是 0xFFFFFFFF。
        // 走 src_dense 的话"没写 layer"会变成"layer = 4294967295"，
        // 而那种 layer 上的东西不会被任何相机画出来 —— 一个静默的"物体不见了"。
        u32 v = 0u;
        return src_u32(s, &v) ? bio_write_u32(w, v) : false;
    }
    case ECS_FIELD_DENSE_ID: {
        // 导出侧把 ECS_DENSE_NONE 写成 null，所以 null 必须读回 NONE；
        // 而"整个字段省略"取的是默认记录里的值 —— 两者不是一回事，分开判。
        u32 v = 0u;
        if (s->node == NULL) {
            memcpy(&v, s->mem, sizeof(v));
            return bio_write_u32(w, v);
        }
        return src_dense(s, &v) ? bio_write_u32(w, v) : false;
    }
    case ECS_FIELD_I32: {
        i32 v = 0;
        if (s->node == NULL) {
            memcpy(&v, s->mem, sizeof(v));
            return bio_write_i32(w, v);
        }
        if (!json_as_i32(s->node, &v)) {
            s->bad = true;
            return false;
        }
        return bio_write_i32(w, v);
    }
    case ECS_FIELD_U8: {
        u8 v = 0u;
        return src_u8(s, &v) ? bio_write_u8(w, v) : false;
    }
    case ECS_FIELD_BOOL: {
        bool v = false;
        return src_bool(s, &v) ? bio_write_u8(w, v ? 1u : 0u) : false;
    }
    case ECS_FIELD_ENTITY: {
        u32 dense = 0u;
        return src_dense(s, &dense) ? bio_write_u32(w, dense) : false;
    }
    case ECS_FIELD_VEC2:
    case ECS_FIELD_VEC3:
    case ECS_FIELD_VEC4:
    case ECS_FIELD_QUAT: {
        const usize lanes = (kind == ECS_FIELD_VEC2) ? 2u
                            : (kind == ECS_FIELD_VEC3) ? 3u
                                                       : 4u;
        if (s->node != NULL) {
            if (s->node->kind != JSON_ARRAY) {
                s->bad = true;
                return false;
            }
            if (s->node->item_count != lanes) {
                // 分量不齐全就拒绝，而不是把缺的当 0：
                // "position": [1,2] 读成 (1,2,0) 会造出一个**看起来正常**的错位物体。
                s->bad = true;
                LOG_ERROR("向量字段需要 %u 个分量，文档给了 %llu 个", (unsigned)lanes,
                          (unsigned long long)s->node->item_count);
                return false;
            }
        }
        for (usize lane = 0u; lane < lanes; ++lane) {
            f32 v = 0.0f;
            if (s->node != NULL) {
                if (!json_as_f32(json_array_at(s->node, lane), &v)) {
                    s->bad = true;
                    return false;
                }
            } else {
                memcpy(&v, s->mem + lane * sizeof(f32), sizeof(f32));
            }
            if (!bio_write_f32(w, v)) {
                return false;
            }
        }
        return true;
    }
    case ECS_FIELD_STRING:
    case ECS_FIELD_MESH_REF:
    case ECS_FIELD_MATERIAL_REF:
    case ECS_FIELD_ATLAS_REF:
    case ECS_FIELD_KIND_COUNT: break; // 名字块由 enc_field 按 capacity 处理
    }
    return false;
}

// 文档里"一个字段的值"有两种数组：向量本身就是数组（[x,y,z]），
// 或者"若干个元素"（材质的 8 个图集名）。这两种必须分清，
// 否则 clear_color 那 4 个分量会被当成"这个字段有 4 个元素"而拒掉。
static bool scene_json_kind_is_vector(ecs_field_kind_t kind)
{
    return kind == ECS_FIELD_VEC2 || kind == ECS_FIELD_VEC3 || kind == ECS_FIELD_VEC4 ||
           kind == ECS_FIELD_QUAT;
}

static bool enc_field(bio_writer_t *w, const ecs_field_desc_t *f, const json_value_t *node,
                      const u8 *def, scene_json_enc_t *e)
{
    const usize count = (f->count > 0u) ? (usize)f->count : 1u;
    const usize stride = ecs_field_kind_size(f->kind);
    // 元素自己就是数组的种类（向量），只在 count>1 时才需要外面再套一层数组。
    const bool element_is_array = scene_json_kind_is_vector(f->kind);
    const bool wrapper =
        (node != NULL && node->kind == JSON_ARRAY) && !(element_is_array && count == 1u);
    const usize given = (node == NULL) ? 0u : (wrapper ? (usize)node->item_count : 1u);
    if (given > count) {
        // 比字段表还长：截断会静默丢掉内容，报错才会让人去数一遍。
        LOG_ERROR("字段 %s 只有 %u 个元素的位置，文档给了 %u 个", f->name, (unsigned)count,
                  (unsigned)given);
        return false;
    }
    if (!wrapper && count > 1u && node != NULL) {
        // 给一个标量填 8 个槽位（材质的图集）也是歧义：到底是"8 个都用它"还是"只有第一个"？
        // 两种猜法都会造出看得见的差别，所以要求写成数组。
        LOG_ERROR("字段 %s 有 %u 个元素，文档必须给数组", f->name, (unsigned)count);
        return false;
    }
    if (f->kind == ECS_FIELD_STRING && (usize)f->capacity > SCENE_JSON_STRING_MAX) {
        LOG_ERROR("字段 %s 的容量 %u 超出转码缓冲区 %u", f->name, (unsigned)f->capacity,
                  (unsigned)SCENE_JSON_STRING_MAX);
        return false;
    }

    for (usize elem = 0u; elem < count; ++elem) {
        scene_json_src_t s;
        s.node = NULL;
        s.bad = false;
        if (wrapper) {
            s.node = json_array_at(node, elem); // 越界 = NULL = "尾部省略"
        } else if (elem == 0u) {
            s.node = node;
        }

        if (f->kind == ECS_FIELD_STRING) {
            u8 buf[SCENE_JSON_STRING_MAX];
            s.mem = def + elem * (usize)f->capacity;
            if (!src_string(&s, buf, f->capacity) || s.bad) {
                return false;
            }
            if (!bio_write_bytes(w, buf, f->capacity)) {
                return false;
            }
            continue;
        }
        if (f->kind == ECS_FIELD_MESH_REF || f->kind == ECS_FIELD_MATERIAL_REF ||
            f->kind == ECS_FIELD_ATLAS_REF) {
            u8 buf[SCENE_RESOURCE_NAME_MAX];
            memset(buf, 0, sizeof(buf));
            s.mem = def + elem * stride;
            if (!src_ref(&s, buf, e) || s.bad) {
                return false;
            }
            if (!bio_write_bytes(w, buf, sizeof(buf))) {
                return false;
            }
            continue;
        }
        s.mem = def + elem * stride;
        if (!enc_scalar(w, f->kind, &s) || s.bad) {
            return false;
        }
    }
    return true;
}

// JSON 对象 → 一条定长记录。def_mem 是**默认记录**的字节（省略字段的取值来源）。
// obj 为 NULL = 整个对象都省略了，于是整条记录就是默认值。
static bool enc_record(bio_writer_t *w, const ecs_component_desc_t *desc, const json_value_t *obj,
                       const u8 *def_mem, scene_json_enc_t *e)
{
    const usize before = bio_writer_used(w);
    for (u32 i = 0; i < desc->field_count; ++i) {
        const ecs_field_desc_t *f = &desc->fields[i];
        const json_value_t *node = json_object_get(obj, f->name);
        if (!enc_field(w, f, node, def_mem + f->offset, e)) {
            LOG_ERROR("%s.%s 编不进存档（类型或长度不对）", desc->name, f->name);
            return false;
        }
    }
    // 每个写记录的地方都核对"字段表算出的长度 == 真写出的字节数"，
    // 判定与措辞在 scene_io_check_record_bytes 里只有一份（二进制那边共用它）。
    return scene_io_check_record_bytes(desc, scene_io_desc_wire_size(desc),
                                       bio_writer_used(w) - before);
}

// ---------------------------------------------------------------- 各段

static bool scene_json_section(const json_value_t *doc, const char *key, json_kind_t want,
                               bool required, const json_value_t **out)
{
    *out = NULL;
    const json_value_t *v = json_object_get(doc, key);
    if (v == NULL || v->kind == JSON_NULL) {
        if (required) {
            LOG_ERROR("JSON 存档缺少 \"%s\" 段", key);
            return false;
        }
        return true; // 可选段没写：调用方按"空/默认"处理
    }
    if (v->kind != want) {
        LOG_ERROR("JSON 存档的 \"%s\" 应该是%s，实际是%s", key, json_kind_name(want),
                  json_kind_name(v->kind));
        return false;
    }
    *out = v;
    return true;
}

// 顶层键的白名单。白名单不是"洁癖"：把 `entites` 当未知键**忽略**的话，
// 症状是"这个文件能读，只是场景是空的"，而空场景在编辑器里和"我刚才存错了"
// 长得一模一样 —— 这类静默的坏输入正是格式最该防的。
static bool scene_json_top_keys_known(const json_value_t *doc)
{
    static const char *const k_allowed[] = {"format",  "version",   "kind",    "registry_hash",
                                            "root",    "environment", "meshes",  "atlases",
                                            "materials", "entities"};
    const usize allowed_count = sizeof(k_allowed) / sizeof(k_allowed[0]);
    for (usize i = 0u; i < doc->item_count; ++i) {
        bool known = false;
        for (usize a = 0u; a < allowed_count; ++a) {
            if (strcmp(doc->keys[i], k_allowed[a]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            LOG_ERROR("JSON 存档顶层有未知的键 \"%s\"（拼错的段名不会被忽略）", doc->keys[i]);
            return false;
        }
    }
    return true;
}

// 资源/材质段：count 条定长记录。array 为 NULL 时要求 count 也是 0（调用方保证）。
static bool scene_json_encode_records(bio_writer_t *w, const ecs_component_desc_t *desc,
                                      const json_value_t *array, const u8 *def_mem, usize count,
                                      scene_json_enc_t *e)
{
    for (usize i = 0u; i < count; ++i) {
        const json_value_t *obj = json_array_at(array, i);
        if (obj == NULL || obj->kind != JSON_OBJECT) {
            LOG_ERROR("%s 段第 %u 项不是对象", desc->name, (unsigned)i);
            return false;
        }
        const char *unknown = scene_json_unknown_key(desc, obj);
        if (unknown != NULL) {
            // 这三段是引擎自己的数据结构，不是插件式的组件类型，所以"键不认识"
            // 只有一个合理解释：打错了。按 schema 漂移处理会静默少一个字段。
            LOG_ERROR("%s 段有一个字段表里没有的键 \"%s\"", desc->name, unknown);
            return false;
        }
        if (!enc_record(w, desc, obj, def_mem, e)) {
            return false;
        }
    }
    return true;
}

// components 数组里的一个元素 → 文件类型下标 + 字段对象 + desc。
//
// 三种遍历都走这一个函数（测量、数条数、写出），因为"这条记录进不进存档"这个判断
// 必须在任何一遍里给出同一个答案，否则两遍数出的组件条数不一样，
// 而那份长度自检是唯一能发现它的地方。
//
// decide == true  ：这一遍**登记**判断（未注册、派生数据、字段不认识 → 记进 dropped）。
// decide == false ：只**查询**已经登记好的判断。
//
// 为什么要分"先登记完再查询"两遍，而不是边走边定：一个类型的字段漂移是在
// **某一条**记录上才发现的，而它的后果必须覆盖这个类型的**所有**实例 ——
// 二进制那边就是整个类型作废（schema 哈希是在类型表那一步定的，还没读到实例）。
// 只丢掉"发现之后"的那几条，同一份文档在两种编码下就会得到两个不同的场景。
static bool scene_json_resolve_component(scene_json_enc_t *e, const json_value_t *item,
                                         u32 *out_index, const json_value_t **out_fields,
                                         const ecs_component_desc_t **out_desc, bool *out_fatal,
                                         bool decide)
{
    *out_fatal = false;
    if (item == NULL || item->kind != JSON_OBJECT) {
        *out_fatal = true;
        LOG_ERROR("components 里每一项都必须是对象");
        return false;
    }
    const json_value_t *type = json_object_get(item, "type");
    const json_value_t *fields = json_object_get(item, "fields");
    for (usize i = 0u; i < item->item_count; ++i) {
        if (strcmp(item->keys[i], "type") != 0 && strcmp(item->keys[i], "fields") != 0) {
            *out_fatal = true;
            LOG_ERROR("组件记录里有未知的键 \"%s\"（只该有 type 与 fields）", item->keys[i]);
            return false;
        }
    }
    if (type == NULL || type->kind != JSON_STRING) {
        *out_fatal = true;
        LOG_ERROR("组件记录缺一个字符串类型的 \"type\"");
        return false;
    }
    if (fields == NULL || fields->kind != JSON_OBJECT) {
        *out_fatal = true;
        LOG_ERROR("组件 %s 缺少 \"fields\" 对象", type->string);
        return false;
    }

    u32 index = 0u;
    if (!scene_json_type_index(e, type->string, &index)) {
        // 注册表里没有这个名字，或者有但它是派生数据（世界矩阵那一类）。
        // 两种情况都按"这个构建不产出这种存档条目"处理，计数单位是类型不是实例。
        if (decide && !scene_json_dropped(e, type->string)) {
            const ecs_component_id id = ecs_find_component(e->world, type->string);
            const ecs_component_desc_t *found =
                (id == ECS_COMPONENT_NONE) ? NULL : ecs_component_desc(e->world, id);
            LOG_WARN("组件类型 %s %s，其实例将被跳过", type->string,
                     (found != NULL) ? "是派生数据（不进存档）" : "本构建未注册");
            scene_json_drop_type(e, type->string, false);
        }
        return false;
    }
    if (scene_json_dropped(e, e->types[index]->name)) {
        return false; // 登记遍已经报过、计过了
    }
    const ecs_component_desc_t *d = e->types[index];
    const char *stray = scene_json_unknown_key(d, fields);
    if (stray != NULL) {
        // 要么打错、要么字段布局变了，而两种情况的正确处置相同：整个类型作废
        // （见文件头"两类失败"的第 2 类）。
        if (decide && !scene_json_dropped(e, d->name)) {
            LOG_WARN("组件类型 %s 的字段 %s 不在本构建的字段表里，该类型的实例全部跳过", d->name,
                     stray);
            scene_json_drop_type(e, d->name, true);
        }
        return false;
    }
    *out_index = index;
    *out_desc = d;
    *out_fields = fields;
    return true;
}

// 实体段：先跑一遍把所有"这个构建不要的类型"登记完，再量条数与字节数。
static bool scene_json_entity(const json_value_t *entities, usize i, const json_value_t **out_comps)
{
    const json_value_t *ent = json_array_at(entities, i);
    if (ent == NULL || ent->kind != JSON_OBJECT) {
        LOG_ERROR("entities 第 %u 项不是对象", (unsigned)i);
        return false;
    }
    *out_comps = json_object_get(ent, "components");
    if (*out_comps == NULL || (*out_comps)->kind != JSON_ARRAY) {
        LOG_ERROR("entities 第 %u 项的 components 不是数组", (unsigned)i);
        return false;
    }
    return true;
}

static bool scene_json_measure_entities(scene_json_enc_t *e, const json_value_t *entities,
                                        arena_t *arena, u32 **out_counts, usize *out_body_bytes,
                                        u32 *out_ent_count)
{
    const usize count = (entities != NULL) ? entities->item_count : 0u;
    if (count > SCENE_IO_MAX_ENTITIES_PER_FILE) {
        LOG_ERROR("JSON 里有 %llu 个实体，超过单文件上限 %u", (unsigned long long)count,
                  (unsigned)SCENE_IO_MAX_ENTITIES_PER_FILE);
        return false;
    }
    u32 *counts = ARENA_NEW_ARRAY(arena, u32, count + 1u);
    if (counts == NULL) {
        LOG_ERROR("实体计数分配失败（%u 个实体）", (unsigned)count);
        return false;
    }
    *out_ent_count = (u32)count;

    // 第一遍：只登记判断（decide=true）。必须先把整个文档扫完，
    // 一个被判定漂移的类型才会连带作废它**靠前**的那些实例 ——
    // 二进制的 schema 哈希是在类型表那一步定的，还没读到任何实例。
    for (usize i = 0u; i < count; ++i) {
        const json_value_t *comps = NULL;
        if (!scene_json_entity(entities, i, &comps)) {
            return false;
        }
        for (usize c = 0u; c < comps->item_count; ++c) {
            u32 index = 0u;
            const json_value_t *fields = NULL;
            const ecs_component_desc_t *d = NULL;
            bool fatal = false;
            const bool kept = scene_json_resolve_component(e, json_array_at(comps, c), &index,
                                                           &fields, &d, &fatal, true);
            if (!kept && fatal) {
                return false;
            }
        }
    }

    // 第二遍：数保留下来的条数与字节。
    usize body = 0u;
    for (usize i = 0u; i < count; ++i) {
        const json_value_t *comps = NULL;
        if (!scene_json_entity(entities, i, &comps)) {
            return false;
        }
        u32 kept = 0u;
        u32 kept_index[ECS_MAX_COMPONENTS]; // 本实体已保留的类型下标（查重复用）
        for (usize c = 0u; c < comps->item_count; ++c) {
            u32 index = 0u;
            const json_value_t *fields = NULL;
            const ecs_component_desc_t *d = NULL;
            bool fatal = false;
            if (!scene_json_resolve_component(e, json_array_at(comps, c), &index, &fields, &d,
                                              &fatal, false)) {
                if (fatal) {
                    return false;
                }
                continue; // 版本漂移：丢掉这一条，其余照常
            }
            // 同一个实体里同一类型出现两次：二进制那边 ecs_add 会覆盖前一条，
            // 于是"存了两个组件"读回来只剩一个 —— 又是静默丢内容。
            // "一个实体一个类型至多一条"是这份格式的既定事实，所以这里拒绝。
            for (u32 k = 0u; k < kept; ++k) {
                if (kept_index[k] == index) {
                    LOG_ERROR("实体第 %u 项有两条 %s 组件", (unsigned)i, d->name);
                    return false;
                }
            }
            kept_index[kept++] = index;
            body += 8u + scene_io_desc_wire_size(d);
        }
        counts[i] = kept;
    }
    *out_counts = counts;
    *out_body_bytes = body;
    return true;
}

// 文本 → 存档字节。产出的字节交给 scene_io_read_archive —— 校验只在那一处。
static bool scene_json_decode_text(scene_t *scene, const json_value_t *doc, arena_t *arena,
                                   void **out_bytes, usize *out_size, scene_io_stats_t *out_stats)
{
    ASSERT(scene != NULL && doc != NULL && arena != NULL);
    *out_bytes = NULL;
    *out_size = 0u;

    scene_json_enc_t e;
    memset(&e, 0, sizeof(e));
    e.scene = scene;
    e.world = scene_world(scene);

    // 类型表整张照抄注册表，顺序与二进制写出那边**同源**（同一个函数）。
    // 这一步决定了"二进制 → JSON → 二进制"能不能逐字节相同：
    // 类型下标出现在每条记录里，表一重排全体就都不一样了。
    ecs_component_id type_ids[ECS_MAX_COMPONENTS];
    e.type_count = scene_serializable_types(e.world, type_ids, ECS_MAX_COMPONENTS);
    for (u32 i = 0u; i < e.type_count; ++i) {
        e.types[i] = ecs_component_desc(e.world, type_ids[i]);
    }

    if (doc->kind != JSON_OBJECT) {
        LOG_ERROR("JSON 存档的顶层必须是对象");
        return false;
    }
    if (!scene_json_top_keys_known(doc)) {
        return false;
    }

    const json_value_t *format = NULL, *version = NULL, *kind = NULL, *registry = NULL;
    const json_value_t *root = NULL, *env = NULL, *entities = NULL;
    const json_value_t *meshes = NULL, *atlases = NULL, *materials = NULL;
    bool ok = scene_json_section(doc, "format", JSON_STRING, false, &format) &&
              scene_json_section(doc, "version", JSON_NUMBER, false, &version) &&
              scene_json_section(doc, "kind", JSON_STRING, false, &kind) &&
              scene_json_section(doc, "registry_hash", JSON_NUMBER, false, &registry) &&
              scene_json_section(doc, "root", JSON_NUMBER, false, &root) &&
              scene_json_section(doc, "environment", JSON_OBJECT, false, &env) &&
              scene_json_section(doc, "meshes", JSON_ARRAY, false, &meshes) &&
              scene_json_section(doc, "atlases", JSON_ARRAY, false, &atlases) &&
              scene_json_section(doc, "materials", JSON_ARRAY, false, &materials) &&
              scene_json_section(doc, "entities", JSON_ARRAY, false, &entities);
    if (!ok) {
        return false;
    }
    if (format != NULL && strcmp(format->string, SCENE_JSON_FORMAT) != 0) {
        LOG_ERROR("不是本引擎的 JSON 场景存档（format = \"%s\"）", format->string);
        return false;
    }
    u32 doc_version = SCENE_FILE_VERSION;
    if (version != NULL && !json_as_u32(version, &doc_version)) {
        LOG_ERROR("JSON 的 version 不是非负整数");
        return false;
    }
    if (doc_version != SCENE_FILE_VERSION) {
        LOG_ERROR("JSON 存档版本 %u，引擎 %u", (unsigned)doc_version,
                  (unsigned)SCENE_FILE_VERSION);
        return false;
    }
    u32 archive_kind = (u32)SCENE_ARCHIVE_SCENE;
    if (kind != NULL) {
        if (strcmp(kind->string, scene_json_kind_name((u32)SCENE_ARCHIVE_PREFAB)) == 0) {
            archive_kind = (u32)SCENE_ARCHIVE_PREFAB;
        } else if (strcmp(kind->string, scene_json_kind_name((u32)SCENE_ARCHIVE_SCENE)) != 0) {
            LOG_ERROR("JSON 存档的 kind \"%s\" 不认识（只支持 scene / prefab）", kind->string);
            return false;
        }
    }
    u32 doc_registry = 0u;
    if (registry != NULL && json_as_u32(registry, &doc_registry) &&
        doc_registry != scene_registry_hash(scene)) {
        // 只报不拒：人手写的文件算不出这个哈希，而真正该防的那件事
        // （字段布局变了）由逐类型的字段名比对与 read_archive 把关。
        LOG_WARN("JSON 里写的 registry_hash 与当前注册表不符，按当前值写进存档");
    }
    u32 root_dense = (u32)ECS_DENSE_NONE;
    if (root != NULL && !json_as_u32(root, &root_dense)) {
        LOG_ERROR("JSON 的 root 不是非负整数");
        return false;
    }

    u32 *counts = NULL;
    usize body_bytes = 0u;
    u32 ent_count = 0u;
    if (!scene_json_measure_entities(&e, entities, arena, &counts, &body_bytes, &ent_count)) {
        return false;
    }
    const usize mesh_count = (meshes != NULL) ? (usize)meshes->item_count : 0u;
    const usize atlas_count = (atlases != NULL) ? (usize)atlases->item_count : 0u;
    const usize material_count = (materials != NULL) ? (usize)materials->item_count : 0u;
    if (mesh_count > SCENE_MAX_MESHES || atlas_count > SCENE_MAX_ATLASES ||
        material_count > SCENE_MAX_MATERIALS) {
        LOG_ERROR("JSON 里的资源条数超过表上限：网格 %u，图集 %u，材质 %u", (unsigned)mesh_count,
                  (unsigned)atlas_count, (unsigned)material_count);
        return false;
    }

    // 先量总长再一次分配 —— 和二进制那边同一条纪律，末尾那条长度自检才说明得了话。
    usize total = SCENE_FILE_HEADER_SIZE;
    total += scene_io_desc_wire_size(&k_env_desc);
    total += 4u + (usize)e.type_count * (SCENE_RESOURCE_NAME_MAX + 4u);
    total += mesh_count * scene_io_desc_wire_size(&k_resource_desc);
    total += atlas_count * scene_io_desc_wire_size(&k_resource_desc);
    total += material_count * scene_io_desc_wire_size(&k_material_desc);
    total += 4u + (usize)ent_count * 12u;
    total += 4u + body_bytes;

    u8 *buffer = (u8 *)arena_alloc(arena, total);
    if (buffer == NULL) {
        LOG_ERROR("JSON 转码的缓冲区分配失败（%llu 字节）", (unsigned long long)total);
        return false;
    }

    scene_env_t env_default = scene_env_default();
    scene_io_material_t mat_default;
    scene_io_material_record_default(&mat_default);
    scene_io_resource_t res_default;
    memset(&res_default, 0, sizeof(res_default));

    bio_writer_t w;
    bio_writer_init(&w, buffer, total);
    ok = bio_write_u32(&w, SCENE_FILE_MAGIC) && bio_write_u32(&w, SCENE_FILE_VERSION) &&
         bio_write_u32(&w, scene_registry_hash(scene)) && bio_write_u32(&w, ent_count) &&
         bio_write_u32(&w, (u32)mesh_count) && bio_write_u32(&w, (u32)atlas_count) &&
         bio_write_u32(&w, (u32)material_count) && bio_write_u32(&w, e.type_count) &&
         bio_write_u32(&w, archive_kind) && bio_write_u32(&w, root_dense) &&
         bio_write_zeros(&w, SCENE_FILE_HEADER_SIZE - bio_writer_used(&w));
    if (!ok) {
        return false;
    }

    if (env != NULL && scene_json_unknown_key(&k_env_desc, env) != NULL) {
        LOG_ERROR("environment 段有字段表里没有的键 \"%s\"",
                  scene_json_unknown_key(&k_env_desc, env));
        return false;
    }
    // env 为 NULL = 整段省略 → 逐字段查不到 → 整块按 scene_env_default() 落地，
    // 与二进制那边"预制体写默认值"给出同一份字节。
    if (!enc_record(&w, &k_env_desc, env, (const u8 *)&env_default, &e)) {
        return false;
    }

    if (!bio_write_u32(&w, e.type_count)) {
        return false;
    }
    for (u32 t = 0u; t < e.type_count; ++t) {
        const ecs_component_desc_t *d = e.types[t];
        char pad[SCENE_RESOURCE_NAME_MAX];
        memset(pad, 0, sizeof(pad));
        snprintf(pad, sizeof(pad), "%s", d->name);
        if (!bio_write_bytes(&w, pad, sizeof(pad)) ||
            !bio_write_u32(&w, ecs_component_schema_hash(d))) {
            return false;
        }
    }

    if (!scene_json_encode_records(&w, &k_resource_desc, meshes, (const u8 *)&res_default,
                                   mesh_count, &e) ||
        !scene_json_encode_records(&w, &k_resource_desc, atlases, (const u8 *)&res_default,
                                   atlas_count, &e) ||
        !scene_json_encode_records(&w, &k_material_desc, materials, (const u8 *)&mat_default,
                                   material_count, &e)) {
        return false;
    }

    if (!bio_write_u32(&w, ent_count)) {
        return false;
    }
    for (u32 i = 0u; i < ent_count; ++i) {
        const json_value_t *ent = json_array_at(entities, i);
        for (usize k = 0u; k < ent->item_count; ++k) {
            if (strcmp(ent->keys[k], "dense") != 0 && strcmp(ent->keys[k], "parent") != 0 &&
                strcmp(ent->keys[k], "components") != 0) {
                // "dens" 被忽略的话这条实体会静默拿到下标 i，
                // 而 i 恰好通常就是作者想写的值 —— 于是错误要等到下次 dense 有洞时才现形。
                LOG_ERROR("entities 第 %u 项有未知的键 \"%s\"（只该有 dense / parent / components）",
                          (unsigned)i, ent->keys[k]);
                return false;
            }
        }
        const json_value_t *dense_node = json_object_get(ent, "dense");
        const json_value_t *parent_node = json_object_get(ent, "parent");
        u32 dense = (u32)i, parent = (u32)ECS_DENSE_NONE;
        // 导出侧把"没有"写成 null，所以 null 必须等价于"省略"（而不是类型错误）；
        // 省略 dense = 用它在这个数组里的下标，人手写时通常不用管这个字段。
        if (dense_node != NULL && dense_node->kind != JSON_NULL &&
            !json_as_u32(dense_node, &dense)) {
            LOG_ERROR("entities 第 %u 项的 dense 不是非负整数", (unsigned)i);
            return false;
        }
        if (parent_node != NULL && parent_node->kind != JSON_NULL &&
            !json_as_u32(parent_node, &parent)) {
            LOG_ERROR("entities 第 %u 项的 parent 不是非负整数", (unsigned)i);
            return false;
        }
        if (!bio_write_u32(&w, dense) || !bio_write_u32(&w, parent) ||
            !bio_write_u32(&w, counts[i])) {
            return false;
        }
    }

    if (!bio_write_u32(&w, ent_count)) {
        return false;
    }
    for (u32 i = 0u; i < ent_count; ++i) {
        const json_value_t *ent = json_array_at(entities, i);
        const json_value_t *comps = json_object_get(ent, "components");
        u32 written = 0u;
        for (usize c = 0u; c < comps->item_count; ++c) {
            u32 index = 0u;
            const json_value_t *fields = NULL;
            const ecs_component_desc_t *d = NULL;
            bool fatal = false;
            if (!scene_json_resolve_component(&e, json_array_at(comps, c), &index, &fields, &d,
                                              &fatal, false)) {
                if (fatal) {
                    return false;
                }
                continue;
            }
            const usize payload = scene_io_desc_wire_size(d);
            if (!bio_write_u32(&w, index) || !bio_write_u32(&w, (u32)payload)) {
                return false;
            }
            // 默认记录：走 desc->construct，和 ecs_add(world, e, id, NULL) 同源。
            // 不能真的 ecs_add 一个临时实体来拿默认值 —— 那时世界还没开始建实体，
            // 而加完再删会在组件池里留下一次交换式移除的痕迹，
            // 于是"存回去的字节"取决于这个副作用。
            u8 *default_mem = (u8 *)arena_alloc(arena, d->size);
            if (default_mem == NULL) {
                LOG_ERROR("组件 %s 的默认记录分配失败（%u 字节）", d->name, (unsigned)d->size);
                return false;
            }
            memset(default_mem, 0, d->size);
            if (d->construct != NULL) {
                d->construct(default_mem);
            }
            if (!enc_record(&w, d, fields, default_mem, &e)) {
                return false;
            }
            written++;
        }
        if (written != counts[i]) {
            LOG_ERROR("实体第 %u 项在两遍里数出了不同的组件数（%u 对 %u）—— 转码自身不一致",
                      (unsigned)i, (unsigned)written, (unsigned)counts[i]);
            return false;
        }
        e.stats.components_loaded += written;
    }

    if (!bio_writer_ok(&w) || bio_writer_used(&w) != total) {
        LOG_ERROR("JSON 转码的存档长度不符：期望 %llu，实际 %llu", (unsigned long long)total,
                  (unsigned long long)bio_writer_used(&w));
        LOG_ERROR("  分段：实体 %u 个 / 数据 %llu 字节，类型 %u 条，网格 %u，图集 %u，材质 %u",
                  (unsigned)ent_count, (unsigned long long)body_bytes, (unsigned)e.type_count,
                  (unsigned)mesh_count, (unsigned)atlas_count, (unsigned)material_count);
        return false;
    }
    *out_bytes = buffer;
    *out_size = total;
    *out_stats = e.stats;
    return true;
}

// ============================================================ 公开入口

// 导出：场景 → 二进制 → JSON 文本。两步都在调用方给的 arena 上，
// 所以"存成 JSON"的临时内存 = 一份二进制 + 一份文本。这是加载期开销，不进帧内。
static bool scene_json_write_any(scene_t *scene, entity_t root, bool as_prefab, arena_t *arena,
                                 void **out_text, usize *out_length, scene_io_stats_t *out_stats)
{
    ASSERT(scene != NULL && arena != NULL);
    if (out_text != NULL) {
        *out_text = NULL;
    }
    if (out_length != NULL) {
        *out_length = 0u;
    }
    void *bytes = NULL;
    usize size = 0u;
    const bool wrote = as_prefab ? scene_write_prefab(scene, root, arena, &bytes, &size, out_stats)
                                 : scene_write_binary(scene, arena, &bytes, &size, out_stats);
    if (!wrote) {
        return false;
    }
    return scene_json_encode_text(scene, bytes, size, arena, out_text, out_length);
}

bool scene_write_json(scene_t *scene, arena_t *arena, void **out_text, usize *out_length,
                      scene_io_stats_t *out_stats)
{
    return scene_json_write_any(scene, ENTITY_NONE, false, arena, out_text, out_length, out_stats);
}

bool scene_write_prefab_json(scene_t *scene, entity_t root, arena_t *arena, void **out_text,
                             usize *out_length, scene_io_stats_t *out_stats)
{
    return scene_json_write_any(scene, root, true, arena, out_text, out_length, out_stats);
}

// 读档：JSON 文本 → 二进制 → read_archive。
//
// scratch arena 的 mark/rewind 在这里做一次就够：DOM、中间的二进制、
// read_archive 自己的三张临时表全在同一块 arena 上，而它们只需要活着到
// read_archive 返回。少了这一步，"实例化 100 次 JSON 预制体"就是 100 份文件大小 ——
// 前 99 次都不会失败，所以这种泄漏只能靠纪律而不是靠运气。
static bool scene_json_read_any(scene_t *scene, const char *text, usize length,
                                scene_io_stats_t *out_stats, entity_t *out_root)
{
    ASSERT(scene != NULL);
    if (out_root != NULL) {
        *out_root = ENTITY_NONE;
    }
    arena_t *scratch = scene_scratch_arena(scene);
    if (scratch == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法读 JSON 存档");
        return false;
    }
    const arena_mark_t mark = arena_mark(scratch);

    const json_value_t *doc = NULL;
    const char *error = NULL;
    usize offset = 0u;
    bool ok = json_parse(scratch, text, length, &doc, &error, &offset);
    if (!ok) {
        LOG_ERROR("JSON 存档解析失败：%s（第 %llu 字节）", (error != NULL) ? error : "未知原因",
                  (unsigned long long)offset);
    }

    scene_io_stats_t mine;
    memset(&mine, 0, sizeof(mine));
    void *bytes = NULL;
    usize size = 0u;
    if (ok) {
        ok = scene_json_decode_text(scene, doc, scratch, &bytes, &size, &mine);
    }
    scene_io_stats_t inner;
    memset(&inner, 0, sizeof(inner));
    if (ok) {
        ok = scene_io_read_archive(scene, bytes, size, &inner, out_root);
    }
    arena_rewind(scratch, mark);

    if (!ok) {
        return false;
    }
    if (out_stats != NULL) {
        // 两份计数相加：那两类"版本漂移"是在**转码层**发现的（没有字段表就读不出
        // 字节，走不到 read_archive 的计数器），其余在 read_archive 里。
        *out_stats = inner;
        out_stats->unknown_component_types += mine.unknown_component_types;
        out_stats->schema_mismatches += mine.schema_mismatches;
        out_stats->dangling_refs += mine.dangling_refs;
    }
    return true;
}

bool scene_read_json(scene_t *scene, const char *text, usize length, scene_io_stats_t *out_stats)
{
    return scene_json_read_any(scene, text, length, out_stats, NULL);
}

bool scene_read_json_root(scene_t *scene, const char *text, usize length,
                          scene_io_stats_t *out_stats, entity_t *out_root)
{
    return scene_json_read_any(scene, text, length, out_stats, out_root);
}

bool scene_save_json(scene_t *scene, const char *path, scene_io_stats_t *out_stats)
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
    void *text = NULL;
    usize length = 0u;
    const bool ok = scene_write_json(scene, scratch, &text, &length, out_stats) &&
                    scene_io_write_file(text, length, path, "场景 JSON");
    arena_rewind(scratch, mark);
    return ok;
}

bool scene_save_prefab_json(scene_t *scene, entity_t root, const char *path,
                            scene_io_stats_t *out_stats)
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
    void *text = NULL;
    usize length = 0u;
    const bool ok =
        scene_write_prefab_json(scene, root, scratch, &text, &length, out_stats) &&
        scene_io_write_file(text, length, path, "预制体 JSON");
    arena_rewind(scratch, mark);
    return ok;
}

bool scene_load_json(scene_t *scene, const char *path, scene_io_stats_t *out_stats)
{
    arena_t *arena = scene_scratch_arena(scene);
    if (arena == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法读 JSON 存档");
        return false;
    }
    // 文件缓冲与转码临时内存同生共死，所以 mark 打在这里、rewind 也在这里。
    const arena_mark_t mark = arena_mark(arena);
    void *raw = NULL;
    usize size = 0u;
    const bool read = scene_io_read_file(path, arena, &raw, &size);
    const char *text = (const char *)raw;
    if (read && size >= 3u) {
        // JSON 文件可以是 UTF-8 with BOM（Windows 记事本"另存为 UTF-8"就这么干）。
        // 不剥掉的话症状是"第一行就解析失败"，而那看起来像文件坏了而不是编码问题。
        const u8 *b = (const u8 *)text;
        if (b[0] == 0xEFu && b[1] == 0xBBu && b[2] == 0xBFu) {
            text += 3u;
            size -= 3u;
        }
    }
    const bool ok = read && scene_json_read_any(scene, text, size, out_stats, NULL);
    arena_rewind(arena, mark);
    return ok;
}

// ============================================================ 两种编码互转

bool scene_json_from_binary(scene_t *scene, const void *bytes, usize size, arena_t *arena,
                            void **out_text, usize *out_length)
{
    ASSERT(scene != NULL && arena != NULL);
    if (out_text != NULL) {
        *out_text = NULL;
    }
    if (out_length != NULL) {
        *out_length = 0u;
    }
    return scene_json_encode_text(scene, bytes, size, arena, out_text, out_length);
}

bool scene_binary_from_json(scene_t *scene, const char *text, usize length, arena_t *arena,
                            void **out_bytes, usize *out_size)
{
    ASSERT(scene != NULL && arena != NULL);
    if (out_bytes != NULL) {
        *out_bytes = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0u;
    }

    const json_value_t *doc = NULL;
    const char *error = NULL;
    usize offset = 0u;
    if (!json_parse(arena, text, length, &doc, &error, &offset)) {
        LOG_ERROR("JSON 存档解析失败：%s（第 %llu 字节）", (error != NULL) ? error : "未知原因",
                  (unsigned long long)offset);
        return false;
    }
    // 这个方向**不**经过 read_archive，所以坏引用、资源表溢出那类"数据级"的问题
    // 不会在这里报（也不该报：转码不建实体）。它只报文档级与漂移级的问题。
    // 认不出的类型在这里就必须丢掉 —— 没有字段表连字节都摆不出来，
    // 而丢掉会记在日志里（LOG_WARN），不会静默。
    scene_io_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    return scene_json_decode_text(scene, doc, arena, out_bytes, out_size, &stats);
}

// ============================================================ 按扩展名挑格式

bool scene_path_is_json(const char *path)
{
    static const char k_ext[] = ".json";
    if (path == NULL) {
        return false;
    }
    const usize n = strlen(path);
    const usize want = sizeof(k_ext) - 1u;
    if (n < want) {
        return false;
    }
    for (usize i = 0u; i < want; ++i) {
        char c = path[n - want + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != k_ext[i]) {
            return false;
        }
    }
    return true;
}

bool scene_load_auto(scene_t *scene, const char *path, scene_io_stats_t *out_stats)
{
    // 先看**内容**再看后缀：把 .scene 后缀的 JSON 文件按二进制读，症状是
    // "魔数不对"（还算清楚），反过来把 .json 的二进制当 JSON 解析，症状是
    // "第 1 字节语法错误" —— 没人会从那条报错想到"文件名起错了"。
    // 探测失败（文件打不开、不足 4 字节）就退回按后缀判，让 fopen 的失败
    // 由下游那一个入口去报，错误信息只有一份。
    FILE *probe = (path != NULL) ? fopen(path, "rb") : NULL;
    if (probe != NULL) {
        u8 head[4] = {0u, 0u, 0u, 0u};
        const size_t got = fread(head, 1u, sizeof(head), probe);
        fclose(probe);
        if (got == sizeof(head)) {
            // 二进制的魔数 0x4E454353 按小端落字节 = 'S','C','E','N'。
            const bool binary = (head[0] == 'S' && head[1] == 'C' && head[2] == 'E' &&
                                 head[3] == 'N');
            return binary ? scene_load_binary(scene, path, out_stats)
                          : scene_load_json(scene, path, out_stats);
        }
    }
    return scene_path_is_json(path) ? scene_load_json(scene, path, out_stats)
                                    : scene_load_binary(scene, path, out_stats);
}
