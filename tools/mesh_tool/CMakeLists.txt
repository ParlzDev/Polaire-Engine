# =============================================================================
#  tools/mesh_tool/CMakeLists.txt
# =============================================================================

add_executable(mesh_tool main.c)

target_link_libraries(mesh_tool PRIVATE engine engine_warnings)

# 默认从仓库根目录运行，这样 assets/models/*.obj 能直接找到。
set_target_properties(mesh_tool PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
