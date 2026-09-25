// =============================================================================
//  src/render/sprite_batch.c —— 精灵批渲染器
//
//  设计要点见 sprite_batch.h 文件头。这里只补充实现上的关键选择：
//
//  * 索引缓冲在创建时一次生成、之后永不更新。所有精灵都是同一个 quad 模式，
//    索引永远是 `0,1,2, 0,2,3` 加 4*N 的偏移。这样每帧只需要上传顶点数据，
//    上传量直接减半。
//
//  * 顶点数据用 glBufferSubData 局部更新，不重新 glBufferData。
//    重新分配会让驱动丢掉已有的缓冲对象，每帧都做等于每帧都在申请显存。
// =============================================================================
#include "engine/render/sprite_batch.h"

#include <string.h>

#include "engine/core/assert.h"
#include "engine/core/log.h"
#include "engine/render/renderer.h"

// =============================================================================
//  内置 2D 精灵着色器
//
//  顶点已经是世界空间（sprite_corners 直接算出来的），所以顶点着色器里
//  不需要乘 u_model —— 省掉每个顶点一次矩阵乘法。
//  ObjectUniforms 块仍然存在（由 RENDER_UNIFORM_BLOCK_GLSL 统一声明），
//  但没被引用，驱动会把它优化掉。
// =============================================================================
static const char *const k_sprite_vertex =
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 4) in vec2 a_texcoord;\n"
    "layout(location = 6) in vec4 a_color;\n"
    "out vec2 v_texcoord;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = a_color;\n"
    // z = 0：配合 mat4_ortho_2d 落在深度 0.5 上（2D 管线关深度测试，
    // 这个值只是为了让顶点落在合法深度范围内）
    "    gl_Position = u_view_projection * vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *const k_sprite_fragment =
    "in vec2 v_texcoord;\n"
    "in vec4 v_color;\n"
    "layout(location = 0) out vec4 frag_color;\n"
    "void main() {\n"
    "    vec4 tex = texture(u_albedo, v_texcoord);\n"
    "    vec4 result = tex * v_color;\n"
    // 丢弃全透明像素：图集里大量区域是空白的（padding、未使用格子），
    // 丢弃它们能省掉一次混合写入。2D 管线本来就关了深度测试，
    // discard 不会影响 early-z。
    "    if (result.a < 0.003) { discard; }\n"
    "    frag_color = result;\n"
    "}\n";

shader_desc_t sprite_batch_builtin_shader(void)
{
    shader_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.vertex_source = k_sprite_vertex;
    desc.fragment_source = k_sprite_fragment;
    desc.debug_name = "builtin:sprite_2d";
    return desc;
}

vertex_layout_t sprite_batch_vertex_layout(void)
{
    vertex_layout_t layout;
    memset(&layout, 0, sizeof(layout));

    layout.binding_count = 1;
    layout.bindings[0].stride = SPRITE_VERTEX_STRIDE;
    layout.bindings[0].per_instance = false;

    layout.attribute_count = 3;

    layout.attributes[0].semantic = VERTEX_SEMANTIC_POSITION;
    layout.attributes[0].binding = 0;
    layout.attributes[0].format = VERTEX_FORMAT_FLOAT2;
    layout.attributes[0].offset = 0;

    layout.attributes[1].semantic = VERTEX_SEMANTIC_TEXCOORD0;
    layout.attributes[1].binding = 0;
    layout.attributes[1].format = VERTEX_FORMAT_FLOAT2;
    layout.attributes[1].offset = 8;

    layout.attributes[2].semantic = VERTEX_SEMANTIC_COLOR0;
    layout.attributes[2].binding = 0;
    layout.attributes[2].format = VERTEX_FORMAT_UNORM8X4;
    layout.attributes[2].offset = 16;

    return layout;
}

sprite_batch_desc_t sprite_batch_desc_default(void)
{
    sprite_batch_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.max_sprites = 16384;
    desc.pipeline = PIPELINE_HANDLE_INVALID;
    desc.sampler = sampler_desc_sprite();
    return desc;
}

