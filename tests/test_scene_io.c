// =============================================================================
//  tests/test_scene_io.c —— 场景二进制存档
//
//  核心判据是**跨场景往返逐字节一致**：
//      场景 A --write--> 字节 X --read--> 场景 B --write--> 字节 Y
//      断言 X 与 Y 完全相同。
//
//  为什么这条比"存了能读回来"强得多：它一次性要求三件事同时成立 ——
//   * 写出顺序与内存布局无关（按 dense id、按类型 id），否则第二次写出就错位；
//   * 浮点可精确往返，否则每过一轮存档就漂一点；
//   * dense id 与资源名字的翻译没接错 —— 接错了第二次写出的字节会变。
//  只验"能读回来"的话，上面任何一条错了都能蒙混过关，
//  而且要等到"重启后实体顺序变了"才暴露，那时最难归因。
//
//  全程用**离线场景**（scene_create 不给 renderer）：不建窗口、不碰 GL。
//  这条测试验的是格式，不是渲染。
//
//  改坏存档时一律**按内容定位**（搜那个零填充的名字）而不是按算出来的偏移：
//  偏移写死的测试在格式加一个字段之后会全体错位，
//  而它错位之后照样"通过" —— 那是最坏的一种测试。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/log.h"
#include "engine/core/memory.h"
#include "engine/ecs/transform.h"
#include "engine/ecs/world.h"
#include "engine/render/mesh.h"
#include "engine/render/texture.h"
#include "engine/scene/scene_io.h"

#define IO_TEST_ENTS 128u

typedef struct scene_io_fixture_t {
    allocator_tracking_t tracker;
    scene_t             *scene;
    scene_component_ids_t ids;
} scene_io_fixture_t;

static void io_fixture_begin(scene_io_fixture_t *fx, const char *name)
{
    memset(fx, 0, sizeof(*fx));
    allocator_tracking_init(&fx->tracker, allocator_heap());

    scene_config_t config;
    memset(&config, 0, sizeof(config));
    config.allocator = &fx->tracker.base;
    config.renderer = NULL; // 离线：不碰 GPU
    config.name = name;
    config.entity_capacity = IO_TEST_ENTS;
    config.resource_capacity_per_type = IO_TEST_ENTS;

    fx->scene = scene_create(&config);
    EXPECT_NOT_NULL(fx->scene);
    if (fx->scene == NULL) {
        return;
    }
    EXPECT_TRUE(scene_register_components(fx->scene, &fx->ids));

    // 材质要引用管线名，所以名字得先登记。句柄在这里是假的：
    // 离线模式不校验句柄有效性（它也没有 GPU 可问）。
    EXPECT_TRUE(scene_register_pipeline(fx->scene, "pbr", (pipeline_handle_t)11));
    EXPECT_TRUE(scene_register_pipeline(fx->scene, "sprite", (pipeline_handle_t)12));
}

static void io_fixture_end(scene_io_fixture_t *fx)
{
    if (fx->scene != NULL) {
        scene_destroy(fx->scene);
        fx->scene = NULL;
    }
    EXPECT_FALSE(allocator_tracking_has_leak(&fx->tracker));
}

// 造一个有代表性的场景：父子链、七种组件里的四种、资源引用、非默认环境值。
// 覆盖面是照着"往返会因什么而失败"挑的，不是照着组件清单扫一遍。
static void io_build_scene(scene_io_fixture_t *fx)
{
    scene_t *scene = fx->scene;
    const scene_component_ids_t ids = fx->ids;
    ecs_world_t *world = scene_world(scene);

    u32 cube = SCENE_RESOURCE_NONE, ball = SCENE_RESOURCE_NONE;
    EXPECT_TRUE(scene_add_mesh_handle(scene, "cube", MESH_HANDLE_INVALID, &cube));
    EXPECT_TRUE(scene_add_mesh_handle(scene, "ball", MESH_HANDLE_INVALID, &ball));
    u32 white = SCENE_RESOURCE_NONE;
    EXPECT_TRUE(scene_add_atlas_handle(scene, "white", TEXTURE_HANDLE_INVALID, &white));

    material_t m = material_default();
    m.tint = vec4_make(0.8f, 0.3f, 0.2f, 1.0f);
    m.pbr_params = vec4_make(0.9f, 0.25f, 0.5f, 0.0f);
    u32 metal = SCENE_RESOURCE_NONE;
    EXPECT_TRUE(scene_add_material(scene, "metal", "pbr", &m, &metal));

    material_t s = material_default();
    s.tint = vec4_make(1.0f, 1.0f, 1.0f, 0.5f);
    u32 glass = SCENE_RESOURCE_NONE;
    EXPECT_TRUE(scene_add_material(scene, "glass", "sprite", &s, &glass));

    // ---- 环境：全取非默认值，这样"读回来还是这些值"才有信息量 ----
    scene_env_t *env = scene_environment(scene);
    env->clear_color = vec4_make(0.1f, 0.2f, 0.3f, 1.0f);
    env->ambient_intensity = 0.37f;
    env->fog_enabled = true;
    env->fog_start = 12.5f;
    env->fog_end = 87.25f;
    env->skybox_enabled = true;
    env->sky_zenith = vec3_make(0.11f, 0.22f, 0.33f);
    env->exposure = 1.75f;
    env->msaa_samples = 8u;
    env->shadow_map_size = 4096u;
    env->culling_enabled = false;
    env->shadows_enabled = true;
    env->shadow_strength = 0.42f;

    // ---- 根节点：一个带网格的实体 ----
    const entity_t root = ecs_create_entity(world);
    EXPECT_TRUE(ecs_transform_set_local(world, root, vec3_make(1.5f, 2.25f, -3.125f),
                                        quat_rotation_y(0.5f), vec3_make(2.0f, 2.0f, 2.0f)));
    scene_mesh_renderer_t mr;
    memset(&mr, 0, sizeof(mr));
    mr.mesh = cube;
    mr.material = metal;
    mr.layer = (u32)RENDER_LAYER_OPAQUE_3D;
    mr.cast_shadow = true;
    EXPECT_NOT_NULL(ecs_add(world, root, ids.mesh_renderer, &mr));

    scene_name_t nm;
    memset(&nm, 0, sizeof(nm));
    snprintf(nm.text, sizeof(nm.text), "%s", "root");
    EXPECT_NOT_NULL(ecs_add(world, root, ids.name, &nm));

    scene_camera_t cam;
    memset(&cam, 0, sizeof(cam));
    cam.projection = (u32)SCENE_CAMERA_PERSPECTIVE;
    cam.fov_y_degrees = 55.0f;
    cam.znear = 0.05f;
    cam.zfar = 400.0f;
    cam.active = true;
    EXPECT_NOT_NULL(ecs_add(world, root, ids.camera, &cam));

    // ---- 子节点：精灵，挂在根下面（验父子引用走 dense id）----
    const entity_t child = ecs_create_entity(world);
    EXPECT_TRUE(ecs_transform_set_local(world, child, vec3_make(-4.5f, 0.0f, 1.0f),
                                        quat_identity(), vec3_one()));
    EXPECT_TRUE(ecs_set_parent(world, child, root));

    scene_sprite_renderer_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.color = vec4_make(1.0f, 0.5f, 0.25f, 0.75f);
    sr.size = vec2_make(64.0f, 32.0f);
    sr.uv_min = vec2_make(0.25f, 0.5f);
    sr.uv_max = vec2_make(0.75f, 1.0f);
    sr.atlas = white;
    sr.layer = (u32)RENDER_LAYER_TRANSPARENT_2D;
    EXPECT_NOT_NULL(ecs_add(world, child, ids.sprite_renderer, &sr));

    const entity_t lamp = ecs_create_entity(world);
    EXPECT_TRUE(ecs_transform_set_local(world, lamp, vec3_make(0.0f, 6.0f, 0.0f), quat_identity(),
                                        vec3_one()));
    scene_light_t lt;
    memset(&lt, 0, sizeof(lt));
    lt.type = 1u; // 点光
    lt.color = vec3_make(1.0f, 0.8f, 0.6f);
    lt.intensity = 3.5f;
    lt.range = 17.25f;
    lt.cast_shadow = false;
    EXPECT_NOT_NULL(ecs_add(world, lamp, ids.light, &lt));

    // 一个只有变换的实体：验证"零额外组件的实体也能往返"（它代表一个逻辑挂点）
    const entity_t marker = ecs_create_entity(world);
    EXPECT_NOT_NULL(ecs_transform_get_or_add(world, marker));

    // 10 个可序列化组件：root 4 + child 3 + lamp 2 + marker 1。
    // 下面几个测试拿这个数字当基线，所以这里改动要同步改断言。
}

// io_build_scene 写出的组件实例数。
#define IO_BUILD_COMPONENTS 10u

