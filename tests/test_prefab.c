// =============================================================================
//  tests/test_prefab.c —— 预制体
//
//  断言围绕"预制体会不会带走不该带的东西、会不会漏掉该带的东西"来挑：
//  子树边界、资源边界、引用边界、以及重复实例化的内存增长。
//  往返一致那一套已经在 test_scene_io.c 里验过了（同一个编码器），这里不重复。
//
//  全程离线场景（scene_create 不给 renderer）。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/mat.h"
#include "engine/core/memory.h"
#include "engine/ecs/system.h" // ecs_world_update 的声明在这里，不在 world.h
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"
#include "engine/render/mesh.h"
#include "engine/render/texture.h"
#include "engine/scene/prefab.h"
#include "engine/scene/scene_io.h"

// 100 次实例化 × 3 个实体 = 300，所以容量要留出这个数。
#define PF_TEST_ENTS 512u

typedef struct prefab_fixture_t {
    allocator_tracking_t tracker;
    scene_t             *scene;
    scene_component_ids_t ids;
} prefab_fixture_t;

// register_pbr 故意做成参数：预制体的资源依赖（管线按名字找）需要一个
// "什么都没登记的场景"才能测，而这种场景用默认 fixture 造不出来。
static void pf_fixture_begin(prefab_fixture_t *fx, const char *name, bool register_pbr)
{
    memset(fx, 0, sizeof(*fx));
    allocator_tracking_init(&fx->tracker, allocator_heap());

    scene_config_t config;
    memset(&config, 0, sizeof(config));
    config.allocator = &fx->tracker.base;
    config.renderer = NULL;
    config.name = name;
    config.entity_capacity = PF_TEST_ENTS;
    config.resource_capacity_per_type = PF_TEST_ENTS;

    fx->scene = scene_create(&config);
    EXPECT_NOT_NULL(fx->scene);
    if (fx->scene == NULL) {
        return;
    }
    EXPECT_TRUE(scene_register_components(fx->scene, &fx->ids));
    if (register_pbr) {
        EXPECT_TRUE(scene_register_pipeline(fx->scene, "pbr", (pipeline_handle_t)11));
    }
}

static void pf_fixture_end(prefab_fixture_t *fx)
{
    if (fx->scene != NULL) {
        scene_destroy(fx->scene);
        fx->scene = NULL;
    }
    EXPECT_FALSE(allocator_tracking_has_leak(&fx->tracker));
}

// 一个带名字（可选）和网格渲染器（可选）的实体。
static entity_t pf_entity(prefab_fixture_t *fx, const char *name, vec3_t position, u32 mesh,
                          u32 material)
{
    ecs_world_t *world = scene_world(fx->scene);
    const entity_t e = ecs_create_entity(world);
    EXPECT_TRUE(ecs_transform_set_local(world, e, position, quat_identity(), vec3_one()));
    if (name != NULL) {
        scene_name_t nm;
        memset(&nm, 0, sizeof(nm));
        snprintf(nm.text, sizeof(nm.text), "%s", name);
        EXPECT_NOT_NULL(ecs_add(world, e, fx->ids.name, &nm));
    }
    if (mesh != (u32)SCENE_RESOURCE_NONE) {
        scene_mesh_renderer_t mr;
        memset(&mr, 0, sizeof(mr));
        mr.mesh = mesh;
        mr.material = material;
        mr.layer = (u32)RENDER_LAYER_OPAQUE_3D;
        EXPECT_NOT_NULL(ecs_add(world, e, fx->ids.mesh_renderer, &mr));
    }
    return e;
}

typedef struct pf_world_t {
    entity_t root, a, b, other;
    u32      mesh_turret, mesh_crate, mesh_tree;
    u32      mat_red, mat_green;
} pf_world_t;