// =============================================================================
//  生命周期
// =============================================================================
static bool sprite_batch_build_indices(sprite_batch_t *batch)
{
    const u32 quad_count = batch->sprite_capacity;
    const u32 index_count = quad_count * SPRITE_INDICES_PER_QUAD;

    // 索引缓冲一次生成后就不再变，所以用帧 arena 当临时缓冲最省事 ——
    // 上传完就随 arena reset 回收。
    arena_t *arena = renderer_frame_arena(batch->renderer);
    u32     *indices = ARENA_NEW_ARRAY(arena, u32, index_count);
    if (indices == NULL) {
        LOG_ERROR("批渲染索引缓冲临时内存不足（%u 个索引）", index_count);
        return false;
    }

    for (u32 i = 0; i < quad_count; ++i) {
        const u32 base = i * SPRITE_VERTICES_PER_QUAD;
        u32      *out = &indices[i * SPRITE_INDICES_PER_QUAD];
        // 两个三角形组成一个 quad。绕序是逆时针（左下->右下->右上），
        // 与 sprite_corners 的输出顺序一致。
        out[0] = base + 0;
        out[1] = base + 1;
        out[2] = base + 2;
        out[3] = base + 0;
        out[4] = base + 2;
        out[5] = base + 3;
    }

    mesh_desc_t mesh_desc;
    memset(&mesh_desc, 0, sizeof(mesh_desc));
    mesh_desc.layout = sprite_batch_vertex_layout();
    // data = NULL：只申请显存，不填初始内容（动态缓冲的常规做法）
    mesh_desc.bindings[0].data = NULL;
    mesh_desc.bindings[0].size = (usize)batch->vertex_capacity * sizeof(sprite_vertex_t);
    mesh_desc.index_data = indices;
    mesh_desc.index_data_size = (usize)index_count * sizeof(u32);
    mesh_desc.index_format = INDEX_FORMAT_U32;
    mesh_desc.vertex_count = batch->vertex_capacity;
    mesh_desc.index_count = index_count;
    mesh_desc.dynamic = true;
    mesh_desc.debug_name = "sprite_batch";

    batch->mesh = renderer_create_mesh(batch->renderer, &mesh_desc);
    return mesh_is_valid(batch->renderer, batch->mesh);
}

bool sprite_batch_init(sprite_batch_t *batch, renderer_t *renderer, allocator_t *alloc,
                       const sprite_batch_desc_t *desc)
{
    ASSERT(batch != NULL);
    ASSERT(renderer != NULL);
    ASSERT(alloc != NULL);

    memset(batch, 0, sizeof(*batch));
    batch->renderer = renderer;
    batch->alloc = alloc;

    const sprite_batch_desc_t actual = (desc != NULL) ? *desc : sprite_batch_desc_default();

    u32 max_sprites = (actual.max_sprites > 0) ? actual.max_sprites : 16384;
    if (max_sprites > 0x0FFFFFFFu / SPRITE_VERTICES_PER_QUAD) {
        LOG_ERROR("max_sprites 过大: %u", max_sprites);
        return false;
    }

    batch->sprite_capacity = max_sprites;
    batch->vertex_capacity = max_sprites * SPRITE_VERTICES_PER_QUAD;
    batch->sampler = actual.sampler;
    batch->current_texture = TEXTURE_HANDLE_INVALID;
    batch->current_pipeline = PIPELINE_HANDLE_INVALID;
    batch->current_layer = RENDER_LAYER_TRANSPARENT_2D;
    batch->current_depth = 0.0f;

    const usize vertex_bytes = (usize)batch->vertex_capacity * sizeof(sprite_vertex_t);
    batch->vertices = (sprite_vertex_t *)alloc->alloc(alloc, vertex_bytes, 16);
    if (batch->vertices == NULL) {
        LOG_ERROR("批渲染顶点缓冲分配失败（%llu 字节）", (unsigned long long)vertex_bytes);
        return false;
    }
    memset(batch->vertices, 0, vertex_bytes);

    // 管线：调用方没给就创建内置的
    if (actual.pipeline != PIPELINE_HANDLE_INVALID) {
        batch->default_pipeline = actual.pipeline;
        batch->owns_pipeline = false;
    } else {
        const shader_desc_t shader_desc = sprite_batch_builtin_shader();
        batch->default_shader = renderer_create_shader(renderer, &shader_desc);
        if (!shader_is_valid(renderer, batch->default_shader)) {
            LOG_ERROR("2D 精灵着色器创建失败");
            sprite_batch_shutdown(batch);
            return false;
        }

        pipeline_desc_t pipeline_desc = pipeline_desc_default();
        pipeline_desc.shader = batch->default_shader;
        pipeline_desc.vertex_layout = sprite_batch_vertex_layout();
        pipeline_desc.topology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pipeline_desc.blend = blend_state_alpha();
        // 2D 不用深度测试：前后关系由 render layer + 提交顺序决定。
        // 用 z 值排序会让同层精灵因为浮点误差偶发闪烁。
        pipeline_desc.depth = depth_state_disabled();
        // 不剔除：调用方可能用负缩放做镜像，剔除会让精灵整个消失
        pipeline_desc.raster = raster_state_no_cull();
        pipeline_desc.multisample.samples = 4;
        pipeline_desc.debug_name = "sprite_2d";

        batch->default_pipeline = renderer_create_pipeline(renderer, &pipeline_desc);
        if (!renderer_pipeline_is_valid(renderer, batch->default_pipeline)) {
            LOG_ERROR("2D 精灵管线创建失败");
            sprite_batch_shutdown(batch);
            return false;
        }
        batch->owns_pipeline = true;
    }

    batch->current_pipeline = batch->default_pipeline;

    if (!sprite_batch_build_indices(batch)) {
        sprite_batch_shutdown(batch);
        return false;
    }

    LOG_DEBUG("精灵批渲染器就绪: 容量 %u 精灵（%u 顶点 / %u 索引，%llu KB 顶点缓冲）",
              batch->sprite_capacity, batch->vertex_capacity,
              batch->sprite_capacity * SPRITE_INDICES_PER_QUAD,
              (unsigned long long)(vertex_bytes / 1024u));
    return true;
}

