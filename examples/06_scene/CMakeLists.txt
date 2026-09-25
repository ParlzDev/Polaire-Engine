# =============================================================================
#  examples/06_scene/CMakeLists.txt
# =============================================================================

add_executable(06_scene main.c)

target_link_libraries(06_scene PRIVATE engine engine_warnings)

# 场景存档是相对路径（assets/scenes/...），所以必须从仓库根目录启动。
# 存档不存在时示例会自己生成一份（见 main.c 里的作者路径），从别处启动也不会崩。
set_target_properties(06_scene PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
