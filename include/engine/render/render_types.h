// =============================================================================
//  engine/render/render_types.h —— 后端无关的渲染类型
//
//  ============================================================================
//   本文件的核心约束：**不允许出现任何图形 API 的具体类型**
//  ============================================================================
//
//  不许出现 GLuint / GLenum / VkImage / ID3D11Texture2D，也不许出现
//  "GL_RGBA8" / "VK_FORMAT_R8G8B8A8_UNORM" 这类具体枚举值。
//
//  为什么这条约束值得反复强调：
//  一旦接口里漏出一个 GL 类型，M13 接 D3D/Vulkan 时就必须改接口 ——
//  而接口一改，所有调用点、所有材质、所有资源管理代码都要跟着改。
//  那不是"加一个后端"，那是重写渲染层。
//
//  下面每个枚举都对应后端需要翻译的一组值：
//    pixel_format_t    -> GL_RGBA8 / DXGI_FORMAT_* / VK_FORMAT_*
//    vertex_format_t   -> glVertexAttribPointer 的 type+size / VkFormat
//    blend_factor_t    -> GL_SRC_ALPHA / D3D11_BLEND_* / VkBlendFactor
//    compare_op_t      -> GL_LESS / D3D11_COMPARISON_* / VkCompareOp
//
//  ============================================================================
//   为什么状态是"打包成 desc 一次性提交"而不是"逐项设置"
//  ============================================================================
//
//  OpenGL 的习惯是：
//      backend_set_blend(true);
//      backend_set_blend_func(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
//      backend_set_depth_test(true);
//      backend_set_cull(BACK);
//      backend_draw();
//
//  这套在 Vulkan 上完全无法实现：Vulkan 的管线状态对象是**不可变**的，
//  必须在创建时一次性给定全部状态，运行时只能整块切换。
//
//  所以接口收成 pipeline_desc_t：一次描述、一次创建、运行时只 bind。
//  GL 后端内部把这些状态"摊平"成若干个 glEnable/glBlendFunc 调用 ——
//  这正是"按 Vulkan 语义设计，让 GL 去适配"的具体体现。
// =============================================================================
#ifndef ENGINE_RENDER_RENDER_TYPES_H
#define ENGINE_RENDER_RENDER_TYPES_H

#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/core/vec.h"

// =============================================================================
//  资源句柄
//
//  全部基于 M0 的 handle_t（24 位索引 + 8 位代际）。
//  代际号让"资源已销毁但句柄还在用"变成一次可检出的失败，
//  而不是野指针或者"纹理莫名其妙变成了另一张"。
// =============================================================================
typedef handle_t shader_handle_t;
typedef handle_t texture_handle_t;
typedef handle_t mesh_handle_t;
typedef handle_t pipeline_handle_t;
typedef handle_t render_target_handle_t;

#define SHADER_HANDLE_INVALID HANDLE_INVALID
#define TEXTURE_HANDLE_INVALID HANDLE_INVALID
#define MESH_HANDLE_INVALID HANDLE_INVALID
#define PIPELINE_HANDLE_INVALID HANDLE_INVALID
#define RENDER_TARGET_HANDLE_INVALID HANDLE_INVALID

// =============================================================================
//  像素格式
//
//  只列出引擎真正会用到的格式。刻意不做"全格式覆盖"：
//  每多一种格式，每个后端都要多一条翻译分支，而其中大部分永远用不上。
//
//  命名规则：<通道><位数>_<类型>[_SRGB]
//  颜色纹理必须用 _SRGB 版本 —— 不用的话贴图会明显偏暗（详见 M2 决策文档）。
// =============================================================================
typedef enum pixel_format_t {
    PIXEL_FORMAT_INVALID = 0,

    // 单通道：遮罩、粗糙度、高度图
    PIXEL_FORMAT_R8_UNORM,

    // 双通道：UV、法线的 xy
    PIXEL_FORMAT_R8G8_UNORM,

    // 四通道 8 位：**颜色纹理的默认选择**
    PIXEL_FORMAT_R8G8B8A8_UNORM,
    PIXEL_FORMAT_R8G8B8A8_UNORM_SRGB, // 颜色纹理用这个
    PIXEL_FORMAT_B8G8R8A8_UNORM,
    PIXEL_FORMAT_B8G8R8A8_UNORM_SRGB, // 很多图片解码库的原始顺序

    // 高位深：HDR、后期处理中间结果
    PIXEL_FORMAT_R16G16B16A16_FLOAT,
    PIXEL_FORMAT_R32_FLOAT,

    // 深度
    PIXEL_FORMAT_D32_FLOAT,
    PIXEL_FORMAT_D24_UNORM_S8_UINT,

    PIXEL_FORMAT_COUNT
} pixel_format_t;

