// =============================================================================
//  engine/core/mat.h —— 3x3 / 4x4 矩阵
//
//  ============================================================================
//   坐标系与投影约定（全引擎唯一权威定义，改动必须同步 docs/M0-设计决策.md）
//  ============================================================================
//
//   世界空间   : 右手系，Y 轴向上，相机默认看向 -Z
//   视角空间   : 相机在原点，前方为 -Z，与 OpenGL 传统一致
//   裁剪空间   : X ∈ [-1,1]，Y ∈ [-1,1]，Z ∈ [0,1]   <-- 注意是 [0,1] 不是 [-1,1]
//   NDC 深度   : [0,1]，近平面 = 0，远平面 = 1
//
//   为什么深度取 [0,1] 而不是 OpenGL 默认的 [-1,1]：
//   Vulkan 与 D3D 原生就是 [0,1]，只有 OpenGL 默认 [-1,1]。如果按 GL 的
//   [-1,1] 设计，等 M13 接 Vulkan 时要么改所有投影矩阵、要么在每个后端插一层
//   修正矩阵，两边的调试成本都很高。反过来，GL 4.5 提供了
//   glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) 可以一次性对齐到 [0,1]。
//   也就是说："按 Vulkan 语义设计，让 GL 去适配"，成本最低。
//
//   存储布局   : 列主序（column-major）。col[c] 是第 c 列，col[c].x 是第 0 行。
//                这与 GLSL 的 mat4 内存布局一致，上传到 GPU 无需转置。
//                代价是手写代码时 M[row][col] 的直觉需要反过来，所以本文件所有
//                矩阵构造都按"列"来写，并在注释里标注每列含义。
//
//   乘法语义   : mat4_mul(a, b) 表示 a * b。对列向量 v，(a*b)*v == a*(b*v)，
//                即先应用 b 再应用 a。模型矩阵写成 T * R * S 就是这个顺序。
// =============================================================================
#ifndef ENGINE_CORE_MAT_H
#define ENGINE_CORE_MAT_H

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

// =============================================================================
//  mat3_t —— 3x3 矩阵，列主序
//
//  每列是一个 vec3_t（16 字节，含对齐填充），因此整体 48 字节而不是 36。
//  多出的 12 字节换来 16 字节对齐，为后续 SIMD 与统一的内存布局留出余地。
//  法线矩阵、旋转、缩放用得上，且数量远少于 mat4，空间开销可接受。
// =============================================================================
typedef struct mat3_t {
    vec3_t col[3];
} mat3_t;

_Static_assert(sizeof(mat3_t) == 48, "mat3_t 必须是 48 字节");
_Static_assert(alignof(mat3_t) == 16, "mat3_t 必须是 16 字节对齐");

static inline mat3_t mat3_zero(void)
{
    mat3_t m;
    m.col[0] = vec3_zero();
    m.col[1] = vec3_zero();
    m.col[2] = vec3_zero();
    return m;
}

static inline mat3_t mat3_identity(void)
{
    mat3_t m     = mat3_zero();
    m.col[0].x   = 1.0f;
    m.col[1].y   = 1.0f;
    m.col[2].z   = 1.0f;
    return m;
}

static inline mat3_t mat3_from_columns(vec3_t c0, vec3_t c1, vec3_t c2)
{
    mat3_t m;
    m.col[0] = c0;
    m.col[1] = c1;
    m.col[2] = c2;
    return m;
}

// 从 9 个标量按"行优先书写顺序"构造，内部自动转成列主序存储。
// 这样调用处可以直接照抄教科书上的矩阵写法，不用心算转置。
static inline mat3_t mat3_from_rows(f32 r0c0, f32 r0c1, f32 r0c2, f32 r1c0, f32 r1c1, f32 r1c2,
                                    f32 r2c0, f32 r2c1, f32 r2c2)
{
    mat3_t m;
    m.col[0] = vec3_make(r0c0, r1c0, r2c0);
    m.col[1] = vec3_make(r0c1, r1c1, r2c1);
    m.col[2] = vec3_make(r0c2, r1c2, r2c2);
    return m;
}