// 一个"名字 + 相机"的实体，用来做跨 dense 的配对对账（见 dense_holes 那条）。
static entity_t io_named_camera(ecs_world_t *world, scene_component_ids_t ids, const char *name,
                                f32 fov)
{
    const entity_t e = ecs_create_entity(world);
    EXPECT_TRUE(ecs_transform_get_or_add(world, e) != NULL);
    scene_name_t nm;
    memset(&nm, 0, sizeof(nm));
    snprintf(nm.text, sizeof(nm.text), "%s", name);
    EXPECT_NOT_NULL(ecs_add(world, e, ids.name, &nm));
    scene_camera_t cam;
    memset(&cam, 0, sizeof(cam));
    cam.projection = (u32)SCENE_CAMERA_PERSPECTIVE;
    cam.fov_y_degrees = fov;
    cam.znear = 0.1f;
    cam.zfar = 100.0f;
    cam.active = false;
    EXPECT_NOT_NULL(ecs_add(world, e, ids.camera, &cam));
    return e;
}

static bool io_write(scene_io_fixture_t *fx, arena_t *arena, void **out_bytes, usize *out_size,
                     scene_io_stats_t *out_stats)
{
    return scene_write_binary(fx->scene, arena, out_bytes, out_size, out_stats);
}

// 按 Name 组件找实体（预制体那几条测试要用子树的根）。
static entity_t io_find_by_name(scene_io_fixture_t *fx, const char *name)
{
    ecs_world_t *world = scene_world(fx->scene);
    entity_t list[IO_TEST_ENTS];
    const u32 count = ecs_collect_entities(world, list, IO_TEST_ENTS);
    for (u32 i = 0u; i < count; ++i) {
        const scene_name_t *n = (const scene_name_t *)ecs_peek(world, list[i], fx->ids.name);
        if (n != NULL && strcmp(n->text, name) == 0) {
            return list[i];
        }
    }
    return ENTITY_NONE;
}

// 把一份文件原样复制到另一个名字下。用来造"后缀与内容互相写反"的文件：
// 自动判格式如果去看后缀，这两份就会读错，而读错的症状是看不懂的解析错误。
static bool io_copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (in == NULL) {
        return false;
    }
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    bool ok = true;
    char buf[4096];
    size_t got = 0u;
    while ((got = fread(buf, 1u, sizeof(buf), in)) > 0u) {
        if (fwrite(buf, 1u, got, out) != got) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) {
        ok = false;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------- 按内容定位

#define IO_NOT_FOUND ((usize)0xFFFFFFFFFFFFFFFFull)

// 找第 skip 次出现的"定长名字"（写出侧一律零填充到 SCENE_RESOURCE_NAME_MAX）。
static usize io_find_padded_name(const u8 *bytes, usize size, const char *name, u32 skip)
{
    u8 needle[SCENE_RESOURCE_NAME_MAX];
    memset(needle, 0, sizeof(needle));
    snprintf((char *)needle, sizeof(needle), "%s", name);

    if (size < SCENE_RESOURCE_NAME_MAX) {
        return IO_NOT_FOUND;
    }
    u32 seen = 0u;
    for (usize at = 0u; at + SCENE_RESOURCE_NAME_MAX <= size; ++at) {
        if (memcmp(bytes + at, needle, sizeof(needle)) != 0) {
            continue;
        }
        if (seen == skip) {
            return at;
        }
        seen++;
    }
    return IO_NOT_FOUND;
}

// ---------------------------------------------------------------- 坏输入的日志

// 故意喂坏数据的那几段临时静音。
// 这些路径**运行时就该吵**（所以实现里保留 LOG_ERROR 是对的），
// 但一轮截断测试能刷十几行，把测试摘要整个埋掉 —— 摘要看不清的测试套件没人跑。
// 只在这几行内静音、出来立刻还原，不改变全局行为。
typedef struct io_log_guard_t {
    log_level_t previous;
} io_log_guard_t;

static void io_quiet_logs(io_log_guard_t *guard)
{
    guard->previous = log_get_level();
    log_set_level(LOG_LEVEL_FATAL);
}

static void io_restore_logs(const io_log_guard_t *guard)
{
    log_set_level(guard->previous);
}

// ---------------------------------------------------------------- 往返

// 空场景单独一条：段计数、"零条目"的边界最容易被"有数据能过"的测试掩盖。
static void scene_io_empty_scene_roundtrips(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_empty_a");

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 256u * 1024u, "io_empty_arena"));

    void *bytes = NULL;
    usize size = 0u;
    scene_io_stats_t wstats;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, &wstats));
    EXPECT_EQ_U64(wstats.entities_written, 0u);
    EXPECT_EQ_U64(wstats.components_written, 0u);
    // 没有实体，但类型表仍在：它描述的是"这个构建认得哪些类型"，不是"用过了哪些"。
    EXPECT_GT_U64(size, SCENE_FILE_HEADER_SIZE);

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_empty_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, bytes, size, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 0u);
    EXPECT_EQ_U64(stats.components_loaded, 0u);
    EXPECT_EQ_U64(stats.unknown_component_types, 0u);
    EXPECT_EQ_U64(stats.dangling_refs, 0u);
    EXPECT_EQ_U64(ecs_collect_entities(scene_world(b.scene), NULL, 0u), 0u);

    // 默认环境值原样往返（这条尤其不该漂：它是"什么都没配"时画面的长相）
    const scene_env_t def = scene_env_default();
    const scene_env_t got = *scene_environment(b.scene);
    EXPECT_NEAR((double)got.exposure, (double)def.exposure, 0.0);
    EXPECT_NEAR((double)got.shadow_strength, (double)def.shadow_strength, 0.0);
    EXPECT_EQ_U64(got.msaa_samples, def.msaa_samples);
    EXPECT_EQ_U64(got.tonemap_mode, def.tonemap_mode);
    EXPECT_TRUE(got.fog_enabled == def.fog_enabled);
    EXPECT_VEC3_NEAR(got.sky_ground, def.sky_ground, 0.0f);

    void *again = NULL;
    usize again_size = 0u;
    EXPECT_TRUE(io_write(&b, &arena, &again, &again_size, NULL));
    EXPECT_EQ_U64(size, again_size);
    if (size == again_size) {
        EXPECT_TRUE(memcmp(bytes, again, size) == 0);
    }

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void scene_io_roundtrip_is_byte_identical(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_round_a");
    io_build_scene(&a);

    allocator_tracking_t tracker_b;
    allocator_tracking_init(&tracker_b, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker_b.base, 512u * 1024u, "io_arena"));

    void *first = NULL;
    usize first_size = 0u;
    scene_io_stats_t wstats;
    EXPECT_TRUE(io_write(&a, &arena, &first, &first_size, &wstats));
    EXPECT_GT_U64(first_size, SCENE_FILE_HEADER_SIZE);
    EXPECT_LT_U64(first_size, 64u * 1024u); // 一个演示场景不该膨胀到几十 KB
    EXPECT_EQ_U64(wstats.entities_written, 4u);
    EXPECT_EQ_U64(wstats.components_written, IO_BUILD_COMPONENTS);
    EXPECT_EQ_U64(wstats.dangling_refs, 0u); // 干净场景不该有坏引用

    // 读进一个**全新的**场景，再写一次
    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_round_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, first, first_size, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS);
    EXPECT_EQ_U64(stats.unknown_component_types, 0u);
    EXPECT_EQ_U64(stats.schema_mismatches, 0u);
    EXPECT_EQ_U64(stats.dangling_refs, 0u);
    EXPECT_EQ_U64(stats.resources_loaded, 5u); // 2 网格 + 1 图集 + 2 材质

    void *second = NULL;
    usize second_size = 0u;
    EXPECT_TRUE(io_write(&b, &arena, &second, &second_size, NULL));

    // 这才是本文件的正题：两次写出的字节必须完全相同
    EXPECT_EQ_U64(first_size, second_size);
    if (first_size == second_size) {
        if (memcmp(first, second, first_size) != 0) {
            // 只说"不相等"等于让人重新二分一遍，所以指出第一个不同的偏移。
            usize diff = 0u;
            while (diff < first_size && ((const u8 *)first)[diff] == ((const u8 *)second)[diff]) {
                diff++;
            }
            TF_CHECK(false, "两次写出不一致：首个差异偏移 %llu（0x%llX）",
                     (unsigned long long)diff, (unsigned long long)diff);
        }
    }

    // 第三次读回同一份字节仍然要成功：第二轮写出没把场景弄脏
    scene_io_stats_t stats3;
    EXPECT_TRUE(scene_read_binary(b.scene, first, first_size, &stats3));
    EXPECT_EQ_U64(stats3.entities_loaded, 4u);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker_b));
}

