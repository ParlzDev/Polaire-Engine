// =============================================================================
//  engine/render/sprite.h —— 精灵
//
//  精灵是**值类型**而不是句柄：它每帧都会被复制到批渲染器的顶点缓冲里，
//  句柄化只会多一层间接。真正需要资源化的是纹理与管线。
//
//  ---- 坐标系 ----
//
//  精灵活在 2D 世界里。用哪套坐标由 2D 相机决定（见 camera_2d.h）：
//  默认情况下 1 世界单位 = 1 像素，原点在屏幕中心，Y 轴向上。
//  这与 M0 定的坐标系一致（右手系、Y 向上、相机看 -Z）。
//
//  ---- 为什么旋转用 f32 而不是四元数 ----
//
//  2D 只有一个旋转自由度，四元数在这里是纯负担（16 字节 vs 4 字节，
//  而且每个顶点都要做四元数旋转）。用角度直接算 cos/sin 更快也更直观。
// =============================================================================
#ifndef ENGINE_RENDER_SPRITE_H
#define ENGINE_RENDER_SPRITE_H

#include "engine/core/mat.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"
#include "engine/render/texture_atlas.h"

typedef struct sprite_t {
    vec2_t position; // 中心点
    vec2_t size;     // 宽高（未经 scale）
    f32    rotation; // 弧度，逆时针为正
    vec2_t scale;
    vec4_t color;    // 颜色调制（RGB 乘算，A 用作透明度）
    vec2_t uv_min;
    vec2_t uv_max;
} sprite_t;

// 默认精灵：位置原点、尺寸 1x1、无旋转、无缩放、白色、整张纹理
sprite_t sprite_default(void);

sprite_t sprite_make(vec2_t position, vec2_t size);

// 用图集区域填好 UV
sprite_t sprite_with_region(vec2_t position, vec2_t size, const atlas_region_t *region);

// 水平/垂直翻转（交换 UV）。用 UV 交换而不是负缩放：
// 负缩放会把三角形的绕序反过来，背面剔除下精灵会整个消失。
void sprite_set_flip(sprite_t *sprite, bool flip_x, bool flip_y);

// 四个角的世界坐标，顺序：左下、右下、右上、左上（Y 向上时是逆时针）。
// 批渲染器直接用它生成顶点 —— 比走一遍模型矩阵再变换要快，
// 而且省掉了每个精灵一个 mat4（64 字节）。
void sprite_corners(const sprite_t *sprite, vec2_t out_corners[4]);

// 等价的模型矩阵。只有需要把精灵当作普通 3D 物体处理时才用得到。
mat4_t sprite_model_matrix(const sprite_t *sprite);

// 精灵的轴对齐包围盒（用于剔除）
void sprite_bounds(const sprite_t *sprite, vec2_t *out_min, vec2_t *out_max);

// 判断精灵是否与给定矩形相交（视锥剔除用，2D 下就是矩形相交测试）
bool sprite_intersects_rect(const sprite_t *sprite, vec2_t rect_min, vec2_t rect_max);

// 把 vec4 颜色打包成 RGBA8（顶点格式里的 u32）。
// 内存顺序是 R,G,B,A —— 与 GL 的 GL_UNSIGNED_BYTE 归一化格式一致。
u32 sprite_pack_color(vec4_t color);

#endif // ENGINE_RENDER_SPRITE_H