// 每像素字节数（未压缩格式）。用于校验上传数据大小。
// 返回 0 表示未知格式。
u32 pixel_format_bytes_per_pixel(pixel_format_t format);

// 该格式是否是 sRGB 编码。颜色纹理需要，数据纹理（法线/粗糙度）不能要。
bool pixel_format_is_srgb(pixel_format_t format);

// 该格式是否可作深度附件。
bool pixel_format_is_depth(pixel_format_t format);

// =============================================================================
//  顶点布局
//
//  Vulkan 风格：一组 binding + 一组 attribute，而不是 GL 那样逐个
//  glVertexAttribPointer 设置。
//
//  binding 表示"来自哪个缓冲区、步长多少"；attribute 表示"第 N 个属性
//  取自哪个 binding 的哪个偏移、什么格式"。
//
//  这种分离让"交错顶点缓冲"（position/normal/uv 交错在一个 buffer 里）
//  和"分离顶点缓冲"（各自一个 buffer）能用同一套描述表达，
//  也为 M3 的实例化数据（per-instance binding）留好了位置。
// =============================================================================
#define RENDER_MAX_VERTEX_BINDINGS 4
#define RENDER_MAX_VERTEX_ATTRIBUTES 16
#define RENDER_MAX_TEXTURE_SLOTS 8

// 各类资源的池容量。
//
// M0 的 pool 是**定容**的（不做扩容，因为扩容会让已发放的指针失效）。
// 这些数字对 M2~M6 够用；M7 的资源层会改成可增长的。
// 放在 render_types.h 而不是 renderer.h，是因为后端也要用它们开池，
// 而后端不应该依赖 renderer（依赖方向是 renderer -> backend，不能反过来）。
#define RENDER_MAX_SHADERS 64
#define RENDER_MAX_TEXTURES 512
#define RENDER_MAX_MESHES 1024
#define RENDER_MAX_PIPELINES 256
#define RENDER_MAX_RENDER_TARGETS 64

typedef enum vertex_semantic_t {
    VERTEX_SEMANTIC_POSITION = 0,
    VERTEX_SEMANTIC_NORMAL,
    VERTEX_SEMANTIC_TANGENT,
    VERTEX_SEMANTIC_BITANGENT,
    VERTEX_SEMANTIC_TEXCOORD0,
    VERTEX_SEMANTIC_TEXCOORD1,
    VERTEX_SEMANTIC_COLOR0,
    VERTEX_SEMANTIC_INSTANCE_MATRIX0, // M3 的实例化变换（占 4 个槽位）
    VERTEX_SEMANTIC_INSTANCE_COLOR,
    VERTEX_SEMANTIC_COUNT
} vertex_semantic_t;

typedef enum vertex_format_t {
    VERTEX_FORMAT_INVALID = 0,

    VERTEX_FORMAT_FLOAT1,
    VERTEX_FORMAT_FLOAT2,
    VERTEX_FORMAT_FLOAT3,
    VERTEX_FORMAT_FLOAT4,

    VERTEX_FORMAT_UINT1,
    VERTEX_FORMAT_UINT2,
    VERTEX_FORMAT_UINT3,
    VERTEX_FORMAT_UINT4,

    // 归一化整数：颜色（4 字节）、法线压缩等场景
    VERTEX_FORMAT_UNORM8X4,
    VERTEX_FORMAT_SNORM8X4,
    VERTEX_FORMAT_UNORM16X2,
    VERTEX_FORMAT_UNORM16X4,

    VERTEX_FORMAT_COUNT
} vertex_format_t;

u32 vertex_format_component_count(vertex_format_t format);
u32 vertex_format_byte_size(vertex_format_t format);

