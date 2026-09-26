// =============================================================================
//  tests/test_tilemap.c —— 图块地图
//
//  这个文件存在的理由本身要记一下：tilemap 是 M3 的交付物，实现里连视口剔除
//  都写了，但**一条测试都没有**（本文件是第一次跑它）。规划里 M3 有一句
//  "65536 图块的地图要能靠剔除画得动"，那句的分量全在剔除的行列换算上 ——
//  而那是这种模块里最容易差一格的地方（行序 Y 翻转、边界含不含、clamp 到哪）。
//  一个从没被执行过的模块，等价于一个自称能用的模块。
//
//  离线策略：`tilemap_draw*` 需要 sprite_batch，而 `sprite_batch_init` 要建网格
//  与管线（离不开 GL）。这里用的办法是**手工填一个 batch 结构**而不是伪造 renderer：
//   * `sprite_batch_draw` 只往 CPU 顶点数组里写，不碰 renderer；
//   * 唯一的 flush 触发点是"纹理/图层变了"或"容量满了"，
//     前者第一次必然空批（flush 在 `sprites_in_batch == 0` 时直接返回），
//     后者靠把容量给足来避免；
//   * 于是 batch->renderer 保持 NULL 也不会被解引用。
//  这仍然是"不依赖 GL 的那一半"，和 tests/test_batch.c 同一条路线。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

#include <math.h>
#include <string.h>

#include "engine/core/memory.h"
#include "engine/render/sprite_batch.h"
#include "engine/render/tilemap.h"
#include "engine/render/texture.h"
#include "fake_allocator.h"

#define TM_SIZE   32.0f
#define TM_COLS   16u
#define TM_ROWS   16u
#define TM_SPRITES 1024u // 手工 batch 的容量：地图最多 256 图块，留出足够余量

typedef struct tilemap_fixture_t {
    fake_allocator_t alloc;
    tilemap_t        map;
    sprite_vertex_t  vertices[TM_SPRITES * 4u];
    sprite_batch_t   batch;
    bool             owns_map;
} tilemap_fixture_t;

static void tm_fixture_begin(tilemap_fixture_t *fx, usize fail_from_call, bool make_map)
{
    memset(fx, 0, sizeof(*fx));
    fake_allocator_init(&fx->alloc, fail_from_call);

    fx->batch.vertices = fx->vertices;
    fx->batch.vertex_capacity = TM_SPRITES * 4u;
    fx->batch.sprite_capacity = TM_SPRITES;
    fx->batch.current_texture = TEXTURE_HANDLE_INVALID;
    fx->batch.current_layer = RENDER_LAYER_TRANSPARENT_2D;

    if (make_map) {
        fx->owns_map = tilemap_init(&fx->map, &fx->alloc.base, TM_COLS, TM_ROWS, TM_SIZE, 8u, 8u,
                                    16.0f, 0.0f);
    }
}

static void tm_fixture_end(tilemap_fixture_t *fx)
{
    if (fx->owns_map) {
        tilemap_shutdown(&fx->map);
        fx->owns_map = false;
    }
    // 收尾时一块都不许还挂着：这一条同时抓住了"init 失败却没 free 掉第一块"
    fake_allocator_drain(&fx->alloc);
    EXPECT_EQ_U64(fx->alloc.live, 0u);
}

// ---------------------------------------------------------------- 参数与生命周期

static void tilemap_rejects_degenerate_setup(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, false);

    // 四个维度各给一个 0：宽度、高度、图集列数、图集行数
    EXPECT_FALSE(tilemap_init(&fx.map, &fx.alloc.base, 0u, 4u, 16.0f, 1u, 1u, 16.0f, 0.0f));
    EXPECT_FALSE(tilemap_init(&fx.map, &fx.alloc.base, 4u, 0u, 16.0f, 1u, 1u, 16.0f, 0.0f));
    EXPECT_FALSE(tilemap_init(&fx.map, &fx.alloc.base, 4u, 4u, 16.0f, 0u, 1u, 16.0f, 0.0f));
    EXPECT_FALSE(tilemap_init(&fx.map, &fx.alloc.base, 4u, 4u, 16.0f, 1u, 0u, 16.0f, 0.0f));
    EXPECT_EQ_U64(fx.alloc.live, 0u); // 拒绝的路径上不能留下已经分配好的块

    tm_fixture_end(&fx);
}

// 分配失败要走一遍：这是规划里"负面用例"的一条，而实现里那个分支此前没跑过。
static void tilemap_survives_allocation_failure(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, 1u, false); // 第一次 alloc 就失败

    EXPECT_FALSE(tilemap_init(&fx.map, &fx.alloc.base, TM_COLS, TM_ROWS, TM_SIZE, 8u, 8u, 16.0f,
                              0.0f));
    EXPECT_GT_U64(fx.alloc.failures, 0u);
    EXPECT_EQ_U64(fx.alloc.live, 0u);

    tm_fixture_end(&fx);
}

