# =============================================================================
#  examples/03_sprite/CMakeLists.txt
# =============================================================================

add_executable(03_sprite main.c)

target_link_libraries(03_sprite PRIVATE engine engine_warnings)
