// =============================================================================
//  src/scene/scene.c —— 场景：世界持有、资源表、环境参数
// =============================================================================
#include "engine/scene/scene.h"

#include <stdio.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/ecs/transform.h"
#include "engine/render/mesh.h"
#include "engine/render/texture.h"

typedef struct scene_mesh_entry_t {
    char          name[SCENE_RESOURCE_NAME_MAX];
    char          path[SCENE_RESOURCE_NAME_MAX * 2u]; // 空串 = 程序化/外部提供的句柄
    mesh_handle_t handle;
    bool          used;
} scene_mesh_entry_t;

typedef struct scene_atlas_entry_t {
    char             name[SCENE_RESOURCE_NAME_MAX];
    char             path[SCENE_RESOURCE_NAME_MAX * 2u];
    texture_handle_t handle;
    bool             used;
} scene_atlas_entry_t;

typedef struct scene_material_entry_t {
    char        name[SCENE_RESOURCE_NAME_MAX];
    char        pipeline_name[SCENE_RESOURCE_NAME_MAX];
    material_t  material;
    bool        used;
} scene_material_entry_t;

typedef struct scene_pipeline_entry_t {
    char             name[SCENE_RESOURCE_NAME_MAX];
    pipeline_handle_t handle;
    bool             used;
} scene_pipeline_entry_t;

struct scene_t {
    allocator_t *alloc;
    renderer_t  *renderer;

    ecs_world_t           *world;
    scene_component_ids_t  ids;
    bool                   components_registered;

    scene_env_t env;

    scene_mesh_entry_t     meshes[SCENE_MAX_MESHES];
    scene_atlas_entry_t    atlases[SCENE_MAX_ATLASES];
    scene_material_entry_t materials[SCENE_MAX_MATERIALS];
    scene_pipeline_entry_t pipelines[SCENE_MAX_PIPELINES];
};

scene_env_t scene_env_default(void)
{
    scene_env_t env;
    memset(&env, 0, sizeof(env));

    env.clear_color = vec4_make(0.04f, 0.05f, 0.08f, 1.0f);
    env.clear_color_enabled = true;
    env.clear_depth_enabled = true;

    env.ambient_color = vec3_make(0.55f, 0.62f, 0.78f);
    env.ambient_intensity = 0.22f;

    env.fog_enabled = false;
    env.fog_color = vec3_make(0.62f, 0.72f, 0.86f);
    env.fog_start = 18.0f;
    env.fog_end = 90.0f;
    env.fog_density = 1.0f;

    env.skybox_enabled = false;
    env.sky_horizon = vec3_make(0.66f, 0.75f, 0.88f);
    env.sky_zenith = vec3_make(0.18f, 0.34f, 0.66f);
    env.sky_ground = vec3_make(0.13f, 0.12f, 0.11f);

    env.tonemap_mode = 1u; // TONEMAP_ACES；存 u32 是为了让字段表不必理解枚举
    env.exposure = 1.0f;
    env.msaa_samples = 4u;

    env.shadows_enabled = true;
    env.shadow_strength = 0.78f;
    env.shadow_map_size = 2048u;
    env.culling_enabled = true;
    env.use_scene_target = true;
    return env;
}

scene_t *scene_create(const scene_config_t *config)
{
    if (config == NULL || config->allocator == NULL || config->renderer == NULL) {
        LOG_ERROR("scene_create 需要 allocator 与 renderer");
        return NULL;
    }

    scene_t *scene = (scene_t *)config->allocator->alloc(config->allocator, sizeof(scene_t), 16);
    if (scene == NULL) {
        LOG_ERROR("场景结构分配失败（%llu 字节）", (unsigned long long)sizeof(scene_t));
        return NULL;
    }
    memset(scene, 0, sizeof(*scene));
    scene->alloc = config->allocator;
    scene->renderer = config->renderer;
    scene->env = scene_env_default();

    ecs_world_config_t wc = ecs_world_config_default(config->allocator);
    if (config->entity_capacity > 0u) {
        wc.entity_capacity = config->entity_capacity;
    }
    if (config->resource_capacity_per_type > 0u) {
        wc.default_component_capacity = config->resource_capacity_per_type;
    }
    wc.name = (config->name != NULL) ? config->name : "scene";

    scene->world = ecs_world_create(config->allocator, &wc);
    if (scene->world == NULL) {
        config->allocator->free(config->allocator, scene, sizeof(scene_t));
        return NULL;
    }
    return scene;
}

