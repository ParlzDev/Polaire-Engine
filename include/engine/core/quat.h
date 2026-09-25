// =============================================================================
//  engine/core/quat.h —— 四元数
//
//  存储顺序：{x, y, z, w}，其中 (x,y,z) 是向量部分，w 是标量部分。
//  为什么 w 放最后而不是最前：与 vec4_t 的字段顺序保持一致，两者可以互相
//  直接 reinterpret（GPU 上传、SIMD 处理时有用），也不会有人把 w 当成 x。
//
//  ---- 乘法顺序（最容易搞错的地方，先记牢） ----
//
//      quat_mul(a, b)  语义是 a * b（Hamilton 积）
//      作用在向量上时：(a * b) 表示"先应用 b，再应用 a"
//
//  也就是说，要"先绕 X 转 30 度，再绕 Y 转 45 度"，写的是：
//      quat_t q = quat_mul(quat_rotation_y(y), quat_rotation_x(x));
//  顺序写反不会报错，只会让物体朝一个"看起来差不多但就是不对"的方向。
//  测试里用 90 度的整数倍专门验证这个顺序。
//
//  ---- 关于 q 与 -q ----
//  q 和 -q 表示同一个旋转（双覆盖）。因此比较两个四元数是否"相同旋转"时，
//  必须同时考虑取负的情况，否则会因为路径不同而误判。quat_near 已处理。
// =============================================================================
#ifndef ENGINE_CORE_QUAT_H
#define ENGINE_CORE_QUAT_H

#include "engine/core/mat.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

typedef struct quat_t {
    _Alignas(16) f32 x;
    f32 y;
    f32 z;
    f32 w;
} quat_t;

_Static_assert(sizeof(quat_t) == 16, "quat_t 必须是 16 字节");
_Static_assert(alignof(quat_t) == 16, "quat_t 必须是 16 字节对齐");

static inline quat_t quat_make(f32 x, f32 y, f32 z, f32 w)
{
    quat_t q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}

static inline quat_t quat_identity(void)
{
    return quat_make(0.0f, 0.0f, 0.0f, 1.0f);
}

static inline vec3_t quat_vector_part(quat_t q)
{
    return vec3_make(q.x, q.y, q.z);
}

// 绕单位轴 axis 旋转 radians 弧度。axis 必须已归一化。
static inline quat_t quat_from_axis_angle(vec3_t axis, f32 radians)
{
    ASSERT(vec3_is_normalized(axis, 1.0e-3f));

    f32 half = radians * 0.5f;
    f32 s    = sinf(half);
    return quat_make(axis.x * s, axis.y * s, axis.z * s, cosf(half));
}

static inline quat_t quat_rotation_x(f32 radians)
{
    return quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), radians);
}

static inline quat_t quat_rotation_y(f32 radians)
{
    return quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), radians);
}

static inline quat_t quat_rotation_z(f32 radians)
{
    return quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), radians);
}

// 欧拉角 -> 四元数。
//
// 约定：按 X → Y → Z 的顺序，依次绕**固定的世界轴**旋转。
// 等价于 q = qz * qy * qx，也等价于"内旋 ZYX"。
// 参数名用 x/y/z 而不是 pitch/yaw/roll，因为后者的对应关系在不同引擎里
// 完全不一致，直接说轴更不容易误解。
static inline quat_t quat_from_euler(f32 x_radians, f32 y_radians, f32 z_radians)
{
    // 直接展开 qz * qy * qx 的 Hamilton 积，而不是构造三个四元数再乘两次。
    // 结果与 quat_mul(qz, quat_mul(qy, qx)) 逐位一致，测试里有对照验证。
    f32 cx = cosf(x_radians * 0.5f), sx = sinf(x_radians * 0.5f);
    f32 cy = cosf(y_radians * 0.5f), sy = sinf(y_radians * 0.5f);
    f32 cz = cosf(z_radians * 0.5f), sz = sinf(z_radians * 0.5f);

    quat_t r;
    r.w = cz * cy * cx + sz * sy * sx;
    r.x = cz * cy * sx - sz * sy * cx;
    r.y = cz * sy * cx + sz * cy * sx;
    r.z = sz * cy * cx - cz * sy * sx;
    return r;
}