static inline mat3_t mat3_scale(f32 sx, f32 sy, f32 sz)
{
    mat3_t m   = mat3_identity();
    m.col[0].x = sx;
    m.col[1].y = sy;
    m.col[2].z = sz;
    return m;
}

static inline mat3_t mat3_rotation_x(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    // 绕 X 轴：[1 0 0; 0 c -s; 0 s c]
    return mat3_from_rows(1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c);
}

static inline mat3_t mat3_rotation_y(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    // 绕 Y 轴：[c 0 s; 0 1 0; -s 0 c]
    return mat3_from_rows(c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c);
}

static inline mat3_t mat3_rotation_z(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    // 绕 Z 轴：[c -s 0; s c 0; 0 0 1]
    return mat3_from_rows(c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f);
}

static inline mat3_t mat3_transpose(mat3_t m)
{
    // 转置后 col[c] 等于原矩阵的第 c 行。
    // 这里必须用 from_columns：如果用 from_rows 再传一遍同样的 16/9 个值，
    // 等于原地转置两次，函数会退化成"返回原矩阵"的空操作 —— 而且
    // 对称矩阵（旋转、缩放、对角阵）上的测试全都会通过，完全看不出来。
    return mat3_from_columns(vec3_make(m.col[0].x, m.col[1].x, m.col[2].x),
                             vec3_make(m.col[0].y, m.col[1].y, m.col[2].y),
                             vec3_make(m.col[0].z, m.col[1].z, m.col[2].z));
}

static inline mat3_t mat3_mul(mat3_t a, mat3_t b)
{
    mat3_t r;
    for (int c = 0; c < 3; ++c) {
        vec3_t bc = b.col[c];
        r.col[c]  = vec3_add(
            vec3_add(vec3_scale(a.col[0], bc.x), vec3_scale(a.col[1], bc.y)),
            vec3_scale(a.col[2], bc.z));
    }
    return r;
}

static inline vec3_t mat3_mul_vec3(mat3_t m, vec3_t v)
{
    return vec3_add(vec3_add(vec3_scale(m.col[0], v.x), vec3_scale(m.col[1], v.y)),
                    vec3_scale(m.col[2], v.z));
}

static inline f32 mat3_determinant(mat3_t m)
{
    f32 a00 = m.col[0].x, a01 = m.col[1].x, a02 = m.col[2].x;
    f32 a10 = m.col[0].y, a11 = m.col[1].y, a12 = m.col[2].y;
    f32 a20 = m.col[0].z, a21 = m.col[1].z, a22 = m.col[2].z;

    return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
           a02 * (a10 * a21 - a11 * a20);
}

// 行列式为 0（奇异矩阵）时返回零矩阵。不抛错：调用方拿到零矩阵后
// 乘出来的结果必然是零，很容易在测试里发现；抛错反而要在每个调用点写分支。
static inline mat3_t mat3_inverse(mat3_t m)
{
    f32 a00 = m.col[0].x, a01 = m.col[1].x, a02 = m.col[2].x;
    f32 a10 = m.col[0].y, a11 = m.col[1].y, a12 = m.col[2].y;
    f32 a20 = m.col[0].z, a21 = m.col[1].z, a22 = m.col[2].z;

    f32 det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
              a02 * (a10 * a21 - a11 * a20);

    if (det == 0.0f) {
        return mat3_zero();
    }
    f32 inv_det = 1.0f / det;

    mat3_t r;
    // 伴随矩阵转置后各列的值（推导见任意线性代数教材的 3x3 逆矩阵公式）
    r.col[0] = vec3_make((a11 * a22 - a12 * a21) * inv_det, //
                         (a12 * a20 - a10 * a22) * inv_det, //
                         (a10 * a21 - a11 * a20) * inv_det);
    r.col[1] = vec3_make((a02 * a21 - a01 * a22) * inv_det, //
                         (a00 * a22 - a02 * a20) * inv_det, //
                         (a01 * a20 - a00 * a21) * inv_det);
    r.col[2] = vec3_make((a01 * a12 - a02 * a11) * inv_det, //
                         (a02 * a10 - a00 * a12) * inv_det, //
                         (a00 * a11 - a01 * a10) * inv_det);
    return r;
}

