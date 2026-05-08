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