// 四元数 -> 欧拉角，与 quat_from_euler 互逆（万向锁附近除外）。
static inline vec3_t quat_to_euler(quat_t q);

static inline mat3_t quat_to_mat3(quat_t q)
{
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    mat3_t m;
    // col[c] = 第 c 列 = (R[0][c], R[1][c], R[2][c])
    m.col[0] = vec3_make(1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy));
    m.col[1] = vec3_make(2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx));
    m.col[2] = vec3_make(2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy));
    return m;
}

static inline mat4_t quat_to_mat4(quat_t q)
{
    return mat4_from_mat3(quat_to_mat3(q));
}

// 旋转矩阵 -> 四元数（Shepperd 方法）。
//
// 为什么要分四种情况：直接解 w = sqrt(1+trace)/2 在 trace 接近 -1 时
// 会因为开根号下的负数而失去精度甚至产生 NaN。这里总是挑对角线上最大的
// 那个分量来开根，保证被除数不会接近 0。
static inline quat_t quat_from_mat3(mat3_t m)
{
    f32 r00 = m.col[0].x, r01 = m.col[1].x, r02 = m.col[2].x;
    f32 r10 = m.col[0].y, r11 = m.col[1].y, r12 = m.col[2].y;
    f32 r20 = m.col[0].z, r21 = m.col[1].z, r22 = m.col[2].z;

    f32 trace = r00 + r11 + r22;

    if (trace > 0.0f) {
        f32 s = sqrtf(trace + 1.0f) * 2.0f; // s = 4w
        return quat_make((r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s);
    }
    if (r00 > r11 && r00 > r22) {
        f32 s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f; // s = 4x
        return quat_make(0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s);
    }
    if (r11 > r22) {
        f32 s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f; // s = 4y
        return quat_make((r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s);
    }
    f32 s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f; // s = 4z
    return quat_make((r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s);
}

static inline quat_t quat_from_mat4(mat4_t m)
{
    return quat_from_mat3(mat4_to_mat3(m));
}

static inline vec3_t quat_to_euler(quat_t q)
{
    mat3_t m = quat_to_mat3(q);

    // R = Rz*Ry*Rx 时：
    //   R[2][0] = -sin(y)      -> y
    //   R[2][1], R[2][2]       -> x
    //   R[1][0], R[0][0]       -> z
    f32 r00 = m.col[0].x;
    f32 r10 = m.col[0].y;
    f32 r20 = m.col[0].z;
    f32 r21 = m.col[1].z;
    f32 r22 = m.col[2].z;

    f32 sy = math_clamp(-r20, -1.0f, 1.0f);

    vec3_t e;
    e.y = asinf(sy);
    if (math_abs(sy) < 0.99999f) {
        e.x = atan2f(r21, r22);
        e.z = atan2f(r10, r00);
    } else {
        // 万向锁：Y 轴旋转 ±90 度时 X 与 Z 退化成一个自由度。
        // 惯例是让 z = 0，把全部旋转量归到 x。
        e.x = atan2f(-m.col[2].y, m.col[1].y);
        e.z = 0.0f;
    }
    return e;
}

// ---------------------------------------------------------------- 算术
static inline quat_t quat_add(quat_t a, quat_t b)
{
    return quat_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

static inline quat_t quat_sub(quat_t a, quat_t b)
{
    return quat_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
}

static inline quat_t quat_scale(quat_t q, f32 s)
{
    return quat_make(q.x * s, q.y * s, q.z * s, q.w * s);
}

static inline quat_t quat_negate(quat_t q)
{
    return quat_make(-q.x, -q.y, -q.z, -q.w);
}

// Hamilton 积。见文件头的顺序说明。
static inline quat_t quat_mul(quat_t a, quat_t b)
{
    return quat_make(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

static inline f32 quat_dot(quat_t a, quat_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

static inline f32 quat_length_sq(quat_t q)
{
    return quat_dot(q, q);
}

static inline f32 quat_length(quat_t q)
{
    return sqrtf(quat_length_sq(q));
}

static inline quat_t quat_normalize(quat_t q)
{
    f32 len_sq = quat_length_sq(q);
    if (len_sq <= 0.0f) {
        return quat_identity();
    }
    return quat_scale(q, 1.0f / sqrtf(len_sq));
}

static inline quat_t quat_conjugate(quat_t q)
{
    return quat_make(-q.x, -q.y, -q.z, q.w);
}

// 数学意义上的逆：q* / |q|²，满足 q * q⁻¹ = 1。
// 注意：对非单位四元数，结果本身也不是单位四元数（模长为 1/|q|）。
// 只有单位四元数才满足"逆 == 共轭"，此时才能直接拿它去旋转向量。
static inline quat_t quat_inverse(quat_t q)
{
    f32 len_sq = quat_length_sq(q);
    if (len_sq <= 0.0f) {
        return quat_identity();
    }
    f32 inv = 1.0f / len_sq;
    return quat_make(-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv);
}

// 用四元数旋转向量。使用展开形式避免两次四元数乘法。
//   t  = 2 * cross(q.xyz, v)
//   v' = v + q.w * t + cross(q.xyz, t)
//
// 前提：q 必须是单位四元数。这个展开式只在 |q| = 1 时才等价于 q·v·q*；
// 传入非单位四元数不会报错，只会让结果既被旋转又被缩放，而且缩放系数
// 取决于 q 的模长 —— 非常难查。Debug 下用断言把它拦住。
static inline vec3_t quat_rotate_vec3(quat_t q, vec3_t v)
{
    ASSERT_MSG(math_near(quat_length_sq(q), 1.0f, 1.0e-3f),
               "quat_rotate_vec3 需要单位四元数，当前模长平方 = %f", (f64)quat_length_sq(q));

    vec3_t u = quat_vector_part(q);
    vec3_t t = vec3_scale(vec3_cross(u, v), 2.0f);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), vec3_cross(u, t));
}

static inline quat_t quat_lerp(quat_t a, quat_t b, f32 t)
{
    return quat_make(math_lerp(a.x, b.x, t), math_lerp(a.y, b.y, t), math_lerp(a.z, b.z, t),
                     math_lerp(a.w, b.w, t));
}

// 归一化线性插值。比 slerp 快，但角速度不均匀；t 接近 0/1 时视觉差异可忽略。
static inline quat_t quat_nlerp(quat_t a, quat_t b, f32 t)
{
    // 走短弧：点积为负说明 b 在"另一侧"，取负后插值距离更短。
    if (quat_dot(a, b) < 0.0f) {
        b = quat_negate(b);
    }
    return quat_normalize(quat_lerp(a, b, t));
}

// 球面线性插值。角速度恒定，用于相机、骨骼动画这类对匀速敏感的场景。
static inline quat_t quat_slerp(quat_t a, quat_t b, f32 t)
{
    f32 cos_theta = quat_dot(a, b);

    if (cos_theta < 0.0f) {
        b         = quat_negate(b);
        cos_theta = -cos_theta;
    }

    // 夹角极小：sin(theta) 接近 0，除它会产生数值爆炸，退化为 nlerp。
    if (cos_theta > 0.9995f) {
        return quat_normalize(quat_lerp(a, b, t));
    }

    f32 theta     = acosf(cos_theta);
    f32 sin_theta = sinf(theta);

    f32 wa = sinf((1.0f - t) * theta) / sin_theta;
    f32 wb = sinf(t * theta) / sin_theta;

    return quat_make(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                     a.w * wa + b.w * wb);
}

// 旋转角度（弧度），范围 [0, PI]。
static inline f32 quat_angle(quat_t q)
{
    f32 w = math_clamp(math_abs(q.w), 0.0f, 1.0f);
    return 2.0f * acosf(w);
}

// 旋转轴。恒等旋转时返回 (0,0,1)。
static inline vec3_t quat_axis(quat_t q)
{
    vec3_t v = quat_vector_part(q);
    if (vec3_length_sq(v) < 1.0e-12f) {
        return vec3_make(0.0f, 0.0f, 1.0f);
    }
    return vec3_normalize(v);
}

// 从单位向量 from 旋转到单位向量 to 的最短旋转。
static inline quat_t quat_from_to(vec3_t from, vec3_t to)
{
    f32 d = vec3_dot(from, to);

    if (d >= 1.0f - 1.0e-6f) {
        return quat_identity();
    }
    if (d <= -1.0f + 1.0e-6f) {
        // 反向：需要绕任意一条垂直轴转 180 度。先试 X 轴，退化再试 Y 轴。
        vec3_t axis = vec3_cross(vec3_make(1.0f, 0.0f, 0.0f), from);
        if (vec3_length_sq(axis) < 1.0e-6f) {
            axis = vec3_cross(vec3_make(0.0f, 1.0f, 0.0f), from);
        }
        return quat_from_axis_angle(vec3_normalize(axis), F32_PI);
    }

    vec3_t axis = vec3_cross(from, to);
    f32    w    = 1.0f + d;
    return quat_normalize(quat_make(axis.x, axis.y, axis.z, w));
}

// 构造一个"看向 forward 方向"的旋转，用于相机与朝向类实体。
// 语义：把局部 -Z 轴对齐到 forward，局部 +Y 轴尽量对齐 up。
static inline quat_t quat_look_rotation(vec3_t forward, vec3_t up)
{
    vec3_t z = vec3_normalize(vec3_negate(forward));
    vec3_t x = vec3_cross(up, z);

    if (vec3_length_sq(x) < 1.0e-12f) {
        vec3_t fallback_up = (math_abs(z.y) > 0.9f) ? vec3_make(0.0f, 0.0f, 1.0f)
                                                    : vec3_make(0.0f, 1.0f, 0.0f);
        x                  = vec3_cross(fallback_up, z);
    }
    x = vec3_normalize(x);

    vec3_t y = vec3_cross(z, x);

    mat3_t basis;
    basis.col[0] = x;
    basis.col[1] = y;
    basis.col[2] = z;
    return quat_from_mat3(basis);
}

static inline bool quat_equal(quat_t a, quat_t b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// 注意：同时接受 q 与 -q，因为二者表示同一个旋转。
static inline bool quat_near(quat_t a, quat_t b, f32 tolerance)
{
    bool same = math_near(a.x, b.x, tolerance) && math_near(a.y, b.y, tolerance) &&
                math_near(a.z, b.z, tolerance) && math_near(a.w, b.w, tolerance);
    bool negated = math_near(a.x, -b.x, tolerance) && math_near(a.y, -b.y, tolerance) &&
                   math_near(a.z, -b.z, tolerance) && math_near(a.w, -b.w, tolerance);
    return same || negated;
}

// ---------------------------------------------------------------- 与 mat4 的桥接

// 由 TRS 组装模型矩阵：先缩放，再旋转，最后平移。
// 等价于 mat4_mul(mat4_translation(pos), mat4_mul(quat_to_mat4(rot), mat4_scale(scale)))
static inline mat4_t mat4_from_trs(vec3_t position, quat_t rotation, vec3_t scale)
{
    return mat4_compose(position, quat_to_mat3(rotation), scale);
}

#endif // ENGINE_CORE_QUAT_H
