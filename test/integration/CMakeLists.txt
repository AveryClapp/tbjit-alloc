# Integration tests: small target programs run under LD_PRELOAD.
# Each test links against nothing from tbjit — it goes through the preload interface.

add_executable(int_hello hello.cpp)

add_test(
  NAME int_hello_preload
  COMMAND ${CMAKE_COMMAND} -E env
    LD_PRELOAD=$<TARGET_FILE:tbjit>
    $<TARGET_FILE:int_hello>
)
