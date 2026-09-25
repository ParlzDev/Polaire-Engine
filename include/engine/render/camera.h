// =============================================================================
//  engine/render/camera.h —— 相机
//
//  ---- 约定（与 M0 保持一致，不另起一套） ----
//
//  右手系 / Y 轴向上 / 相机看向**局部 -Z** / 裁剪空间深度 **[0, 1]**。
//
//  视图矩阵与投影矩阵**直接复用 M0 的 `mat4_look_at` / `mat4_perspective` /
//  `mat4_ortho`**，不在这里另写一份。两份实现必然会在某个时刻不一致 ——
//  而"投影矩阵对不上"这类问题的表现是"画面看起来差不多但就是歪了"，
//  排查成本极高。
//
//  ---- 为什么旋转用四元数 ----
//
//  相机的朝向是"一个旋转"，用四元数存不会遇到万向锁，插值（做相机平滑）
//  也是良定义的。yaw/pitch 只是输入方式，通过 camera_set_yaw_pitch 转成四元数。
// =============================================================================
#ifndef ENGINE_RENDER_CAMERA_H
#define ENGINE_RENDER_CAMERA_H

#include "engine/core/mat.h"
#include "engine/core/quat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

typedef enum camera_projection_t {
    CAMERA_PROJECTION_PERSPECTIVE = 0,
    CAMERA_PROJECTION_ORTHOGRAPHIC
} camera_projection_t;

typedef struct camera_t {
    vec3_t position;
    quat_t rotation; // 朝向：把局部 -Z 旋转到看向的方向

    camera_projection_t projection;

    // 透视参数
    f32 fov_y_radians;
    f32 aspect; // width / height

    // 正交参数：可见区域的高度（世界单位）。宽度 = height * aspect。
    f32 ortho_height;

    f32 znear;
    f32 zfar;
} camera_t;

camera_t camera_default(void);

camera_t camera_perspective(vec3_t position, quat_t rotation, f32 fov_y_degrees, f32 aspect,
                            f32 znear, f32 zfar);

camera_t camera_orthographic(vec3_t position, quat_t rotation, f32 view_height, f32 aspect,
                             f32 znear, f32 zfar);

// 用 yaw（绕 Y）与 pitch（绕 X）设置朝向。
// 顺序：先 pitch 再 yaw，即 rotation = qy(yaw) * qx(pitch)。
void camera_set_yaw_pitch(camera_t *camera, f32 yaw_radians, f32 pitch_radians);

// 让相机看向某个点（保持当前位置与 up = 世界 +Y）
void camera_look_at(camera_t *camera, vec3_t target);

// 朝向的基向量（世界空间，单位长度）
vec3_t camera_forward(const camera_t *camera); // 局部 -Z 方向
vec3_t camera_right(const camera_t *camera);   // 局部 +X 方向
vec3_t camera_up(const camera_t *camera);      // 局部 +Y 方向

mat4_t camera_view_matrix(const camera_t *camera);
mat4_t camera_projection_matrix(const camera_t *camera);
mat4_t camera_view_projection_matrix(const camera_t *camera);

// 世界坐标在相机空间里的深度（正值表示在相机前方）。
// 渲染队列用它排序：不透明按它升序（近的先画，early-z 效率高），
// 半透明按它降序（远的先画，混合才正确）。
f32 camera_view_depth(const camera_t *camera, vec3_t world_point);

// 世界坐标到相机的直线距离（不是沿视线的深度）。用于 LOD、音效衰减。
f32 camera_distance(const camera_t *camera, vec3_t world_point);

// 世界坐标是否落在视锥内（粗略判据：NDC 的 x/y 在 [-1,1]、z 在 [0,1]）。
// M2 的视锥剔除用这个；精确的平面剔除放到 M4。
bool camera_contains_point(const camera_t *camera, vec3_t world_point, f32 radius);

#endif // ENGINE_RENDER_CAMERA_H
