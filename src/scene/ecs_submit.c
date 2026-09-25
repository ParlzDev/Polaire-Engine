// =============================================================================
//  src/scene/ecs_submit.c —— ECS → 渲染队列的桥接
// =============================================================================
#include "engine/scene/ecs_submit.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/core/quat.h"
#include "engine/ecs/transform.h"
#include "engine/render/light.h"
#include "engine/render/texture.h"

bool scene_submit_create(scene_submit_t *sub, scene_t *scene, sprite_batch_t *batch)
{
    ASSERT(sub != NULL);
    memset(sub, 0, sizeof(*sub));

    scene_component_ids_t ids;
    if (!scene_register_components(scene, &ids)) {
        return false;
    }

    sub->scene = scene;
    sub->batch = batch;
    sub->ids = ids;
    ecs_world_t *world = scene_world(scene);

    // 世界矩阵必须先于一切提交：所以这四个查询都要求 world_transform 存在。
    // 少了这个条件，实体会在"只有本地变换"的时候被画出来 ——
    // 表现是新加的实体第一帧在原点闪一下。
    ecs_query_desc_t d = ecs_query_desc_2(ids.world_transform, ids.mesh_renderer, "submit/mesh");
    sub->mesh_query = ecs_query_create(world, &d);

    d = ecs_query_desc_2(ids.world_transform, ids.sprite_renderer, "submit/sprite");
    sub->sprite_query = ecs_query_create(world, &d);

    d = ecs_query_desc_2(ids.world_transform, ids.light, "submit/light");
    sub->light_query = ecs_query_create(world, &d);

    d = ecs_query_desc_2(ids.world_transform, ids.camera, "submit/camera");
    sub->camera_query = ecs_query_create(world, &d);

    // 四个查询各自对应一个已注册的类型，所以任何一个为 NULL 都只能是
    // 内存不足 —— 没有"精灵查询可以缺省"这种特例要照顾。
    if (sub->mesh_query == NULL || sub->sprite_query == NULL || sub->light_query == NULL ||
        sub->camera_query == NULL) {
        scene_submit_destroy(sub);
        return false;
    }
    return true;
}

void scene_submit_destroy(scene_submit_t *sub)
{
    if (sub == NULL || sub->scene == NULL) {
        return;
    }
    ecs_world_t *world = scene_world(sub->scene);
    ecs_query_destroy(world, sub->mesh_query);
    ecs_query_destroy(world, sub->sprite_query);
    ecs_query_destroy(world, sub->light_query);
    ecs_query_destroy(world, sub->camera_query);
    memset(sub, 0, sizeof(*sub));
}

// ---------------------------------------------------------------- 相机

// 世界矩阵的 3x3 可能带缩放（相机挂在被缩放的父节点下面完全是合理场景），
// 而 quat_from_mat3 假设输入是纯旋转。所以逐列归一化后再交给它。
// 某一列退化成零（缩放为 0）时直接给单位朝向：一个可解释的默认值，
// 比从退化基里硬解出一个乱转的相机好归因得多。
static mat3_t basis_without_scale(mat4_t m)
{
    mat3_t r;
    for (u32 i = 0u; i < 3u; ++i) {
        const vec3_t axis = vec3_make(m.col[i].x, m.col[i].y, m.col[i].z);
        const f32    len = vec3_length(axis);
        if (len <= 1e-6f) {
            return mat3_identity();
        }
        r.col[i] = vec3_scale(axis, 1.0f / len);
    }
    return r;
}

bool scene_submit_active_camera(const scene_submit_t *sub, f32 aspect, camera_t *out)
{
    if (sub == NULL || out == NULL || sub->camera_query == NULL) {
        return false;
    }

    // 多个 active 相机时取**实体 id 最小**的那个，而不是"迭代顺序第一个"：
    // 迭代顺序 = 组件数组的行顺序，而交换式移除不保证它稳定。
    // 靠它决定"这一帧用哪个相机"，就会得到"删了个实体之后主相机悄悄换了"。
    dense_id_t best_dense = ECS_DENSE_NONE;
    scene_camera_t best_params;
    ecs_world_transform_t best_wt;
    bool found = false;

    ecs_iter_t it;
    ecs_query_begin((ecs_query_t *)sub->camera_query, &it);
    while (ecs_query_next(&it)) {
        const scene_camera_t *cam = ECS_COLUMN(&it, 1, const scene_camera_t);
        const ecs_world_transform_t *wt = ECS_COLUMN(&it, 0, const ecs_world_transform_t);
        if (cam == NULL || wt == NULL || !cam->active) {
            continue;
        }
        const dense_id_t dense = ecs_entity_to_dense(scene_world(sub->scene), it.entity);
        if (!found || dense < best_dense) {
            best_dense = dense;
            best_params = *cam;
            best_wt = *wt;
            found = true;
        }
    }
    ecs_query_end(&it);

    if (!found) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->position = vec3_make(best_wt.model.col[3].x, best_wt.model.col[3].y,
                              best_wt.model.col[3].z);
    // 朝向直接从世界矩阵的 3x3 取，而不是让调用方再传一次 yaw/pitch：
    // 相机既然是一个实体，它的朝向就该和别的实体一样由父子链算出来 ——
    // "轨道相机"因此只是"相机是某个会转的节点的子节点"，不需要特例代码。
    //
    // 取之前先把三个基向量归一化：矩阵可能带缩放（相机挂在被缩放的节点下面
    // 是完全合理的场景），而 quat_from_mat3 假设输入是纯旋转。
    // 不归一化的话缩放会污染四元数，表现是"画面歪了但没人改过旋转"。
    out->rotation = quat_normalize(quat_from_mat3(basis_without_scale(best_wt.model)));
    out->projection = (best_params.projection == (u32)SCENE_CAMERA_ORTHOGRAPHIC)
                          ? CAMERA_PROJECTION_ORTHOGRAPHIC
                          : CAMERA_PROJECTION_PERSPECTIVE;
    out->fov_y_radians = math_radians(best_params.fov_y_degrees);
    out->ortho_height = best_params.ortho_height;
    out->znear = best_params.znear;
    out->zfar = best_params.zfar;
    out->aspect = (aspect > 0.0f) ? aspect : 1.0f;
    return true;
}