void sprite_batch_shutdown(sprite_batch_t *batch)
{
    if (batch == NULL) {
        return;
    }

    if (batch->renderer != NULL) {
        if (mesh_is_valid(batch->renderer, batch->mesh)) {
            renderer_destroy_mesh(batch->renderer, batch->mesh);
        }
        if (batch->owns_pipeline) {
            if (renderer_pipeline_is_valid(batch->renderer, batch->default_pipeline)) {
                renderer_destroy_pipeline(batch->renderer, batch->default_pipeline);
            }
            if (shader_is_valid(batch->renderer, batch->default_shader)) {
                renderer_destroy_shader(batch->renderer, batch->default_shader);
            }
        }
    }

    if (batch->vertices != NULL && batch->alloc != NULL) {
        const usize bytes = (usize)batch->vertex_capacity * sizeof(sprite_vertex_t);
        batch->alloc->free(batch->alloc, batch->vertices, bytes);
    }

    memset(batch, 0, sizeof(*batch));
}

// =============================================================================
//  帧
// =============================================================================
void sprite_batch_begin(sprite_batch_t *batch)
{
    ASSERT(batch != NULL);

    batch->sprite_count = 0;
    batch->sprites_in_batch = 0;
    batch->batch_first_index = 0;
    batch->batch_count = 0;
    batch->dropped_sprites = 0;
    batch->current_texture = TEXTURE_HANDLE_INVALID;
    batch->current_pipeline = batch->default_pipeline;
    batch->current_layer = RENDER_LAYER_TRANSPARENT_2D;
    batch->current_depth = 0.0f;
}

void sprite_batch_set_pipeline(sprite_batch_t *batch, pipeline_handle_t pipeline)
{
    ASSERT(batch != NULL);

    if (pipeline == batch->current_pipeline) {
        return;
    }
    // 切管线必须断批：一次 draw 只能用一条管线
    sprite_batch_flush(batch);
    batch->current_pipeline = pipeline;
}

void sprite_batch_set_layer(sprite_batch_t *batch, render_layer_t layer)
{
    ASSERT(batch != NULL);

    if (layer == batch->current_layer) {
        return;
    }
    // 切 layer 必须断批：渲染队列按 layer 排序，跨 layer 的精灵不可能同批
    sprite_batch_flush(batch);
    batch->current_layer = layer;
}

void sprite_batch_set_depth(sprite_batch_t *batch, f32 depth)
{
    ASSERT(batch != NULL);

    if (depth == batch->current_depth) {
        return;
    }
    // 同 layer 内按深度排序，深度不同就要断开
    sprite_batch_flush(batch);
    batch->current_depth = depth;
}

void sprite_batch_set_sampler(sprite_batch_t *batch, const sampler_desc_t *sampler)
{
    ASSERT(batch != NULL);

    if (sampler == NULL) {
        return;
    }
    // 采样器是渲染项的一部分，改了就要断批
    sprite_batch_flush(batch);
    batch->sampler = *sampler;
}

// 断批：把当前累积的精灵发成一个渲染项
void sprite_batch_flush(sprite_batch_t *batch)
{
    ASSERT(batch != NULL);

    if (batch->sprites_in_batch == 0) {
        return;
    }

    const u32 index_count = batch->sprites_in_batch * SPRITE_INDICES_PER_QUAD;

    render_item_t item = render_item_default();
    item.pipeline = batch->current_pipeline;
    item.mesh = batch->mesh;
    item.layer = batch->current_layer;
    // 顶点已经是世界空间，模型矩阵是单位阵
    item.uniforms = object_uniforms_default();
    item.call.first_index = batch->batch_first_index;
    item.call.index_count = index_count;
    item.call.instance_count = 1;
    item.textures[RENDER_TEXTURE_SLOT_ALBEDO] = batch->current_texture;
    item.samplers[RENDER_TEXTURE_SLOT_ALBEDO] = batch->sampler;
    item.texture_count = 1;
    item.view_depth = batch->current_depth;

    renderer_push_item(batch->renderer, &item);

    batch->batch_first_index += index_count;
    batch->sprites_in_batch = 0;
    batch->batch_count++;
}