typedef struct vertex_binding_t {
    u32  stride;       // 相邻两个顶点在这个 binding 上的字节间隔
    bool per_instance; // true = 每实例前进一次（实例化渲染，M3 用）
} vertex_binding_t;

typedef struct vertex_attribute_t {
    vertex_semantic_t semantic;
    u8                binding; // 取自哪个 binding
    vertex_format_t   format;
    u32               offset;  // 相对该 binding 起始的字节偏移
} vertex_attribute_t;

typedef struct vertex_layout_t {
    vertex_binding_t   bindings[RENDER_MAX_VERTEX_BINDINGS];
    u32                binding_count;
    vertex_attribute_t attributes[RENDER_MAX_VERTEX_ATTRIBUTES];
    u32                attribute_count;
} vertex_layout_t;

// 按语义查属性下标；找不到返回 -1。
i32 vertex_layout_find_attribute(const vertex_layout_t *layout, vertex_semantic_t semantic);

// 校验布局自洽：binding 下标有效、偏移不越界、语义不重复。
// 返回 false 时把原因写进 out_reason（允许为 NULL）。
bool vertex_layout_validate(const vertex_layout_t *layout, const char **out_reason);

// ---------------------------------------------------------------- 标准顶点格式
//
// 引擎内置着色器与程序化网格统一使用这个"位置 + 法线 + UV"的交错格式。
// 32 字节，紧凑排布（不是 vec3_t 的 16 字节对齐版本 —— 顶点数据要传给 GPU，
// 每多 4 字节浪费在 10 万个顶点上就是 400KB）。
typedef struct mesh_vertex_t {
    f32 position[3]; // 偏移 0
    f32 normal[3];   // 偏移 12
    f32 texcoord[2]; // 偏移 24
} mesh_vertex_t;

_Static_assert(sizeof(mesh_vertex_t) == 32, "mesh_vertex_t 必须是 32 字节");

#define MESH_VERTEX_STRIDE 32u

// 返回与 mesh_vertex_t 对应的顶点布局描述。
// 属性 location 约定见 shader.h（location = 语义枚举值）。
vertex_layout_t vertex_layout_mesh_vertex(void);

// =============================================================================
//  索引与拓扑
// =============================================================================
typedef enum index_format_t {
    INDEX_FORMAT_NONE = 0, // 非索引绘制
    INDEX_FORMAT_U16,
    INDEX_FORMAT_U32
} index_format_t;

u32 index_format_byte_size(index_format_t format);

typedef enum primitive_topology_t {
    PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 0,
    PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    PRIMITIVE_TOPOLOGY_LINE_LIST,
    PRIMITIVE_TOPOLOGY_LINE_STRIP,
    PRIMITIVE_TOPOLOGY_POINT_LIST,
    PRIMITIVE_TOPOLOGY_COUNT
} primitive_topology_t;

// =============================================================================
//  混合
//
//  用完整的 blend_state_t 而不是"一个 blend func"：
//  Vulkan 的 VkPipelineColorBlendAttachmentState 就是这种结构
//  （颜色通道与 alpha 通道可以分开设，还带 write mask）。
//  按 GL 的 glBlendFunc 设计接口的话，等接 Vulkan 时会发现少了一半字段。
// =============================================================================
typedef enum blend_factor_t {
    BLEND_FACTOR_ZERO = 0,
    BLEND_FACTOR_ONE,
    BLEND_FACTOR_SRC_COLOR,
    BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    BLEND_FACTOR_DST_COLOR,
    BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    BLEND_FACTOR_SRC_ALPHA,
    BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    BLEND_FACTOR_DST_ALPHA,
    BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    BLEND_FACTOR_SRC_ALPHA_SATURATE,
    BLEND_FACTOR_COUNT
} blend_factor_t;

typedef enum blend_op_t {
    BLEND_OP_ADD = 0,
    BLEND_OP_SUBTRACT,
    BLEND_OP_REVERSE_SUBTRACT,
    BLEND_OP_MIN,
    BLEND_OP_MAX,
    BLEND_OP_COUNT
} blend_op_t;