// ---------------------------------------------------------------- 提交

static void submit_env(const scene_env_t *env, renderer_t *renderer)
{
    renderer_set_clear(renderer, env->clear_color_enabled, env->clear_color,
                       env->clear_depth_enabled, 1.0f);
    renderer_set_ambient(renderer, env->ambient_color, env->ambient_intensity);
    renderer_set_fog(renderer, env->fog_enabled, env->fog_color, env->fog_start, env->fog_end,
                     env->fog_density);
    renderer_set_skybox(renderer, env->skybox_enabled, env->sky_horizon, env->sky_zenith,
                        env->sky_ground);
    renderer_set_tonemap(renderer, (tonemap_mode_t)env->tonemap_mode, env->exposure);
    renderer_set_msaa_samples(renderer, env->msaa_samples);
    renderer_set_culling_enabled(renderer, env->culling_enabled);
    renderer_set_shadow_params(renderer, env->shadows_enabled ? env->shadow_strength : 0.0f,
                               env->shadow_map_size);
}

// 从世界矩阵里取绕 Z 的旋转角，给 2D 精灵用。
//
// 2D 精灵只需要一个标量角，而 transform 存的是四元数 —— 有意只用一个 transform
// 类型（分两套的话 M6 的"2D 挂在 3D 节点下"立刻变特例）。
// 取法是"变换 x 轴后看它在 XY 平面里的角度"，对纯 Z 旋转的 quat 完全精确；
// 若某个 2D 实体被加了 X/Y 分量旋转，这里会退化成"取投影角"，
// 那是可接受的降级而不是错乱（并会在编辑器里被看出来）。
static f32 sprite_rotation_from_matrix(mat4_t model)
{
    const f32 x = model.col[0].x;
    const f32 y = model.col[0].y;
    if (x == 0.0f && y == 0.0f) {
        return 0.0f;
    }
    return atan2f(y, x);
}

