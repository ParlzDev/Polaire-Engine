// =============================================================================
//  engine/render/frustum.h —— 视锥与剔除
//
//  ---- 平面提取的推导（列主序 + 右手系 + 深度 [0,1]，三者叠加最容易错） ----
//
//  裁剪空间的条件是：
//      -w <= x <= w,  -w <= y <= w,  0 <= z <= w
//
//  注意深度是 **[0,1]**（M0 定的约定），所以近平面是 `z >= 0` 而不是 `z >= -w`。
//  写成后者的话近平面会跑到相机背后，剔除结果看起来"差不多对"，
//  但相机贴脸时会漏剔一堆物体 —— 而且不报任何错。
//
//  把 clip = M * v 代入，每个不等式都变成 v 的一个线性式（就是 M 的某一行组合）：
//
//      left   : row3 + row0 >= 0
//      right  : row3 - row0 >= 0
//      bottom : row3 + row1 >= 0
//      top    : row3 - row1 >= 0
//      near   : row2        >= 0        <- 深度 [0,1] 的关键
//      far    : row3 - row2 >= 0
//
//  其中 rowN = (col[0][N], col[1][N], col[2][N], col[3][N])。
//
//  ---- 剔除判据要"宁可多画不可漏画" ----
//
//  AABB 与视锥的相交测试用"正顶点"法：对每个平面取包围盒在法线方向上最远的
//  那个角点，如果它在平面外侧，则整个盒子在外侧。
//  这个判据会把"与视锥角擦过"的物体判为可见（保守），但绝不会把可见物体
//  判为不可见 —— 后者表现为"物体在视野边缘突然消失"，是最不能接受的错误。
// =============================================================================
#ifndef ENGINE_RENDER_FRUSTUM_H
#define ENGINE_RENDER_FRUSTUM_H

#include "engine/core/mat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

typedef struct plane_t {
    vec3_t normal;
    f32    distance; // 平面方程：dot(normal, p) + distance >= 0 表示在内侧
} plane_t;

typedef enum frustum_plane_t {
    FRUSTUM_PLANE_LEFT = 0,
    FRUSTUM_PLANE_RIGHT,
    FRUSTUM_PLANE_BOTTOM,
    FRUSTUM_PLANE_TOP,
    FRUSTUM_PLANE_NEAR,
    FRUSTUM_PLANE_FAR,
    FRUSTUM_PLANE_COUNT
} frustum_plane_t;

typedef struct frustum_t {
    plane_t planes[FRUSTUM_PLANE_COUNT];
} frustum_t;

// 从视图投影矩阵提取 6 个平面（已归一化）。
frustum_t frustum_from_view_projection(mat4_t view_projection);

// 点的包含判定
bool frustum_contains_point(const frustum_t *frustum, vec3_t point);

// 轴对齐包围盒与视锥是否相交（保守：可能返回"相交"但实际不相交）
bool frustum_intersects_aabb(const frustum_t *frustum, vec3_t bounds_min, vec3_t bounds_max);

// 球与视锥是否相交。球的判据比 AABB 更快（每个平面一次点积），
// 所以骨骼动画的包围球、粒子系统优先用它。
bool frustum_intersects_sphere(const frustum_t *frustum, vec3_t center, f32 radius);

// 把 AABB 从局部空间变换到世界空间（8 个角点变换后重新求包围盒）。
// 旋转过的物体会让包围盒变大 —— 这是保守做法，宁可多画。
void frustum_transform_aabb(mat4_t transform, vec3_t local_min, vec3_t local_max,
                            vec3_t *out_world_min, vec3_t *out_world_max);

#endif // ENGINE_RENDER_FRUSTUM_H
