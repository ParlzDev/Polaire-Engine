// =============================================================================
//  src/render/third_party/stb_image_impl.c —— stb_image 的实现单元
//
//  stb_image 是"单头库"：头文件里包含实现，但必须在**恰好一个** .c 文件里
//  定义 STB_IMAGE_IMPLEMENTATION 才会展开。单独放一个文件的好处是：
//    * 其它文件只 include 头文件，不会重复展开实现；
//    * 可以只给这一个文件关闭告警（见 CMakeLists 里的 COMPILE_OPTIONS -w），
//      不用为了第三方代码放宽整个引擎的告警标准。
//
//  ---- 为什么裁剪格式 ----
//
//  默认 stb_image 支持十几种格式，编译后约 1.5 万行。这里只保留游戏会用到的
//  六种，编译时间与体积都明显下降。需要新格式时加一行 STBI_ONLY_* 即可。
// =============================================================================
#define STB_IMAGE_IMPLEMENTATION

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_ONLY_HDR

// 不用线程局部缓冲（部分工具链下 TLS 配置麻烦），代价是失败原因字符串
// 不再线程安全。本引擎只在加载线程用，可接受。
#define STBI_NO_THREAD_LOCALS

// 关掉 stb 自带的 SSE2 路径：它在 -std=c11 严格模式下会用到一些
// 非标准的写法。纯 C 路径慢一些，但加载不是每帧的热路径。
#define STBI_NO_SIMD

#include "stb_image.h"
