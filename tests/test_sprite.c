// =============================================================================
//  tests/test_sprite.c —— 精灵与 2D 相机
//
//  这些都是纯数学，不需要 GL 上下文 —— 这一点很重要：
//  让单元测试依赖显卡驱动是 CI 不稳定的头号原因。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

// =============================================================================
//  精灵
// =============================================================================
static void sprite_defaults_are_sane(void)
{
    const sprite_t sprite = sprite_default();

    EXPECT_VEC2_NEAR(sprite.position, vec2_zero(), 0.0f);
    EXPECT_VEC2_NEAR(sprite.size, vec2_one(), 0.0f);
    EXPECT_NEAR(sprite.rotation, 0.0f, 0.0f);
    EXPECT_VEC2_NEAR(sprite.scale, vec2_one(), 0.0f);
    EXPECT_VEC4_NEAR(sprite.color, vec4_one(), 0.0f);
    // 默认铺满整张纹理
    EXPECT_VEC2_NEAR(sprite.uv_min, vec2_zero(), 0.0f);
    EXPECT_VEC2_NEAR(sprite.uv_max, vec2_one(), 0.0f);
}

static void sprite_make_sets_position_and_size(void)
{
    const sprite_t sprite = sprite_make(vec2_make(100.0f, 50.0f), vec2_make(32.0f, 64.0f));
    EXPECT_VEC2_NEAR(sprite.position, vec2_make(100.0f, 50.0f), 0.0f);
    EXPECT_VEC2_NEAR(sprite.size, vec2_make(32.0f, 64.0f), 0.0f);
    // 其余字段保持默认
    EXPECT_VEC4_NEAR(sprite.color, vec4_one(), 0.0f);
}

static void sprite_with_region_copies_uv(void)
{
    atlas_region_t region;
    memset(&region, 0, sizeof(region));
    region.uv_min = vec2_make(0.25f, 0.5f);
    region.uv_max = vec2_make(0.5f, 0.75f);

    const sprite_t sprite = sprite_with_region(vec2_zero(), vec2_one(), &region);
    EXPECT_VEC2_NEAR(sprite.uv_min, vec2_make(0.25f, 0.5f), 0.0f);
    EXPECT_VEC2_NEAR(sprite.uv_max, vec2_make(0.5f, 0.75f), 0.0f);

    // NULL 区域不能崩，且保持默认 UV
    const sprite_t fallback = sprite_with_region(vec2_zero(), vec2_one(), NULL);
    EXPECT_VEC2_NEAR(fallback.uv_min, vec2_zero(), 0.0f);
    EXPECT_VEC2_NEAR(fallback.uv_max, vec2_one(), 0.0f);
}

static void sprite_flip_swaps_uv(void)
{
    sprite_t sprite = sprite_default();

    sprite_set_flip(&sprite, true, false);
    EXPECT_VEC2_NEAR(sprite.uv_min, vec2_make(1.0f, 0.0f), 0.0f);
    EXPECT_VEC2_NEAR(sprite.uv_max, vec2_make(0.0f, 1.0f), 0.0f);

    // 再翻一次回到原状
    sprite_set_flip(&sprite, true, false);
    EXPECT_VEC2_NEAR(sprite.uv_min, vec2_zero(), 0.0f);
    EXPECT_VEC2_NEAR(sprite.uv_max, vec2_one(), 0.0f);

    sprite_set_flip(&sprite, false, true);
    EXPECT_VEC2_NEAR(sprite.uv_min, vec2_make(0.0f, 1.0f), 0.0f);
    EXPECT_VEC2_NEAR(sprite.uv_max, vec2_make(1.0f, 0.0f), 0.0f);

    // 翻转不能改变 scale —— 用负 scale 实现翻转会把绕序反过来，
    // 开了背面剔除的管线会让精灵整个消失。
    sprite_t negative_test = sprite_make(vec2_zero(), vec2_make(10.0f, 10.0f));
    sprite_set_flip(&negative_test, true, true);
    EXPECT_GT_U64((u64)(negative_test.scale.x * 100.0f), 0u);
    EXPECT_GT_U64((u64)(negative_test.scale.y * 100.0f), 0u);
}

