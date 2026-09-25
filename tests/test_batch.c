// =============================================================================
//  tests/test_batch.c —— 批渲染器的可测部分
//
//  批渲染器本身需要 GL 上下文（要建动态网格），那部分由 examples/03_sprite
//  端到端验证。这里测的是**不依赖 GL 的部分**：
//  顶点格式、布局描述、内置着色器、以及"断批判定所依赖的排序前提"。
//
//  断批逻辑（纹理/管线/图层/深度 变化就断开）在 03_sprite 里用
//  "1 万精灵 = 3 次 draw call" 这个事实间接验证了 —— 如果断批条件写错，
//  draw call 数会爆炸到几千。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static void sprite_vertex_layout_matches_struct(void)
{
    // 顶点格式是"1 万精灵 < 16ms"能否达成的基础之一。
    // 2D 不需要法线，用 20 字节而不是 M2 的 32 字节 ——
    // 1 万精灵省下 480KB/帧的带宽。
    EXPECT_EQ_U64(sizeof(sprite_vertex_t), 20u);
    EXPECT_EQ_U64(SPRITE_VERTEX_STRIDE, 20u);
    EXPECT_EQ_U64(SPRITE_VERTICES_PER_QUAD, 4u);
    EXPECT_EQ_U64(SPRITE_INDICES_PER_QUAD, 6u);

    // 字段偏移必须与布局描述一致
    EXPECT_EQ_U64(ENGINE_OFFSETOF(sprite_vertex_t, x), 0u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(sprite_vertex_t, y), 4u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(sprite_vertex_t, u), 8u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(sprite_vertex_t, v), 12u);
    EXPECT_EQ_U64(ENGINE_OFFSETOF(sprite_vertex_t, color), 16u);
}

static void sprite_batch_layout_is_valid(void)
{
    const vertex_layout_t layout = sprite_batch_vertex_layout();

    EXPECT_EQ_U64(layout.binding_count, 1u);
    EXPECT_EQ_U64(layout.attribute_count, 3u);
    EXPECT_EQ_U64(layout.bindings[0].stride, SPRITE_VERTEX_STRIDE);
    EXPECT_FALSE(layout.bindings[0].per_instance);

    const char *reason = NULL;
    EXPECT_TRUE(vertex_layout_validate(&layout, &reason));

    // 属性：位置 / UV / 颜色
    EXPECT_EQ_I64(vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_POSITION), 0);
    EXPECT_EQ_I64(vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_TEXCOORD0), 1);
    EXPECT_EQ_I64(vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_COLOR0), 2);

    // 2D 精灵不需要法线
    EXPECT_EQ_I64(vertex_layout_find_attribute(&layout, VERTEX_SEMANTIC_NORMAL), -1);

    EXPECT_EQ_U64(layout.attributes[0].format, VERTEX_FORMAT_FLOAT2);
    EXPECT_EQ_U64(layout.attributes[0].offset, 0u);
    EXPECT_EQ_U64(layout.attributes[1].format, VERTEX_FORMAT_FLOAT2);
    EXPECT_EQ_U64(layout.attributes[1].offset, 8u);
    // 颜色用 4 字节归一化整数而不是 16 字节的 float4 —— 省 12 字节/顶点
    EXPECT_EQ_U64(layout.attributes[2].format, VERTEX_FORMAT_UNORM8X4);
    EXPECT_EQ_U64(layout.attributes[2].offset, 16u);

    // 属性 location 用语义枚举值，与着色器里的 layout(location = N) 对应
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(layout.attributes[0].semantic), 0u);
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(layout.attributes[1].semantic), 4u);
    EXPECT_EQ_U64(RENDER_ATTRIBUTE_LOCATION(layout.attributes[2].semantic), 6u);
}

