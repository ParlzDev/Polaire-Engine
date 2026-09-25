// =============================================================================
//  engine/core/transform.h —— 位置/旋转/缩放
//
//  为什么用四元数而不是欧拉角存储旋转：
//  欧拉角看起来直观，但三个角度之间存在万向锁与插值路径问题。把欧拉角当作
//  "存储格式"几乎必然会在某一天引入"物体转到某个角度就抖一下"的 bug。
//  四元数存的是旋转本身，插值、合成都是良定义的。欧拉角只应该出现在
//  "给人看的输入/输出"边界上（编辑器面板、配置文件的 rotation 字段）。
//
//  层级变换（父子关系、场景图）不在 M0 范围内，M5 的 ECS + 场景层再引入。
// =============================================================================
#ifndef ENGINE_CORE_TRANSFORM_H
#define ENGINE_CORE_TRANSFORM_H

#include "engine/core/mat.h"
#include "engine/core/quat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

typedef struct transform_t {
    vec3_t position;
    quat_t rotation;
    vec3_t scale;
} transform_t;

_Static_assert(sizeof(transform_t) == 48, "transform_t 必须是 48 字节");
_Static_assert(alignof(transform_t) == 16, "transform_t 必须是 16 字节对齐");

static inline transform_t transform_identity(void)
{
    transform_t t;
    t.position = vec3_zero();
    t.rotation = quat_identity();
    t.scale    = vec3_one();
    return t;
}

static inline transform_t transform_make(vec3_t position, quat_t rotation, vec3_t scale)
{
    transform_t t;
    t.position = position;
    t.rotation = rotation;
    t.scale    = scale;
    return t;
}

static inline mat4_t transform_to_mat4(transform_t t)
{
    return mat4_from_trs(t.position, t.rotation, t.scale);
}

// 变换一个点（受缩放、旋转、平移影响）
static inline vec3_t transform_point(transform_t t, vec3_t p)
{
    return vec3_add(t.position, quat_rotate_vec3(t.rotation, vec3_mul(p, t.scale)));
}

// 变换一个方向（不受平移影响，受缩放影响）
static inline vec3_t transform_direction(transform_t t, vec3_t d)
{
    return quat_rotate_vec3(t.rotation, vec3_mul(d, t.scale));
}

static inline bool transform_equal(transform_t a, transform_t b)
{
    return vec3_equal(a.position, b.position) && quat_equal(a.rotation, b.rotation) &&
           vec3_equal(a.scale, b.scale);
}

static inline bool transform_near(transform_t a, transform_t b, f32 tolerance)
{
    return vec3_near(a.position, b.position, tolerance) && quat_near(a.rotation, b.rotation, tolerance) &&
           vec3_near(a.scale, b.scale, tolerance);
}

#endif // ENGINE_CORE_TRANSFORM_H