static void sprite_corners_are_ccw(void)
{
    const sprite_t sprite = sprite_make(vec2_make(10.0f, 20.0f), vec2_make(4.0f, 2.0f));

    vec2_t corners[4];
    sprite_corners(&sprite, corners);

    // 顺序：左下、右下、右上、左上（Y 向上时是逆时针）
    EXPECT_VEC2_NEAR(corners[0], vec2_make(8.0f, 19.0f), 1e-5f);
    EXPECT_VEC2_NEAR(corners[1], vec2_make(12.0f, 19.0f), 1e-5f);
    EXPECT_VEC2_NEAR(corners[2], vec2_make(12.0f, 21.0f), 1e-5f);
    EXPECT_VEC2_NEAR(corners[3], vec2_make(8.0f, 21.0f), 1e-5f);
}

static void sprite_rotation_rotates_around_center(void)
{
    sprite_t sprite = sprite_make(vec2_zero(), vec2_make(4.0f, 2.0f));
    sprite.rotation = F32_HALF_PI; // 逆时针 90 度

    vec2_t corners[4];
    sprite_corners(&sprite, corners);

    // 宽 4 高 2 的矩形转 90 度后：宽 2 高 4
    // 左下角 (-2,-1) -> (1,-2)
    EXPECT_VEC2_NEAR(corners[0], vec2_make(1.0f, -2.0f), 1e-5f);
    EXPECT_VEC2_NEAR(corners[2], vec2_make(-1.0f, 2.0f), 1e-5f);

    // 中心不动
    const vec2_t center = vec2_scale(vec2_add(corners[0], corners[2]), 0.5f);
    EXPECT_VEC2_NEAR(center, vec2_zero(), 1e-5f);
}

static void sprite_scale_affects_corners(void)
{
    sprite_t sprite = sprite_make(vec2_zero(), vec2_make(10.0f, 10.0f));
    sprite.scale = vec2_make(2.0f, 0.5f);

    vec2_t corners[4];
    sprite_corners(&sprite, corners);

    EXPECT_VEC2_NEAR(corners[1], vec2_make(10.0f, -2.5f), 1e-5f);
    EXPECT_VEC2_NEAR(corners[2], vec2_make(10.0f, 2.5f), 1e-5f);
}

static void sprite_bounds_cover_rotated_sprite(void)
{
    sprite_t sprite = sprite_make(vec2_make(5.0f, 5.0f), vec2_make(10.0f, 10.0f));
    sprite.rotation = math_radians(45.0f);

    vec2_t min_value;
    vec2_t max_value;
    sprite_bounds(&sprite, &min_value, &max_value);

    // 边长 10 的正方形转 45 度后，包围盒半宽 = 10 * sqrt(2) / 2 ≈ 7.07
    EXPECT_NEAR(max_value.x - min_value.x, 10.0f * sqrtf(2.0f), 1e-3f);
    EXPECT_NEAR(max_value.y - min_value.y, 10.0f * sqrtf(2.0f), 1e-3f);

    // 中心不变
    EXPECT_VEC2_NEAR(vec2_scale(vec2_add(min_value, max_value), 0.5f), vec2_make(5.0f, 5.0f),
                     1e-4f);
}

static void sprite_rect_intersection(void)
{
    const sprite_t sprite = sprite_make(vec2_zero(), vec2_make(10.0f, 10.0f));

    // 完全包含
    EXPECT_TRUE(sprite_intersects_rect(&sprite, vec2_make(-20.0f, -20.0f),
                                       vec2_make(20.0f, 20.0f)));
    // 完全不相交
    EXPECT_FALSE(sprite_intersects_rect(&sprite, vec2_make(100.0f, 100.0f),
                                        vec2_make(200.0f, 200.0f)));
    // 部分相交
    EXPECT_TRUE(sprite_intersects_rect(&sprite, vec2_make(0.0f, 0.0f), vec2_make(100.0f, 100.0f)));
    // 边缘相接算相交（保守判定，宁可多画不可漏画）
    EXPECT_TRUE(sprite_intersects_rect(&sprite, vec2_make(5.0f, -100.0f),
                                       vec2_make(100.0f, 100.0f)));
}