static void sprite_batch_builtin_shader_exists(void)
{
    const shader_desc_t desc = sprite_batch_builtin_shader();

    EXPECT_NOT_NULL(desc.vertex_source);
    EXPECT_NOT_NULL(desc.fragment_source);
    EXPECT_NOT_NULL(desc.debug_name);

    // 源码里**不能**有 #version —— 版本行由后端按上下文实际版本生成。
    // 写死版本会让驱动降级时直接编译失败。
    EXPECT_NULL(strstr(desc.vertex_source, "#version"));
    EXPECT_NULL(strstr(desc.fragment_source, "#version"));

    // 也不能重复声明 uniform 块（由 RENDER_UNIFORM_BLOCK_GLSL 统一前置）。
    // 重复声明会让着色器编译报"重定义"。
    EXPECT_NULL(strstr(desc.vertex_source, "uniform"));
    EXPECT_NULL(strstr(desc.fragment_source, "uniform "));

    // 顶点着色器必须声明批渲染用到的三个属性 location
    EXPECT_NOT_NULL(strstr(desc.vertex_source, "location = 0"));
    EXPECT_NOT_NULL(strstr(desc.vertex_source, "location = 4"));
    EXPECT_NOT_NULL(strstr(desc.vertex_source, "location = 6"));

    // 顶点着色器不能用 u_model：批渲染的顶点已经是世界空间，
    // 乘一遍模型矩阵纯属浪费（每帧 4 万次矩阵乘法）
    EXPECT_NULL(strstr(desc.vertex_source, "u_model"));
}

static void sprite_batch_desc_default_is_usable(void)
{
    const sprite_batch_desc_t desc = sprite_batch_desc_default();

    EXPECT_GT_U64(desc.max_sprites, 0u);
    EXPECT_EQ_U64(desc.pipeline, PIPELINE_HANDLE_INVALID); // 无效 = 用内置管线

    // 默认采样器必须是"无 mipmap"的。
    // 图集不生成 mipmap，如果采样器用 SAMPLER_MIPMAP_LINEAR，
    // GL 会认为纹理不完整 → 采样结果是**全黑**，而且不报任何错。
    EXPECT_EQ_U64(desc.sampler.mipmap_filter, SAMPLER_MIPMAP_NONE);
    EXPECT_EQ_U64(desc.sampler.wrap_u, SAMPLER_WRAP_CLAMP_TO_EDGE);
}

static void sprite_sampler_preset_is_mipmap_free(void)
{
    const sampler_desc_t sprite = sampler_desc_sprite();
    EXPECT_EQ_U64(sprite.mipmap_filter, SAMPLER_MIPMAP_NONE);
    EXPECT_EQ_U64(sprite.min_filter, SAMPLER_FILTER_LINEAR);
    EXPECT_EQ_U64(sprite.wrap_u, SAMPLER_WRAP_CLAMP_TO_EDGE);
    EXPECT_EQ_U64(sprite.wrap_v, SAMPLER_WRAP_CLAMP_TO_EDGE);

    // 默认采样器是三线性的，用在图集上会得到全黑 —— 两者必须不同
    const sampler_desc_t standard = sampler_desc_default();
    EXPECT_TRUE(standard.mipmap_filter != sprite.mipmap_filter);
}

