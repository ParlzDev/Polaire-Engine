// =============================================================================
//  engine/platform/platform.h —— 平台层总入口
//
//  这一层只封装系统调用，**不含任何游戏逻辑**。
//  改动这里的代码时请守住这条线：一旦平台层开始判断"玩家有没有跳跃"，
//  移植到 Linux/macOS 就变成重写而不是替换。
// =============================================================================
#ifndef ENGINE_PLATFORM_PLATFORM_H
#define ENGINE_PLATFORM_PLATFORM_H

#include "engine/platform/input.h"
#include "engine/platform/time.h"
#include "engine/platform/window.h"

#endif // ENGINE_PLATFORM_PLATFORM_H