// 颜色通道写掩码
#define BLEND_WRITE_R ((u8)0x1)
#define BLEND_WRITE_G ((u8)0x2)
#define BLEND_WRITE_B ((u8)0x4)
#define BLEND_WRITE_A ((u8)0x8)
#define BLEND_WRITE_ALL ((u8)0xF)

typedef struct blend_state_t {
    bool           enabled;
    blend_factor_t src_color;
    blend_factor_t dst_color;
    blend_op_t     color_op;
    blend_factor_t src_alpha;
    blend_factor_t dst_alpha;
    blend_op_t     alpha_op;
    u8             write_mask;
} blend_state_t;

// 不透明：关闭混合，写全部通道
blend_state_t blend_state_opaque(void);

// 标准 alpha 混合：src_alpha / one_minus_src_alpha（2D 精灵与半透明 3D 都用它）
blend_state_t blend_state_alpha(void);

// 预乘 alpha 混合：one / one_minus_src_alpha（粒子系统用，避免边缘发黑）
blend_state_t blend_state_premultiplied(void);

// 加法混合：src_alpha / one（光效、火焰）
blend_state_t blend_state_additive(void);

// =============================================================================
//  深度
// =============================================================================
typedef enum compare_op_t {
    COMPARE_OP_NEVER = 0,
    COMPARE_OP_LESS,
    COMPARE_OP_EQUAL,
    COMPARE_OP_LESS_EQUAL,
    COMPARE_OP_GREATER,
    COMPARE_OP_NOT_EQUAL,
    COMPARE_OP_GREATER_EQUAL,
    COMPARE_OP_ALWAYS,
    COMPARE_OP_COUNT
} compare_op_t;

typedef struct depth_state_t {
    bool         test_enabled;
    bool         write_enabled;
    compare_op_t compare;
} depth_state_t;

// 3D 不透明：测试开、写入开、LESS
depth_state_t depth_state_default(void);

// 3D 半透明：测试开、写入关（否则后面的透明物体会被挡住）
depth_state_t depth_state_read_only(void);

// 2D：全部关闭
depth_state_t depth_state_disabled(void);

// =============================================================================
//  光栅化
// =============================================================================
typedef enum cull_mode_t {
    CULL_MODE_NONE = 0,
    CULL_MODE_FRONT,
    CULL_MODE_BACK
} cull_mode_t;

typedef enum front_face_t {
    FRONT_FACE_COUNTER_CLOCKWISE = 0, // 逆时针为正面（引擎默认）
    FRONT_FACE_CLOCKWISE
} front_face_t;

typedef enum fill_mode_t {
    FILL_MODE_SOLID = 0,
    FILL_MODE_WIREFRAME
} fill_mode_t;

typedef struct raster_state_t {
    cull_mode_t  cull;
    front_face_t front_face;
    fill_mode_t  fill;
    bool         depth_bias_enabled; // 阴影贴图用（M4）
    f32          depth_bias_constant;
    f32          depth_bias_slope;
} raster_state_t;

raster_state_t raster_state_default(void);        // 背面剔除 + 逆时针正面 + 实心
raster_state_t raster_state_no_cull(void);        // 双面渲染（薄物体、2D）
raster_state_t raster_state_wireframe(void);      // 调试

// =============================================================================
//  多重采样
// =============================================================================
typedef struct multisample_state_t {
    u32 samples; // 1 = 关闭，4 = 4x MSAA
} multisample_state_t;

// =============================================================================
//  采样器
// =============================================================================
typedef enum sampler_filter_t {
    SAMPLER_FILTER_NEAREST = 0,
    SAMPLER_FILTER_LINEAR
} sampler_filter_t;

typedef enum sampler_mipmap_filter_t {
    SAMPLER_MIPMAP_NONE = 0, // 只用 base level
    SAMPLER_MIPMAP_NEAREST,
    SAMPLER_MIPMAP_LINEAR
} sampler_mipmap_filter_t;

typedef enum sampler_wrap_t {
    SAMPLER_WRAP_REPEAT = 0,
    SAMPLER_WRAP_MIRRORED_REPEAT,
    SAMPLER_WRAP_CLAMP_TO_EDGE,
    SAMPLER_WRAP_CLAMP_TO_BORDER
} sampler_wrap_t;

