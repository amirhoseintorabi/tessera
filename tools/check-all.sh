#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Everything that has to pass before a change is worth committing. Kept as a
# script rather than only as a CI workflow so that it is the same command in
# both places: a check that only exists in CI is one you first run after the
# mistake has already been pushed.
#
#   tools/check-all.sh              gcc, clang, sanitizers, ARM
#   tools/check-all.sh --quick      gcc and the tests only
set -euo pipefail

cd "$(dirname "$0")/.."

quick=0
[[ "${1:-}" == "--quick" ]] && quick=1

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

step "gcc, warnings as errors"
cmake -B build/check-gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build/check-gcc -j"$(nproc)" >/dev/null
ctest --test-dir build/check-gcc --output-on-failure

if [[ $quick -eq 1 ]]; then
    printf '\n\033[1mquick check passed\033[0m\n'
    exit 0
fi

if have clang; then
    step "clang, warnings as errors"
    # clang and gcc disagree about enough -- -Wdouble-promotion on float
    # literals most often -- that building with only one of them lets real
    # findings through.  INFINITY is a float, for instance, and only clang
    # says so.
    CC=clang cmake -B build/check-clang -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    cmake --build build/check-clang -j"$(nproc)" >/dev/null
    ctest --test-dir build/check-clang --output-on-failure
else
    echo "clang not found: skipping"
fi

step "address and undefined-behaviour sanitizers"
cmake -B build/check-asan -DCMAKE_BUILD_TYPE=Debug \
      -DTESSERA_SANITIZE=ON >/dev/null
cmake --build build/check-asan -j"$(nproc)" >/dev/null
ctest --test-dir build/check-asan --output-on-failure

step "thread sanitizer, on the port tests"
# Only the port tests spawn a thread, so only they can show a data race --
# and the loader/GUI contention is the thing worth proving is race-free.
# setarch -R because TSan's shadow mapping does not survive some kernels'
# address-space randomisation.
cmake -B build/check-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=thread -g" >/dev/null
cmake --build build/check-tsan -j"$(nproc)" --target test_ports >/dev/null
setarch "$(uname -m)" -R ./build/check-tsan/test_ports

if have arm-none-eabi-gcc; then
    step "cross-compile for Cortex-M4F"
    cmake -B build/check-arm \
          -DCMAKE_TOOLCHAIN_FILE=cmake/arm-cortex-m4.cmake \
          -DCMAKE_BUILD_TYPE=MinSizeRel \
          -DTESSERA_BUILD_TESTS=OFF >/dev/null
    cmake --build build/check-arm -j"$(nproc)" >/dev/null

    echo
    echo "  code size on the target:"
    for lib in build/check-arm/libtessera.a \
               build/check-arm/libtessera_ports.a \
               build/check-arm/libtessera_framebuffer.a; do
        printf '    %-34s %s bytes\n' "$(basename "$lib")" \
               "$(arm-none-eabi-size -t "$lib" | tail -1 | awk '{print $1}')"
    done
else
    echo "arm-none-eabi-gcc not found: skipping the target build"
fi

printf '\n\033[1mall checks passed\033[0m\n'
