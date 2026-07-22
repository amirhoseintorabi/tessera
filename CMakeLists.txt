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
option(TESSERA_BUILD_PORTS "Build the FatFs, CMSIS-RTOS and emWin ports"     ON)

# Where to find real emWin, FatFs and CMSIS headers. Empty means "use the
# interoperability declarations under port/*/interop", which is what CI does --
# see port/emwin/interop/README.md for what that does and does not establish.
set(TESSERA_EMWIN_DIR "" CACHE PATH  "Directory containing a licensed emWin's headers")
set(TESSERA_FATFS_DIR "" CACHE PATH  "Directory containing FatFs's ff.h")
set(TESSERA_CMSIS_DIR "" CACHE PATH  "Directory containing cmsis_os.h")

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
    src/cache.c
    src/grid.c
    src/view.c
    src/marker.c
    src/map.c
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

# ------------------------------------------------------- the software render --
#
# A tile source that synthesises tiles and a painter that writes into an RGB
# framebuffer. Not a mock: a second, independent implementation of the same two
# interfaces the display bindings implement, which is what makes the engine's
# output something you can look at.
add_library(tessera_framebuffer STATIC port/framebuffer/framebuffer.c)
add_library(tessera::framebuffer ALIAS tessera_framebuffer)
target_include_directories(tessera_framebuffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/port/framebuffer)
target_link_libraries(tessera_framebuffer PUBLIC tessera)
target_compile_options(tessera_framebuffer PRIVATE ${TESSERA_WARNINGS})

if(TESSERA_SANITIZE)
    foreach(target tessera tessera_framebuffer)
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

# ---------------------------------------------------------------- the ports --
#
# The sources that go into firmware. They cross-compile for a target with
# nothing from interop/ but the declarations, which is why the two are kept
# apart.
if(TESSERA_BUILD_PORTS)
    add_library(tessera_ports STATIC
        port/fatfs/tessera_fatfs.c
        port/cmsis-rtos/tessera_rtos.c
    )
    add_library(tessera::ports ALIAS tessera_ports)
    target_include_directories(tessera_ports PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/port/fatfs
        ${CMAKE_CURRENT_SOURCE_DIR}/port/cmsis-rtos
    )

    # Real headers when given, the interoperability declarations otherwise.
    foreach(dep EMWIN FATFS CMSIS)
        if(TESSERA_${dep}_DIR)
            target_include_directories(tessera_ports PUBLIC ${TESSERA_${dep}_DIR})
        endif()
    endforeach()
    if(NOT TESSERA_FATFS_DIR)
        target_include_directories(tessera_ports PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/port/fatfs/interop)
    endif()
    if(NOT TESSERA_CMSIS_DIR)
        target_include_directories(tessera_ports PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/port/cmsis-rtos/interop)
    endif()

    target_link_libraries(tessera_ports PUBLIC tessera)
    target_compile_options(tessera_ports PRIVATE ${TESSERA_WARNINGS})
    target_compile_definitions(tessera_ports PRIVATE _GNU_SOURCE)

    # The interoperability implementations: stdio, pthreads and a recorder.
    # Host only, and deliberately not linked against tessera_ports -- the
    # dependency runs the other way, so a consumer lists ports first.
    if(NOT CMAKE_CROSSCOMPILING
       AND NOT TESSERA_EMWIN_DIR AND NOT TESSERA_FATFS_DIR AND NOT TESSERA_CMSIS_DIR)
        add_library(tessera_interop STATIC
            port/fatfs/interop/ff_stdio.c
            port/cmsis-rtos/interop/cmsis_os_pthread.c
        )
        target_include_directories(tessera_interop PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/port/fatfs/interop
            ${CMAKE_CURRENT_SOURCE_DIR}/port/cmsis-rtos/interop
        )
        target_compile_options(tessera_interop PRIVATE ${TESSERA_WARNINGS})
        target_compile_definitions(tessera_interop PRIVATE _GNU_SOURCE)

        find_package(Threads REQUIRED)
        target_link_libraries(tessera_interop PUBLIC Threads::Threads)
    endif()
endif()

# ----------------------------------------------------------------- the tests --
if(TESSERA_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    foreach(suite projection types queue cache grid view marker map)
        add_executable(test_${suite} tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE tessera_framebuffer)
        target_compile_options(test_${suite} PRIVATE ${TESSERA_WARNINGS})
        target_compile_definitions(test_${suite} PRIVATE _GNU_SOURCE)
        add_test(NAME ${suite} COMMAND test_${suite})
    endforeach()

    # Renders frames to PPM files. Run by the suite as well, because a demo
    # nobody builds is a demo that has quietly stopped compiling.
    add_executable(tessera_demo tests/demo.c)
    target_link_libraries(tessera_demo PRIVATE tessera_framebuffer)
    target_compile_options(tessera_demo PRIVATE ${TESSERA_WARNINGS})
    target_compile_definitions(tessera_demo PRIVATE _GNU_SOURCE)
    add_test(NAME demo COMMAND tessera_demo)
endif()
