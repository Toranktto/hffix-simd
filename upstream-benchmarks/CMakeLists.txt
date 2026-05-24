cmake_minimum_required(VERSION 3.20)

project(hffix_upstream_benchmarks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Path to an upstream jamesdbrock/hffix include tree; must contain hffix.hpp.
# scripts/compare2upstream.sh sets this to the cloned upstream-src.
set(HFFIX_UPSTREAM_INCLUDE_DIR "" CACHE PATH
    "Path to upstream jamesdbrock/hffix include directory (must contain hffix.hpp).")
if(NOT HFFIX_UPSTREAM_INCLUDE_DIR)
    message(FATAL_ERROR
        "hffix_upstream_benchmarks: set "
        "-DHFFIX_UPSTREAM_INCLUDE_DIR=/path/to/upstream/include")
endif()
if(NOT EXISTS "${HFFIX_UPSTREAM_INCLUDE_DIR}/hffix.hpp")
    message(FATAL_ERROR
        "HFFIX_UPSTREAM_INCLUDE_DIR='${HFFIX_UPSTREAM_INCLUDE_DIR}' "
        "does not contain hffix.hpp.")
endif()

# Shared dataset path with the main repo's hffix_benchmarks. Default points at
# the sibling benchmarks/data directory.
set(HFFIX_BENCH_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../benchmarks/data"
    CACHE PATH "Directory holding .fix datasets")

option(HFFIX_NATIVE_ARCH "Tune codegen for the host CPU" ON)
option(HFFIX_LTO "Enable full link-time optimization" ON)

if(HFFIX_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            add_compile_options(-flto=full)
            add_link_options(-flto=full)
        endif()
    else()
        message(WARNING "LTO requested but not supported: ${_ipo_err}")
    endif()
endif()

if(HFFIX_NATIVE_ARCH)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            add_compile_options(-mcpu=native)
        else()
            add_compile_options(-march=native)
        endif()
    endif()
endif()

find_package(benchmark REQUIRED)

add_executable(hffix_upstream_benchmarks upstream_benchmarks.cpp)
target_link_libraries(hffix_upstream_benchmarks PRIVATE benchmark::benchmark)
target_include_directories(hffix_upstream_benchmarks PRIVATE
    "${HFFIX_UPSTREAM_INCLUDE_DIR}")
target_compile_definitions(hffix_upstream_benchmarks PRIVATE
    HFFIX_BENCH_DATA_DIR="${HFFIX_BENCH_DATA_DIR}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(hffix_upstream_benchmarks PRIVATE /Zc:__cplusplus)
endif()
