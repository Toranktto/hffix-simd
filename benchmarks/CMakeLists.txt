find_package(benchmark REQUIRED)

set(HFFIX_BENCH_DATA_DIR "${CMAKE_SOURCE_DIR}/benchmarks/data"
    CACHE PATH "Directory holding .fix datasets for hffix_benchmarks")
set(HFFIX_BENCH_MESSAGES 500000
    CACHE STRING "Number of messages in the synthetic dataset for bench_data")
set(HFFIX_BENCH_REPETITIONS 5
    CACHE STRING "Repetitions passed to Google Benchmark via the `bench` target")
set(HFFIX_BENCH_MIN_TIME "1s"
    CACHE STRING "--benchmark_min_time argument for the `bench` target")

add_executable(hffix_benchmarks benchmarks.cpp)
target_link_libraries(hffix_benchmarks PRIVATE
    hffix
    benchmark::benchmark
)
target_compile_definitions(hffix_benchmarks PRIVATE
    HFFIX_BENCH_DATA_DIR="${HFFIX_BENCH_DATA_DIR}"
)

add_custom_target(bench_data
    COMMAND ${CMAKE_COMMAND} -E make_directory ${HFFIX_BENCH_DATA_DIR}
    COMMAND $<TARGET_FILE:fixgen>
            -o ${HFFIX_BENCH_DATA_DIR}/synthetic.fix
            -n ${HFFIX_BENCH_MESSAGES}
    DEPENDS fixgen
    USES_TERMINAL
    COMMENT "Regenerating ${HFFIX_BENCH_DATA_DIR}/synthetic.fix (${HFFIX_BENCH_MESSAGES} messages)"
    VERBATIM
)

add_custom_target(bench
    COMMAND $<TARGET_FILE:hffix_benchmarks>
            --benchmark_min_time=${HFFIX_BENCH_MIN_TIME}
            --benchmark_repetitions=${HFFIX_BENCH_REPETITIONS}
            --benchmark_report_aggregates_only=true
            --benchmark_color=false
    DEPENDS hffix_benchmarks
    USES_TERMINAL
    COMMENT "Running hffix benchmarks (uses existing ${HFFIX_BENCH_DATA_DIR}/synthetic.fix)"
)
