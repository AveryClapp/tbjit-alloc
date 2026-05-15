# Benchmark programs. Each is a standalone binary that runs the pattern
# in its name; under LD_PRELOAD with libtbjit it exercises the JIT path,
# without LD_PRELOAD it gives a glibc baseline. bench/run_bench.sh runs
# them all and prints a side-by-side table.

function(add_bench name src)
  add_executable(${name} ${src})
  target_compile_options(${name} PRIVATE -O2 -fno-exceptions -fno-rtti)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

add_bench(bench_monomorphic monomorphic.cpp)
add_bench(bench_arena       arena.cpp)
add_bench(bench_mixed       mixed.cpp)
add_bench(bench_pulse       pulse.cpp)

# Picker-specific patterns added with the deferred-items work; each is
# designed to push one of the new analyzer signals over its trigger
# threshold (multi-class, paired-stack, producer-consumer, hold lifetime).
add_bench(bench_bimodal         bimodal.cpp)
add_bench(bench_lifo            lifo.cpp)
add_bench(bench_producer_consumer producer_consumer.cpp)
target_link_libraries(bench_producer_consumer PRIVATE pthread)
add_bench(bench_hold            hold.cpp)

# Direct-call microbench: links against tbjit_obj to call codegen::compile
# and invoke the emitted routine without going through the LD_PRELOAD
# trampoline. Isolates the JIT inner-loop cost from interposition overhead.
add_executable(bench_micro_jit_direct micro_jit_direct.cpp)
target_compile_options(bench_micro_jit_direct PRIVATE
  -O2 -fno-exceptions -fno-rtti)
target_include_directories(bench_micro_jit_direct PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench_micro_jit_direct PRIVATE tbjit_obj)

# Legacy alias — older docs/scripts referenced bench_throughput.
add_custom_target(bench_throughput DEPENDS bench_monomorphic)
