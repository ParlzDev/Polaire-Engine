// =============================================================================
//  src/render/camera.c —— 相机实现
// =============================================================================
#include "engine/render/camera.h"

#include <string.h>

#include "engine/core/assert.h"

camera_t camera_default(void)
{
    camera_t camera;
    memset(&camera, 0, sizeof(camera));
    camera.position = vec3_make(0.0f, 0.0f, 5.0f);
    camera.rotation = quat_identity();
    camera.projection = CAMERA_PROJECTION_PERSPECTIVE;
    camera.fov_y_radians = math_radians(60.0f);
    camera.aspect = 16.0f / 9.0f;
    camera.ortho_height = 10.0f;
    camera.znear = 0.1f;
    camera.zfar = 1000.0f;
    return camera;
}

camera_t camera_perspective(vec3_t position, quat_t rotation, f32 fov_y_degrees, f32 aspect,
                            f32 znear, f32 zfar)
{
    ASSERT(aspect > 0.0f);
    ASSERT(znear > 0.0f);
    ASSERT(zfar > znear);

    camera_t camera;
    memset(&camera, 0, sizeof(camera));
    camera.position = position;
    camera.rotation = rotation;
    camera.projection = CAMERA_PROJECTION_PERSPECTIVE;
    camera.fov_y_radians = math_radians(fov_y_degrees);
    camera.aspect = aspect;
    camera.ortho_height = 10.0f;
    camera.znear = znear;
    camera.zfar = zfar;
    return camera;
}

camera_t camera_orthographic(vec3_t position, quat_t rotation, f32 view_height, f32 aspect,
                             f32 znear, f32 zfar)
{
    ASSERT(view_height > 0.0f);
    ASSERT(aspect > 0.0f);

    camera_t camera;
    memset(&camera, 0, sizeof(camera));
    camera.position = position;
    camera.rotation = rotation;
    camera.projection = CAMERA_PROJECTION_ORTHOGRAPHIC;
    camera.fov_y_radians = math_radians(60.0f);
    camera.aspect = aspect;
    camera.ortho_height = view_height;
    camera.znear = znear;
    camera.zfar = zfar;
    return camera;
}

void camera_set_yaw_pitch(camera_t *camera, f32 yaw_radians, f32 pitch_radians)
{
    ASSERT(camera != NULL);

    // 顺序：先 pitch（绕 X）再 yaw（绕 Y）。
    // 写成 quat_mul(qy, qx) 是因为 quat_mul(a, b) 的语义是"先应用 b 再应用 a"。
    // 反过来写会得到"yaw 轴跟着 pitch 一起歪掉"的滚转相机。
    const quat_t pitch = quat_rotation_x(pitch_radians);
    const quat_t yaw = quat_rotation_y(yaw_radians);
    camera->rotation = quat_mul(yaw, pitch);
}

void camera_look_at(camera_t *camera, vec3_t target)
{
    ASSERT(camera != NULL);

    const vec3_t to_target = vec3_sub(target, camera->position);
    if (vec3_length_sq(to_target) < 1.0e-12f) {
        return; // 目标与相机重合，保持原朝向
    }

    camera->rotation = quat_look_rotation(vec3_normalize(to_target), vec3_unit_y());
}

vec3_t camera_forward(const camera_t *camera)
{
    ASSERT(camera != NULL);
    return quat_rotate_vec3(camera->rotation, vec3_make(0.0f, 0.0f, -1.0f));
}

vec3_t camera_right(const camera_t *camera)
{
    ASSERT(camera != NULL);
    return quat_rotate_vec3(camera->rotation, vec3_make(1.0f, 0.0f, 0.0f));
}

vec3_t camera_up(const camera_t *camera)
{
    ASSERT(camera != NULL);
    return quat_rotate_vec3(camera->rotation, vec3_make(0.0f, 1.0f, 0.0f));
}

mat4_t camera_view_matrix(const camera_t *camera)
{
    ASSERT(camera != NULL);

    // 复用 M0 的 look_at：目标点 = 当前位置 + 前方。
    // 不自己拼视图矩阵，理由见 camera.h 文件头。
    const vec3_t forward = camera_forward(camera);
    const vec3_t target = vec3_add(camera->position, forward);
    const vec3_t up = camera_up(camera);

    return mat4_look_at(camera->position, target, up);
}

mat4_t camera_projection_matrix(const camera_t *camera)
{
    ASSERT(camera != NULL);
    ASSERT(camera->aspect > 0.0f);

    if (camera->projection == CAMERA_PROJECTION_ORTHOGRAPHIC) {
        const f32 half_height = camera->ortho_height * 0.5f;
        const f32 half_width = half_height * camera->aspect;
        return mat4_ortho(-half_width, half_width, -half_height, half_height, camera->znear,
                          camera->zfar);
    }

    return mat4_perspective(camera->fov_y_radians, camera->aspect, camera->znear, camera->zfar);
}

mat4_t camera_view_projection_matrix(const camera_t *camera)
{
    ASSERT(camera != NULL);
    return mat4_mul(camera_projection_matrix(camera), camera_view_matrix(camera));
}

f32 camera_view_depth(const camera_t *camera, vec3_t world_point)
{
    ASSERT(camera != NULL);

    // 相机看向局部 -Z，所以"前方"的点满足 to_point·forward > 0：
    // forward = (0,0,-1)，前方点的 to_point.z < 0，点积为正。
    // 直接返回点积就是"沿视线方向的距离"，前方为正、背后为负。
    //
    // 踩过一次：这里多写了一个负号，结果排序用的深度全部反号，
    // 不透明层变成"从远到近"绘制 —— 画面看起来正常（不透明物体谁先画
    // 都对），但 early-z 完全失效，而且半透明的顺序也错了。
    const vec3_t to_point = vec3_sub(world_point, camera->position);
    return vec3_dot(to_point, camera_forward(camera));
}

f32 camera_distance(const camera_t *camera, vec3_t world_point)
{
    ASSERT(camera != NULL);
    return vec3_distance(camera->position, world_point);
}

bool camera_contains_point(const camera_t *camera, vec3_t world_point, f32 radius)
{
    ASSERT(camera != NULL);

    const mat4_t view_projection = camera_view_projection_matrix(camera);
    const vec4_t clip = mat4_mul_vec4(view_projection, vec4_make(world_point.x, world_point.y,
                                                                 world_point.z, 1.0f));

    // w <= 0 表示点在相机背后，透视除法会得到镜像的结果，
    // 必须单独判掉 —— 否则"相机背后的物体"会因为除完落在 [-1,1] 而被误判为可见。
    if (clip.w <= 0.0f) {
        return false;
    }

    const f32 inv_w = 1.0f / clip.w;
    const f32 ndc_x = clip.x * inv_w;
    const f32 ndc_y = clip.y * inv_w;
    const f32 ndc_z = clip.z * inv_w;

    // 半径按距离折算成 NDC 的容差。透视下远处的物体 NDC 半径更小，
    // 除以 w 正好得到近似值。
    const f32 margin = radius * math_abs(inv_w);

    return ndc_x >= -1.0f - margin && ndc_x <= 1.0f + margin && //
           ndc_y >= -1.0f - margin && ndc_y <= 1.0f + margin && //
           ndc_z >= 0.0f && ndc_z <= 1.0f;
}