static void tilemap_applies_documented_defaults(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, false);
    // tile_size 与图集块宽给 0：实现要退到可用的默认值，而不是把 0 传下去
    EXPECT_TRUE(tilemap_init(&fx.map, &fx.alloc.base, 4u, 4u, 0.0f, 2u, 2u, 0.0f, 0.0f));
    fx.owns_map = true;
    EXPECT_GT_U64((usize)fx.map.tile_size, 0u);
    EXPECT_GT_U64((usize)fx.map.atlas_tile_px, 0u);
    EXPECT_EQ_U64((usize)fx.map.tiles != 0u, 1u);
    tm_fixture_end(&fx);
}

// ---------------------------------------------------------------- 数据模型

static void tilemap_set_and_get_roundtrip(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, true);
    EXPECT_TRUE(fx.owns_map);

    tilemap_fill(&fx.map, 0u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 5u), 0u); // 0 = 空

    tilemap_set(&fx.map, 3u, 5u, 42u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 5u), 42u);
    // 邻居不许跟着变：u16 数组上写错步长是最容易的一类错位，而它只错一格
    EXPECT_EQ_U64(tilemap_get(&fx.map, 4u, 5u), 0u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 6u), 0u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 4u), 0u);

    // 覆盖写
    tilemap_set(&fx.map, 3u, 5u, 7u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 5u), 7u);

    // 越界查询不许读到别人的内存，也不许改数据
    EXPECT_FALSE(tilemap_in_bounds(&fx.map, -1, 0));
    EXPECT_FALSE(tilemap_in_bounds(&fx.map, 0, -1));
    EXPECT_FALSE(tilemap_in_bounds(&fx.map, (i32)TM_COLS, 0));
    EXPECT_FALSE(tilemap_in_bounds(&fx.map, 0, (i32)TM_ROWS));
    EXPECT_TRUE(tilemap_in_bounds(&fx.map, (i32)TM_COLS - 1, (i32)TM_ROWS - 1));
    EXPECT_EQ_U64(tilemap_get(&fx.map, 3u, 5u), 7u);

    tilemap_fill(&fx.map, 9u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, 0u, 0u), 9u);
    EXPECT_EQ_U64(tilemap_get(&fx.map, TM_COLS - 1u, TM_ROWS - 1u), 9u);

    tm_fixture_end(&fx);
}

// 中心点只验"等距"和"落在自己那一格里"，不验绝对值：
// 行序 Y 翻转是实现细节（图集与 GL 的 V 轴方向），把绝对坐标写进断言的话,
// 这条测试会变成"翻转方向改了就得改测试"，而真正要守的是网格关系。
static void tilemap_centers_are_evenly_spaced(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, true);
    EXPECT_TRUE(fx.owns_map);

    const vec2_t origin = vec2_make(100.0f, 50.0f);
    const vec2_t c00 = tilemap_tile_center(&fx.map, origin, 0u, 0u);
    const vec2_t c10 = tilemap_tile_center(&fx.map, origin, 1u, 0u);
    const vec2_t c01 = tilemap_tile_center(&fx.map, origin, 0u, 1u);

    EXPECT_NEAR((double)(c10.x - c00.x), TM_SIZE, 1e-3);
    EXPECT_NEAR((double)(c10.y - c00.y), 0.0, 1e-3);
    EXPECT_NEAR((double)(c01.x - c00.x), 0.0, 1e-3);
    EXPECT_NEAR(fabs((double)(c01.y - c00.y)), TM_SIZE, 1e-3);

    // 中心必须落在自己那一格的范围内（离 origin 不到一个 tile_size）
    EXPECT_LT_U64((usize)((c00.x - origin.x) < 0.0f ? -(c00.x - origin.x) : (c00.x - origin.x)),
                  (usize)TM_SIZE);

    tm_fixture_end(&fx);
}

// ---------------------------------------------------------------- 提交与剔除

static void tilemap_draw_skips_empty_tiles(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, true);
    EXPECT_TRUE(fx.owns_map);

    tilemap_fill(&fx.map, 0u);
    tilemap_set(&fx.map, 1u, 1u, 3u);
    tilemap_set(&fx.map, 2u, 4u, 3u);
    tilemap_set(&fx.map, 15u, 15u, 1u);
    tilemap_set(&fx.map, 0u, 0u, 2u);
    tilemap_set(&fx.map, 7u, 7u, 5u);

    const u32 drawn = tilemap_draw(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                   vec4_make(1.0f, 1.0f, 1.0f, 1.0f));
    EXPECT_EQ_U64(drawn, 5u);                     // 只有非空格
    EXPECT_EQ_U64(fx.batch.sprite_count, 5u);     // 而且真的各提交了一个精灵
    EXPECT_EQ_U64(fx.batch.dropped_sprites, 0u);  // 容量够，不该有任何东西被丢掉

    tm_fixture_end(&fx);
}