static void scene_io_values_survive(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_val_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_val_arena"));

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, NULL));

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_val_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, bytes, size, &stats));

    // 环境
    const scene_env_t *env = scene_environment(b.scene);
    EXPECT_NEAR((double)env->ambient_intensity, 0.37, 1e-6);
    EXPECT_TRUE(env->fog_enabled && env->skybox_enabled && !env->culling_enabled);
    EXPECT_NEAR((double)env->fog_start, 12.5, 1e-6);
    EXPECT_NEAR((double)env->sky_zenith.z, 0.33, 1e-6);
    EXPECT_NEAR((double)env->exposure, 1.75, 1e-6);
    EXPECT_EQ_U64(env->msaa_samples, 8u);
    EXPECT_EQ_U64(env->shadow_map_size, 4096u);
    EXPECT_VEC4_NEAR(env->clear_color, vec4_make(0.1f, 0.2f, 0.3f, 1.0f), 1e-6f);

    // 资源表按名字重建，所以**下标也必须一致**（组件里存的是下标）
    const u32 cube_b = scene_find_mesh(b.scene, "cube");
    const u32 ball_b = scene_find_mesh(b.scene, "ball");
    const u32 white_b = scene_find_atlas(b.scene, "white");
    const u32 metal_b = scene_find_material(b.scene, "metal");
    EXPECT_TRUE(cube_b != (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(ball_b != (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(white_b != (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(metal_b != (u32)SCENE_RESOURCE_NONE);
    EXPECT_STR_EQ(scene_material_pipeline(b.scene, metal_b), "pbr");
    const material_t *metal = scene_material(b.scene, metal_b);
    EXPECT_NOT_NULL(metal);
    if (metal != NULL) {
        EXPECT_NEAR((double)metal->pbr_params.x, 0.9, 1e-6);
        EXPECT_NEAR((double)metal->tint.y, 0.3, 1e-6);
    }

    // 实体：按 dense id 取回，检查本地变换、父子、资源引用
    ecs_world_t *world = scene_world(b.scene);
    entity_t ents[8];
    const u32 n = ecs_collect_entities(world, ents, 8u);
    EXPECT_EQ_U64(n, 4u);

    bool saw_parent = false, saw_mesh = false, saw_sprite = false, saw_light = false;
    u32 names = 0u;
    for (u32 i = 0; i < n; ++i) {
        const ecs_transform_t *t = (const ecs_transform_t *)ecs_peek(world, ents[i], b.ids.transform);
        EXPECT_NOT_NULL(t);
        if (t == NULL) {
            continue; // 没变换的实体说明 Transform 没读出来，剩下的一条条断言都是噪声
        }
        if (t->position.x > 1.0f) {
            EXPECT_NEAR((double)t->position.y, 2.25, 1e-6);
            EXPECT_NEAR((double)t->position.z, -3.125, 1e-6);
            EXPECT_NEAR((double)t->scale.x, 2.0, 1e-6);
            EXPECT_QUAT_NEAR(t->rotation, quat_rotation_y(0.5f), 1e-6f);

            const scene_mesh_renderer_t *mr =
                (const scene_mesh_renderer_t *)ecs_peek(world, ents[i], b.ids.mesh_renderer);
            EXPECT_NOT_NULL(mr);
            if (mr != NULL) {
                saw_mesh = true;
                EXPECT_EQ_U64(mr->mesh, cube_b);
                EXPECT_EQ_U64(mr->material, metal_b);
                EXPECT_TRUE(mr->cast_shadow);
                EXPECT_EQ_U64(mr->layer, (u64)RENDER_LAYER_OPAQUE_3D);
            }
            const scene_camera_t *cam = (const scene_camera_t *)ecs_peek(world, ents[i], b.ids.camera);
            EXPECT_NOT_NULL(cam);
            if (cam != NULL) {
                EXPECT_NEAR((double)cam->fov_y_degrees, 55.0, 1e-6);
                EXPECT_NEAR((double)cam->znear, 0.05, 1e-6);
                EXPECT_TRUE(cam->active);
            }
        }
        if (t->position.y > 5.0f) {
            const scene_light_t *lt = (const scene_light_t *)ecs_peek(world, ents[i], b.ids.light);
            EXPECT_NOT_NULL(lt);
            if (lt != NULL) {
                saw_light = true;
                EXPECT_EQ_U64(lt->type, 1u);
                EXPECT_NEAR((double)lt->intensity, 3.5, 1e-6);
                EXPECT_NEAR((double)lt->range, 17.25, 1e-6);
                EXPECT_FALSE(lt->cast_shadow);
            }
        }
        const scene_sprite_renderer_t *sr =
            (const scene_sprite_renderer_t *)ecs_peek(world, ents[i], b.ids.sprite_renderer);
        if (sr != NULL) {
            saw_sprite = true;
            EXPECT_EQ_U64(sr->atlas, white_b);
            EXPECT_NEAR((double)sr->size.x, 64.0, 1e-6);
            EXPECT_NEAR((double)sr->uv_max.y, 1.0, 1e-6);
            EXPECT_NEAR((double)sr->color.w, 0.75, 1e-6);
        }
        const scene_name_t *nm = (const scene_name_t *)ecs_peek(world, ents[i], b.ids.name);
        if (nm != NULL) {
            names++;
            EXPECT_STR_EQ(nm->text, "root");
        }
        const entity_t parent = ecs_get_parent(world, ents[i]);
        if (entity_is_valid(parent)) {
            saw_parent = true;
            EXPECT_TRUE(ecs_has(world, parent, b.ids.mesh_renderer)); // 父确实是有网格那个
            EXPECT_EQ_U64(ecs_hierarchy_depth(world, ents[i]), 1u);
        }
    }
    EXPECT_TRUE(saw_mesh);
    EXPECT_TRUE(saw_sprite);
    EXPECT_TRUE(saw_light);
    EXPECT_TRUE(saw_parent);
    EXPECT_EQ_U64(names, 1u);

    // 世界矩阵是派生数据：**不该**进存档。读完整份场景也不该凭空多出世界变换。
    EXPECT_EQ_U64(ecs_component_count(world, b.ids.world_transform), 0u);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// ---------------------------------------------------------------- 坏输入

// dense 空间有洞是**常态**而不是边角：实体销毁后 id 不回收（只增不减），
// 所以编辑器里存出来的场景，dense 列一定是 0,1,2,4,6 这种带洞的序列。
// 读侧必须走"记录 → 文件 dense → 新实体"这条链；拿循环下标当 dense 用，
// 组件就会挂到隔壁实体上 —— 而且不报错。
//
// 顺带钉住一条格式性质：**加载会压实 dense**。所以"逐字节一致"要分两段说 ——
// 第一次往返可能只有内容一致而 dense 列不同，从第二次起才永远逐字节一致。
static void scene_io_dense_holes_canonicalize(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_hole_a");
    io_build_scene(&a);

    ecs_world_t *wa = scene_world(a.scene);
    const entity_t cam1 = io_named_camera(wa, a.ids, "cam1", 11.0f);
    const entity_t hole = io_named_camera(wa, a.ids, "hole", 66.0f);
    const entity_t cam2 = io_named_camera(wa, a.ids, "cam2", 22.0f);
    EXPECT_TRUE(entity_is_valid(cam1) && entity_is_valid(cam2));
    EXPECT_TRUE(ecs_destroy_entity(wa, hole)); // 中间挖一个洞

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_hole_arena"));

    void *x1 = NULL;
    usize s1 = 0u;
    scene_io_stats_t w1;
    EXPECT_TRUE(io_write(&a, &arena, &x1, &s1, &w1));
    EXPECT_EQ_U64(w1.entities_written, 6u);
    EXPECT_EQ_U64(w1.components_written, 16u); // 10 + cam1 的 3 个 + cam2 的 3 个

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_hole_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, x1, s1, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 6u);
    EXPECT_EQ_U64(stats.components_loaded, 16u);
    EXPECT_EQ_U64(stats.dangling_refs, 0u);

    // 数据对账：名字和它的相机参数必须还挂在**同一个**实体上。
    // 这才是这条测试的正题 —— dense 错位的第一症状就是"配对散架"，且不报错。
    ecs_world_t *wb = scene_world(b.scene);
    entity_t ents[16];
    const u32 n = ecs_collect_entities(wb, ents, 16u);
    EXPECT_EQ_U64(n, 6u);
    u32 matched = 0u;
    for (u32 i = 0; i < n; ++i) {
        const scene_name_t *nm = (const scene_name_t *)ecs_peek(wb, ents[i], b.ids.name);
        const scene_camera_t *cam = (const scene_camera_t *)ecs_peek(wb, ents[i], b.ids.camera);
        if (nm == NULL || cam == NULL) {
            continue;
        }
        if (strncmp(nm->text, "cam1", SCENE_NAME_MAX) == 0 &&
            fabs((double)cam->fov_y_degrees - 11.0) < 1e-6) {
            matched++;
        }
        if (strncmp(nm->text, "cam2", SCENE_NAME_MAX) == 0 &&
            fabs((double)cam->fov_y_degrees - 22.0) < 1e-6) {
            matched++;
        }
        if (strncmp(nm->text, "hole", SCENE_NAME_MAX) == 0) {
            TF_CHECK(false, "被销毁的实体竟然还在");
        }
    }
    EXPECT_EQ_U64(matched, 2u);

    void *x2 = NULL;
    usize s2 = 0u;
    EXPECT_TRUE(io_write(&b, &arena, &x2, &s2, NULL));
    EXPECT_EQ_U64(s1, s2); // 压实不改长度，只改 dense 列里的值

    // 再读一次、再写一次，就该和 x2 逐字节相同：规范形从此稳定
    scene_io_fixture_t c;
    io_fixture_begin(&c, "io_hole_c");
    scene_io_stats_t stats3;
    EXPECT_TRUE(scene_read_binary(c.scene, x2, s2, &stats3));
    void *x3 = NULL;
    usize s3 = 0u;
    EXPECT_TRUE(io_write(&c, &arena, &x3, &s3, NULL));
    EXPECT_EQ_U64(s2, s3);
    if (s2 == s3) {
        EXPECT_TRUE(memcmp(x2, x3, s2) == 0);
    }

    io_fixture_end(&c);
    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void scene_io_rejects_bad_header(void)
{
    scene_io_fixture_t fx;
    io_fixture_begin(&fx, "io_bad");
    io_build_scene(&fx);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_bad_arena"));

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&fx, &arena, &bytes, &size, NULL));
    u8 *raw = (u8 *)bytes;

    scene_io_stats_t stats;
    io_log_guard_t quiet;
    io_quiet_logs(&quiet);

    // 魔数：把 OBJ 或别的什么喂进来
    u8 saved[4];
    memcpy(saved, raw, 4u);
    memset(raw, 0xFF, 4u);
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, size, &stats));
    memcpy(raw, saved, 4u);

    // 版本：不认识的版本一律拒绝，而不是"尽量读"
    raw[4] = 99u;
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, size, &stats));
    raw[4] = SCENE_FILE_VERSION;

    // 截断：每一段都可能被切一半，都必须失败而不是返回半份场景。
    // 步进取质数：正好落在段边界上的截断是最容易"意外通过"的一种，
    // 用 size/9+1 让切点扫过不同的对齐位置。
    const usize step = size / 9u + 1u;
    for (usize cut = 1u; cut < size; cut += step) {
        if (scene_read_binary(fx.scene, raw, cut, &stats)) {
            TF_CHECK(false, "截断到 %llu 字节竟然读成功了", (unsigned long long)cut);
        }
    }
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, size - 1u, &stats));
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, 0u, &stats));
    EXPECT_FALSE(scene_read_binary(fx.scene, NULL, size, &stats));

    // 完整的那份必须还能读 —— 否则上面的"失败"可能只是碰巧（比如 arena 被写坏了）
    EXPECT_TRUE(scene_read_binary(fx.scene, raw, size, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);

    io_restore_logs(&quiet);
    io_fixture_end(&fx);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 头部里的计数是外部输入：超上限必须在读任何记录之前拒掉。
static void scene_io_rejects_over_capacity(void)
{
    scene_io_fixture_t fx;
    io_fixture_begin(&fx, "io_cap");
    io_build_scene(&fx);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_cap_arena"));

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&fx, &arena, &bytes, &size, NULL));
    u8 *raw = (u8 *)bytes;

    // 头部按 u32 小端依次是 magic/version/hash/ent/mesh/atlas/material/type
    scene_io_stats_t stats;
    const u32 bad = SCENE_MAX_MESHES + 1u;
    memcpy(raw + 16u, &bad, 4u);
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, size, &stats));

    const u32 huge = 0xFFFFFFFFu;
    memcpy(raw + 12u, &huge, 4u); // ent_count
    memcpy(raw + 16u, &bad, 4u);
    EXPECT_FALSE(scene_read_binary(fx.scene, raw, size, &stats));

    io_fixture_end(&fx);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 注册表哈希：组件布局变了，旧存档必须被拒绝。