static inline bool mat3_equal(mat3_t a, mat3_t b)
{
    return vec3_equal(a.col[0], b.col[0]) && vec3_equal(a.col[1], b.col[1]) &&
           vec3_equal(a.col[2], b.col[2]);
}

static inline bool mat3_near(mat3_t a, mat3_t b, f32 tolerance)
{
    return vec3_near(a.col[0], b.col[0], tolerance) && vec3_near(a.col[1], b.col[1], tolerance) &&
           vec3_near(a.col[2], b.col[2], tolerance);
}

// =============================================================================
//  mat4_t —— 4x4 矩阵，列主序
// =============================================================================
typedef struct mat4_t {
    vec4_t col[4];
} mat4_t;

_Static_assert(sizeof(mat4_t) == 64, "mat4_t 必须是 64 字节");
_Static_assert(alignof(mat4_t) == 16, "mat4_t 必须是 16 字节对齐");

static inline mat4_t mat4_zero(void)
{
    mat4_t m;
    m.col[0] = vec4_zero();
    m.col[1] = vec4_zero();
    m.col[2] = vec4_zero();
    m.col[3] = vec4_zero();
    return m;
}

static inline mat4_t mat4_identity(void)
{
    mat4_t m   = mat4_zero();
    m.col[0].x = 1.0f;
    m.col[1].y = 1.0f;
    m.col[2].z = 1.0f;
    m.col[3].w = 1.0f;
    return m;
}

static inline mat4_t mat4_from_columns(vec4_t c0, vec4_t c1, vec4_t c2, vec4_t c3)
{
    mat4_t m;
    m.col[0] = c0;
    m.col[1] = c1;
    m.col[2] = c2;
    m.col[3] = c3;
    return m;
}

// 从 16 个标量按"行优先书写顺序"构造。调用处可以直接照抄教科书矩阵，
// 不用自己心算转置 —— 这是最容易出错、也最值得包一层的地方。
static inline mat4_t mat4_from_rows(f32 r0c0, f32 r0c1, f32 r0c2, f32 r0c3, //
                                    f32 r1c0, f32 r1c1, f32 r1c2, f32 r1c3, //
                                    f32 r2c0, f32 r2c1, f32 r2c2, f32 r2c3, //
                                    f32 r3c0, f32 r3c1, f32 r3c2, f32 r3c3)
{
    mat4_t m;
    m.col[0] = vec4_make(r0c0, r1c0, r2c0, r3c0);
    m.col[1] = vec4_make(r0c1, r1c1, r2c1, r3c1);
    m.col[2] = vec4_make(r0c2, r1c2, r2c2, r3c2);
    m.col[3] = vec4_make(r0c3, r1c3, r2c3, r3c3);
    return m;
}

static inline mat4_t mat4_from_mat3(mat3_t m)
{
    mat4_t r   = mat4_identity();
    r.col[0].x = m.col[0].x;
    r.col[0].y = m.col[0].y;
    r.col[0].z = m.col[0].z;
    r.col[1].x = m.col[1].x;
    r.col[1].y = m.col[1].y;
    r.col[1].z = m.col[1].z;
    r.col[2].x = m.col[2].x;
    r.col[2].y = m.col[2].y;
    r.col[2].z = m.col[2].z;
    return r;
}