void scene_destroy(scene_t *scene)
{
    if (scene == NULL) {
        return;
    }
    allocator_t *alloc = scene->alloc;

    // 场景只**引用**资源句柄，不拥有它们：网格与纹理的销毁权在 renderer。
    // 在这里顺手 texture_destroy 会把"谁拥有 GPU 资源"变成两个地方各说一半 ——
    // M7 的资源层要接管这件事，届时唯一的归属点是资源层。
    if (scene->world != NULL) {
        ecs_world_destroy(scene->world);
        scene->world = NULL;
    }
    alloc->free(alloc, scene, sizeof(scene_t));
}

ecs_world_t *scene_world(scene_t *scene)
{
    return (scene != NULL) ? scene->world : NULL;
}

renderer_t *scene_renderer(scene_t *scene)
{
    return (scene != NULL) ? scene->renderer : NULL;
}

const scene_component_ids_t *scene_components(const scene_t *scene)
{
    return (scene != NULL) ? &scene->ids : NULL;
}

scene_env_t *scene_environment(scene_t *scene)
{
    return (scene != NULL) ? &scene->env : NULL;
}

void scene_set_environment(scene_t *scene, const scene_env_t *env)
{
    if (scene != NULL && env != NULL) {
        scene->env = *env;
    }
}

// ---------------------------------------------------------------- 组件注册

bool scene_register_components(scene_t *scene, scene_component_ids_t *out_ids)
{
    ASSERT(scene != NULL);
    if (out_ids == NULL) {
        return false;
    }

    if (!scene->components_registered) {
        // 内置三个组件由 ECS 层注册，返回的是它自己的 id 打包结构；
        // 这里显式搬运而不是靠"两个结构体前三个字段一样"来强转 ——
        // 那种写法在其中一个加字段时会静默错位。
        ecs_builtin_components_t builtin;
        if (!ecs_register_builtin_components(scene->world, &builtin)) {
            return false;
        }
        scene->ids.transform = builtin.transform;
        scene->ids.world_transform = builtin.world_transform;
        scene->ids.parent = builtin.parent;

        static const ecs_component_desc_t *const descs[] = {
            &scene_mesh_renderer_desc, &scene_sprite_renderer_desc, &scene_camera_desc,
            &scene_light_desc, &scene_name_desc,
        };
        ecs_component_id *const slots[] = {
            &scene->ids.mesh_renderer, &scene->ids.sprite_renderer, &scene->ids.camera,
            &scene->ids.light, &scene->ids.name,
        };
        for (usize i = 0; i < ENGINE_ARRAY_COUNT(descs); ++i) {
            *slots[i] = ecs_register_component(scene->world, descs[i]);
            if (*slots[i] == ECS_COMPONENT_NONE) {
                LOG_ERROR("场景组件注册失败：%s", descs[i]->name);
                return false;
            }
        }
        scene->components_registered = true;
    }

    *out_ids = scene->ids;
    return true;
}

// ---------------------------------------------------------------- 表操作

static bool scene_copy_name(char *dst, const char *name, const char *what)
{
    if (name == NULL || name[0] == '\0') {
        LOG_ERROR("%s 需要一个非空名字", what);
        return false;
    }
    if (strlen(name) >= SCENE_RESOURCE_NAME_MAX) {
        LOG_ERROR("%s 名字过长（>= %u）：%s", what, (unsigned)SCENE_RESOURCE_NAME_MAX, name);
        return false;
    }
    snprintf(dst, SCENE_RESOURCE_NAME_MAX, "%s", name);
    return true;
}

static bool scene_copy_path(char *dst, usize cap, const char *path)
{
    if (path == NULL) {
        dst[0] = '\0';
        return true;
    }
    if (strlen(path) >= cap) {
        LOG_ERROR("资源路径过长：%s", path);
        return false;
    }
    snprintf(dst, cap, "%s", path);
    return true;
}

// ---------------------------------------------------------------- 管线

bool scene_register_pipeline(scene_t *scene, const char *name, pipeline_handle_t pipeline)
{
    ASSERT(scene != NULL);
    char slot[SCENE_RESOURCE_NAME_MAX];
    if (!scene_copy_name(slot, name, "管线")) {
        return false;
    }

    for (u32 i = 0; i < SCENE_MAX_PIPELINES; ++i) {
        if (scene->pipelines[i].used &&
            strncmp(scene->pipelines[i].name, slot, SCENE_RESOURCE_NAME_MAX) == 0) {
            scene->pipelines[i].handle = pipeline; // 重新登记 = 覆盖（热重载路径）
            return true;
        }
    }
    for (u32 i = 0; i < SCENE_MAX_PIPELINES; ++i) {
        if (!scene->pipelines[i].used) {
            scene->pipelines[i].used = true;
            memcpy(scene->pipelines[i].name, slot, SCENE_RESOURCE_NAME_MAX);
            scene->pipelines[i].handle = pipeline;
            return true;
        }
    }
    LOG_ERROR("管线表已满（%u）", SCENE_MAX_PIPELINES);
    return false;
}

