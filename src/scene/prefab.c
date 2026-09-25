// =============================================================================
//  src/scene/prefab.c —— 预制体
//
//  设计取舍都写在 prefab.h 的文件头。这个文件里只有"持有字节"和"交出字节"两件事。
// =============================================================================
#include "engine/scene/prefab.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"

struct prefab_t {
    allocator_t *alloc;
    u8          *bytes;
    usize        size;
    u32          entity_count; // 头部里那个数，只为"查"服务，不参与任何判断
};

static prefab_t *prefab_alloc_empty(allocator_t *alloc, usize size)
{
    if (alloc == NULL) {
        LOG_ERROR("预制体需要 allocator（它自己持有那份字节）");
        return NULL;
    }
    if (size < SCENE_FILE_HEADER_SIZE) {
        LOG_ERROR("预制体字节太短：%llu 字节", (unsigned long long)size);
        return NULL;
    }

    prefab_t *p = (prefab_t *)alloc->alloc(alloc, sizeof(*p), 16);
    if (p == NULL) {
        LOG_ERROR("预制体结构分配失败");
        return NULL;
    }
    p->bytes = (u8 *)alloc->alloc(alloc, size, 16);
    if (p->bytes == NULL) {
        alloc->free(alloc, p, sizeof(*p));
        LOG_ERROR("预制体字节分配失败（%llu 字节）", (unsigned long long)size);
        return NULL;
    }
    p->alloc = alloc;
    p->size = size;
    p->entity_count = 0u;
    return p;
}

// 头部只 peek 不校验：校验只有一处，就是真正要用的时候（prefab.h 里说明了为什么）。
static void prefab_fill_counts(prefab_t *p)
{
    u32 entities = 0u, kind = 0u;
    if (!scene_archive_peek(p->bytes, p->size, &entities, &kind)) {
        LOG_WARN("预制体头部读不出实体数（这份字节多半有问题，实例化时会明确报错）");
        return;
    }
    if (kind != (u32)SCENE_ARCHIVE_PREFAB) {
        LOG_WARN("这份存档不是预制体（kind=%u）：整场景存档也可以实例化，"
                 "但它会连带环境设置与全部资源",
                 (unsigned)kind);
    }
    p->entity_count = entities;
}

prefab_t *prefab_create(allocator_t *alloc, const void *bytes, usize size)
{
    if (bytes == NULL) {
        LOG_ERROR("prefab_create 拿到的字节是空的");
        return NULL;
    }
    prefab_t *p = prefab_alloc_empty(alloc, size);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p->bytes, bytes, size);
    prefab_fill_counts(p);
    return p;
}

prefab_t *prefab_load(allocator_t *alloc, const char *path)
{
    if (path == NULL) {
        LOG_ERROR("prefab_load 没有路径");
        return NULL;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        LOG_ERROR("无法打开预制体文件: %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        LOG_ERROR("预制体文件无法定位到末尾: %s", path);
        fclose(file);
        return NULL;
    }
    const long length = ftell(file);
    if (length <= 0) {
        LOG_ERROR("预制体文件为空: %s", path);
        fclose(file);
        return NULL;
    }
    rewind(file);

    prefab_t *p = prefab_alloc_empty(alloc, (usize)length);
    if (p == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(p->bytes, 1u, (usize)length, file) != (usize)length) {
        LOG_ERROR("预制体文件读取不完整: %s", path);
        prefab_destroy(p);
        fclose(file);
        return NULL;
    }
    fclose(file);

    prefab_fill_counts(p);
    return p;
}

prefab_t *prefab_from_subtree(scene_t *scene, entity_t root, allocator_t *alloc,
                              scene_io_stats_t *out_stats)
{
    if (scene == NULL) {
        LOG_ERROR("prefab_from_subtree 需要一个场景");
        return NULL;
    }

    // 中间缓冲走场景的 scratch，用完立刻丢掉：prefab 自己留了一份副本，
    // 所以这里没有任何东西需要在这之后还指向那块 arena。
    arena_t *scratch = scene_scratch_arena(scene);
    if (scratch == NULL) {
        LOG_ERROR("场景缺少 scratch arena，无法导出预制体");
        return NULL;
    }
    const arena_mark_t mark = arena_mark(scratch);
    void *bytes = NULL;
    usize size = 0u;
    prefab_t *prefab = NULL;
    if (scene_write_prefab(scene, root, scratch, &bytes, &size, out_stats)) {
        prefab = prefab_create(alloc, bytes, size);
    }
    arena_rewind(scratch, mark);
    return prefab;
}

void prefab_destroy(prefab_t *prefab)
{
    if (prefab == NULL) {
        return;
    }
    // 顺序有讲究：先取回 alloc 和两块内存各自的大小，再放开。
    // pool/arena 系的分配器都要求 free 时给出当初 alloc 的那个 size，
    // 而结构体一旦被释放，里面就没有可信的字段可读第二次的了。
    allocator_t *alloc = prefab->alloc;
    const usize size = prefab->size;
    u8 *bytes = prefab->bytes;
    prefab->bytes = NULL;
    prefab->size = 0u;
    alloc->free(alloc, bytes, size);
    alloc->free(alloc, prefab, sizeof(*prefab));
}

bool prefab_instantiate(const prefab_t *prefab, scene_t *target, entity_t parent,
                        entity_t *out_root, scene_io_stats_t *out_stats)
{
    if (out_root != NULL) {
        *out_root = ENTITY_NONE;
    }
    if (prefab == NULL || target == NULL) {
        LOG_ERROR("prefab_instantiate 参数不完整");
        return false;
    }

    scene_io_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    entity_t root = ENTITY_NONE;
    if (!scene_read_binary_root(target, prefab->bytes, prefab->size, &stats, &root)) {
        return false; // 为什么不行，scene_read_binary_root 已经按段说清楚了
    }

    // 挂到父节点：预制体的根成为它的一个子节点，本地变换按存档原样保留
    // （本地变换本来就是相对父节点的 —— 这是"2D/3D 共用同一份 transform"
    // 换来的第二个好处：子树整体搬走不需要重算任何矩阵）。
    bool attached = true;
    if (entity_is_valid(parent)) {
        attached = ecs_set_parent(scene_world(target), root, parent);
        if (!attached) {
            LOG_ERROR("预制体挂到指定的父实体下失败（成环、超深度，或那个实体已经失效了）");
        }
    }
    if (out_root != NULL) {
        *out_root = root;
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    if (!attached) {
        // **不回滚**。回滚要把这一批实体按 dense 找回去、还要处理它们之间已经
        // 挂好的父子关系，那套代码出错的概率比它留下的问题更大。
        // 所以这里把根交回给调用方（*out_root 仍然有效）并明确返回 false：
        // 要不要销毁这一批，是调用方的决定，而且它手里正好有根实体。
        return false;
    }
    return true;
}

usize prefab_byte_size(const prefab_t *prefab)
{
    return (prefab != NULL) ? prefab->size : 0u;
}

u32 prefab_entity_count(const prefab_t *prefab)
{
    return (prefab != NULL) ? prefab->entity_count : 0u;
}
