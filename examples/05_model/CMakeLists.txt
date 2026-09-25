# =============================================================================
#  examples/05_model/CMakeLists.txt
# =============================================================================

add_executable(05_model main.c)

target_link_libraries(05_model PRIVATE engine engine_warnings)

# 示例默认从仓库根目录运行，这样 assets/models/*.obj 能被找到。
# 找不到时会自动回落到内置模型，所以即使从别处启动也不会崩。
set_target_properties(05_model PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