static inline mat3_t mat4_to_mat3(mat4_t m)
{
    mat3_t r;
    r.col[0] = vec3_make(m.col[0].x, m.col[0].y, m.col[0].z);
    r.col[1] = vec3_make(m.col[1].x, m.col[1].y, m.col[1].z);
    r.col[2] = vec3_make(m.col[2].x, m.col[2].y, m.col[2].z);
    return r;
}

static inline mat4_t mat4_translation(vec3_t t)
{
    mat4_t m   = mat4_identity();
    m.col[3].x = t.x;
    m.col[3].y = t.y;
    m.col[3].z = t.z;
    return m;
}

static inline mat4_t mat4_scale(vec3_t s)
{
    mat4_t m   = mat4_identity();
    m.col[0].x = s.x;
    m.col[1].y = s.y;
    m.col[2].z = s.z;
    return m;
}

static inline mat4_t mat4_rotation_x(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    return mat4_from_rows(1.0f, 0.0f, 0.0f, 0.0f, //
                          0.0f, c, -s, 0.0f,       //
                          0.0f, s, c, 0.0f,        //
                          0.0f, 0.0f, 0.0f, 1.0f);
}

static inline mat4_t mat4_rotation_y(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    return mat4_from_rows(c, 0.0f, s, 0.0f,    //
                          0.0f, 1.0f, 0.0f, 0.0f, //
                          -s, 0.0f, c, 0.0f,   //
                          0.0f, 0.0f, 0.0f, 1.0f);
}

static inline mat4_t mat4_rotation_z(f32 radians)
{
    f32 c = cosf(radians);
    f32 s = sinf(radians);
    return mat4_from_rows(c, -s, 0.0f, 0.0f,   //
                          s, c, 0.0f, 0.0f,    //
                          0.0f, 0.0f, 1.0f, 0.0f, //
                          0.0f, 0.0f, 0.0f, 1.0f);
}

// 组合模型矩阵：先缩放，再旋转（3x3），最后平移。等价于 T * R * S。
static inline mat4_t mat4_compose(vec3_t translation, mat3_t rotation, vec3_t scale)
{
    mat4_t m;
    m.col[0] = vec4_make(rotation.col[0].x * scale.x, rotation.col[0].y * scale.x,
                         rotation.col[0].z * scale.x, 0.0f);
    m.col[1] = vec4_make(rotation.col[1].x * scale.y, rotation.col[1].y * scale.y,
                         rotation.col[1].z * scale.y, 0.0f);
    m.col[2] = vec4_make(rotation.col[2].x * scale.z, rotation.col[2].y * scale.z,
                         rotation.col[2].z * scale.z, 0.0f);
    m.col[3] = vec4_make(translation.x, translation.y, translation.z, 1.0f);
    return m;
}

static inline mat4_t mat4_transpose(mat4_t m)
{
    // 同 mat3_transpose：转置后 col[c] 等于原矩阵的第 c 行。
    return mat4_from_columns(vec4_make(m.col[0].x, m.col[1].x, m.col[2].x, m.col[3].x),
                             vec4_make(m.col[0].y, m.col[1].y, m.col[2].y, m.col[3].y),
                             vec4_make(m.col[0].z, m.col[1].z, m.col[2].z, m.col[3].z),
                             vec4_make(m.col[0].w, m.col[1].w, m.col[2].w, m.col[3].w));
}

static inline mat4_t mat4_mul(mat4_t a, mat4_t b)
{
    mat4_t r;
    for (int c = 0; c < 4; ++c) {
        vec4_t bc = b.col[c];
        r.col[c]  = vec4_add(vec4_add(vec4_scale(a.col[0], bc.x), vec4_scale(a.col[1], bc.y)),
                             vec4_add(vec4_scale(a.col[2], bc.z), vec4_scale(a.col[3], bc.w)));
    }
    return r;
}

static inline vec4_t mat4_mul_vec4(mat4_t m, vec4_t v)
{
    return vec4_add(vec4_add(vec4_scale(m.col[0], v.x), vec4_scale(m.col[1], v.y)),
                    vec4_add(vec4_scale(m.col[2], v.z), vec4_scale(m.col[3], v.w)));
}

