cmake_minimum_required(VERSION 3.20)

project(hffix-fuzz LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "hffix-fuzz requires LLVM Clang with the libFuzzer runtime. "
        "(got '${CMAKE_CXX_COMPILER_ID}').")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# libFuzzer wants `-O1 -g` plus the sanitizer instrumentation; pair it with
# AddressSanitizer + UndefinedBehaviorSanitizer so every fuzz iteration also
# checks for OOB, UAF, signed overflow, alignment, and bad casts.
set(_fuzz_flags
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined
    -g
    -O1)

add_executable(fuzz_reader fuzz_reader.cpp)
target_include_directories(fuzz_reader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(fuzz_reader PRIVATE ${_fuzz_flags})
target_link_options(fuzz_reader PRIVATE ${_fuzz_flags})

set(_dataset_dir "${CMAKE_CURRENT_BINARY_DIR}/dataset")
set(_dataset_stamp "${CMAKE_CURRENT_BINARY_DIR}/dataset.stamp")
set(_seed_src "${CMAKE_CURRENT_SOURCE_DIR}/../tests/data")

file(GLOB _seed_files CONFIGURE_DEPENDS "${_seed_src}/*")

add_custom_command(
    OUTPUT "${_dataset_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dataset_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_seed_src}" "${_dataset_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_dataset_stamp}"
    DEPENDS ${_seed_files}
    VERBATIM)

add_custom_target(fuzz_dataset DEPENDS "${_dataset_stamp}")

add_custom_target(fuzz
    COMMAND ${CMAKE_COMMAND} -E echo "Running fuzz_reader (Ctrl+C to stop)"
    COMMAND $<TARGET_FILE:fuzz_reader>
            -print_final_stats=1
            -timeout=10
            -max_total_time=$<IF:$<BOOL:${HFFIX_FUZZ_MAX_TIME}>,${HFFIX_FUZZ_MAX_TIME},60>
            ${_dataset_dir}
    DEPENDS fuzz_reader fuzz_dataset
    USES_TERMINAL
    VERBATIM)