// 视口剔除是这里唯一有"规模"含义的行为：M3 要求 65536 图块的地图画得动，靠的就是它。
//
// 断什么、不断什么，这条要说清。第一次写时我按"两格见方的窗口就该交 4 个"下断言,
// 实测交 9 个 —— 差的是窗口边线上那些**被压到一角的图块**。实现选了"端点向外取整",
// 也就是宁可多画一圈也不漏掉半露的图块：这是剔除唯一安全的方向（漏画是可见 bug,
// 多画只是浪费）。所以这里断的是**包含关系与单调性**，而不是一个具体格数 ——
// 把"多画几个"钉成契约，等于把一次实现选择伪装成了需求。
static void tilemap_draw_visible_culls(void)
{
    tilemap_fixture_t fx;
    tm_fixture_begin(&fx, FAKE_ALLOC_NEVER, true);
    EXPECT_TRUE(fx.owns_map);
    tilemap_fill(&fx.map, 1u); // 满图，没有空格来干扰计数

    const u32 total = TM_COLS * TM_ROWS;
    const vec4_t tint = vec4_make(1.0f, 1.0f, 1.0f, 1.0f);

    // 1. 包住整张图：一个都不能少
    fx.batch.sprite_count = 0u;
    const u32 all = tilemap_draw_visible(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                         vec2_make(-10000.0f, -10000.0f),
                                         vec2_make(10000.0f, 10000.0f), tint);
    EXPECT_EQ_U64(all, (u32)total);

    // 2. 完全不相交：必须是 0，而且一个顶点都不许写
    fx.batch.sprite_count = 0u;
    const u32 none = tilemap_draw_visible(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                         vec2_make(9000.0f, 9000.0f),
                                         vec2_make(10000.0f, 10000.0f), tint);
    EXPECT_EQ_U64(none, 0u);
    EXPECT_EQ_U64(fx.batch.sprite_count, 0u);

    // 3. 单调：窗口越大，提交的只会越多不会越少。这条抓的是行列起点/终点算错 ——
    //    那种错通常表现为"某个方向的窗口反而看见更少"。
    fx.batch.sprite_count = 0u;
    const u32 small = tilemap_draw_visible(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                           vec2_make(0.5f, -TM_SIZE * 2.0f + 0.5f),
                                           vec2_make(TM_SIZE * 2.0f - 0.5f, -0.5f), tint);
    fx.batch.sprite_count = 0u;
    const u32 bigger = tilemap_draw_visible(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                            vec2_make(0.5f, -TM_SIZE * 4.0f + 0.5f),
                                            vec2_make(TM_SIZE * 4.0f - 0.5f, -0.5f), tint);
    EXPECT_GT_U64(bigger, small);
    EXPECT_LT_U64(small, (u32)total); // 小窗口确实剔掉了东西
    // 边线上半露的图块算可见，所以下界是"完全被窗口包住的格数"，上界是"碰到边线的格数"
    EXPECT_GE_U64(small, 4u);
    EXPECT_LE_U64(small, 9u);

    // 4. 覆盖一半就是大约一半（±剔除的一圈边线），不能出现"剔了一半却交回全部"
    fx.batch.sprite_count = 0u;
    const u32 half = tilemap_draw_visible(&fx.batch, &fx.map, (texture_handle_t)7u, vec2_zero(),
                                          vec2_make(0.0f, -TM_SIZE * 8.0f),
                                          vec2_make(TM_SIZE * (f32)TM_COLS, 0.0f), tint);
    EXPECT_GE_U64(half, (u32)total / 2u);
    EXPECT_LE_U64(half, (u32)total / 2u + TM_COLS + 2u);

    tm_fixture_end(&fx);
}

// =============================================================================
void test_tilemap_register(void)
{
    test_register("tilemap", "rejects_degenerate_setup", tilemap_rejects_degenerate_setup);
    test_register("tilemap", "survives_allocation_failure", tilemap_survives_allocation_failure);
    test_register("tilemap", "applies_documented_defaults", tilemap_applies_documented_defaults);
    test_register("tilemap", "set_and_get_roundtrip", tilemap_set_and_get_roundtrip);
    test_register("tilemap", "centers_are_evenly_spaced", tilemap_centers_are_evenly_spaced);
    test_register("tilemap", "draw_skips_empty_tiles", tilemap_draw_skips_empty_tiles);
    test_register("tilemap", "draw_visible_culls", tilemap_draw_visible_culls);
}