// 这条是 D3（字段表要手维护）剩下的唯一一道防线，值得单独测。
static void scene_io_rejects_registry_mismatch(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_reg_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_reg_arena"));

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, NULL));

    const u32 hash_before = scene_registry_hash(a.scene);

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_reg_b");
    // 多注册一个组件类型：注册表哈希随之改变
    static const ecs_field_desc_t extra_field = { "v", 0u, ECS_FIELD_F32, 1u, 0u, 0u };
    static const ecs_component_desc_t extra_desc = { .name = "ExtraThing",
                                                     .size = 16u,
                                                     .align = 16u,
                                                     .capacity = 8u,
                                                     .fields = &extra_field,
                                                     .field_count = 1u };
    EXPECT_TRUE(ecs_register_component(scene_world(b.scene), &extra_desc) != ECS_COMPONENT_NONE);
    EXPECT_TRUE(scene_registry_hash(b.scene) != hash_before);

    scene_io_stats_t stats;
    EXPECT_FALSE(scene_read_binary(b.scene, bytes, size, &stats));

    // 反过来：同样的注册表，读另一份存档仍然要成功（证明上面拒的是哈希不是别的）
    scene_io_fixture_t c;
    io_fixture_begin(&c, "io_reg_c");
    EXPECT_TRUE(scene_read_binary(c.scene, bytes, size, &stats));
    io_fixture_end(&c);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 类型名被改坏（人手编辑最容易犯的一类错）：该跳过的跳过，其余照常读出来。
static void scene_io_skips_unknown_component_type(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_skip_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_skip_arena"));

    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, NULL));

    // 只改类型表里 "Name" 的首字母。选 Name 是有意的：它只有 1 个实例，
    // 于是"丢了多少"有精确答案 —— 一个组件被跳过、九个照常，
    // 这比"总数变小了"更能说明跳过是没伤到别人的。
    u8 *raw = (u8 *)bytes;
    const usize at = io_find_padded_name(raw, size, "Name", 0u);
    if (at == IO_NOT_FOUND) {
        TF_CHECK(false, "存档里找不到类型名 Name —— 这条测试的定位方式失效了");
    } else {
        raw[at] = 'Z';
    }

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_skip_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, raw, size, &stats));
    EXPECT_EQ_U64(stats.unknown_component_types, 1u); // 按**类型**计一次，不是按实例
    EXPECT_EQ_U64(stats.schema_mismatches, 0u);
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS - 1u);

    // 其余组件真的还在：跳过不是"从这个实体起后面全不读"
    ecs_world_t *world = scene_world(b.scene);
    entity_t ents[8];
    const u32 n = ecs_collect_entities(world, ents, 8u);
    EXPECT_EQ_U64(n, 4u);
    bool mesh_survived = false;
    for (u32 i = 0; i < n; ++i) {
        if (ecs_peek(world, ents[i], b.ids.name) != NULL) {
            TF_CHECK(false, "被改坏的类型竟然还在");
        }
        if (ecs_peek(world, ents[i], b.ids.mesh_renderer) != NULL) {
            mesh_survived = true;
        }
    }
    EXPECT_TRUE(mesh_survived);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 组件里存着越界的资源下标：写出侧就要报，读回侧落成 NONE。
static void scene_io_write_side_reports_dangling_ref(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_dw_a");
    io_build_scene(&a);

    const entity_t e = ecs_create_entity(scene_world(a.scene));
    EXPECT_NOT_NULL(ecs_transform_get_or_add(scene_world(a.scene), e));
    scene_mesh_renderer_t mr;
    memset(&mr, 0, sizeof(mr));
    mr.mesh = 9999u;     // 不存在
    mr.material = 8888u; // 不存在
    EXPECT_NOT_NULL(ecs_add(scene_world(a.scene), e, a.ids.mesh_renderer, &mr));

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_dw_arena"));

    void *bytes = NULL;
    usize size = 0u;
    scene_io_stats_t wstats;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, &wstats));
    // 坏引用在**存盘那一刻**就该看得见。等读的时候才发现，文件里已经只剩
    // 一个空名字了 —— 连"这里曾经有个坏引用"都没留下。
    EXPECT_EQ_U64(wstats.dangling_refs, 2u);

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_dw_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, bytes, size, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 5u);

    ecs_world_t *world = scene_world(b.scene);
    entity_t ents[16];
    const u32 n = ecs_collect_entities(world, ents, 16u);
    EXPECT_EQ_U64(n, 5u);
    bool saw_none = false;
    for (u32 i = 0; i < n; ++i) {
        const scene_mesh_renderer_t *got =
            (const scene_mesh_renderer_t *)ecs_peek(world, ents[i], b.ids.mesh_renderer);
        if (got != NULL && got->mesh == (u32)SCENE_RESOURCE_NONE) {
            saw_none = true;
            EXPECT_EQ_U64(got->material, (u64)SCENE_RESOURCE_NONE);
        }
    }
    EXPECT_TRUE(saw_none);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 存档里引用了一个不存在的资源名（表被删过一行、或者手改错了）：