bool scene_lookup_pipeline(const scene_t *scene, const char *name, pipeline_handle_t *out)
{
    if (scene == NULL || name == NULL) {
        return false;
    }
    for (u32 i = 0; i < SCENE_MAX_PIPELINES; ++i) {
        if (scene->pipelines[i].used &&
            strncmp(scene->pipelines[i].name, name, SCENE_RESOURCE_NAME_MAX) == 0) {
            if (out != NULL) {
                *out = scene->pipelines[i].handle;
            }
            return true;
        }
    }
    return false;
}

u32 scene_pipeline_count(const scene_t *scene)
{
    u32 n = 0u;
    if (scene == NULL) {
        return 0u;
    }
    for (u32 i = 0; i < SCENE_MAX_PIPELINES; ++i) {
        if (scene->pipelines[i].used) {
            n++;
        }
    }
    return n;
}

const char *scene_pipeline_name_at(const scene_t *scene, u32 index)
{
    if (scene == NULL) {
        return NULL;
    }
    u32 n = 0u;
    for (u32 i = 0; i < SCENE_MAX_PIPELINES; ++i) {
        if (scene->pipelines[i].used) {
            if (n == index) {
                return scene->pipelines[i].name;
            }
            n++;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- 网格

bool scene_add_mesh_handle(scene_t *scene, const char *name, mesh_handle_t mesh, u32 *out_id)
{
    ASSERT(scene != NULL);
    char slot[SCENE_RESOURCE_NAME_MAX];
    if (!scene_copy_name(slot, name, "网格")) {
        return false;
    }
    if (!mesh_is_valid(scene->renderer, mesh)) {
        LOG_ERROR("登记了一个无效网格句柄：%s", name);
        return false;
    }

    for (u32 i = 0; i < SCENE_MAX_MESHES; ++i) {
        if (scene->meshes[i].used && strncmp(scene->meshes[i].name, slot, SCENE_RESOURCE_NAME_MAX) == 0) {
            if (out_id != NULL) {
                *out_id = i;
            }
            return true; // 按名字去重
        }
    }
    for (u32 i = 0; i < SCENE_MAX_MESHES; ++i) {
        if (!scene->meshes[i].used) {
            scene->meshes[i].used = true;
            memcpy(scene->meshes[i].name, slot, SCENE_RESOURCE_NAME_MAX);
            scene->meshes[i].path[0] = '\0';
            scene->meshes[i].handle = mesh;
            if (out_id != NULL) {
                *out_id = i;
            }
            return true;
        }
    }
    LOG_ERROR("网格表已满（%u）", SCENE_MAX_MESHES);
    return false;
}

bool scene_add_mesh_from_file(scene_t *scene, const char *name, const char *path, u32 *out_id)
{
    ASSERT(scene != NULL);
    if (path == NULL || path[0] == '\0') {
        LOG_ERROR("scene_add_mesh_from_file 缺少路径");
        return false;
    }

    const usize length = strlen(path);
    mesh_handle_t mesh = MESH_HANDLE_INVALID;

    // 后缀决定走哪条解析路径。.mesh 是预烘焙的二进制（一次 memcpy），
    // 其余按 OBJ 文本处理 —— 这是"源格式 vs 运行格式"的分工，见 mesh_file.h。
    const bool is_binary = (length >= 5u && strcmp(path + length - 5u, ".mesh") == 0);
    if (is_binary) {
        mesh = mesh_create_from_mesh_file(scene->renderer, path);
    } else {
        mesh = mesh_create_from_obj_file(scene->renderer, path);
    }
    if (!mesh_is_valid(scene->renderer, mesh)) {
        LOG_ERROR("网格加载失败: %s", path);
        return false;
    }

    u32 id = SCENE_RESOURCE_NONE;
    if (!scene_add_mesh_handle(scene, name, mesh, &id)) {
        mesh_destroy(scene->renderer, mesh); // 表没收下这个资源，就得由我们释放
        return false;
    }
    if (id < SCENE_MAX_MESHES && scene->meshes[id].path[0] == '\0') {
        (void)scene_copy_path(scene->meshes[id].path, sizeof(scene->meshes[id].path), path);
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return true;
}

u32 scene_find_mesh(const scene_t *scene, const char *name)
{
    if (scene == NULL || name == NULL) {
        return SCENE_RESOURCE_NONE;
    }
    for (u32 i = 0; i < SCENE_MAX_MESHES; ++i) {
        if (scene->meshes[i].used && strncmp(scene->meshes[i].name, name, SCENE_RESOURCE_NAME_MAX) == 0) {
            return i;
        }
    }
    return SCENE_RESOURCE_NONE;
}

bool scene_mesh_id_valid(const scene_t *scene, u32 id)
{
    return scene != NULL && id < SCENE_MAX_MESHES && scene->meshes[id].used;
}

mesh_handle_t scene_mesh_handle(const scene_t *scene, u32 id)
{
    return scene_mesh_id_valid(scene, id) ? scene->meshes[id].handle : MESH_HANDLE_INVALID;
}

const char *scene_mesh_name(const scene_t *scene, u32 id)
{
    return scene_mesh_id_valid(scene, id) ? scene->meshes[id].name : NULL;
}

const char *scene_mesh_path(const scene_t *scene, u32 id)
{
    if (!scene_mesh_id_valid(scene, id)) {
        return NULL;
    }
    return (scene->meshes[id].path[0] != '\0') ? scene->meshes[id].path : NULL;
}

u32 scene_mesh_count(const scene_t *scene)
{
    u32 n = 0u;
    if (scene == NULL) {
        return 0u;
    }
    for (u32 i = 0; i < SCENE_MAX_MESHES; ++i) {
        if (scene->meshes[i].used) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------- 图集

bool scene_add_atlas_handle(scene_t *scene, const char *name, texture_handle_t texture,
                            u32 *out_id)
{
    ASSERT(scene != NULL);
    char slot[SCENE_RESOURCE_NAME_MAX];
    if (!scene_copy_name(slot, name, "图集")) {
        return false;
    }
    if (!texture_is_valid(scene->renderer, texture)) {
        LOG_ERROR("登记了一个无效纹理句柄：%s", name);
        return false;
    }
    for (u32 i = 0; i < SCENE_MAX_ATLASES; ++i) {
        if (scene->atlases[i].used &&
            strncmp(scene->atlases[i].name, slot, SCENE_RESOURCE_NAME_MAX) == 0) {
            if (out_id != NULL) {
                *out_id = i;
            }
            return true;
        }
    }
    for (u32 i = 0; i < SCENE_MAX_ATLASES; ++i) {
        if (!scene->atlases[i].used) {
            scene->atlases[i].used = true;
            memcpy(scene->atlases[i].name, slot, SCENE_RESOURCE_NAME_MAX);
            scene->atlases[i].path[0] = '\0';
            scene->atlases[i].handle = texture;
            if (out_id != NULL) {
                *out_id = i;
            }
            return true;
        }
    }
    LOG_ERROR("图集表已满（%u）", SCENE_MAX_ATLASES);
    return false;
}

bool scene_add_atlas_from_file(scene_t *scene, const char *name, const char *path, u32 *out_id)
{
    ASSERT(scene != NULL);
    if (path == NULL || path[0] == '\0') {
        LOG_ERROR("scene_add_atlas_from_file 缺少路径");
        return false;
    }

    // 一律按 sRGB 载入：颜色贴图存的是 gamma 编码值，采样时要解码到线性再算光照
    // （M4 定的管线）。将来加**法线贴图**时这条必须能关掉 —— 法线是线性数据，
    // 用 sRGB 载入会让光照方向偏掉。
    const texture_handle_t texture = texture_load_from_file(scene->renderer, path, true);
    if (!texture_is_valid(scene->renderer, texture)) {
        LOG_ERROR("纹理加载失败: %s", path);
        return false;
    }

    u32 id = SCENE_RESOURCE_NONE;
    if (!scene_add_atlas_handle(scene, name, texture, &id)) {
        texture_destroy(scene->renderer, texture);
        return false;
    }
    if (id < SCENE_MAX_ATLASES && scene->atlases[id].path[0] == '\0') {
        (void)scene_copy_path(scene->atlases[id].path, sizeof(scene->atlases[id].path), path);
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return true;
}

u32 scene_find_atlas(const scene_t *scene, const char *name)
{
    if (scene == NULL || name == NULL) {
        return SCENE_RESOURCE_NONE;
    }
    for (u32 i = 0; i < SCENE_MAX_ATLASES; ++i) {
        if (scene->atlases[i].used &&
            strncmp(scene->atlases[i].name, name, SCENE_RESOURCE_NAME_MAX) == 0) {
            return i;
        }
    }
    return SCENE_RESOURCE_NONE;
}

bool scene_atlas_id_valid(const scene_t *scene, u32 id)
{
    return scene != NULL && id < SCENE_MAX_ATLASES && scene->atlases[id].used;
}

texture_handle_t scene_atlas_handle(const scene_t *scene, u32 id)
{
    return scene_atlas_id_valid(scene, id) ? scene->atlases[id].handle : TEXTURE_HANDLE_INVALID;
}

const char *scene_atlas_name(const scene_t *scene, u32 id)
{
    return scene_atlas_id_valid(scene, id) ? scene->atlases[id].name : NULL;
}

u32 scene_atlas_count(const scene_t *scene)
{
    u32 n = 0u;
    if (scene == NULL) {
        return 0u;
    }
    for (u32 i = 0; i < SCENE_MAX_ATLASES; ++i) {
        if (scene->atlases[i].used) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------- 材质

bool scene_add_material(scene_t *scene, const char *name, const char *pipeline_name,
                        const material_t *material, u32 *out_id)
{
    ASSERT(scene != NULL);
    char slot[SCENE_RESOURCE_NAME_MAX];
    if (!scene_copy_name(slot, name, "材质")) {
        return false;
    }
    if (material == NULL) {
        LOG_ERROR("材质 %s 缺少描述", name);
        return false;
    }

    // 管线名字**在这里**就解析成句柄。解析不到就直接失败 ——
    // 留一个"pipeline 无效、运行时用默认"的状态，将来画面对不上时
    // 没人会自动想到"是材质引用了一个不存在的管线"。
    pipeline_handle_t pipeline = PIPELINE_HANDLE_INVALID;
    if (pipeline_name == NULL || !scene_lookup_pipeline(scene, pipeline_name, &pipeline)) {
        LOG_ERROR("材质 %s 引用了未登记的管线 %s（先用 scene_register_pipeline 注册）", name,
                  (pipeline_name != NULL) ? pipeline_name : "<null>");
        return false;
    }

    for (u32 i = 0; i < SCENE_MAX_MATERIALS; ++i) {
        if (scene->materials[i].used &&
            strncmp(scene->materials[i].name, slot, SCENE_RESOURCE_NAME_MAX) == 0) {
            if (out_id != NULL) {
                *out_id = i;
            }
            return true;
        }
    }
    for (u32 i = 0; i < SCENE_MAX_MATERIALS; ++i) {
        if (!scene->materials[i].used) {
            scene->materials[i].used = true;
            memcpy(scene->materials[i].name, slot, SCENE_RESOURCE_NAME_MAX);
            scene_copy_name(scene->materials[i].pipeline_name, pipeline_name, "管线索引用");
            scene->materials[i].material = *material;
            scene->materials[i].material.pipeline = pipeline;
            if (out_id != NULL) {
                *out_id = i;
            }
            return true;
        }
    }
    LOG_ERROR("材质表已满（%u）", SCENE_MAX_MATERIALS);
    return false;
}

bool scene_material_set_atlas(scene_t *scene, u32 material_id, u32 slot, u32 atlas_id)
{
    if (!scene_material_id_valid(scene, material_id) || !scene_atlas_id_valid(scene, atlas_id)) {
        LOG_ERROR("给材质挂图集失败：材质 %u / 图集 %u 有一个不存在", (unsigned)material_id,
                  (unsigned)atlas_id);
        return false;
    }
    material_t *m = &scene->materials[material_id].material;
    material_set_texture(m, slot, scene->atlases[atlas_id].handle, NULL);
    return true;
}

u32 scene_find_material(const scene_t *scene, const char *name)
{
    if (scene == NULL || name == NULL) {
        return SCENE_RESOURCE_NONE;
    }
    for (u32 i = 0; i < SCENE_MAX_MATERIALS; ++i) {
        if (scene->materials[i].used &&
            strncmp(scene->materials[i].name, name, SCENE_RESOURCE_NAME_MAX) == 0) {
            return i;
        }
    }
    return SCENE_RESOURCE_NONE;
}

bool scene_material_id_valid(const scene_t *scene, u32 id)
{
    return scene != NULL && id < SCENE_MAX_MATERIALS && scene->materials[id].used;
}

const material_t *scene_material(const scene_t *scene, u32 id)
{
    return scene_material_id_valid(scene, id) ? &scene->materials[id].material : NULL;
}

const char *scene_material_name(const scene_t *scene, u32 id)
{
    return scene_material_id_valid(scene, id) ? scene->materials[id].name : NULL;
}

u32 scene_material_count(const scene_t *scene)
{
    u32 n = 0u;
    if (scene == NULL) {
        return 0u;
    }
    for (u32 i = 0; i < SCENE_MAX_MATERIALS; ++i) {
        if (scene->materials[i].used) {
            n++;
        }
    }
    return n;
}
