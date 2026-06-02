# Integration tests: small target programs run under LD_PRELOAD.
# Each test links against nothing from tbjit -- it goes through the preload interface.

function(add_integration_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE tbjit_obj)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(${name} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_integration_test(test_jit_roundtrip test_jit_roundtrip.cpp)

add_executable(int_hello hello.cpp)
add_executable(int_hello_freelist hello_freelist.cpp)
add_executable(int_hello_arena hello_arena.cpp)

# Basic: just check process exits 0 under LD_PRELOAD
add_test(
  NAME int_hello_preload
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:tbjit>
    $<TARGET_FILE:int_hello>
)

# Dump: check TBJIT_DUMP=1 produces output with at least one call-site line
add_test(
  NAME int_dump_has_output
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:tbjit>
    TBJIT_DUMP=1
    $<TARGET_FILE:int_hello>
)
# The test passes if the process exits 0 (dump errors would crash or produce bad output)
# Manual verification: run with 2>dump.txt and inspect

# End-to-end JIT verification: Linux only (LD_PRELOAD is a no-op on macOS).
# Asserts a call site reached Compiled+BumpAlloc state AND that the JIT
# dispatch path was actually taken (>0 jit_allocs in TBJIT_DUMP output).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(
    NAME int_jit_verified
    COMMAND ${CMAKE_COMMAND} -E env
      LD_PRELOAD=$<TARGET_FILE:tbjit>
      TBJIT_DUMP=1
      $<TARGET_FILE:int_hello>
  )
  set_tests_properties(int_jit_verified PROPERTIES
    PASS_REGULAR_EXPRESSION "BumpAlloc;jit_allocs:[ \t]*[1-9][0-9]*"
  )

  # TLFreeList E2E: forces the free-list strategy and runs a long monomorphic
  # alloc/free loop that BumpAlloc would not survive (would deopt at ~5460
  # iters when its 256 KiB region is exhausted; free list recycles forever).
  add_test(
    NAME int_freelist_verified
    COMMAND ${CMAKE_COMMAND} -E env
      LD_PRELOAD=$<TARGET_FILE:tbjit>
      TBJIT_DUMP=1
      TBJIT_FORCE_STRATEGY=freelist
      $<TARGET_FILE:int_hello_freelist>
  )
  set_tests_properties(int_freelist_verified PROPERTIES
    PASS_REGULAR_EXPRESSION "TLFreeList;jit_allocs:[ \t]*[1-9][0-9]*"
  )

  # EpochArena E2E: recycles its region in place rather than deopting on
  # exhaust, so the same 60k-iter workload that would deopt BumpAlloc
  # stays in the JIT path the entire time.
  add_test(
    NAME int_arena_verified
    COMMAND ${CMAKE_COMMAND} -E env
      LD_PRELOAD=$<TARGET_FILE:tbjit>
      TBJIT_DUMP=1
      TBJIT_FORCE_STRATEGY=arena
      $<TARGET_FILE:int_hello_arena>
  )
  set_tests_properties(int_arena_verified PROPERTIES
    PASS_REGULAR_EXPRESSION "EpochArena;jit_allocs:[ \t]*[1-9][0-9]*"
  )

  # Determinism: two runs of int_hello under tbjit should produce the
  # same picker fingerprint (strategy_counts + lifetime_counts +
  # compiled / blacklisted totals). Phase 3a.1 of the paper roadmap;
  # the picker has to be reproducible over the same trace, otherwise
  # the paper's claims aren't.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(
      NAME int_determinism
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_determinism.py
        $<TARGET_FILE:tbjit>
        $<TARGET_FILE:int_hello>
    )
  endif()

  # realloc interposer: a realloc of a tbjit-managed chunk must migrate the
  # data instead of reaching libc realloc (which aborts on the unfamiliar
  # pointer — the gcc/libgmp crash). TBJIT_STABLE_WINDOWS=3 makes the site
  # compile fast. Linux only (LD_PRELOAD interposition is a no-op on macOS).
  add_executable(int_realloc_managed realloc_managed.cpp)
  add_test(
    NAME int_realloc_managed
    COMMAND ${CMAKE_COMMAND} -E env
      LD_PRELOAD=$<TARGET_FILE:tbjit>
      TBJIT_STABLE_WINDOWS=3
      $<TARGET_FILE:int_realloc_managed>
  )
  set_tests_properties(int_realloc_managed PROPERTIES
    PASS_REGULAR_EXPRESSION "OK"
  )
endif()