typedef struct sampler_desc_t {
    sampler_filter_t        min_filter;
    sampler_filter_t        mag_filter;
    sampler_mipmap_filter_t mipmap_filter;
    sampler_wrap_t          wrap_u;
    sampler_wrap_t          wrap_v;
    sampler_wrap_t          wrap_w;
    f32                     max_anisotropy; // 1.0 = 关闭
} sampler_desc_t;

sampler_desc_t sampler_desc_default(void);       // 三线性 + 重复
sampler_desc_t sampler_desc_pixel_art(void);     // 最近邻 + 钳制（像素风）
sampler_desc_t sampler_desc_clamp_linear(void);  // 双线性 + 钳制（UI、贴图预览）

// 精灵/图集专用。
//
// **必须用这个而不是 sampler_desc_default**：图集不生成 mipmap，
// 而 GL 里如果 MIN_FILTER 是 mipmap 过滤器、纹理却只有一级，
// 纹理会是"不完整"的 —— 采样结果是**全黑**，而且不报任何错。
// 这是 2D 渲染最常见的"精灵全黑"原因。
sampler_desc_t sampler_desc_sprite(void);

// =============================================================================
//  管线描述
//
//  这是"一次描述、一次创建、运行时只 bind"的载体，也是整个后端接口里
//  最 Vulkan 友好的一处设计。GL 后端内部把它摊平成若干个 glEnable/glBlendFunc。
// =============================================================================
typedef struct pipeline_desc_t {
    shader_handle_t         shader;
    vertex_layout_t         vertex_layout;
    primitive_topology_t    topology;
    raster_state_t          raster;
    depth_state_t           depth;
    blend_state_t           blend;
    multisample_state_t     multisample;
    const char             *debug_name;
} pipeline_desc_t;

pipeline_desc_t pipeline_desc_default(void);

// =============================================================================
//  着色器描述
//
//  源码是**不带 #version 与 uniform 块声明**的正文：
//  版本号由后端按上下文实际版本生成，uniform 块声明由引擎统一前置
//  （见 uniforms.h）。这样 C 结构体与 GLSL 声明的布局不可能漂移。
// =============================================================================
typedef struct shader_desc_t {
    const char *vertex_source;
    const char *fragment_source;
    const char *debug_name;
} shader_desc_t;

// =============================================================================
//  纹理描述
// =============================================================================
typedef struct texture_desc_t {
    u32            width;
    u32            height;
    u32            mip_levels;   // 0 = 由 generate_mipmaps 决定
    u32            array_layers; // 1 = 普通 2D 纹理
    pixel_format_t format;
    bool           generate_mipmaps;
    const char    *debug_name;
} texture_desc_t;

// =============================================================================
//  网格描述
//
//  支持多个 binding（交错或分离的顶点缓冲）+ 可选索引缓冲。
// =============================================================================
typedef struct mesh_binding_data_t {
    const void *data;
    usize       size;
} mesh_binding_data_t;

typedef struct mesh_desc_t {
    vertex_layout_t      layout;
    mesh_binding_data_t  bindings[RENDER_MAX_VERTEX_BINDINGS];
    const void          *index_data;
    usize                index_data_size;
    index_format_t       index_format;
    u32                  vertex_count;
    u32                  index_count;
    // true 表示这个网格的数据会被频繁更新（批渲染的动态顶点缓冲）。
    // 后端据此选择缓冲用途：GL 用 GL_DYNAMIC_DRAW 而不是 GL_STATIC_DRAW，
    // 这个选择会影响驱动的内存放置策略（显存 vs 系统内存），
    // 选错会让每帧更新变成跨总线拷贝。
    bool                 dynamic;
    const char          *debug_name;
} mesh_desc_t;

// =============================================================================
//  绘制调用
// =============================================================================
typedef struct draw_call_t {
    u32 index_count;    // 0 = 使用整个网格
    u32 first_index;
    i32 vertex_offset;
    u32 instance_count; // 0 视为 1
    u32 first_instance;
} draw_call_t;

draw_call_t draw_call_all(void);

// =============================================================================
//  视口与裁剪
// =============================================================================
typedef struct viewport_t {
    f32 x;
    f32 y;
    f32 width;
    f32 height;
    f32 min_depth; // 约定：0
    f32 max_depth; // 约定：1（深度范围 [0,1]，见 M0 决策）
} viewport_t;

