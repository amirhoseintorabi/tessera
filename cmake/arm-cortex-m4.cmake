# SPDX-License-Identifier: MIT
#
# Cross-compile for a Cortex-M4F, a representative target for this kind of
# display.
#
# Building for a real target on every change is how "portable" stays a fact
# rather than an aspiration, and it is the only way the code-size figures in
# the README mean anything.
#
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-cortex-m4.cmake \
#         -DCMAKE_BUILD_TYPE=MinSizeRel -DTESSERA_BUILD_TESTS=OFF
#   cmake --build build-arm --target tessera

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)

# There is no libc startup to link against here, so let CMake's compiler probe
# build a static library rather than try to produce an executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_TESSERA_ARM_FLAGS
    "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
     -ffunction-sections -fdata-sections")

# -fno-rtti and -fno-exceptions belong on the C++ flags only; passing them to
# the C compiler is an error under -Werror.
set(CMAKE_C_FLAGS_INIT   "${_TESSERA_ARM_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_TESSERA_ARM_FLAGS} -fno-exceptions -fno-rtti")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