// 一棵三节点的树 root -> a -> b，加一棵无关的 other。
// 三条链用**不同**的资源，这样"带走了谁"能从目标场景的资源表里数出来。
static void pf_build(prefab_fixture_t *fx, pf_world_t *w)
{
    memset(w, 0, sizeof(*w));
    EXPECT_TRUE(scene_add_mesh_handle(fx->scene, "turret", MESH_HANDLE_INVALID, &w->mesh_turret));
    EXPECT_TRUE(scene_add_mesh_handle(fx->scene, "crate", MESH_HANDLE_INVALID, &w->mesh_crate));
    EXPECT_TRUE(scene_add_mesh_handle(fx->scene, "tree", MESH_HANDLE_INVALID, &w->mesh_tree));

    material_t m = material_default();
    m.tint = vec4_make(0.9f, 0.1f, 0.1f, 1.0f);
    EXPECT_TRUE(scene_add_material(fx->scene, "red", "pbr", &m, &w->mat_red));
    EXPECT_TRUE(scene_add_material(fx->scene, "green", "pbr", &m, &w->mat_green));

    w->root = pf_entity(fx, "root", vec3_make(1.0f, 0.0f, 0.0f), w->mesh_turret, w->mat_red);
    w->a = pf_entity(fx, "a", vec3_make(0.0f, 2.0f, 0.0f), (u32)SCENE_RESOURCE_NONE,
                     (u32)SCENE_RESOURCE_NONE);
    w->b = pf_entity(fx, "b", vec3_make(0.0f, 0.0f, 3.0f), w->mesh_turret, w->mat_red);
    w->other = pf_entity(fx, "other", vec3_make(9.0f, 0.0f, 0.0f), w->mesh_tree, w->mat_green);

    EXPECT_TRUE(ecs_set_parent(scene_world(fx->scene), w->a, w->root));
    EXPECT_TRUE(ecs_set_parent(scene_world(fx->scene), w->b, w->a));
    // other 故意不挂父节点：它是一棵独立的树，不该出现在 root 的预制体里
}

// 实例化一份预制体，失败时返回 ENTITY_NONE（调用方负责断言）。
static entity_t pf_instantiate(const prefab_t *p, prefab_fixture_t *dst, entity_t parent,
                               scene_io_stats_t *stats)
{
    entity_t root = ENTITY_NONE;
    if (!prefab_instantiate(p, dst->scene, parent, &root, stats)) {
        return ENTITY_NONE;
    }
    return root;
}

// ---------------------------------------------------------------- 边界

