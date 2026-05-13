cmake_minimum_required(VERSION 3.20)
project(tbjit-alloc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# tbjit_obj is linked into both libtbjit.so (PIC) and the unit-test
# executables. Building everything PIE matches the codegen.cpp's TLS access
# pattern — without this, the linker tries to relax IE→LE for the
# unit-test executables and fails on `tl_*` references emitted as
# GOTTPOFF relocations.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT UNIX OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  message(WARNING "tbjit-alloc targets Linux x86-64; building on host for dev/test only")
endif()

set(TBJIT_SOURCES
  src/alloc/alloc.cpp
  src/trace/trace.cpp
  src/trace/writer.cpp
  src/trace/trampoline.cpp
  src/analysis/analysis.cpp
  src/analysis/dump.cpp
  src/codegen/codegen.cpp
  src/codegen/emitter.cpp
  src/codegen/tls.cpp
  src/codegen/slow_init.cpp
  src/dispatch/dispatch.cpp
  src/deopt/deopt.cpp
  src/seg/segment.cpp
)

set(TBJIT_COMPILE_OPTIONS -Wall -Wextra -fno-exceptions -fno-rtti)

add_library(tbjit SHARED ${TBJIT_SOURCES})
target_include_directories(tbjit PRIVATE src)
target_compile_options(tbjit PRIVATE ${TBJIT_COMPILE_OPTIONS})

add_library(tbjit_shadow SHARED ${TBJIT_SOURCES} src/shadow/shadow.cpp)
target_include_directories(tbjit_shadow PRIVATE src)
target_compile_options(tbjit_shadow PRIVATE ${TBJIT_COMPILE_OPTIONS})
target_compile_definitions(tbjit_shadow PRIVATE TBJIT_SHADOW=1)

# Object library for tests to link against internals directly
add_library(tbjit_obj OBJECT ${TBJIT_SOURCES})
target_include_directories(tbjit_obj PRIVATE src)
target_compile_options(tbjit_obj PRIVATE ${TBJIT_COMPILE_OPTIONS})

enable_testing()
add_subdirectory(test)
add_subdirectory(bench)
add_subdirectory(tools)