// 读侧计数 + 落成 NONE，且不影响别的实体。
static void scene_io_read_side_reports_missing_resource(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_dr_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 512u * 1024u, "io_dr_arena"));

    void *bytes = NULL;
    usize size = 0u;
    scene_io_stats_t wstats;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, &wstats));
    EXPECT_EQ_U64(wstats.dangling_refs, 0u); // 存盘时是干净的，坏的是**文件**

    u8 *raw = (u8 *)bytes;
    // "cube" 出现两次：资源记录的名字（第 0 次）、组件里的引用（第 1 次）。
    // 只改引用那一处，资源表本身保持完好。
    const usize at = io_find_padded_name(raw, size, "cube", 1u);
    if (at == IO_NOT_FOUND) {
        TF_CHECK(false, "存档里找不到第二处 cube 引用 —— 定位方式失效了");
    } else {
        raw[at] = 'x'; // cube -> xube
    }

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_dr_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_binary(b.scene, raw, size, &stats));
    EXPECT_EQ_U64(stats.dangling_refs, 1u);
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS); // 组件照读了，只是那个字段空了

    // 资源记录还在表里：说明丢的只是那一处引用，不是整个资源
    EXPECT_TRUE(scene_find_mesh(b.scene, "cube") != (u32)SCENE_RESOURCE_NONE);
    EXPECT_TRUE(scene_find_mesh(b.scene, "xube") == (u32)SCENE_RESOURCE_NONE);

    ecs_world_t *world = scene_world(b.scene);
    entity_t ents[8];
    const u32 n = ecs_collect_entities(world, ents, 8u);
    bool mesh_was_none = false, material_survived = false;
    for (u32 i = 0; i < n; ++i) {
        const scene_mesh_renderer_t *mr =
            (const scene_mesh_renderer_t *)ecs_peek(world, ents[i], b.ids.mesh_renderer);
        if (mr == NULL) {
            continue;
        }
        mesh_was_none = (mr->mesh == (u32)SCENE_RESOURCE_NONE);
        material_survived = (mr->material == scene_find_material(b.scene, "metal"));
    }
    EXPECT_TRUE(mesh_was_none);
    EXPECT_TRUE(material_survived);

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 文件形式：走一次真实的磁盘往返（离线场景，仍然不碰 GPU）
static void scene_io_file_roundtrip(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "io_file_a");
    io_build_scene(&a);

    const char *path = "scene_io_file_test.tmp.bin";
    scene_io_stats_t wstats;
    EXPECT_TRUE(scene_save_binary(a.scene, path, &wstats));
    EXPECT_EQ_U64(wstats.entities_written, 4u);

    scene_io_fixture_t b;
    io_fixture_begin(&b, "io_file_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_load_binary(b.scene, path, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS);

    // 不存在的路径必须失败而不是崩，也不返回半份状态
    scene_io_stats_t missing;
    EXPECT_FALSE(scene_load_binary(b.scene, "scene_io_file_no_such_file.bin", &missing));
    EXPECT_FALSE(scene_save_binary(b.scene, NULL, &missing));

    remove(path); // 临时文件用完就删，别把"仓库干净"变成要人记着做的事

    io_fixture_end(&b);
    io_fixture_end(&a);
}

// =============================================================================
//                       JSON 形式（转码）
//
//  判据分三层，缺一不可：
//   1. **纯转码无损**：二进制 → JSON → 二进制，字节必须完全相同。
//      这条最强，因为它不经过"读进场景再写出来"，所以 dense 压实之类的
//      正常规范化不会掩盖真正的丢内容。
//   2. **跨格式等价**：场景 A 的二进制 与 (A→JSON→场景 B) 的二进制，字节相同。
//      这条证明两种编码描述的**是同一份数据**，而不只是都能读回来。
//   3. **文本自稳定**：JSON → 场景 → JSON，文本相同 —— 人手改完存盘再改再存,
//      不能每过一轮就漂几个字节。
//
//  另外几条是 JSON 独有的风险，二进制那边覆盖不到：
//   * 省略字段必须取**默认记录**的值（不是 0、不是 0xFFFFFFFF）；
//   * 浮点经过十进制文本之后必须逐位回来；
//   * 文档写坏了要整份拒绝，版本漂移要按类型跳过并计数。
// =============================================================================

// 两段字节必须相同；不等时指出首个差异偏移。
// 只说"不相等"等于把二分查找的工作交给下一个人，而那个工作要几十分钟。
static void io_expect_same_bytes(const void *a, usize a_size, const void *b, usize b_size,
                                 const char *what)
{
    if (a_size != b_size) {
        TF_CHECK(false, "%s：长度不符 %llu 对 %llu", what, (unsigned long long)a_size,
                 (unsigned long long)b_size);
        return;
    }
    usize diff = 0u;
    while (diff < a_size && ((const u8 *)a)[diff] == ((const u8 *)b)[diff]) {
        diff++;
    }
    if (diff != a_size) {
        TF_CHECK(false, "%s：首个差异在偏移 %llu（0x%llX），前 %llu 字节相同", what,
                 (unsigned long long)diff, (unsigned long long)diff, (unsigned long long)diff);
    }
}

static usize io_arena_used(const scene_io_fixture_t *fx)
{
    return arena_bytes_used(scene_scratch_arena(fx->scene));
}

// 1. 纯转码无损：二进制 ↔ JSON 文本，不经过场景
static void scene_json_transcode_is_lossless(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_xcode_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 1024u * 1024u, "js_arena"));

    void *binary = NULL;
    usize binary_size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &binary, &binary_size, NULL));

    void *text = NULL;
    usize text_length = 0u;
    EXPECT_TRUE(scene_write_json(a.scene, &arena, &text, &text_length, NULL));
    EXPECT_GT_U64(text_length, 200u); // 太短说明只写出去了一小段
    EXPECT_EQ_U64(((const char *)text)[text_length], 0u); // 结尾那个 NUL

    // 文本编回字节：必须和原来那份**一模一样**
    scene_io_fixture_t b;
    io_fixture_begin(&b, "js_xcode_b");
    void *back = NULL;
    usize back_size = 0u;
    EXPECT_TRUE(scene_binary_from_json(b.scene, (const char *)text, text_length, &arena, &back,
                                       &back_size));
    io_expect_same_bytes(binary, binary_size, back, back_size, "二进制→JSON→二进制");

    // 反方向也走一遍：JSON → 二进制 → JSON，文本必须逐字节相同
    void *again = NULL;
    usize again_length = 0u;
    EXPECT_TRUE(scene_json_from_binary(b.scene, back, back_size, &arena, &again, &again_length));
    EXPECT_EQ_U64(text_length, again_length);
    io_expect_same_bytes(text, text_length, again, again_length, "JSON→二进制→JSON");

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 2. 跨格式等价：A 的二进制 == (A→JSON→B) 的二进制
static void scene_json_cross_format_equivalent(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_cross_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 1024u * 1024u, "js_cross_arena"));

    void *binary = NULL;
    usize binary_size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &binary, &binary_size, NULL));

    void *text = NULL;
    usize text_length = 0u;
    EXPECT_TRUE(scene_write_json(a.scene, &arena, &text, &text_length, NULL));

    scene_io_fixture_t b;
    io_fixture_begin(&b, "js_cross_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_json(b.scene, (const char *)text, text_length, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS);
    EXPECT_EQ_U64(stats.resources_loaded, 5u);
    EXPECT_EQ_U64(stats.unknown_component_types, 0u);
    EXPECT_EQ_U64(stats.schema_mismatches, 0u);
    EXPECT_EQ_U64(stats.dangling_refs, 0u);

    void *second = NULL;
    usize second_size = 0u;
    EXPECT_TRUE(io_write(&b, &arena, &second, &second_size, NULL));
    io_expect_same_bytes(binary, binary_size, second, second_size, "JSON 读进来再写成二进制");

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 3. 文本自稳定 + 临时内存不累积
static void scene_json_text_is_stable(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_stable_a");
    io_build_scene(&a);

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 1024u * 1024u, "js_stable_arena"));

    void *first = NULL;
    usize first_length = 0u;
    EXPECT_TRUE(scene_write_json(a.scene, &arena, &first, &first_length, NULL));

    // 读进一个全新场景、再写成文本：三轮下来文本必须一个字节都不漂。
    // 漂了就意味着"每存一次盘场景就变一点"，那是编辑器里最难查的一类问题。
    for (u32 round = 0u; round < 3u; ++round) {
        scene_io_fixture_t b;
        io_fixture_begin(&b, "js_stable_b");
        const usize scratch_before = io_arena_used(&b);
        scene_io_stats_t stats;
        EXPECT_TRUE(scene_read_json(b.scene, (const char *)first, first_length, &stats));
        // 读档的三张临时表（dense 映射、父 dense、组件计数）用完必须整体丢掉：
        // 前 99 次都不会失败，所以这种泄漏只能靠断言而不是靠运气。
        EXPECT_EQ_U64(io_arena_used(&b), scratch_before);

        void *next = NULL;
        usize next_length = 0u;
        EXPECT_TRUE(scene_write_json(b.scene, &arena, &next, &next_length, NULL));
        EXPECT_EQ_U64(first_length, next_length);
        if (first_length == next_length) {
            io_expect_same_bytes(first, first_length, next, next_length, "再写出的 JSON 文本");
        }
        io_fixture_end(&b);
    }

    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 4. 文件形式 + 按内容挑格式