static void prefab_covers_only_the_subtree(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_sub_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);
    if (p == NULL) {
        pf_fixture_end(&src);
        return;
    }
    EXPECT_EQ_U64(prefab_entity_count(p), 3u); // root + a + b，不含 other
    EXPECT_GT_U64(prefab_byte_size(p), SCENE_FILE_HEADER_SIZE);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_sub_dst", true);
    scene_io_stats_t stats;
    const entity_t root = pf_instantiate(p, &dst, ENTITY_NONE, &stats);
    EXPECT_TRUE(entity_is_valid(root));
    EXPECT_EQ_U64(stats.entities_loaded, 3u);

    ecs_world_t *world = scene_world(dst.scene);
    entity_t ents[16];
    const u32 n = ecs_collect_entities(world, ents, 16u);
    EXPECT_EQ_U64(n, 3u);

    bool saw_other = false, saw_root_name = false;
    for (u32 i = 0; i < n; ++i) {
        const scene_name_t *nm = (const scene_name_t *)ecs_peek(world, ents[i], dst.ids.name);
        if (nm != NULL) {
            saw_root_name |= (strncmp(nm->text, "root", SCENE_NAME_MAX) == 0);
            saw_other |= (strncmp(nm->text, "other", SCENE_NAME_MAX) == 0);
        }
    }
    EXPECT_TRUE(saw_root_name);
    EXPECT_FALSE(saw_other);

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// 只带被引用的资源。这条是"预制体会不会把整张资源表搬过去"的唯一防线 ——
// 真搬过去的话症状不是报错，而是目标场景的资源表被灌满、第三次实例化开始失败。
static void prefab_carries_only_referenced_resources(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_res_src", true);
    pf_world_t w;
    pf_build(&src, &w);
    EXPECT_EQ_U64(scene_mesh_count(src.scene), 3u);
    EXPECT_EQ_U64(scene_material_count(src.scene), 2u);

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_res_dst", true);
    scene_io_stats_t stats;
    EXPECT_TRUE(entity_is_valid(pf_instantiate(p, &dst, ENTITY_NONE, &stats)));

    // 树里只引用 turret / red：crate、green、tree 都不该跟过来
    EXPECT_EQ_U64(scene_mesh_count(dst.scene), 1u);
    EXPECT_EQ_U64(scene_material_count(dst.scene), 1u);
    EXPECT_TRUE(scene_find_mesh(dst.scene, "turret") != (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(scene_find_mesh(dst.scene, "crate") == (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(scene_find_mesh(dst.scene, "tree") == (u32)SCENE_RESOURCE_NONE);
    EXPECT_EQ_U64(stats.dangling_refs, 0u); // 该带的都带上了，所以没有落空的引用

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// 子树外的引用一律变 NONE：预制体不许偷偷依赖外面的实体。
static void prefab_drops_references_outside_the_subtree(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_out_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    // 给 root 挂一个在子树**外面**的父节点
    const entity_t outside =
        pf_entity(&src, "parent_of_root", vec3_make(0.0f, 100.0f, 0.0f),
                  (u32)SCENE_RESOURCE_NONE, (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(ecs_set_parent(scene_world(src.scene), w.root, outside));

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_out_dst", true);
    scene_io_stats_t stats;
    const entity_t root = pf_instantiate(p, &dst, ENTITY_NONE, &stats);
    EXPECT_TRUE(entity_is_valid(root));
    EXPECT_EQ_U64(stats.entities_loaded, 3u);

    ecs_world_t *world = scene_world(dst.scene);
    EXPECT_FALSE(entity_is_valid(ecs_get_parent(world, root))); // 外面那个父节点没跟过来

    // 树内的父子关系仍在，而且指的就是这一次实例化造出来的实体
    entity_t ents[16];
    const u32 n = ecs_collect_entities(world, ents, 16u);
    u32 children = 0u, grandchildren = 0u;
    for (u32 i = 0; i < n; ++i) {
        const entity_t p1 = ecs_get_parent(world, ents[i]);
        if (p1 == root) {
            children++;
        } else if (entity_is_valid(p1) && ecs_get_parent(world, p1) == root) {
            grandchildren++;
        }
    }
    EXPECT_EQ_U64(children, 1u);
    EXPECT_EQ_U64(grandchildren, 1u);

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// ---------------------------------------------------------------- 用

// 实例化 100 次：实体数线性增长、资源表不涨、scratch arena 也不涨。
// arena 那一条是这条测试存在的另一半理由 —— 少了读档入口的 mark/rewind，
// 前 99 次都不会失败，第 100 次才会以"分配失败"的面目出现。
static void prefab_instantiate_many_times(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_many_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_many_dst", true);
    arena_t *scratch = scene_scratch_arena(dst.scene);
    const usize scratch_before = arena_bytes_used(scratch);

    for (u32 i = 0u; i < 100u; ++i) {
        const entity_t root = pf_instantiate(p, &dst, ENTITY_NONE, NULL);
        if (!entity_is_valid(root)) {
            TF_CHECK(false, "第 %u 次实例化失败", (unsigned)i);
            break;
        }
    }

    EXPECT_EQ_U64(ecs_collect_entities(scene_world(dst.scene), NULL, 0u), 300u);
    EXPECT_EQ_U64(scene_mesh_count(dst.scene), 1u); // 按名字去重，不是每次加一份
    EXPECT_EQ_U64(scene_material_count(dst.scene), 1u);

    const usize scratch_after = arena_bytes_used(scratch);
    if (scratch_after >= scratch_before + 4096u) {
        TF_CHECK(false, "scratch arena 每次实例化都在涨：%llu -> %llu",
                 (unsigned long long)scratch_before, (unsigned long long)scratch_after);
    }

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// 挂到指定父节点下，世界矩阵按父节点算。
static void prefab_attaches_under_a_parent(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_at_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_at_dst", true);
    const entity_t carrier =
        pf_entity(&dst, "carrier", vec3_make(10.0f, 0.0f, 0.0f), (u32)SCENE_RESOURCE_NONE,
                  (u32)SCENE_RESOURCE_NONE);

    ecs_world_t *world = scene_world(dst.scene);
    const entity_t root = pf_instantiate(p, &dst, carrier, NULL);
    EXPECT_TRUE(entity_is_valid(root));
    EXPECT_TRUE(ecs_get_parent(world, root) == carrier);
    EXPECT_EQ_U64(ecs_hierarchy_depth(world, root), 1u);

    // 世界矩阵是派生数据：实例化本身不算它（也没人算），跑一次变换系统才有。
    EXPECT_TRUE(ecs_rebuild_hierarchy(world));
    ecs_update_world_transforms(world);

    // root 的本地位置是 (1,0,0)，父节点在 (10,0,0) → 世界位置 (11,0,0)。
    // 这一条同时证明了"子树整体搬走不需要重算任何本地变换"。
    ecs_world_transform_t wt;
    EXPECT_TRUE(ecs_transform_get_world(world, root, &wt));
    const vec3_t at = mat4_get_translation(wt.model);
    EXPECT_NEAR((double)at.x, 11.0, 1e-5);
    EXPECT_NEAR((double)at.y, 0.0, 1e-5);

    // b 在链条上第四层：carrier(0) -> root(1) -> a(2) -> b(3)
    EXPECT_GE_U64(ecs_hierarchy_depth_max_seen(world), 3u);

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// 目标场景没登记那个管线：实例化必须失败，而且失败点要说在材质上，
// 不是等到"画面全黑"才被发现。
static void prefab_needs_pipelines_in_the_target(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_pipe_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    prefab_t *p = prefab_from_subtree(src.scene, w.root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_pipe_dst", false); // 故意不登记 "pbr"

    // 这条路径必然打日志（那就是它的主要产出），所以只在断言期间静音，出来还原。
    const log_level_t saved = log_get_level();
    log_set_level(LOG_LEVEL_FATAL);
    entity_t root = ENTITY_NONE;
    const bool ok = prefab_instantiate(p, dst.scene, ENTITY_NONE, &root, NULL);
    log_set_level(saved);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(entity_is_valid(root));

    // 补登记之后再实例化就该成功：失败不是"这份预制体坏了"，是缺依赖
    EXPECT_TRUE(scene_register_pipeline(dst.scene, "pbr", (pipeline_handle_t)11));
    root = pf_instantiate(p, &dst, ENTITY_NONE, NULL);
    EXPECT_TRUE(entity_is_valid(root));

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// 文件形式：导出一份 .prefab 再读回来，实例化结果和内存版一致。
// 验收里那句"改动模板不影响已实例化的"此前没有任何断言。
//
// 而且这条测试第一次跑就抓到一个真缺陷：第二次实例化出来的实体拿不到世界变换,
// 因为 `ecs_create_entity` 只标 dense_dirty 不标 hierarchy_dirty ——
// 新实体进不了拓扑快照，于是"建成功了、组件都在、就是看不见"。
// 根因的回归测试在 tests/test_ecs.c 的 entity_created_after_update_gets_world_transform,
// 这条留着是因为它是用户能碰到的那个形态：单实体预制体实例化两次。
static void prefab_is_a_snapshot_not_a_window(void)
{
    prefab_fixture_t src, dst;
    pf_fixture_begin(&src, "pf_snap_src", true);
    pf_fixture_begin(&dst, "pf_snap_dst", true);

    const entity_t root = pf_entity(&src, "snap_root", vec3_make(3.0f, 1.0f, 0.0f),
                                    SCENE_RESOURCE_NONE, SCENE_RESOURCE_NONE);
    EXPECT_TRUE(entity_is_valid(root));

    prefab_t *p = prefab_from_subtree(src.scene, root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p);
    const usize snapshot_size = prefab_byte_size(p);
    EXPECT_GT_U64(snapshot_size, SCENE_FILE_HEADER_SIZE);

    const entity_t first = pf_instantiate(p, &dst, ENTITY_NONE, NULL);
    EXPECT_TRUE(entity_is_valid(first));
    (void)ecs_world_update(scene_world(dst.scene), 0.0f);
    ecs_world_transform_t wt_first;
    EXPECT_TRUE(ecs_transform_get_world(scene_world(dst.scene), first, &wt_first));
    EXPECT_NEAR((double)wt_first.model.col[3].x, 3.0, 1e-4);

    // 改模板：根挪远，并且往源场景再添一个实体（相当于"编辑器里又改了一版"）
    EXPECT_TRUE(ecs_transform_set_local(scene_world(src.scene), root,
                                        vec3_make(77.0f, 77.0f, 77.0f), quat_identity(),
                                        vec3_splat(3.0f)));
    (void)pf_entity(&src, "snap_extra", vec3_make(0.0f, 5.0f, 0.0f), SCENE_RESOURCE_NONE,
                    SCENE_RESOURCE_NONE);

    // 已经拿到的那份预制体字节不许因为源场景被改而变（它是快照，不是窗口）
    EXPECT_EQ_U64(prefab_byte_size(p), snapshot_size);

    // 同一个预制体再实例化：位置必须与第一次一致（不受源场景改动影响）
    const entity_t second = pf_instantiate(p, &dst, ENTITY_NONE, NULL);
    EXPECT_TRUE(entity_is_valid(second));
    (void)ecs_world_update(scene_world(dst.scene), 0.0f);
    ecs_world_transform_t wt_second;
    EXPECT_TRUE(ecs_transform_get_world(scene_world(dst.scene), second, &wt_second));
    EXPECT_NEAR((double)wt_second.model.col[3].x, (double)wt_first.model.col[3].x, 1e-4);
    EXPECT_NEAR((double)wt_second.model.col[3].y, (double)wt_first.model.col[3].y, 1e-4);

    // 第一个实例也不漂 —— 这才是"不影响已实例化的"那半句
    ecs_world_transform_t wt_first_again;
    EXPECT_TRUE(ecs_transform_get_world(scene_world(dst.scene), first, &wt_first_again));
    EXPECT_NEAR((double)wt_first_again.model.col[3].x, (double)wt_first.model.col[3].x, 1e-4);
    EXPECT_NEAR((double)wt_first_again.model.col[3].z, (double)wt_first.model.col[3].z, 1e-4);

    // 反过来也要成立：重新导出的那一份**必须**带上改动。
    // 没有这一条的话，上面的"没变"可能只是因为源场景的改动被吞掉了 —— 那是另一个 bug。
    prefab_t *p2 = prefab_from_subtree(src.scene, root, &src.tracker.base, NULL);
    EXPECT_NOT_NULL(p2);
    const entity_t third = pf_instantiate(p2, &dst, ENTITY_NONE, NULL);
    EXPECT_TRUE(entity_is_valid(third));
    (void)ecs_world_update(scene_world(dst.scene), 0.0f);
    ecs_world_transform_t wt_third;
    EXPECT_TRUE(ecs_transform_get_world(scene_world(dst.scene), third, &wt_third));
    EXPECT_TRUE(wt_third.model.col[3].x > 50.0f);
    EXPECT_EQ_U64(scene_subtree_size(dst.scene, third), 1u);

    prefab_destroy(p2);
    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

static void prefab_file_roundtrip(void)
{
    prefab_fixture_t src;
    pf_fixture_begin(&src, "pf_file_src", true);
    pf_world_t w;
    pf_build(&src, &w);

    const char *path = "prefab_file_test.tmp.bin";
    scene_io_stats_t wstats;
    EXPECT_TRUE(scene_save_prefab(src.scene, w.root, path, &wstats));
    EXPECT_EQ_U64(wstats.entities_written, 3u);

    prefab_t *p = prefab_load(&src.tracker.base, path);
    EXPECT_NOT_NULL(p);
    EXPECT_EQ_U64(prefab_entity_count(p), 3u);

    prefab_fixture_t dst;
    pf_fixture_begin(&dst, "pf_file_dst", true);
    scene_io_stats_t stats;
    EXPECT_TRUE(entity_is_valid(pf_instantiate(p, &dst, ENTITY_NONE, &stats)));
    EXPECT_EQ_U64(stats.entities_loaded, 3u);
    EXPECT_EQ_U64(scene_mesh_count(dst.scene), 1u);

    remove(path); // 临时文件用完就删，别把"仓库干净"变成要人记着做的事
    EXPECT_NULL(prefab_load(&src.tracker.base, "prefab_no_such_file.tmp.bin"));
    EXPECT_NULL(prefab_load(&src.tracker.base, path)); // 删掉的同名文件也要是 NULL
    EXPECT_NULL(prefab_create(&src.tracker.base, NULL, 0u));

    prefab_destroy(p);
    pf_fixture_end(&dst);
    pf_fixture_end(&src);
}

// =============================================================================
void test_prefab_register(void)
{
    test_register("prefab", "covers_only_the_subtree", prefab_covers_only_the_subtree);
    test_register("prefab", "carries_only_referenced_resources",
                  prefab_carries_only_referenced_resources);
    test_register("prefab", "drops_references_outside_the_subtree",
                  prefab_drops_references_outside_the_subtree);
    test_register("prefab", "instantiate_many_times", prefab_instantiate_many_times);
    test_register("prefab", "attaches_under_a_parent", prefab_attaches_under_a_parent);
    test_register("prefab", "needs_pipelines_in_the_target", prefab_needs_pipelines_in_the_target);
    test_register("prefab", "file_roundtrip", prefab_file_roundtrip);
    test_register("prefab", "is_a_snapshot_not_a_window", prefab_is_a_snapshot_not_a_window);
}
