# =============================================================================
#  examples/00_smoke/CMakeLists.txt
# =============================================================================

add_executable(00_smoke main.c)

target_link_libraries(00_smoke PRIVATE engine engine_warnings)
