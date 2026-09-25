// =============================================================================
//  src/render/render_queue.c —— 渲染队列实现
// =============================================================================
#include "engine/render/render_queue.h"

#include <stdlib.h>
#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"

bool render_layer_is_transparent(render_layer_t layer)
{
    return layer == RENDER_LAYER_TRANSPARENT_3D || layer == RENDER_LAYER_TRANSPARENT_2D;
}

const char *render_layer_name(render_layer_t layer)
{
    switch (layer) {
    case RENDER_LAYER_BACKGROUND:
        return "Background";
    case RENDER_LAYER_OPAQUE_3D:
        return "Opaque3D";
    case RENDER_LAYER_TRANSPARENT_3D:
        return "Transparent3D";
    case RENDER_LAYER_OPAQUE_2D:
        return "Opaque2D";
    case RENDER_LAYER_TRANSPARENT_2D:
        return "Transparent2D";
    case RENDER_LAYER_UI:
        return "UI";
    default:
        return "Unknown";
    }
}

render_item_t render_item_default(void)
{
    render_item_t item;
    memset(&item, 0, sizeof(item));

    item.pipeline = PIPELINE_HANDLE_INVALID;
    item.mesh = MESH_HANDLE_INVALID;
    item.uniforms = object_uniforms_default();
    item.call = draw_call_all();
    item.layer = RENDER_LAYER_OPAQUE_3D;
    item.cast_shadow = true; // 默认投影；线框/调试绘制这类要显式关掉

    for (u32 i = 0; i < RENDER_MAX_TEXTURE_SLOTS; ++i) {
        item.textures[i] = TEXTURE_HANDLE_INVALID;
        item.samplers[i] = sampler_desc_default();
    }

    return item;
}

bool render_queue_init(render_queue_t *queue, allocator_t *alloc)
{
    ASSERT(queue != NULL);
    ASSERT(alloc != NULL);

    memset(queue, 0, sizeof(*queue));

    // 初始容量给 256：绝大多数场景一帧的渲染项不会超过这个数，
    // 给足可以让"暖机后不再分配"更早成立。
    if (!array_init(&queue->items, alloc, sizeof(render_item_t), 256)) {
        return false;
    }

    queue->depth_range_near = 0.1f;
    queue->depth_range_far = 1000.0f;
    return true;
}

void render_queue_destroy(render_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    array_destroy(&queue->items);
    memset(queue, 0, sizeof(*queue));
}

void render_queue_clear(render_queue_t *queue)
{
    ASSERT(queue != NULL);

    // 只把 size 归零，保留已分配的内存。
    // 这是"帧内零 malloc"的关键：第一帧扩容到峰值后，之后每帧都不再分配。
    array_clear(&queue->items);
    queue->next_sequence = 0;
}

void render_queue_begin(render_queue_t *queue, const camera_t *camera)
{
    ASSERT(queue != NULL);

    render_queue_clear(queue);

    if (camera != NULL) {
        queue->camera = *camera;
        queue->has_camera = true;
        queue->depth_range_near = camera->znear;
        queue->depth_range_far = camera->zfar;
    } else {
        queue->has_camera = false;
    }
}

u64 render_queue_make_sort_key(render_layer_t layer, f32 view_depth, f32 depth_near,
                               f32 depth_far)
{
    ASSERT(depth_far > depth_near);

    f32 t = (view_depth - depth_near) / (depth_far - depth_near);
    t = math_saturate(t);

    // 用 f64 中转：4294967295 在 f32 里无法精确表示（会进位到 4294967296），
    // 于是 t == 1.0 时 (u32) 转换的结果是未定义的。f64 能精确表示，
    // 转 u32 也就安全。
    u32 depth_bits = (u32)((f64)t * 4294967295.0);

    if (render_layer_is_transparent(layer)) {
        // 半透明：从远到近 → 把深度反过来，让"远"排在小键值一边
        depth_bits = 0xFFFFFFFFu - depth_bits;
    }

    return ((u64)(u32)layer << 32) | (u64)depth_bits;
}

u32 render_queue_push(render_queue_t *queue, const render_item_t *item)
{
    ASSERT(queue != NULL);
    ASSERT(item != NULL);

    render_item_t *slot = (render_item_t *)array_push_uninit(&queue->items);
    if (slot == NULL) {
        LOG_ERROR("渲染队列扩容失败（当前 %u 项）", array_size(&queue->items));
        return 0xFFFFFFFFu;
    }

    *slot = *item;
    slot->sequence = queue->next_sequence++;
    slot->sort_key = render_queue_make_sort_key(slot->layer, slot->view_depth,
                                                queue->depth_range_near, queue->depth_range_far);

    const u32 count = array_size(&queue->items);
    if (count > queue->peak_item_count) {
        queue->peak_item_count = count;
    }
    return count - 1u;
}

int render_queue_compare_items(const void *a, const void *b)
{
    const render_item_t *x = (const render_item_t *)a;
    const render_item_t *y = (const render_item_t *)b;

    if (x->sort_key < y->sort_key) {
        return -1;
    }
    if (x->sort_key > y->sort_key) {
        return 1;
    }

    // 深度与 layer 都相同时用插入序号决胜负，构成全序。
    // 有了全序，qsort 的不稳定性就不会影响结果 ——
    // 省掉了自己实现稳定排序，同时消除了"透明物体偶发闪烁"。
    if (x->sequence < y->sequence) {
        return -1;
    }
    if (x->sequence > y->sequence) {
        return 1;
    }
    return 0;
}

void render_queue_sort(render_queue_t *queue)
{
    ASSERT(queue != NULL);

    const u32 count = array_size(&queue->items);
    if (count < 2) {
        return;
    }

    qsort(array_data(&queue->items), count, sizeof(render_item_t), render_queue_compare_items);
}

const render_item_t *render_queue_items(const render_queue_t *queue)
{
    ASSERT(queue != NULL);
    return (const render_item_t *)array_data(&queue->items);
}

u32 render_queue_count(const render_queue_t *queue)
{
    ASSERT(queue != NULL);
    return array_size(&queue->items);
}

u32 render_queue_peak_count(const render_queue_t *queue)
{
    ASSERT(queue != NULL);
    return queue->peak_item_count;
}
