// =============================================================================
//  src/render/sprite.c —— 精灵
// =============================================================================
#include "engine/render/sprite.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/quat.h"

sprite_t sprite_default(void)
{
    sprite_t sprite;
    memset(&sprite, 0, sizeof(sprite));
    sprite.position = vec2_zero();
    sprite.size = vec2_make(1.0f, 1.0f);
    sprite.rotation = 0.0f;
    sprite.scale = vec2_one();
    sprite.color = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    sprite.uv_min = vec2_make(0.0f, 0.0f);
    sprite.uv_max = vec2_make(1.0f, 1.0f);
    return sprite;
}

sprite_t sprite_make(vec2_t position, vec2_t size)
{
    sprite_t sprite = sprite_default();
    sprite.position = position;
    sprite.size = size;
    return sprite;
}

sprite_t sprite_with_region(vec2_t position, vec2_t size, const atlas_region_t *region)
{
    sprite_t sprite = sprite_make(position, size);
    if (region != NULL) {
        sprite.uv_min = region->uv_min;
        sprite.uv_max = region->uv_max;
    }
    return sprite;
}

void sprite_set_flip(sprite_t *sprite, bool flip_x, bool flip_y)
{
    ASSERT(sprite != NULL);

    // 交换 UV 而不是把 scale 取负。
    // 负缩放会把三角形的绕序反过来，开了背面剔除的管线会让精灵整个消失 ——
    // 这个 bug 的表现是"翻转之后就看不见了"，很容易误以为是别的问题。
    if (flip_x) {
        const f32 tmp = sprite->uv_min.x;
        sprite->uv_min.x = sprite->uv_max.x;
        sprite->uv_max.x = tmp;
    }
    if (flip_y) {
        const f32 tmp = sprite->uv_min.y;
        sprite->uv_min.y = sprite->uv_max.y;
        sprite->uv_max.y = tmp;
    }
}

void sprite_corners(const sprite_t *sprite, vec2_t out_corners[4])
{
    ASSERT(sprite != NULL);
    ASSERT(out_corners != NULL);

    const f32 half_w = sprite->size.x * 0.5f * sprite->scale.x;
    const f32 half_h = sprite->size.y * 0.5f * sprite->scale.y;

    const f32 c = cosf(sprite->rotation);
    const f32 s = sinf(sprite->rotation);

    // 局部坐标（Y 向上）：左下、右下、右上、左上 —— 逆时针
    static const f32 k_local[4][2] = {
        {-1.0f, -1.0f},
        {1.0f, -1.0f},
        {1.0f, 1.0f},
        {-1.0f, 1.0f},
    };

    for (u32 i = 0; i < 4; ++i) {
        const f32 lx = k_local[i][0] * half_w;
        const f32 ly = k_local[i][1] * half_h;
        out_corners[i].x = sprite->position.x + lx * c - ly * s;
        out_corners[i].y = sprite->position.y + lx * s + ly * c;
    }
}

mat4_t sprite_model_matrix(const sprite_t *sprite)
{
    ASSERT(sprite != NULL);

    const quat_t rotation = quat_rotation_z(sprite->rotation);
    const vec3_t position = vec3_make(sprite->position.x, sprite->position.y, 0.0f);
    const vec3_t scale = vec3_make(sprite->size.x * sprite->scale.x,
                                   sprite->size.y * sprite->scale.y, 1.0f);

    // 精灵的局部空间是 [-0.5, 0.5]^2 的单位四边形，
    // 所以 size 直接作为缩放用。
    return mat4_from_trs(position, rotation, scale);
}

void sprite_bounds(const sprite_t *sprite, vec2_t *out_min, vec2_t *out_max)
{
    ASSERT(sprite != NULL);

    vec2_t corners[4];
    sprite_corners(sprite, corners);

    vec2_t min_value = corners[0];
    vec2_t max_value = corners[0];
    for (u32 i = 1; i < 4; ++i) {
        min_value = vec2_min(min_value, corners[i]);
        max_value = vec2_max(max_value, corners[i]);
    }

    if (out_min != NULL) {
        *out_min = min_value;
    }
    if (out_max != NULL) {
        *out_max = max_value;
    }
}

bool sprite_intersects_rect(const sprite_t *sprite, vec2_t rect_min, vec2_t rect_max)
{
    ASSERT(sprite != NULL);

    // 用轴对齐包围盒做粗判。旋转精灵在四个角都不在矩形内但中心在矩形内时
    // 会被判为"不相交"，但 2D 剔除的代价是"偶尔多画一个精灵"，
    // 远小于"漏画一个可见精灵"的代价 —— 所以宁可保守（多画）。
    vec2_t sprite_min;
    vec2_t sprite_max;
    sprite_bounds(sprite, &sprite_min, &sprite_max);

    return !(sprite_max.x < rect_min.x || sprite_min.x > rect_max.x ||
             sprite_max.y < rect_min.y || sprite_min.y > rect_max.y);
}

u32 sprite_pack_color(vec4_t color)
{
    const u32 r = (u32)(math_saturate(color.x) * 255.0f + 0.5f);
    const u32 g = (u32)(math_saturate(color.y) * 255.0f + 0.5f);
    const u32 b = (u32)(math_saturate(color.z) * 255.0f + 0.5f);
    const u32 a = (u32)(math_saturate(color.w) * 255.0f + 0.5f);

    // 小端序下这个 u32 在内存里的字节顺序就是 R,G,B,A，
    // 与 GL 的 GL_UNSIGNED_BYTE + normalized 读取顺序一致。
    return r | (g << 8) | (b << 16) | (a << 24);
}
