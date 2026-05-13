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

# Legacy alias — older docs/scripts referenced bench_throughput.
add_custom_target(bench_throughput DEPENDS bench_monomorphic)