typedef struct scissor_rect_t {
    i32 x;
    i32 y;
    u32 width;
    u32 height;
} scissor_rect_t;

// =============================================================================
//  渲染通道
//
//  Vulkan 的 render pass 是显式对象，并且必须绑定一个 framebuffer；
//  GL 没有对应概念（只有默认帧缓冲 + FBO）。
//  接口保留这个概念，是为了让 GL 后端也走同一条代码路径 ——
//  M4 的离屏渲染、M6 的"3D 渲染到纹理"都不需要改调用方。
// =============================================================================
typedef struct render_pass_desc_t {
    // 渲染目标。RENDER_TARGET_HANDLE_INVALID = 默认帧缓冲（窗口）。
    render_target_handle_t target;

    viewport_t     viewport;
    scissor_rect_t scissor;
    bool           has_scissor;

    bool clear_color;
    f32  clear_color_value[4];

    bool clear_depth;
    f32  clear_depth_value;

    bool clear_stencil;
    u32  clear_stencil_value;

    const char *debug_name;
} render_pass_desc_t;

// =============================================================================
//  渲染目标描述
//
//  samples > 1 时颜色附件用多重采样 renderbuffer，并自动创建一个单采样的
//  解析目标（resolve）供采样 —— 这一步不能省：MSAA 缓冲**不能**直接采样，
//  必须 blit 到单采样纹理。漏掉的话采样结果是黑屏或噪点。
// =============================================================================
typedef struct render_target_desc_t {
    u32 width;
    u32 height;

    // PIXEL_FORMAT_INVALID = 用默认值（颜色 R8G8B8A8_UNORM，深度 D24_UNORM_S8_UINT）
    pixel_format_t color_format;
    pixel_format_t depth_format;

    u32  samples;    // 1 = 关闭 MSAA
    bool has_depth;  // 深度附件（3D 渲染需要，纯后处理不需要）

    // 是否创建**可采样**的颜色纹理。
    // 阴影贴图这类"只写不采样颜色"的目标可以关掉它，省一块显存。
    bool sampleable_color;

    // 深度专用目标（阴影贴图）：没有颜色附件，深度附件可采样。
    bool depth_only;

    const char *debug_name;
} render_target_desc_t;

render_target_desc_t render_target_desc_default(void);

// 把描述里"没填"或"填错"的字段补成合法值，返回归一化后的描述。
//
// 为什么要单独抽出来：这段逻辑原本写在 GL 后端的 create_render_target 里，
// 于是"深度专用目标会自动关掉 MSAA"这类规则只有在有 GL 上下文时才能验证 ——
// 而它恰恰是最容易写错、错了又不报错的部分（比如忘了把 samples 压回 1，
// 就会去申请一个多重采样的深度纹理，驱动直接返回 GL_INVALID_OPERATION）。
//
// 归一化的规则（都是"让目标自洽"，不是"帮调用方猜意图"）：
//   * width/height 为 0 -> 返回全 0 描述（调用方据此判断失败）
//   * color_format/depth_format 为 INVALID -> 填默认值
//   * depth_format 非深度格式 -> 换成默认深度格式（否则 FBO 必然不完整）
//   * color_format 是深度格式 -> 换成默认颜色格式
//   * samples < 1 -> 1；非 2 的幂或超过上限 -> 回落到 1（MSAA 只支持有限档位）
//   * depth_only -> 强制 has_depth = true、sampleable_color = true、samples = 1
//   * has_depth == false 且 !depth_only -> depth_format 归为 INVALID（不建深度附件）
render_target_desc_t render_target_desc_normalize(const render_target_desc_t *desc);

// =============================================================================
//  渲染统计
// =============================================================================
typedef struct render_stats_t {
    u32   draw_calls;
    u32   render_items;
    u32   triangles;
    u32   vertices;
    u32   pipeline_switches;
    u32   texture_bindings;
    u32   buffer_uploads;
    usize buffer_upload_bytes;
} render_stats_t;

void render_stats_reset(render_stats_t *stats);
void render_stats_accumulate(render_stats_t *target, const render_stats_t *source);

#endif // ENGINE_RENDER_RENDER_TYPES_H