static void scene_json_file_roundtrip(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_file_a");
    io_build_scene(&a);

    const char *bin_path = "scene_json_file_test.tmp.bin";
    const char *json_path = "scene_json_file_test.tmp.json";
    // 后缀与内容**互相写反**的四个名字：自动判别必须看内容而不是后缀
    const char *liar_bin = "scene_json_liar_test.tmp.bin";  // 里面其实是 JSON
    const char *liar_json = "scene_json_liar_test.tmp.json"; // 里面其实是二进制

    scene_io_stats_t wstats;
    EXPECT_TRUE(scene_save_binary(a.scene, bin_path, &wstats));
    EXPECT_TRUE(scene_save_json(a.scene, json_path, &wstats));

    // 先把两种内容抄到写反的名字下
    EXPECT_TRUE(io_copy_file(bin_path, liar_json));
    EXPECT_TRUE(io_copy_file(json_path, liar_bin));

    scene_io_fixture_t b;
    io_fixture_begin(&b, "js_file_b");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_load_auto(b.scene, bin_path, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    io_fixture_end(&b);

    io_fixture_begin(&b, "js_file_c");
    EXPECT_TRUE(scene_load_auto(b.scene, json_path, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    EXPECT_EQ_U64(stats.components_loaded, IO_BUILD_COMPONENTS);
    io_fixture_end(&b);

    io_fixture_begin(&b, "js_file_d");
    EXPECT_TRUE(scene_load_auto(b.scene, liar_bin, &stats)); // 后缀骗不了内容探测
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    io_fixture_end(&b);

    io_fixture_begin(&b, "js_file_e");
    EXPECT_TRUE(scene_load_auto(b.scene, liar_json, &stats));
    EXPECT_EQ_U64(stats.entities_loaded, 4u);
    io_fixture_end(&b);

    // 不存在的文件：失败而不是崩，两种入口都要拒
    io_fixture_begin(&b, "js_file_f");
    EXPECT_FALSE(scene_load_json(b.scene, "scene_json_no_such_file.json", &stats));
    EXPECT_FALSE(scene_load_auto(b.scene, "scene_json_no_such_file.json", &stats));
    EXPECT_FALSE(scene_save_json(b.scene, NULL, &stats));
    io_fixture_end(&b);

    remove(bin_path);
    remove(json_path);
    remove(liar_bin);
    remove(liar_json);
    io_fixture_end(&a);
}

static void scene_json_path_suffix_rules(void)
{
    EXPECT_TRUE(scene_path_is_json("a/b/c.scene.json"));
    EXPECT_TRUE(scene_path_is_json("CAPITAL.JSON"));
    EXPECT_TRUE(scene_path_is_json(".json"));
    EXPECT_FALSE(scene_path_is_json("a.scene"));
    EXPECT_FALSE(scene_path_is_json("json"));     // 太短，连点都没有
    EXPECT_FALSE(scene_path_is_json("a.jsonx"));  // 后缀必须在最末尾
    EXPECT_FALSE(scene_path_is_json(NULL));
    EXPECT_FALSE(scene_path_is_json(""));
}

// 5. 手写的极简文档：省略的东西一律取默认值
static void scene_json_hand_written_defaults(void)
{
    static const char *const k_doc =
        "{"
        "  \"kind\": \"scene\","
        "  \"entities\": ["
        "    { \"components\": [ { \"type\": \"Camera\", \"fields\": {} } ] },"
        "    { \"parent\": 0, \"components\": ["
        "        { \"type\": \"Name\", \"fields\": { \"text\": \"手写的\" } },"
        "        { \"type\": \"Transform\", \"fields\": { \"position\": [1.5, -2.25, 30.5] } }"
        "      ] },"
        "    { \"components\": [ { \"type\": \"WorldTransform\", \"fields\": {} } ] }"
        "  ]"
        "}";

    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_hand_a");
    io_log_guard_t quiet;
    io_quiet_logs(&quiet); // 派生组件那条 WARN 是预期内的，不该刷屏
    scene_io_stats_t stats;
    const bool ok = scene_read_json(a.scene, k_doc, strlen(k_doc), &stats);
    io_restore_logs(&quiet);
    EXPECT_TRUE(ok);
    EXPECT_EQ_U64(stats.entities_loaded, 3u);
    // 派生组件（skip_serialize）按"不认识"计一次，而不是当成打字错误拒掉整份
    EXPECT_EQ_U64(stats.unknown_component_types, 1u);
    EXPECT_EQ_U64(stats.schema_mismatches, 0u);
    // 没写资源表 → 空的；没写环境 → 全默认
    EXPECT_EQ_U64(scene_mesh_count(a.scene), 0u);
    EXPECT_EQ_U64(scene_material_count(a.scene), 0u);
    const scene_env_t d = scene_env_default();
    const scene_env_t *env = scene_environment(a.scene);
    EXPECT_EQ_U64(env->msaa_samples, d.msaa_samples);
    EXPECT_EQ_U64(env->shadow_map_size, d.shadow_map_size);
    EXPECT_EQ_U64(env->tonemap_mode, d.tonemap_mode);
    EXPECT_NEAR((double)env->fog_end, (double)d.fog_end, 1e-6);
    EXPECT_NEAR((double)env->exposure, (double)d.exposure, 1e-6);

    ecs_world_t *world = scene_world(a.scene);
    const entity_t cam = ecs_entity_from_dense(world, 0u);
    const entity_t kid = ecs_entity_from_dense(world, 1u);
    const entity_t derived = ecs_entity_from_dense(world, 2u);
    EXPECT_TRUE(entity_is_valid(cam));

    // Camera 一个字段都没写 → 全部是 construct 的默认值
    const scene_camera_t *c = (const scene_camera_t *)ecs_peek(world, cam, a.ids.camera);
    EXPECT_NOT_NULL(c);
    if (c != NULL) {
        scene_camera_t probe;
        memset(&probe, 0, sizeof(probe));
        if (scene_camera_desc.construct != NULL) {
            scene_camera_desc.construct(&probe); // 默认值的唯一来源，测试不自己抄一份
        }
        EXPECT_EQ_U64(c->projection, probe.projection);
        EXPECT_NEAR((double)c->fov_y_degrees, (double)probe.fov_y_degrees, 1e-6);
        EXPECT_NEAR((double)c->zfar, (double)probe.zfar, 1e-6);
        EXPECT_TRUE(c->active == probe.active);
    }
    // Transform 写了 position，rotation/scale 必须仍然是默认值而不是零
    const ecs_transform_t *t = (const ecs_transform_t *)ecs_peek(world, kid, a.ids.transform);
    EXPECT_NOT_NULL(t);
    if (t != NULL) {
        EXPECT_NEAR((double)t->position.x, 1.5, 1e-6);
        EXPECT_NEAR((double)t->position.y, -2.25, 1e-6);
        EXPECT_NEAR((double)t->position.z, 30.5, 1e-6);
        EXPECT_NEAR((double)t->scale.x, 1.0, 1e-6); // 默认 scale = 1，不是 0
        EXPECT_NEAR((double)t->rotation.w, 1.0, 1e-6);
    }
    const scene_name_t *n = (const scene_name_t *)ecs_peek(world, kid, a.ids.name);
    EXPECT_NOT_NULL(n);
    if (n != NULL) {
        EXPECT_STR_EQ(n->text, "手写的"); // UTF-8 名字要能原样回来
    }
    // 父子：dense 1 的父是 dense 0
    EXPECT_TRUE(ecs_get_parent(world, kid) == cam);
    EXPECT_TRUE(ecs_get_parent(world, cam) == ENTITY_NONE);
    // 派生组件被跳过，但实体本身照建
    EXPECT_TRUE(entity_is_valid(derived));
    EXPECT_NULL(ecs_peek(world, derived, a.ids.world_transform));

    io_fixture_end(&a);
}

// 5b. 省略的 u32 取默认值，不是 0xFFFFFFFF —— 这条是回归防线
static void scene_json_omitted_u32_keeps_default(void)
{
    static const char *const k_doc =
        "{"
        "  \"meshes\": [ { \"name\": \"cube\" } ],"
        "  \"entities\": [ { \"components\": ["
        "     { \"type\": \"MeshRenderer\", \"fields\": { \"mesh\": \"cube\" } } ] } ]"
        "}";
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_u32_a");
    scene_io_stats_t stats;
    EXPECT_TRUE(scene_read_json(a.scene, k_doc, strlen(k_doc), &stats));
    ecs_world_t *world = scene_world(a.scene);
    const scene_mesh_renderer_t *mr =
        (const scene_mesh_renderer_t *)ecs_peek(world, ecs_entity_from_dense(world, 0u),
                                                a.ids.mesh_renderer);
    EXPECT_NOT_NULL(mr);
    if (mr != NULL) {
        // layer 与 cast_shadow 都没写 → 取默认记录的值。
        // layer 的默认是 0；曾经这里是 ECS_DENSE_NONE（4294967295），
        // 症状是"物体读进来了却怎么都不显示"，而且不报任何错。
        EXPECT_EQ_U64(mr->layer, 0u);
        EXPECT_TRUE(mr->cast_shadow == false);
        // 写了的那个字段照常生效，而且名字要翻译成这条文档里的资源下标
        EXPECT_EQ_U64(mr->mesh, scene_find_mesh(a.scene, "cube"));
        EXPECT_TRUE(mr->mesh != (u32)SCENE_RESOURCE_NONE);
        EXPECT_TRUE(mr->material == (u32)SCENE_RESOURCE_NONE); // 引用没写就是"无"
    }
    io_fixture_end(&a);
}

// 6. 文档写坏了：每一种都必须**整份拒绝**
static void scene_json_rejects_bad_documents(void)
{
    static const char *const k_bad[] = {
        "{ \"entities\": [] } { \"extra\": 1 }",      // 尾部垃圾（解析器就拒）
        "{ \"entites\": [] }",                        // 顶层键拼错：静默忽略=空场景
        "{ \"version\": 99 }",                        // 版本不支持
        "{ \"kind\": \"level\" }",                    // kind 不认识
        "{ \"environment\": 5 }",                     // 段类型不对
        "{ \"meshes\": [ 5 ] }",                      // 数组元素不是对象
        "{ \"meshes\": [ { \"name\": \"a\", \"nope\": 1 } ] }", // 记录里的未知键
        "{ \"entities\": [ { \"components\": [ { \"type\": \"Transform\","
        "  \"fields\": { \"position\": [1,2] } } ] } ] }",      // 向量分量不齐
        "{ \"entities\": [ { \"components\": [ { \"type\": \"Transform\","
        "  \"fields\": { \"position\": 1 } } ] } ] }",          // 该给数组却给了标量
        "{ \"entities\": [ { \"components\": [ { \"type\": \"Camera\","
        "  \"fields\": {} }, { \"type\": \"Camera\", \"fields\": {} } ] } ] }", // 组件重复
        "{ \"entities\": [ { \"components\": [ { \"type\": \"Camera\""
        "  \"fields\": {} } ] } ] }",                            // 语法错误（缺逗号）
        "{ \"entities\": [ { \"components\": [ 7 ] } ] }",       // 组件记录不是对象
        "{ \"entities\": [ { \"dense\": \"zero\" } ] }",         // dense 类型不对
        "{ \"entities\": [ { \"parent\": -1 } ] }",             // 负数不是合法 dense
        "{ \"name\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" }",
    };
    const usize count = sizeof(k_bad) / sizeof(k_bad[0]);
    for (usize i = 0; i < count; ++i) {
        scene_io_fixture_t a;
        io_fixture_begin(&a, "js_bad_a");
        io_log_guard_t quiet;
        io_quiet_logs(&quiet); // 每条都要报错，但不必刷屏
        scene_io_stats_t stats;
        const bool ok = scene_read_json(a.scene, k_bad[i], strlen(k_bad[i]), &stats);
        io_restore_logs(&quiet);
        if (ok) {
            TF_CHECK(false, "第 %u 条坏文档被接受了：%.60s", (unsigned)i, k_bad[i]);
        }
        io_fixture_end(&a);
    }
}

// 7. 版本漂移：不认识的类型按**类型**计数并跳过，其余照常读出来
static void scene_json_drift_skips_by_type(void)
{
    static const char *const k_doc =
        "{"
        "  \"entities\": ["
        "    { \"components\": [ { \"type\": \"NotAComponent\", \"fields\": {} } ] },"
        "    { \"components\": [ { \"type\": \"NotAComponent\", \"fields\": {} },"
        "                         { \"type\": \"Name\", \"fields\": { \"text\": \"还在\" } } ] },"
        "    { \"components\": [ { \"type\": \"MeshRenderer\","
        "                         \"fields\": { \"layer\": 3, \"cast_shadow\": true } },"
        "                         { \"type\": \"Name\", \"fields\": { \"text\": \"无网格\" } } ] }"
        "  ]"
        "}";
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_drift_a");
    io_log_guard_t quiet;
    io_quiet_logs(&quiet);
    scene_io_stats_t stats;
    const bool ok = scene_read_json(a.scene, k_doc, strlen(k_doc), &stats);
    io_restore_logs(&quiet);

    EXPECT_TRUE(ok);
    EXPECT_EQ_U64(stats.entities_loaded, 3u);
    // 一个不认识的类型出现了两次，计数是 **1** 而不是 2：单位是类型，不是实例
    EXPECT_EQ_U64(stats.unknown_component_types, 1u);
    EXPECT_EQ_U64(stats.schema_mismatches, 0u);
    // 落地的组件：Name×2 + MeshRenderer×1 = 3（两条 NotAComponent 被丢掉）
    EXPECT_EQ_U64(stats.components_loaded, 3u);

    ecs_world_t *world = scene_world(a.scene);
    const scene_name_t *n1 =
        (const scene_name_t *)ecs_peek(world, ecs_entity_from_dense(world, 1u), a.ids.name);
    EXPECT_NOT_NULL(n1);
    if (n1 != NULL) {
        EXPECT_STR_EQ(n1->text, "还在");
    }
    // MeshRenderer 那两条字段都在表里，所以正常落地
    const scene_mesh_renderer_t *mr =
        (const scene_mesh_renderer_t *)ecs_peek(world, ecs_entity_from_dense(world, 2u),
                                                a.ids.mesh_renderer);
    EXPECT_NOT_NULL(mr);
    if (mr != NULL) {
        EXPECT_EQ_U64(mr->layer, 3u);
        EXPECT_TRUE(mr->cast_shadow == true);
    }
    io_fixture_end(&a);
}

// 7b. 字段名不在表里 = 布局漂移 → 整个类型作废（与二进制的 schema 哈希同一条判断）
static void scene_json_unknown_field_voids_type(void)
{
    static const char *const k_doc =
        "{"
        "  \"entities\": ["
        "    { \"components\": [ { \"type\": \"Light\", \"fields\":"
        "        { \"color\": [1,1,1], \"intensity\": 2.5 } } ] },"
        "    { \"components\": [ { \"type\": \"Light\", \"fields\":"
        "        { \"brightness\": 3.0 } } ] },"
        "    { \"components\": [ { \"type\": \"Name\", \"fields\": { \"text\": \"活着\" } } ] }"
        "  ]"
        "}";
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_field_a");
    io_log_guard_t quiet;
    io_quiet_logs(&quiet);
    scene_io_stats_t stats;
    const bool ok = scene_read_json(a.scene, k_doc, strlen(k_doc), &stats);
    io_restore_logs(&quiet);
    EXPECT_TRUE(ok);
    EXPECT_EQ_U64(stats.schema_mismatches, 1u); // 按类型计一次，不是按实例
    EXPECT_EQ_U64(stats.unknown_component_types, 0u);
    EXPECT_EQ_U64(stats.entities_loaded, 3u);

    ecs_world_t *world = scene_world(a.scene);
    // 两条 Light **全都**没了 —— 包括那条字段写对了的。
    // 为什么不是"忽略不认识的那个字段、其余照常"：字段表算出的字节块里,
    // 顺序与偏移就是格式本身，认不出键名意味着这份文件的表形状和这个引擎的
    // 不是一套，剩下那几个字段该按哪套摆没人知道。
    EXPECT_NULL(ecs_peek(world, ecs_entity_from_dense(world, 0u), a.ids.light));
    EXPECT_NULL(ecs_peek(world, ecs_entity_from_dense(world, 1u), a.ids.light));
    // 其余类型不受影响：一个坏类型不毁掉整个场景
    const scene_name_t *n =
        (const scene_name_t *)ecs_peek(world, ecs_entity_from_dense(world, 2u), a.ids.name);
    EXPECT_NOT_NULL(n);
    if (n != NULL) {
        EXPECT_STR_EQ(n->text, "活着");
    }
    io_fixture_end(&a);
}

// 8. 预制体的 JSON 形式：带根、环境不应用
static void scene_json_prefab_roundtrip(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_pf_a");
    io_build_scene(&a);
    const entity_t root = io_find_by_name(&a, "root");
    EXPECT_TRUE(entity_is_valid(root));

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 1024u * 1024u, "js_pf_arena"));
    void *text = NULL;
    usize length = 0u;
    EXPECT_TRUE(scene_write_prefab_json(a.scene, root, &arena, &text, &length, NULL));

    scene_io_fixture_t b;
    io_fixture_begin(&b, "js_pf_b");
    // 环境放一个**两个场景都不该有**的值：预制体不许改它，被覆盖就能看出来。
    // （只比"与源场景不同"是不够的 —— 两边碰巧相同的话这条断言就白写了。）
    scene_env_t *env = scene_environment(b.scene);
    env->msaa_samples = 4095u;
    env->exposure = -7.5f;
    const u32 msaa_before = env->msaa_samples;
    EXPECT_TRUE(msaa_before != scene_environment(a.scene)->msaa_samples);

    scene_io_stats_t stats;
    entity_t loaded_root = ENTITY_NONE;
    EXPECT_TRUE(scene_read_json_root(b.scene, (const char *)text, length, &stats, &loaded_root));
    EXPECT_TRUE(entity_is_valid(loaded_root));
    EXPECT_EQ_U64(stats.unknown_component_types, 0u);
    EXPECT_EQ_U64(stats.dangling_refs, 0u);
    EXPECT_EQ_U64(scene_environment(b.scene)->msaa_samples, msaa_before); // 没被覆盖
    EXPECT_NEAR((double)scene_environment(b.scene)->exposure, -7.5, 1e-6);
    EXPECT_EQ_U64(scene_subtree_size(b.scene, loaded_root), scene_subtree_size(a.scene, root));
    // 预制体只带被引用到的资源，所以目标场景的资源表**不该**是源场景那么大
    EXPECT_TRUE(scene_mesh_count(b.scene) <= scene_mesh_count(a.scene));

    io_fixture_end(&b);
    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 9. JSON 存不下非有限浮点，而二进制能 —— 这条差别必须可见
static void scene_json_rejects_non_finite(void)
{
    scene_io_fixture_t a;
    io_fixture_begin(&a, "js_nan_a");
    io_build_scene(&a);
    scene_environment(a.scene)->exposure = (f32)(0.0 / 0.0); // NaN

    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());
    arena_t arena;
    EXPECT_TRUE(arena_init(&arena, &tracker.base, 1024u * 1024u, "js_nan_arena"));

    // 二进制：什么位模式都存得下
    void *bytes = NULL;
    usize size = 0u;
    EXPECT_TRUE(io_write(&a, &arena, &bytes, &size, NULL));

    // JSON：写出 "NaN" 会产出不合法的文档，所以必须拒绝而不是将就
    void *text = NULL;
    usize length = 0u;
    io_log_guard_t quiet;
    io_quiet_logs(&quiet);
    const bool saved = scene_write_json(a.scene, &arena, &text, &length, NULL);
    io_restore_logs(&quiet);
    EXPECT_FALSE(saved);
    EXPECT_NULL(text);

    io_fixture_end(&a);
    arena_destroy(&arena);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

// 10. 空文档与"什么段都没写"
static void scene_json_minimal_documents(void)
{
    static const char *const k_docs[] = {
        "{}",
        "{ }",
        "{\"entities\":[]}",
        "{\"environment\":{},\"meshes\":[],\"atlases\":[],\"materials\":[],\"entities\":[]}",
        "{\"entities\":[{\"components\":[]}]}",
    };
    const usize count = sizeof(k_docs) / sizeof(k_docs[0]);
    for (usize i = 0; i < count; ++i) {
        scene_io_fixture_t a;
        io_fixture_begin(&a, "js_min_a");
        io_log_guard_t quiet;
        io_quiet_logs(&quiet);
        scene_io_stats_t stats;
        const bool ok = scene_read_json(a.scene, k_docs[i], strlen(k_docs[i]), &stats);
        io_restore_logs(&quiet);
        if (!ok) {
            TF_CHECK(false, "第 %u 份最小文档被拒：%.40s", (unsigned)i, k_docs[i]);
            continue;
        }
        EXPECT_EQ_U64(stats.unknown_component_types, 0u);
        EXPECT_EQ_U64(stats.schema_mismatches, 0u);
        // 空场景的存档仍然是**满长度**的（所有类型都登记了，一条实体都没有）：
        // 于是"注册表变了"仍然能被读出来，而不是靠实体数量猜。
        void *bytes = NULL;
        usize size = 0u;
        allocator_tracking_t tracker;
        allocator_tracking_init(&tracker, allocator_heap());
        arena_t arena;
        EXPECT_TRUE(arena_init(&arena, &tracker.base, 64u * 1024u, "js_min_arena"));
        EXPECT_TRUE(scene_write_binary(a.scene, &arena, &bytes, &size, NULL));
        EXPECT_GT_U64(size, SCENE_FILE_HEADER_SIZE);
        u32 entities = 0u, kind = 0u;
        EXPECT_TRUE(scene_archive_peek(bytes, size, &entities, &kind));
        EXPECT_EQ_U64(entities, (i >= 4u) ? 1u : 0u);
        EXPECT_EQ_U64(kind, (u32)SCENE_ARCHIVE_SCENE);
        arena_destroy(&arena);
        EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
        io_fixture_end(&a);
    }
}

// =============================================================================
void test_scene_io_register(void)
{
    test_register("scene_io", "empty_scene_roundtrips", scene_io_empty_scene_roundtrips);
    test_register("scene_io", "roundtrip_is_byte_identical", scene_io_roundtrip_is_byte_identical);
    test_register("scene_io", "values_survive_roundtrip", scene_io_values_survive);
    test_register("scene_io", "dense_holes_canonicalize", scene_io_dense_holes_canonicalize);
    test_register("scene_io", "rejects_bad_header", scene_io_rejects_bad_header);
    test_register("scene_io", "rejects_over_capacity", scene_io_rejects_over_capacity);
    test_register("scene_io", "rejects_registry_mismatch", scene_io_rejects_registry_mismatch);
    test_register("scene_io", "skips_unknown_component_type", scene_io_skips_unknown_component_type);
    test_register("scene_io", "write_side_reports_dangling_ref",
                  scene_io_write_side_reports_dangling_ref);
    test_register("scene_io", "read_side_reports_missing_resource",
                  scene_io_read_side_reports_missing_resource);
    test_register("scene_io", "file_roundtrip", scene_io_file_roundtrip);

    // JSON 形式（转码）。判据分三层，见上面的分节注释。
    test_register("scene_io", "json_transcode_is_lossless", scene_json_transcode_is_lossless);
    test_register("scene_io", "json_cross_format_equivalent",
                  scene_json_cross_format_equivalent);
    test_register("scene_io", "json_text_is_stable", scene_json_text_is_stable);
    test_register("scene_io", "json_file_roundtrip", scene_json_file_roundtrip);
    test_register("scene_io", "json_path_suffix_rules", scene_json_path_suffix_rules);
    test_register("scene_io", "json_hand_written_defaults", scene_json_hand_written_defaults);
    test_register("scene_io", "json_omitted_u32_keeps_default",
                  scene_json_omitted_u32_keeps_default);
    test_register("scene_io", "json_rejects_bad_documents", scene_json_rejects_bad_documents);
    test_register("scene_io", "json_drift_skips_by_type", scene_json_drift_skips_by_type);
    test_register("scene_io", "json_unknown_field_voids_type",
                  scene_json_unknown_field_voids_type);
    test_register("scene_io", "json_prefab_roundtrip", scene_json_prefab_roundtrip);
    test_register("scene_io", "json_rejects_non_finite", scene_json_rejects_non_finite);
    test_register("scene_io", "json_minimal_documents", scene_json_minimal_documents);
}