// 变换一个点：隐含 w = 1，平移会生效。
static inline vec3_t mat4_transform_point(mat4_t m, vec3_t p)
{
    vec4_t r = mat4_mul_vec4(m, vec4_make(p.x, p.y, p.z, 1.0f));
    return vec3_make(r.x, r.y, r.z);
}

// 变换一个方向：隐含 w = 0，平移不生效。
static inline vec3_t mat4_transform_direction(mat4_t m, vec3_t d)
{
    vec4_t r = mat4_mul_vec4(m, vec4_make(d.x, d.y, d.z, 0.0f));
    return vec3_make(r.x, r.y, r.z);
}

// -----------------------------------------------------------------------------
//  行列式与逆
//
//  用局部扁平数组把 4x4 摊开，而不是取 &mat.col[0].x 做指针算术：
//  后者在 C 里严格来说是越界访问成员对象，UBSan 会报，优化器理论上也可以
//  做意外的事。摊成 16 个局部 float 后编译器照样放进寄存器，没有性能损失。
// -----------------------------------------------------------------------------
static inline void mat4_flatten(mat4_t m, f32 out[16])
{
    out[0]  = m.col[0].x;
    out[1]  = m.col[0].y;
    out[2]  = m.col[0].z;
    out[3]  = m.col[0].w;
    out[4]  = m.col[1].x;
    out[5]  = m.col[1].y;
    out[6]  = m.col[1].z;
    out[7]  = m.col[1].w;
    out[8]  = m.col[2].x;
    out[9]  = m.col[2].y;
    out[10] = m.col[2].z;
    out[11] = m.col[2].w;
    out[12] = m.col[3].x;
    out[13] = m.col[3].y;
    out[14] = m.col[3].z;
    out[15] = m.col[3].w;
}

