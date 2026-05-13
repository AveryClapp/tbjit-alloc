# Offline tools that link against tbjit internals via tbjit_obj.

add_executable(tbjit_replay replay.cpp)
target_include_directories(tbjit_replay PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tbjit_replay PRIVATE tbjit_obj)
target_compile_options(tbjit_replay PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
