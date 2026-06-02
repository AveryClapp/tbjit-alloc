# Offline tools that link against tbjit internals via tbjit_obj.

add_executable(tbjit_replay replay.cpp)
target_include_directories(tbjit_replay PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tbjit_replay PRIVATE tbjit_obj)
target_compile_options(tbjit_replay PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)

# bound_bench links the tbjit internals WITHOUT trampoline.cpp (no process-wide
# malloc interposition, no auto-started background thread) and supplies its own
# g_real_malloc shim. tbjit_notramp_obj keeps those sources -fPIC so the manual
# TLS access in codegen.cpp links (see CMakeLists.txt / TBJIT_NOTRAMP_SOURCES).
add_executable(tbjit_bound_bench bound_bench.cpp)
target_include_directories(tbjit_bound_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tbjit_bound_bench PRIVATE tbjit_notramp_obj)
target_compile_options(tbjit_bound_bench PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
