# =============================================================================
#  examples/02_render/CMakeLists.txt
# =============================================================================

add_executable(02_render main.c)

target_link_libraries(02_render PRIVATE engine engine_warnings)