// 计算伴随矩阵的 16 个余子式，返回行列式。
// out_cof 为列主序，语义与 mat4_flatten 一致。
static inline f32 mat4_cofactors(const f32 m[16], f32 out_cof[16])
{
    out_cof[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
                 m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    out_cof[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
                 m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    out_cof[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
                 m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    out_cof[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
                  m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    out_cof[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
                 m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    out_cof[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
                 m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    out_cof[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
                 m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    out_cof[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
                  m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    out_cof[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
                 m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    out_cof[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
                 m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    out_cof[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
                  m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    out_cof[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
                  m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    out_cof[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
                 m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    out_cof[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
                 m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    out_cof[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
                  m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    out_cof[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
                  m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    return m[0] * out_cof[0] + m[1] * out_cof[4] + m[2] * out_cof[8] + m[3] * out_cof[12];
}

static inline f32 mat4_determinant(mat4_t mat)
{
    f32 m[16];
    f32 cof[16];
    mat4_flatten(mat, m);
    return mat4_cofactors(m, cof);
}

// 奇异矩阵返回零矩阵，理由同 mat3_inverse。
static inline mat4_t mat4_inverse(mat4_t mat)
{
    f32 m[16];
    f32 cof[16];
    mat4_flatten(mat, m);

    f32 det = mat4_cofactors(m, cof);
    if (det == 0.0f) {
        return mat4_zero();
    }
    f32 inv_det = 1.0f / det;

    mat4_t r;
    r.col[0] = vec4_make(cof[0] * inv_det, cof[1] * inv_det, cof[2] * inv_det, cof[3] * inv_det);
    r.col[1] = vec4_make(cof[4] * inv_det, cof[5] * inv_det, cof[6] * inv_det, cof[7] * inv_det);
    r.col[2] = vec4_make(cof[8] * inv_det, cof[9] * inv_det, cof[10] * inv_det, cof[11] * inv_det);
    r.col[3] =
        vec4_make(cof[12] * inv_det, cof[13] * inv_det, cof[14] * inv_det, cof[15] * inv_det);
    return r;
}

// 法线矩阵：(M^-1)^T 的左上 3x3。
// 直接用模型矩阵变换法线是错的 —— 非均匀缩放会把法线拉偏，光照会跟着错。
static inline mat3_t mat4_normal_matrix(mat4_t model)
{
    return mat4_to_mat3(mat4_transpose(mat4_inverse(model)));
}

static inline vec3_t mat4_get_translation(mat4_t m)
{
    return vec3_make(m.col[3].x, m.col[3].y, m.col[3].z);
}

static inline bool mat4_equal(mat4_t a, mat4_t b)
{
    return vec4_equal(a.col[0], b.col[0]) && vec4_equal(a.col[1], b.col[1]) &&
           vec4_equal(a.col[2], b.col[2]) && vec4_equal(a.col[3], b.col[3]);
}

static inline bool mat4_near(mat4_t a, mat4_t b, f32 tolerance)
{
    return vec4_near(a.col[0], b.col[0], tolerance) && vec4_near(a.col[1], b.col[1], tolerance) &&
           vec4_near(a.col[2], b.col[2], tolerance) && vec4_near(a.col[3], b.col[3], tolerance);
}

// =============================================================================
//  投影矩阵
//
//  约定见文件头：右手系，相机看 -Z，深度 [0,1]。
// =============================================================================

// 透视投影。fov_y_radians 为垂直视场角。
//
// 产出矩阵（列主序，每列含义）：
//   col0 = ( f/aspect, 0,        0,                     0 )
//   col1 = ( 0,        f,        0,                     0 )
//   col2 = ( 0,        0,        zfar/(znear-zfar),    -1 )
//   col3 = ( 0,        0,        znear*zfar/(znear-zfar), 0 )
//
// 其中 f = 1/tan(fov/2)。col2.w = -1 是"透视除法"的来源：
// 裁剪空间 w 等于 -z_view，即相机前方距离。
static inline mat4_t mat4_perspective(f32 fov_y_radians, f32 aspect, f32 znear, f32 zfar)
{
    ASSERT(aspect > 0.0f);
    ASSERT(znear > 0.0f);
    ASSERT(zfar > znear);

    f32 f = 1.0f / tanf(fov_y_radians * 0.5f);

    mat4_t m = mat4_zero();
    m.col[0].x = f / aspect;
    m.col[1].y = f;
    m.col[2].z = zfar / (znear - zfar);
    m.col[2].w = -1.0f;
    m.col[3].z = (znear * zfar) / (znear - zfar);
    return m;
}

// 正交投影，深度同样映射到 [0,1]。
//   col2 = (0, 0, -1/(zfar-znear), 0)
//   col3 = (..., ..., -znear/(zfar-znear), 1)
static inline mat4_t mat4_ortho(f32 left, f32 right, f32 bottom, f32 top, f32 znear, f32 zfar)
{
    ASSERT(right != left);
    ASSERT(top != bottom);
    ASSERT(zfar != znear);

    mat4_t m = mat4_identity();
    m.col[0].x = 2.0f / (right - left);
    m.col[1].y = 2.0f / (top - bottom);
    m.col[2].z = -1.0f / (zfar - znear);
    m.col[3].x = -(right + left) / (right - left);
    m.col[3].y = -(top + bottom) / (top - bottom);
    m.col[3].z = -znear / (zfar - znear);
    return m;
}

// 2D 专用正交投影：原点在屏幕中心，X 向右，Y 向上，单位 = 像素。
// 深度范围取 [-1, 1]，使 z = 0 的精灵落在深度 0.5 上 —— 这样 2D 内容既有
// 前后排序空间，又能和 3D 内容共享同一个深度缓冲（2.5D 混合渲染的前提）。
static inline mat4_t mat4_ortho_2d(f32 width, f32 height)
{
    ASSERT(width > 0.0f);
    ASSERT(height > 0.0f);

    f32 half_w = width * 0.5f;
    f32 half_h = height * 0.5f;
    return mat4_ortho(-half_w, half_w, -half_h, half_h, -1.0f, 1.0f);
}

// 视图矩阵。
//
// 构造思路：视图矩阵 = 相机变换的逆。
//   相机到世界的旋转 R，其列就是相机的三个基向量（右 s、上 u、后 -f）；
//   逆变换的旋转部分是 R^T，平移部分是 -R^T * eye。
//
// R^T 的第 i 行是 R 的第 i 列，所以 R^T 的行依次是 s、u、-f。
// 列主序存储时，col[c] = R^T 的第 c 列 = (s[c], u[c], -f[c])：
//
//   col0 = ( s.x,  u.x, -f.x, 0)
//   col1 = ( s.y,  u.y, -f.y, 0)
//   col2 = ( s.z,  u.z, -f.z, 0)
//   col3 = (-dot(s,eye), -dot(u,eye), dot(f,eye), 1)
//
// 踩过的坑：最初按"col0 = (s.x, s.y, s.z)"来写，那存进去的其实是 R 而不是
// R^T，也就是少转置了一次。当相机恰好沿坐标轴看向原点时 R 是对称的，
// 测试照样全过；只有相机带斜角时才会暴露 —— 表现为"整个世界被斜着拧了一下"。
// 现在测试里专门用了一台斜视角相机，并对变换前后的距离做了不变量检查。
static inline mat4_t mat4_look_at(vec3_t eye, vec3_t target, vec3_t up)
{
    vec3_t f = vec3_normalize(vec3_sub(target, eye));

    // 视线与 up 平行时 cross 会退化成零向量，此时换一个参考轴。
    // 不做这个兜底的话，相机垂直向上看的瞬间整个场景会变成 NaN。
    vec3_t s = vec3_cross(f, up);
    if (vec3_length_sq(s) < 1.0e-12f) {
        vec3_t fallback_up = (math_abs(f.y) > 0.9f) ? vec3_make(0.0f, 0.0f, 1.0f)
                                                    : vec3_make(0.0f, 1.0f, 0.0f);
        s                  = vec3_cross(f, fallback_up);
    }
    s = vec3_normalize(s);

    vec3_t u = vec3_cross(s, f);

    mat4_t m;
    m.col[0] = vec4_make(s.x, u.x, -f.x, 0.0f);
    m.col[1] = vec4_make(s.y, u.y, -f.y, 0.0f);
    m.col[2] = vec4_make(s.z, u.z, -f.z, 0.0f);
    m.col[3] = vec4_make(-vec3_dot(s, eye), -vec3_dot(u, eye), vec3_dot(f, eye), 1.0f);
    return m;
}

// =============================================================================
//  后端适配辅助
//
//  本引擎的 NDC 约定是 Y 向上、深度 [0,1]。OpenGL 4.5 通过
//  glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) 可以完全对齐，不需要修正矩阵。
//  但某些后端（D3D 的默认 NDC、Vulkan 的 Y 轴方向）Y 轴朝下，需要在投影后
//  再乘一个 Y 翻转。
//
//  提供成显式的函数而不是让后端自己拼，是为了让"这里为什么要有一次翻转"
//  有唯一的解释位置。
// =============================================================================
static inline mat4_t mat4_ndc_y_flip(void)
{
    mat4_t m   = mat4_identity();
    m.col[1].y = -1.0f;
    return m;
}

// 如果后端不支持 [0,1] 深度（例如无法使用 glClipControl 的老 GL），
// 用这个把 [0,1] 重映射到 [-1,1]：ndc_z' = ndc_z * 2 - 1
static inline mat4_t mat4_ndc_depth_to_negative_one(void)
{
    mat4_t m   = mat4_identity();
    m.col[2].z = 2.0f;
    m.col[3].z = -1.0f;
    return m;
}

#endif // ENGINE_CORE_MAT_H