static void sprite_pack_color_layout(void)
{
    // 全白不透明
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(1.0f, 1.0f, 1.0f, 1.0f)), 0xFFFFFFFFu);
    // 全透明黑
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(0.0f, 0.0f, 0.0f, 0.0f)), 0x00000000u);
    // 纯红
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(1.0f, 0.0f, 0.0f, 1.0f)), 0xFF0000FFu);
    // 纯绿
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(0.0f, 1.0f, 0.0f, 1.0f)), 0xFF00FF00u);
    // 纯蓝
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(0.0f, 0.0f, 1.0f, 1.0f)), 0xFFFF0000u);

    // 越界输入要被钳制，不能溢出到相邻通道
    EXPECT_EQ_U64(sprite_pack_color(vec4_make(2.0f, -1.0f, 0.5f, 1.0f)), 0xFF8000FFu);
}

// =============================================================================
//  2D 相机
// =============================================================================
static void camera_2d_default_maps_pixels_to_world(void)
{
    const camera_2d_t camera = camera_2d_default();

    EXPECT_NEAR(camera.zoom, 1.0f, 0.0f);
    EXPECT_VEC2_NEAR(camera.viewport_size, vec2_make(1280.0f, 720.0f), 0.0f);
    EXPECT_TRUE(camera.pixel_snap);
}

static void camera_2d_produces_orthographic_camera(void)
{
    const camera_2d_t camera_2d = camera_2d_make(vec2_zero(), vec2_make(1280.0f, 720.0f));
    const camera_t    camera = camera_2d_to_camera(&camera_2d);

    EXPECT_EQ_U64(camera.projection, CAMERA_PROJECTION_ORTHOGRAPHIC);
    // zoom = 1 时可见高度 = 视口高度
    EXPECT_NEAR(camera.ortho_height, 720.0f, 1e-4f);
    EXPECT_NEAR(camera.aspect, 1280.0f / 720.0f, 1e-6f);
    // 相机在 +Z 上看向 -Z
    EXPECT_NEAR(camera.position.z, 10.0f, 1e-4f);
    EXPECT_VEC3_NEAR(camera_forward(&camera), vec3_make(0.0f, 0.0f, -1.0f), 1e-5f);
}

static void camera_2d_zoom_scales_view(void)
{
    const camera_2d_t camera_2d =
        camera_2d_make_zoomed(vec2_zero(), 2.0f, vec2_make(1280.0f, 720.0f));
    const camera_t camera = camera_2d_to_camera(&camera_2d);

    // zoom = 2 时可见高度减半
    EXPECT_NEAR(camera.ortho_height, 360.0f, 1e-4f);

    vec2_t min_value;
    vec2_t max_value;
    camera_2d_view_bounds(&camera_2d, &min_value, &max_value);
    EXPECT_NEAR(max_value.x - min_value.x, 640.0f, 1e-4f);
    EXPECT_NEAR(max_value.y - min_value.y, 360.0f, 1e-4f);
}

static void camera_2d_view_bounds_center_on_position(void)
{
    camera_2d_t camera = camera_2d_make(vec2_make(100.0f, 50.0f), vec2_make(800.0f, 600.0f));
    camera.pixel_snap = false;

    vec2_t min_value;
    vec2_t max_value;
    camera_2d_view_bounds(&camera, &min_value, &max_value);

    EXPECT_VEC2_NEAR(min_value, vec2_make(-300.0f, -250.0f), 1e-4f);
    EXPECT_VEC2_NEAR(max_value, vec2_make(500.0f, 350.0f), 1e-4f);
}

static void camera_2d_screen_world_roundtrip(void)
{
    camera_2d_t camera = camera_2d_make_zoomed(vec2_make(30.0f, -20.0f), 1.5f,
                                               vec2_make(1280.0f, 720.0f));
    camera.pixel_snap = false;

    // 屏幕中心对应相机位置
    const vec2_t center = camera_2d_screen_to_world(&camera, vec2_make(640.0f, 360.0f));
    EXPECT_VEC2_NEAR(center, vec2_make(30.0f, -20.0f), 1e-4f);

    // 屏幕原点（左上）在相机的左上方（世界 Y 向上）
    const vec2_t top_left = camera_2d_screen_to_world(&camera, vec2_make(0.0f, 0.0f));
    EXPECT_TRUE(top_left.x < center.x);
    EXPECT_TRUE(top_left.y > center.y);

    // 往返一致
    const vec2_t world = vec2_make(123.0f, -45.0f);
    const vec2_t screen = camera_2d_world_to_screen(&camera, world);
    EXPECT_VEC2_NEAR(camera_2d_screen_to_world(&camera, screen), world, 1e-3f);
}