void scene_submit_frame(scene_submit_t *sub, renderer_t *renderer, f32 aspect)
{
    ASSERT(sub != NULL);

    sub->meshes_submitted = 0u;
    sub->sprites_submitted = 0u;
    sub->lights_collected = 0u;
    sub->skipped_missing_resource = 0u;
    sub->cameras_found = 0u;

    const scene_env_t *env = scene_environment(sub->scene);

    submit_env(env, renderer);

    // ---- 相机 ----
    camera_t camera;
    if (scene_submit_active_camera(sub, aspect, &camera)) {
        renderer_set_camera(renderer, &camera);
        sub->cameras_found = 1u;
    }

    // ---- 灯光 ----
    //
    // 位置取实体的**世界**变换：所以"把灯挂在角色下面"就自然变成提着灯走，
    // 不需要在组件里再存一份坐标（存两份迟早不一致）。
    light_set_t lights;
    light_set_clear(&lights);

    ecs_iter_t it;
    ecs_query_begin(sub->light_query, &it);
    while (ecs_query_next(&it)) {
        const ecs_world_transform_t *wt = ECS_COLUMN(&it, 0, const ecs_world_transform_t);
        const scene_light_t *sl = ECS_COLUMN(&it, 1, const scene_light_t);
        if (wt == NULL || sl == NULL) {
            continue;
        }
        const vec3_t at = vec3_make(wt->model.col[3].x, wt->model.col[3].y, wt->model.col[3].z);

        light_t l;
        memset(&l, 0, sizeof(l));
        l.color = sl->color;
        l.intensity = sl->intensity;
        l.range = sl->range;
        l.shadow_bias = 0.0f;

        // M4 只有一张阴影贴图，而 shader 里判的是 `i == 0`：
        // **投射阴影的那盏必须是集合里的第一盏**。所以下面刻意把平行光
        // 先收进来一轮，保证主光排在 0 号位。
        const bool wants_shadow = sl->cast_shadow && env->shadows_enabled;
        l.cast_shadow = wants_shadow;

        switch (sl->type) {
        case 1u:
            l.type = LIGHT_TYPE_POINT;
            l.position = at;
            l.direction = vec3_make(0.0f, -1.0f, 0.0f);
            break;
        case 2u:
            l.type = LIGHT_TYPE_SPOT;
            l.position = at;
            l.direction = vec3_normalize(sl->direction);
            l.inner_angle = math_radians(sl->inner_angle_degrees);
            l.outer_angle = math_radians(sl->outer_angle_degrees);
            break;
        default:
            l.type = LIGHT_TYPE_DIRECTIONAL;
            l.position = vec3_zero();
            l.direction = vec3_normalize(sl->direction);
            break;
        }
        if (lights.count < MAX_LIGHTS) {
            light_set_add(&lights, &l);
        } else {
            LOG_WARN("场景光源数达到上限 %u，后续光源被忽略", MAX_LIGHTS);
        }
        sub->lights_collected++;
    }
    ecs_query_end(&it);

    // 把投影者挪到 0 号位（见上）
    for (u32 i = 0; i < light_set_count(&lights); ++i) {
        const light_t *l = light_set_get(&lights, i);
        if (l != NULL && l->cast_shadow && i != 0u) {
            const light_t first = lights.lights[0];
            lights.lights[0] = *l;
            lights.lights[i] = first;
            lights.shadow_caster = 0u;
            break;
        }
    }
    renderer_set_lights(renderer, &lights);

    // ---- 3D 网格 ----
    ecs_query_begin(sub->mesh_query, &it);
    while (ecs_query_next(&it)) {
        const ecs_world_transform_t *wt = ECS_COLUMN(&it, 0, const ecs_world_transform_t);
        const scene_mesh_renderer_t *mr = ECS_COLUMN(&it, 1, const scene_mesh_renderer_t);
        if (wt == NULL || mr == NULL) {
            continue;
        }
        if (!scene_mesh_id_valid(sub->scene, mr->mesh) ||
            !scene_material_id_valid(sub->scene, mr->material)) {
            sub->skipped_missing_resource++;
            continue; // 组件引用了不存在的资源下标：不画，但计数报出来
        }
        const material_t *mat = scene_material(sub->scene, mr->material);
        const mesh_handle_t mesh = scene_mesh_handle(sub->scene, mr->mesh);
        renderer_push_mesh(renderer, mat, mesh, wt->model, (render_layer_t)mr->layer);
        sub->meshes_submitted++;
    }
    ecs_query_end(&it);

    // ---- 2D 精灵 ----
    if (sub->batch != NULL) {
        render_layer_t current_layer = RENDER_LAYER_COUNT; // 一个不可能的值 = 还没设过
        ecs_query_begin(sub->sprite_query, &it);
        while (ecs_query_next(&it)) {
            const ecs_world_transform_t *wt = ECS_COLUMN(&it, 0, const ecs_world_transform_t);
            const scene_sprite_renderer_t *sr = ECS_COLUMN(&it, 1, const scene_sprite_renderer_t);
            if (wt == NULL || sr == NULL) {
                continue;
            }
            texture_handle_t texture = TEXTURE_HANDLE_INVALID;
            if (sr->atlas != SCENE_RESOURCE_NONE) {
                if (!scene_atlas_id_valid(sub->scene, sr->atlas)) {
                    sub->skipped_missing_resource++;
                    continue;
                }
                texture = scene_atlas_handle(sub->scene, sr->atlas);
            }

            // layer 变化才 flush 一次：逐精灵 set_depth 会把批次切碎，
            // 等于把 M3 的合批收益扔掉（M3 文档里那条"断批条件"在这里同样成立）。
            if ((render_layer_t)sr->layer != current_layer) {
                current_layer = (render_layer_t)sr->layer;
                sprite_batch_set_layer(sub->batch, current_layer);
            }

            sprite_t sprite;
            memset(&sprite, 0, sizeof(sprite));
            sprite.position = vec2_make(wt->model.col[3].x, wt->model.col[3].y);
            sprite.size = sr->size;
            // 缩放从世界矩阵的轴向量取绝对值：2D 精灵的尺寸不该被父节点的
            // 旋转变成负数（负的 width 会让三角形翻面，看起来像"精灵消失了"）。
            sprite.scale = vec2_make(math_abs(wt->model.col[0].x),
                                     math_abs(wt->model.col[1].y));
            sprite.rotation = sprite_rotation_from_matrix(wt->model);
            sprite.color = sr->color;
            sprite.uv_min = sr->uv_min;
            sprite.uv_max = sr->uv_max;

            sprite_batch_draw(sub->batch, &sprite, texture);
            sub->sprites_submitted++;
        }
        ecs_query_end(&it);
    }
}
