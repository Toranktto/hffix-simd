cmake_minimum_required(VERSION 3.20)

project(hffix2 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_hffix_default_build ON)
else()
    set(_hffix_default_build OFF)
endif()

option(HFFIX_BUILD
    "Build tests, integration examples, benchmarks and CLI utilities"
    ${_hffix_default_build})
option(HFFIX_BUILD_DOCS "Build Doxygen documentation" OFF)
option(HFFIX_BUILD_FIXSPEC_GEN
    "Build the fixspec-gen-fields utility (needed by hffix_generate_fields)"
    ${HFFIX_BUILD})
option(HFFIX_NATIVE_ARCH "Tune codegen for the host CPU (-march=native / -mcpu=native)" ON)
option(HFFIX_LTO "Enable full link-time optimization" ON)
option(HFFIX_SANITIZE
    "Enable AddressSanitizer + UndefinedBehaviorSanitizer (Debug builds)" OFF)
option(HFFIX_SANITIZE_THREAD
    "Enable ThreadSanitizer (Debug builds). Mutually exclusive with HFFIX_SANITIZE." OFF)

if(HFFIX_SANITIZE AND HFFIX_SANITIZE_THREAD)
    message(FATAL_ERROR
        "Pick at most one of HFFIX_SANITIZE / HFFIX_SANITIZE_THREAD; they are incompatible.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    if(HFFIX_SANITIZE OR HFFIX_SANITIZE_THREAD)
        set(CMAKE_BUILD_TYPE Debug)
    else()
        set(CMAKE_BUILD_TYPE Release)
    endif()
endif()

if(HFFIX_SANITIZE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "HFFIX_SANITIZE requested but unsupported on ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(HFFIX_SANITIZE_THREAD)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(WARNING "HFFIX_SANITIZE_THREAD requested but unsupported on ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(HFFIX_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _hffix_ipo_ok OUTPUT _hffix_ipo_err)
    if(_hffix_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        # CMake IPO defaults to -flto=thin for Clang; force full LTO instead.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            add_compile_options(-flto=full)
            add_link_options(-flto=full)
        endif()
    else()
        message(WARNING "LTO requested but not supported: ${_hffix_ipo_err}")
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

add_library(hffix INTERFACE)
add_library(hffix2::hffix ALIAS hffix)
target_include_directories(hffix INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(hffix INTERFACE -Wall -Wextra -Wpedantic)
    add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-fno-omit-frame-pointer>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(hffix INTERFACE /W4)
endif()

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/hffix_generate_fields.cmake)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hffix_generate_fields.cmake
        DESTINATION ${CMAKE_INSTALL_DATADIR}/hffix2/cmake)

add_subdirectory(utils)

if(HFFIX_BUILD)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(benchmarks)
endif()

if(HFFIX_BUILD_DOCS)
    add_subdirectory(docs)
endif()
