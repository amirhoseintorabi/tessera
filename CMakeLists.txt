# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(tessera VERSION 0.1.0 LANGUAGES C
        DESCRIPTION "A slippy-map widget for embedded displays")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TESSERA_WERROR      "Treat warnings as errors"                        ON)
option(TESSERA_SANITIZE    "Build with address and UB sanitizers"            OFF)
option(TESSERA_BUILD_TESTS "Build the test suite and the demo"               ON)

set(TESSERA_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wformat=2 -Wcast-qual -Wnull-dereference
    -Wstrict-prototypes -Wmissing-prototypes -Wswitch-enum
)
if(TESSERA_WERROR)
    list(APPEND TESSERA_WARNINGS -Werror)
endif()

# --------------------------------------------------------------- the engine --
#
# C11 and libm. No display, no filesystem, no scheduler -- that is what makes it
# testable on a workstation and portable to anything with a C compiler.
add_library(tessera STATIC
    src/projection.c
    src/types.c
    src/queue.c
)
add_library(tessera::tessera ALIAS tessera)
target_include_directories(tessera PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(tessera PRIVATE ${TESSERA_WARNINGS})
target_compile_definitions(tessera PRIVATE _GNU_SOURCE)  # M_PI on strict libcs

find_library(M_LIB m)
if(M_LIB)
    target_link_libraries(tessera PUBLIC ${M_LIB})
endif()

if(TESSERA_SANITIZE)
    foreach(target tessera)
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

# ----------------------------------------------------------------- the tests --
if(TESSERA_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    foreach(suite projection types queue)
        add_executable(test_${suite} tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE tessera)
        target_compile_options(test_${suite} PRIVATE ${TESSERA_WARNINGS})
        target_compile_definitions(test_${suite} PRIVATE _GNU_SOURCE)
        add_test(NAME ${suite} COMMAND test_${suite})
    endforeach()
endif()
