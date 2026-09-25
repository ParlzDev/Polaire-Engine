// =============================================================================
//  engine/render/camera_2d.h —— 2D 相机
//
//  ---- 与 3D 相机的关系 ----
//
//  2D 相机是 3D 相机的一个特例（正交投影 + 固定朝向），最终产出的是一个
//  `camera_t`。这样渲染队列、剔除、uniform 上传全部复用 M2 的代码路径，
//  不需要为 2D 单独开一条路。
//
//  ---- 坐标约定 ----
//
//  默认：1 世界单位 = 1 像素，原点在**相机中心**，Y 轴向上。
//  相机放在 z = +10 处看向 -Z，精灵在 z = 0 平面上。
//  可见世界范围 = [position - half_view, position + half_view]，
//  其中 half_view = viewport_size / (2 * zoom)。
//
//  ---- 像素对齐（pixel snapping） ----
//
//  正交投影下，如果精灵的世界坐标落在非整数像素上，光栅化会让边缘
//  落在像素中间，双线性采样之后表现为"精灵发虚"或"边缘缺一列"。
//
//  两个层次的防护：
//    1. 相机本身做对齐（camera_2d_snap）：把相机中心吸附到
//       `1/zoom` 的整数倍，这样"世界坐标的整数"与"屏幕像素"对齐。
//    2. 精灵位置由调用方给整数（渲染器不做强制 —— 平滑移动的物体
//       本来就该落在亚像素位置，强行取整会让运动一顿一顿）。
//
//  所以对齐是**相机的事**，不是精灵的事。相机不动时画面清晰；
//  相机平滑移动时按 zoom 的粒度吸附，避免画面抖动。
// =============================================================================
#ifndef ENGINE_RENDER_CAMERA_2D_H
#define ENGINE_RENDER_CAMERA_2D_H

#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/camera.h"

typedef struct camera_2d_t {
    vec2_t position;      // 相机中心（世界坐标）
    f32    zoom;          // 1.0 = 1 世界单位对应 1 像素
    vec2_t viewport_size; // 逻辑视口尺寸（像素）
    f32    znear;
    f32    zfar;
    bool   pixel_snap;    // 是否把相机位置吸附到像素网格
} camera_2d_t;

camera_2d_t camera_2d_default(void);

camera_2d_t camera_2d_make(vec2_t position, vec2_t viewport_size);
camera_2d_t camera_2d_make_zoomed(vec2_t position, f32 zoom, vec2_t viewport_size);

// 转换成通用的 camera_t（正交投影）。
// 调用方把这个相机交给 renderer_begin_frame 即可。
camera_t camera_2d_to_camera(const camera_2d_t *camera_2d);

mat4_t camera_2d_view_projection(const camera_2d_t *camera_2d);

// 可见世界范围（用于视锥剔除）
void camera_2d_view_bounds(const camera_2d_t *camera_2d, vec2_t *out_min, vec2_t *out_max);

// 屏幕坐标（像素，原点左上）-> 世界坐标
vec2_t camera_2d_screen_to_world(const camera_2d_t *camera_2d, vec2_t screen);

// 世界坐标 -> 屏幕坐标（像素，原点左上）
vec2_t camera_2d_world_to_screen(const camera_2d_t *camera_2d, vec2_t world);

// 把世界坐标吸附到"屏幕像素网格"上。
// 只有在 camera_2d.pixel_snap 为 true 时相机自己会调用它；
// 需要手动对齐单个物体的位置时也可以直接用。
vec2_t camera_2d_snap_point(const camera_2d_t *camera_2d, vec2_t world);

#endif // ENGINE_RENDER_CAMERA_2D_H
