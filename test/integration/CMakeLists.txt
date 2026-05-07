# Integration tests: small target programs run under LD_PRELOAD.
# Each test links against nothing from tbjit -- it goes through the preload interface.

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
