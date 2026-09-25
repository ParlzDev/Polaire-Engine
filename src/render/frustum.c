// =============================================================================
//  src/render/frustum.c —— 视锥提取与剔除
// =============================================================================
#include "engine/render/frustum.h"

#include <string.h>

#include "engine/core/assert.h"

// 把 (a, b, c, d) 归一化成平面。法线长度接近 0 时返回一个"永远在内侧"的
// 退化平面，这样它不会误剔除任何东西 —— 退化的投影矩阵（例如宽高比为 0）
// 应该导致"全都画"而不是"全都不画"。
//
// 注意退化时 distance 必须是 **+F32_MAX**：
// 判据是 `dot(normal, p) + distance < 0` 就剔除，而退化平面的 normal 是零向量，
// 于是判据退化成 `distance < 0`。取 -F32_MAX 的话它对任何点都成立，
// 结果是"一个退化平面就把整个场景剔干净了" —— 与注释的意图正好相反。
static plane_t make_plane(f32 a, f32 b, f32 c, f32 d)
{
    plane_t plane;
    const f32 length = sqrtf(a * a + b * b + c * c);
    if (length < 1.0e-8f) {
        plane.normal = vec3_zero();
        plane.distance = F32_MAX;
        return plane;
    }

    const f32 inv = 1.0f / length;
    plane.normal = vec3_make(a * inv, b * inv, c * inv);
    plane.distance = d * inv;
    return plane;
}

frustum_t frustum_from_view_projection(mat4_t view_projection)
{
    // 行 N = 各列的 N 号分量。列主序下这一步很容易写错，
    // 所以显式展开成 4 个 vec4，而不是在循环里索引。
    const vec4_t row0 = vec4_make(view_projection.col[0].x, view_projection.col[1].x,
                                  view_projection.col[2].x, view_projection.col[3].x);
    const vec4_t row1 = vec4_make(view_projection.col[0].y, view_projection.col[1].y,
                                  view_projection.col[2].y, view_projection.col[3].y);
    const vec4_t row2 = vec4_make(view_projection.col[0].z, view_projection.col[1].z,
                                  view_projection.col[2].z, view_projection.col[3].z);
    const vec4_t row3 = vec4_make(view_projection.col[0].w, view_projection.col[1].w,
                                  view_projection.col[2].w, view_projection.col[3].w);

    frustum_t frustum;

    const vec4_t left = vec4_add(row3, row0);
    const vec4_t right = vec4_sub(row3, row0);
    const vec4_t bottom = vec4_add(row3, row1);
    const vec4_t top = vec4_sub(row3, row1);
    // 深度范围是 [0,1]，所以近平面就是 z >= 0（不是 z >= -w）
    const vec4_t near_plane = row2;
    const vec4_t far_plane = vec4_sub(row3, row2);

    frustum.planes[FRUSTUM_PLANE_LEFT] = make_plane(left.x, left.y, left.z, left.w);
    frustum.planes[FRUSTUM_PLANE_RIGHT] = make_plane(right.x, right.y, right.z, right.w);
    frustum.planes[FRUSTUM_PLANE_BOTTOM] = make_plane(bottom.x, bottom.y, bottom.z, bottom.w);
    frustum.planes[FRUSTUM_PLANE_TOP] = make_plane(top.x, top.y, top.z, top.w);
    frustum.planes[FRUSTUM_PLANE_NEAR] =
        make_plane(near_plane.x, near_plane.y, near_plane.z, near_plane.w);
    frustum.planes[FRUSTUM_PLANE_FAR] = make_plane(far_plane.x, far_plane.y, far_plane.z,
                                                   far_plane.w);

    return frustum;
}

bool frustum_contains_point(const frustum_t *frustum, vec3_t point)
{
    ASSERT(frustum != NULL);

    for (u32 i = 0; i < FRUSTUM_PLANE_COUNT; ++i) {
        const plane_t *plane = &frustum->planes[i];
        if (vec3_dot(plane->normal, point) + plane->distance < 0.0f) {
            return false;
        }
    }
    return true;
}

bool frustum_intersects_aabb(const frustum_t *frustum, vec3_t bounds_min, vec3_t bounds_max)
{
    ASSERT(frustum != NULL);

    for (u32 i = 0; i < FRUSTUM_PLANE_COUNT; ++i) {
        const plane_t *plane = &frustum->planes[i];

        // 取"正顶点"：包围盒在法线方向上最远的那个角点。
        // 如果连它都在平面外侧，整个盒子必然在外侧。
        const vec3_t positive = vec3_make((plane->normal.x >= 0.0f) ? bounds_max.x : bounds_min.x,
                                          (plane->normal.y >= 0.0f) ? bounds_max.y : bounds_min.y,
                                          (plane->normal.z >= 0.0f) ? bounds_max.z : bounds_min.z);

        if (vec3_dot(plane->normal, positive) + plane->distance < 0.0f) {
            return false;
        }
    }
    return true;
}

bool frustum_intersects_sphere(const frustum_t *frustum, vec3_t center, f32 radius)
{
    ASSERT(frustum != NULL);

    for (u32 i = 0; i < FRUSTUM_PLANE_COUNT; ++i) {
        const plane_t *plane = &frustum->planes[i];
        if (vec3_dot(plane->normal, center) + plane->distance < -radius) {
            return false;
        }
    }
    return true;
}

void frustum_transform_aabb(mat4_t transform, vec3_t local_min, vec3_t local_max,
                            vec3_t *out_world_min, vec3_t *out_world_max)
{
    ASSERT(out_world_min != NULL);
    ASSERT(out_world_max != NULL);

    vec3_t world_min = vec3_splat(F32_MAX);
    vec3_t world_max = vec3_splat(-F32_MAX);

    // 8 个角点全部变换后再求包围盒。
    // 只变换 min/max 两个点会得到错误的结果（旋转 45 度就能看出来）——
    // 表现为"物体转到某些角度就被错误剔除"。
    for (u32 i = 0; i < 8; ++i) {
        const vec3_t corner = vec3_make((i & 1u) ? local_max.x : local_min.x,
                                        (i & 2u) ? local_max.y : local_min.y,
                                        (i & 4u) ? local_max.z : local_min.z);
        const vec3_t world = mat4_transform_point(transform, corner);
        world_min = vec3_min(world_min, world);
        world_max = vec3_max(world_max, world);
    }

    *out_world_min = world_min;
    *out_world_max = world_max;
}