void sprite_batch_draw(sprite_batch_t *batch, const sprite_t *sprite, texture_handle_t texture)
{
    ASSERT(batch != NULL);
    ASSERT(sprite != NULL);

    if (batch->sprite_count >= batch->sprite_capacity) {
        // 容量按"整帧"计算：所有批次共用同一块顶点缓冲，
        // 中途复用会让前面批次的数据被覆盖（渲染队列是帧末统一绘制的）。
        batch->dropped_sprites++;
        if (batch->dropped_sprites == 1) {
            LOG_WARN("精灵批渲染器容量已满（%u），后续精灵将被丢弃。"
                     "请调大 sprite_batch_desc_t.max_sprites。",
                     batch->sprite_capacity);
        }
        return;
    }

    // 断批判定：只有纹理是在 draw 里决定的，所以这里只需比较纹理。
    //
    // 管线 / 图层 / 深度 都通过 set_* 接口改变，而那几个接口自己会断批 ——
    // 在这里重复判断反而会漏掉"set 之后一个精灵都没画"的情况。
    if (batch->sprites_in_batch > 0 && texture != batch->current_texture) {
        sprite_batch_flush(batch);
    }
    batch->current_texture = texture;

    vec2_t corners[4];
    sprite_corners(sprite, corners);

    const u32 color = sprite_pack_color(sprite->color);
    const f32 u0 = sprite->uv_min.x;
    const f32 v0 = sprite->uv_min.y;
    const f32 u1 = sprite->uv_max.x;
    const f32 v1 = sprite->uv_max.y;

    sprite_vertex_t *out = &batch->vertices[batch->sprite_count * SPRITE_VERTICES_PER_QUAD];

    // UV 的 V 轴方向：纹理坐标 v=0 对应上传数据的第一行（图片顶部），
    // 而世界坐标 Y 向上。所以精灵的**下**边要采 v1（图片底部）。
    // 搞反的表现是"所有精灵上下颠倒"。
    out[0].x = corners[0].x;
    out[0].y = corners[0].y;
    out[0].u = u0;
    out[0].v = v1;
    out[0].color = color;

    out[1].x = corners[1].x;
    out[1].y = corners[1].y;
    out[1].u = u1;
    out[1].v = v1;
    out[1].color = color;

    out[2].x = corners[2].x;
    out[2].y = corners[2].y;
    out[2].u = u1;
    out[2].v = v0;
    out[2].color = color;

    out[3].x = corners[3].x;
    out[3].y = corners[3].y;
    out[3].u = u0;
    out[3].v = v0;
    out[3].color = color;

    batch->sprite_count++;
    batch->sprites_in_batch++;
    if (batch->sprite_count > batch->peak_sprite_count) {
        batch->peak_sprite_count = batch->sprite_count;
    }
}

void sprite_batch_draw_region(sprite_batch_t *batch, vec2_t center, vec2_t size, f32 rotation,
                              const atlas_region_t *region, vec4_t color,
                              texture_handle_t texture)
{
    sprite_t sprite = sprite_make(center, size);
    sprite.rotation = rotation;
    sprite.color = color;
    if (region != NULL) {
        sprite.uv_min = region->uv_min;
        sprite.uv_max = region->uv_max;
    }
    sprite_batch_draw(batch, &sprite, texture);
}

void sprite_batch_end(sprite_batch_t *batch)
{
    ASSERT(batch != NULL);

    sprite_batch_flush(batch);

    if (batch->sprite_count > 0) {
        // 只上传用到的那一段。传整块容量会把没用的部分也过一遍总线。
        const usize used_bytes = (usize)batch->sprite_count * SPRITE_VERTICES_PER_QUAD *
                                 sizeof(sprite_vertex_t);
        renderer_update_mesh(batch->renderer, batch->mesh, batch->vertices, used_bytes, NULL, 0);
    }
}

// =============================================================================
//  查询
// =============================================================================
u32 sprite_batch_batch_count(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    return batch->batch_count;
}

u32 sprite_batch_sprite_count(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    return batch->sprite_count;
}

u32 sprite_batch_peak_sprite_count(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    return batch->peak_sprite_count;
}

u32 sprite_batch_dropped_sprites(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    return batch->dropped_sprites;
}

bool sprite_batch_is_full(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    return batch->sprite_count >= batch->sprite_capacity;
}

f32 sprite_batch_usage(const sprite_batch_t *batch)
{
    ASSERT(batch != NULL);
    if (batch->sprite_capacity == 0) {
        return 0.0f;
    }
    return (f32)batch->sprite_count / (f32)batch->sprite_capacity;
}
