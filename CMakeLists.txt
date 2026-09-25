# =============================================================================
#  MyEngine —— Windows 优先、纯 C11、2D/3D/2.5D 三模式统一的游戏引擎
#
#  构建（推荐走 CMakePresets）：
#      cmake --preset debug
#      cmake --build --preset debug
#      ctest --preset debug
#
#  本文件只描述"怎么编译"，不描述"引擎怎么设计"。
#  设计决策记录在 docs/ 下，改动前先读 docs/M0-设计决策.md。
# =============================================================================
cmake_minimum_required(VERSION 3.25)

project(MyEngine
        VERSION     0.1.0
        DESCRIPTION "Windows-first, pure-C11, 2D/3D/2.5D unified game engine"
        LANGUAGES   C)

# ------------------------------------------------------------------ 语言标准
# C_EXTENSIONS OFF => 传 -std=c11 而非 -std=gnu11。
# 目的：把"不小心用了 GNU 扩展"变成编译期错误，而不是等到换编译器才发现。
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)

# 生成 compile_commands.json，供 clangd / clang-tidy / IDE 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# ------------------------------------------------------------------ 构建选项
option(MYENGINE_BUILD_TESTS    "构建单元测试"                       ON)
option(MYENGINE_BUILD_EXAMPLES "构建示例程序"                       ON)
option(MYENGINE_BUILD_TOOLS    "构建离线工具（资产转换等）"          ON)
option(MYENGINE_WERROR         "把警告视为错误（CI 建议开启）"       OFF)
option(MYENGINE_SANITIZE       "启用 ASan + UBSan"                  OFF)

# =============================================================================
#  engine_warnings —— 统一的告警策略（INTERFACE 库，零编译产物）
#
#  为什么用 INTERFACE 而不是往每个 target 上重复贴 options：
#  告警集合是"团队约定"，只应有一份定义，新增 target 时不会漏配。
# =============================================================================
add_library(engine_warnings INTERFACE)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(engine_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wundef
        -Wvla                      # 禁止变长数组：栈溢出风险 + 无法静态分析
        -Wdouble-promotion         # float 被隐式提升为 double —— 数学库的头号性能坑
        -Wpointer-arith
        -Wwrite-strings
        -Wimplicit-fallthrough
        -Wcast-qual
        -Wno-unused-parameter      # 接口实现常需占位参数，此条噪音大于价值

        # 内置 shader 的 GLSL 源码是单个长字符串字面量（PBR 那个约 5.3 KB）。
        # ISO C99 只保证支持 4095 字符，超了会告警。
        # 为什么不拆成多段拼接：拼接发生在翻译阶段 6，clang 仍然按拼接后的长度告警；
        # 而把 GLSL 切成几段再拼，会让 shader 与 pbr.c 里的 CPU 参考实现对不上，
        # 将来排查"GPU 和 CPU 算出来不一样"时非常难受。
        # 本项目只支持 clang / MSVC，两者都没有 4095 的限制，故直接关掉这条。
        -Wno-overlength-strings
    )
elseif(MSVC)
    target_compile_options(engine_warnings INTERFACE
        /W4
        /permissive-
        /utf-8
    )
endif()

if(MYENGINE_WERROR)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(engine_warnings INTERFACE -Werror)
    else()
        target_compile_options(engine_warnings INTERFACE /WX)
    endif()
endif()

if(MYENGINE_SANITIZE)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(engine_warnings INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all)
        target_link_options(engine_warnings INTERFACE
            -fsanitize=address,undefined)
    else()
        message(WARNING "MYENGINE_SANITIZE 目前仅对 Clang 生效，已忽略。")
    endif()
endif()