static void camera_2d_pixel_snapping(void)
{
    camera_2d_t camera = camera_2d_make(vec2_zero(), vec2_make(1280.0f, 720.0f));

    // zoom = 1 时吸附到整数像素
    camera.zoom = 1.0f;
    const vec2_t snapped = camera_2d_snap_point(&camera, vec2_make(10.4f, -3.6f));
    EXPECT_VEC2_NEAR(snapped, vec2_make(10.0f, -4.0f), 1e-5f);

    // zoom = 2 时吸附粒度是半个像素
    camera.zoom = 2.0f;
    const vec2_t half_snapped = camera_2d_snap_point(&camera, vec2_make(10.3f, 0.0f));
    EXPECT_NEAR(half_snapped.x, 10.5f, 1e-5f);

    // 关掉吸附就原样返回
    camera.pixel_snap = false;
    const vec2_t raw = camera_2d_snap_point(&camera, vec2_make(10.3f, 0.0f));
    EXPECT_NEAR(raw.x, 10.3f, 1e-6f);
}

static void camera_2d_snapping_keeps_world_on_pixel_grid(void)
{
    // 吸附的意义：相机位置落在像素网格上时，世界坐标的整数
    // 恰好对应屏幕像素的整数，精灵边缘不会落在像素中间。
    camera_2d_t camera = camera_2d_make(vec2_make(7.3f, -2.8f), vec2_make(1280.0f, 720.0f));
    camera.zoom = 1.0f;

    const camera_t snapped_camera = camera_2d_to_camera(&camera);
    EXPECT_NEAR(snapped_camera.position.x, 7.0f, 1e-5f);
    EXPECT_NEAR(snapped_camera.position.y, -3.0f, 1e-5f);
}

void test_sprite_register(void)
{
    test_register("sprite", "sprite_defaults_are_sane", sprite_defaults_are_sane);
    test_register("sprite", "sprite_make_sets_position_and_size", sprite_make_sets_position_and_size);
    test_register("sprite", "sprite_with_region_copies_uv", sprite_with_region_copies_uv);
    test_register("sprite", "sprite_flip_swaps_uv", sprite_flip_swaps_uv);
    test_register("sprite", "sprite_corners_are_ccw", sprite_corners_are_ccw);
    test_register("sprite", "sprite_rotation_rotates_around_center",
                  sprite_rotation_rotates_around_center);
    test_register("sprite", "sprite_scale_affects_corners", sprite_scale_affects_corners);
    test_register("sprite", "sprite_bounds_cover_rotated_sprite", sprite_bounds_cover_rotated_sprite);
    test_register("sprite", "sprite_rect_intersection", sprite_rect_intersection);
    test_register("sprite", "sprite_pack_color_layout", sprite_pack_color_layout);

    test_register("sprite", "camera_2d_default_maps_pixels_to_world",
                  camera_2d_default_maps_pixels_to_world);
    test_register("sprite", "camera_2d_produces_orthographic_camera",
                  camera_2d_produces_orthographic_camera);
    test_register("sprite", "camera_2d_zoom_scales_view", camera_2d_zoom_scales_view);
    test_register("sprite", "camera_2d_view_bounds_center_on_position",
                  camera_2d_view_bounds_center_on_position);
    test_register("sprite", "camera_2d_screen_world_roundtrip", camera_2d_screen_world_roundtrip);
    test_register("sprite", "camera_2d_pixel_snapping", camera_2d_pixel_snapping);
    test_register("sprite", "camera_2d_snapping_keeps_world_on_pixel_grid",
                  camera_2d_snapping_keeps_world_on_pixel_grid);
}
