// =============================================================================
//  engine/scene/ecs_submit.h —— ECS → 渲染队列的桥接
//
//  这是整个 M5 里唯一同时看得见 ecs/ 和 render/ 的位置。
//  把这条边界立起来的好处很具体：ECS 层可以被物理、脚本、编辑器复用，
//  而不用把 GL 一起拖进去；换渲染后端（M13）时也只用改渲染那一侧。
//
//  ---- 为什么必须经渲染队列而不是直接画 ----
//  精灵批渲染要求"同纹理同管线的提交连续"，而 ECS 的迭代顺序是内存顺序。
//  直接从 query 里 sprite_batch_draw 会按内存布局来回切纹理，
//  把 M3 辛苦做来的合批打回原形。所以：
//      3D → renderer_push_mesh（内部进队列，帧末统一排序绘制）
//      2D → sprite_batch（批本身就是"按纹理合并"的机制，且只在
//           layer/纹理变化时 flush，所以这里刻意**不**逐精灵改 depth）
// =============================================================================
#ifndef ENGINE_SCENE_ECS_SUBMIT_H
#define ENGINE_SCENE_ECS_SUBMIT_H

#include "engine/ecs/query.h"
#include "engine/render/camera.h"
#include "engine/render/renderer.h"
#include "engine/render/sprite_batch.h"
#include "engine/scene/scene.h"

typedef struct scene_submit_t {
    scene_t             *scene;
    sprite_batch_t      *batch; // 调用方持有（与 renderer 的关系同上）
    scene_component_ids_t ids;

    ecs_query_t *camera_query;
    ecs_query_t *light_query;
    ecs_query_t *mesh_query;
    ecs_query_t *sprite_query;

    // 统计：给控制台与"剔除前后 draw call 差异"这类可观测性用
    u32 meshes_submitted;
    u32 sprites_submitted;
    u32 lights_collected;
    u32 skipped_missing_resource; // 组件引用了不存在的资源下标
    u32 cameras_found;
} scene_submit_t;

// 建好四个查询。batch 允许为 NULL（纯 3D 场景），那时跳过精灵提交。
bool scene_submit_create(scene_submit_t *sub, scene_t *scene, sprite_batch_t *batch);
void scene_submit_destroy(scene_submit_t *sub);

// 把场景里的相机/灯光/环境写进渲染器，并提交所有可见物体。
//
// **调用时机**：必须在 renderer_begin_frame 之后、end_frame 之前，
// 且在世界的 TRANSFORM 阶段之后（世界矩阵得是新的）。
// aspect 由调用方按当前帧缓冲尺寸给 —— 桥接层不该去猜窗口有多大。
void scene_submit_frame(scene_submit_t *sub, renderer_t *renderer, f32 aspect);

// 从场景里挑出活动相机，填成渲染器要的 camera_t。
// 返回 false 表示场景里没有活动相机（这时调用方要么自己给一个，
// 要么画面会保持上一帧的相机 —— 后者是"看起来能跑但其实错了"的状态）。
bool scene_submit_active_camera(const scene_submit_t *sub, f32 aspect, camera_t *out);

#endif // ENGINE_SCENE_ECS_SUBMIT_H
