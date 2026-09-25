// =============================================================================
//  src/render/camera_2d.c —— 2D 相机
// =============================================================================
#include "engine/render/camera_2d.h"

#include <string.h>

#include "engine/core/assert.h"

// 2D 相机放在 z = +10，看向 -Z。精灵在 z = 0 平面上。
// 选 10 而不是 1：给"2.5D 混合渲染"留出空间 —— M6 会让 3D 内容
// 出现在 2D 精灵的前后，近平面不能贴得太近。
#define CAMERA_2D_Z 10.0f

camera_2d_t camera_2d_default(void)
{
    camera_2d_t camera;
    memset(&camera, 0, sizeof(camera));
    camera.position = vec2_zero();
    camera.zoom = 1.0f;
    camera.viewport_size = vec2_make(1280.0f, 720.0f);
    camera.znear = -100.0f;
    camera.zfar = 200.0f;
    camera.pixel_snap = true;
    return camera;
}

camera_2d_t camera_2d_make(vec2_t position, vec2_t viewport_size)
{
    camera_2d_t camera = camera_2d_default();
    camera.position = position;
    camera.viewport_size = viewport_size;
    return camera;
}

camera_2d_t camera_2d_make_zoomed(vec2_t position, f32 zoom, vec2_t viewport_size)
{
    camera_2d_t camera = camera_2d_make(position, viewport_size);
    camera.zoom = (zoom > 0.0001f) ? zoom : 0.0001f;
    return camera;
}

vec2_t camera_2d_snap_point(const camera_2d_t *camera_2d, vec2_t world)
{
    ASSERT(camera_2d != NULL);

    if (!camera_2d->pixel_snap) {
        return world;
    }

    // 吸附粒度是"一个屏幕像素对应的世界距离" = 1/zoom。
    // zoom = 1 时就是整数像素；zoom = 2（放大两倍）时半像素，
    // 这样放大后仍然能落在物理像素网格上。
    const f32 granularity = 1.0f / camera_2d->zoom;
    return vec2_make(floorf(world.x / granularity + 0.5f) * granularity,
                     floorf(world.y / granularity + 0.5f) * granularity);
}

camera_t camera_2d_to_camera(const camera_2d_t *camera_2d)
{
    ASSERT(camera_2d != NULL);
    ASSERT(camera_2d->zoom > 0.0f);
    ASSERT(camera_2d->viewport_size.y > 0.0f);

    // 相机中心吸附到像素网格：相机不动时画面清晰，
    // 相机平滑移动时按 zoom 粒度吸附，避免画面抖动。
    const vec2_t position = camera_2d_snap_point(camera_2d, camera_2d->position);

    const f32 aspect = camera_2d->viewport_size.x / camera_2d->viewport_size.y;

    // 可见高度 = 视口高度 / zoom。zoom = 1 时 1 世界单位 = 1 像素。
    const f32 view_height = camera_2d->viewport_size.y / camera_2d->zoom;

    const vec3_t eye = vec3_make(position.x, position.y, CAMERA_2D_Z);

    // 朝向 -Z、Y 向上：quat_identity 就是这个朝向
    return camera_orthographic(eye, quat_identity(), view_height, aspect, camera_2d->znear,
                               camera_2d->zfar);
}

mat4_t camera_2d_view_projection(const camera_2d_t *camera_2d)
{
    const camera_t camera = camera_2d_to_camera(camera_2d);
    return camera_view_projection_matrix(&camera);
}

void camera_2d_view_bounds(const camera_2d_t *camera_2d, vec2_t *out_min, vec2_t *out_max)
{
    ASSERT(camera_2d != NULL);

    const vec2_t center = camera_2d_snap_point(camera_2d, camera_2d->position);
    const vec2_t half = vec2_make(camera_2d->viewport_size.x / (2.0f * camera_2d->zoom),
                                  camera_2d->viewport_size.y / (2.0f * camera_2d->zoom));

    if (out_min != NULL) {
        *out_min = vec2_sub(center, half);
    }
    if (out_max != NULL) {
        *out_max = vec2_add(center, half);
    }
}

vec2_t camera_2d_screen_to_world(const camera_2d_t *camera_2d, vec2_t screen)
{
    ASSERT(camera_2d != NULL);

    // 屏幕坐标原点在左上、Y 向下；世界坐标原点在相机中心、Y 向上。
    const vec2_t center = camera_2d_snap_point(camera_2d, camera_2d->position);
    const f32    half_w = camera_2d->viewport_size.x * 0.5f;
    const f32    half_h = camera_2d->viewport_size.y * 0.5f;

    return vec2_make(center.x + (screen.x - half_w) / camera_2d->zoom,
                     center.y - (screen.y - half_h) / camera_2d->zoom);
}

vec2_t camera_2d_world_to_screen(const camera_2d_t *camera_2d, vec2_t world)
{
    ASSERT(camera_2d != NULL);

    const vec2_t center = camera_2d_snap_point(camera_2d, camera_2d->position);
    const f32    half_w = camera_2d->viewport_size.x * 0.5f;
    const f32    half_h = camera_2d->viewport_size.y * 0.5f;

    return vec2_make((world.x - center.x) * camera_2d->zoom + half_w,
                     (center.y - world.y) * camera_2d->zoom + half_h);
}