// =============================================================================
//  批渲染的排序前提
//
//  批渲染器把"同一纹理/管线/图层/深度"的精灵合成一个渲染项。
//  这个合并是否**正确**，取决于渲染队列的排序是否保持了提交顺序 ——
//  如果同一层内的精灵被重新排序，2D 的前后关系就错了。
// =============================================================================
static void queue_preserves_submission_order_within_layer(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    // 模拟 2D 相机：所有精灵深度相同
    camera_t camera = camera_perspective(vec3_zero(), quat_identity(), 60.0f, 1.0f, 1.0f, 100.0f);
    render_queue_begin(&queue, &camera);

    // 提交 5 个"批次"（模拟批渲染器的 flush），深度全相同
    const u32 batch_sizes[5] = {100, 50, 200, 25, 75};
    u32       expected_first_index = 0;

    for (u32 batch = 0; batch < 5; ++batch) {
        render_item_t item = render_item_default();
        item.layer = RENDER_LAYER_TRANSPARENT_2D;
        item.view_depth = 0.0f; // 2D 精灵的虚拟深度全都一样
        item.call.first_index = expected_first_index;
        item.call.index_count = batch_sizes[batch] * SPRITE_INDICES_PER_QUAD;
        expected_first_index += item.call.index_count;
        render_queue_push(&queue, &item);
    }

    render_queue_sort(&queue);

    // 排序后必须还是提交顺序 —— 否则批次里的精灵会画到别的批次上面去
    const render_item_t *items = render_queue_items(&queue);
    u32                  running_index = 0;

    for (u32 i = 0; i < 5; ++i) {
        EXPECT_EQ_U64(items[i].call.first_index, running_index);
        EXPECT_EQ_U64(items[i].sequence, i);
        running_index += items[i].call.index_count;
    }

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void queue_separates_2d_layers(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    camera_t camera = camera_perspective(vec3_zero(), quat_identity(), 60.0f, 1.0f, 1.0f, 100.0f);
    render_queue_begin(&queue, &camera);

    // 乱序提交三个图层
    const render_layer_t layers[] = {RENDER_LAYER_UI, RENDER_LAYER_OPAQUE_2D,
                                     RENDER_LAYER_TRANSPARENT_2D};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(layers); ++i) {
        render_item_t item = render_item_default();
        item.layer = layers[i];
        item.view_depth = 0.0f;
        render_queue_push(&queue, &item);
    }

    render_queue_sort(&queue);
    const render_item_t *items = render_queue_items(&queue);

    // 无论提交顺序如何，绘制顺序必须是 不透明2D -> 半透明2D -> UI
    EXPECT_EQ_U64(items[0].layer, RENDER_LAYER_OPAQUE_2D);
    EXPECT_EQ_U64(items[1].layer, RENDER_LAYER_TRANSPARENT_2D);
    EXPECT_EQ_U64(items[2].layer, RENDER_LAYER_UI);

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

static void queue_2d_depth_orders_within_layer(void)
{
    allocator_tracking_t tracker;
    allocator_tracking_init(&tracker, allocator_heap());

    render_queue_t queue;
    EXPECT_TRUE(render_queue_init(&queue, &tracker.base));

    camera_t camera = camera_perspective(vec3_zero(), quat_identity(), 60.0f, 1.0f, 1.0f, 100.0f);
    render_queue_begin(&queue, &camera);

    // 半透明层内：深度大的先画（远 -> 近）
    const f32 depths[] = {5.0f, 50.0f, 20.0f};
    for (usize i = 0; i < ENGINE_ARRAY_COUNT(depths); ++i) {
        render_item_t item = render_item_default();
        item.layer = RENDER_LAYER_TRANSPARENT_2D;
        item.view_depth = depths[i];
        render_queue_push(&queue, &item);
    }

    render_queue_sort(&queue);
    const render_item_t *items = render_queue_items(&queue);

    EXPECT_NEAR(items[0].view_depth, 50.0f, 0.0f);
    EXPECT_NEAR(items[1].view_depth, 20.0f, 0.0f);
    EXPECT_NEAR(items[2].view_depth, 5.0f, 0.0f);

    render_queue_destroy(&queue);
    EXPECT_FALSE(allocator_tracking_has_leak(&tracker));
}

void test_batch_register(void)
{
    test_register("batch", "sprite_vertex_layout_matches_struct",
                  sprite_vertex_layout_matches_struct);
    test_register("batch", "sprite_batch_layout_is_valid", sprite_batch_layout_is_valid);
    test_register("batch", "sprite_batch_builtin_shader_exists", sprite_batch_builtin_shader_exists);
    test_register("batch", "sprite_batch_desc_default_is_usable",
                  sprite_batch_desc_default_is_usable);
    test_register("batch", "sprite_sampler_preset_is_mipmap_free",
                  sprite_sampler_preset_is_mipmap_free);

    test_register("batch", "queue_preserves_submission_order_within_layer",
                  queue_preserves_submission_order_within_layer);
    test_register("batch", "queue_separates_2d_layers", queue_separates_2d_layers);
    test_register("batch", "queue_2d_depth_orders_within_layer",
                  queue_2d_depth_orders_within_layer);
}
