// =============================================================================
//  src/render/third_party/stb_truetype_impl.c —— stb_truetype 的实现单元
//
//  与 stb_image 同理：头文件里包含实现，必须在**恰好一个** .c 文件里
//  定义 STB_TRUETYPE_IMPLEMENTATION 才会展开。
//  单独放一个文件，就能只给它关闭告警（见 CMakeLists）。
// =============================================================================
#define STB_TRUETYPE_IMPLEMENTATION

// 不编译 TrueType 的 hinting 与亚像素定位 —— 我们对字形做的是"按像素高度
// 光栅化后放进图集"，不需要额外精度，关掉能省几百行代码体积。
#define STBTT_DEF extern

#include "stb_truetype.h"