# =============================================================================
#  engine —— 引擎静态库
#
#  源文件按分层放在不同目录：
#    src/core/            基础层（无平台依赖，只通过 core/*.h 里的"端口"调用平台）
#    src/platform/win32/  平台层
#    src/render/          渲染层
#
#  目前合成一个库而不是拆成三个：
#   * core 层需要 console / mutex 两个端口，它们的实现在平台层。
#     拆库会让 core 反向依赖 platform，比合成一个库更混乱。
#   * 分层的强制靠目录约定 + 代码审查（core 不允许 include engine/platform/*）。
#     等 M5 之后模块多了、依赖关系稳定了，再考虑拆成多个 target。
#
#  显式列出源文件而非 GLOB：新增文件必须改这里。
#  代价是每次加文件多改一行，收益是"构建系统里的东西永远等于仓库里的东西"，
#  不会因为 file(GLOB) 的缓存行为出现"本地能编、CI 编不过"。
# =============================================================================
add_library(engine STATIC
    # 基础层
    src/core/assert.c
    src/core/log.c
    src/core/math.c
    src/core/memory.c
    src/core/array.c
    src/core/hash.c
    src/core/byte_io.c
    src/core/json.c
    # 平台层（Win32）
    src/platform/win32/win32_console.c
    src/platform/win32/win32_thread.c
    src/platform/win32/win32_time.c
    src/platform/win32/win32_input.c
    src/platform/win32/win32_window.c
    # 渲染层
    src/render/render_types.c
    src/render/uniforms.c
    src/render/camera.c
    src/render/render_queue.c
    src/render/renderer.c
    src/render/shader.c
    src/render/texture.c
    src/render/mesh.c
    src/render/material.c
    src/render/obj_loader.c
    # 3D 管线（M4）
    src/render/frustum.c
    src/render/light.c
    src/render/pbr.c
    src/render/shadow.c
    src/render/mesh_file.c
    # ECS 层（M5）。刻意不含任何 render 头：桥接在 src/scene/。
    src/ecs/ecs_world.c
    src/ecs/ecs_component.c
    src/ecs/ecs_query.c
    src/ecs/ecs_system.c
    src/ecs/ecs_transform.c
    # 场景层（M5）：唯一同时看得见 ecs 与 render 的一层
    src/scene/scene.c
    src/scene/scene_io.c
    src/scene/scene_json.c
    src/scene/prefab.c
    src/scene/ecs_submit.c
    # 2D 管线（M3）
    src/render/sprite.c
    src/render/sprite_batch.c
    src/render/camera_2d.c
    src/render/texture_atlas.c
    src/render/debug_draw.c
    src/render/font.c
    src/render/text.c
    src/render/tilemap.c
    src/render/gl_loader.c
    src/render/gl_context_win32.c
    src/render/backend/opengl/gl_backend.c
    # 第三方（单头库的实现单元）
    src/render/third_party/stb_image_impl.c
    src/render/third_party/stb_truetype_impl.c
)

add_library(MyEngine::engine ALIAS engine)

# 第三方单头库放在 third_party/ 下，只给需要它的实现文件用。
# 用 PRIVATE：业务代码不应该直接 include stb_image.h。
target_include_directories(engine
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# 第三方代码单独关掉告警。
#
# stb_image 是 public domain 的第三方代码，用 -Wall -Wextra -Wpedantic 编译
# 会产生几十条它自己风格的告警。为了它把整个引擎的告警标准放宽是本末倒置；
# 单独 -w 掉这一个文件才是正确做法。
#
# 注意 COMPILE_OPTIONS 是**追加**到目标选项之后的，所以 -w 会覆盖前面的 -Wall。
set_source_files_properties(
    src/render/third_party/stb_image_impl.c
    src/render/third_party/stb_truetype_impl.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(engine PRIVATE engine_warnings)

# 非 Windows 平台需要显式链接 libm
if(NOT WIN32)
    target_link_libraries(engine PUBLIC m)
endif()

# Win32 系统库：
#   opengl32  GL 1.1 的导入函数 + wglGetProcAddress
#   user32   窗口、消息、DPI
#   gdi32    ChoosePixelFormat / SetPixelFormat / DescribePixelFormat / SwapBuffers
#   winmm    timeBeginPeriod（提升系统计时器精度，帧率限制器依赖它）
if(WIN32)
    target_link_libraries(engine PUBLIC opengl32 user32 gdi32 winmm)
endif()

target_compile_definitions(engine
    PUBLIC
        # Win32 应用必须是 Unicode 版本，且要显式告诉 Windows 头文件
        $<$<PLATFORM_ID:Windows>:UNICODE>
        $<$<PLATFORM_ID:Windows>:_UNICODE>
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        # MSVC CRT 会把 strcpy/vsnprintf 之类标记为"不安全"，
        # 我们用的是标准 C 接口，不需要它的安全版本。
        $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    PRIVATE
        $<$<CONFIG:Debug>:ENGINE_DEBUG=1>
        $<$<CONFIG:RelWithDebInfo>:ENGINE_DEBUG=1>
        $<$<CONFIG:Release>:ENGINE_DEBUG=0>
        $<$<CONFIG:MinSizeRel>:ENGINE_DEBUG=0>
)

# ------------------------------------------------------------------ 子目录
if(MYENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MYENGINE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MYENGINE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ------------------------------------------------------------------ 摘要
message(STATUS "")
message(STATUS "MyEngine ${PROJECT_VERSION}")
message(STATUS "  编译器      : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  编译器路径  : ${CMAKE_C_COMPILER}")
message(STATUS "  构建类型    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  单元测试    : ${MYENGINE_BUILD_TESTS}")
message(STATUS "  示例        : ${MYENGINE_BUILD_EXAMPLES}")
message(STATUS "  离线工具    : ${MYENGINE_BUILD_TOOLS}")
message(STATUS "  Sanitizer   : ${MYENGINE_SANITIZE}")
message(STATUS "")
