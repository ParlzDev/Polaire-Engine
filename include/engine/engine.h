// =============================================================================
//  engine/engine.h —— 引擎总头文件
//
//  包含顺序 = 依赖顺序（从下到上）：
//    core      基础层：类型、内存、数学、容器、日志
//    platform  平台层：窗口、输入、计时
//    render    渲染层：GL 上下文与后端
//
//  上层可以包含下层的头文件，反过来不行。
//  例外是 core 自己声明的"端口"（core/console.h、core/thread.h）——
//  它们的实现在平台层，但接口属于 core，所以 core 不构成对 platform 的依赖。
//
//  M5 的 ecs/ 与 scene/ **刻意不进这个总头文件**：
//    1. 00_smoke 这类最小示例自己定义了 entity_t / scene 之类的占位类型，
//       总头文件一放开就是 typedef 重定义 —— 让它们显式包含才是诚实的做法；
//    2. ECS 层不该和"引擎 = 一个头"绑在一起，物理/脚本/编辑器头表各自按需包含
//       （engine/ecs/ecs.h 与 engine/scene/scene.h 是分开的入口，故意的）。
//
//  后续按里程碑逐步加入：
//    M7  resource/   资源加载与热重载
// =============================================================================
#ifndef ENGINE_ENGINE_H
#define ENGINE_ENGINE_H

#include "engine/core/core.h"
#include "engine/platform/platform.h"
#include "engine/render/render.h"

#define MYENGINE_VERSION_MAJOR 0
#define MYENGINE_VERSION_MINOR 1
#define MYENGINE_VERSION_PATCH 0
#define MYENGINE_VERSION_STRING "0.1.0"

#endif // ENGINE_ENGINE_H
