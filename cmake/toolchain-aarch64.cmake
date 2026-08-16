# Cross-compile ReHLDS for AArch64 Linux.
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
#         -DREHLDS_ENABLE_JIT=OFF -DREHLDS_ENABLE_SSE=OFF -B build-arm64
#
# JIT and SSE must be off: the delta JIT is an IA-32 code generator and mathlib_sse.cpp is
# x86 intrinsics. Both have portable fallbacks that are exercised by the existing unit tests
# (see docs/audit/11-portable-fallbacks.md), which is why they were proven on x86-32 first.
#
# Run the result with qemu-user-static:
#   qemu-aarch64-static -L /usr/aarch64-linux-gnu ./engine_arm64
#
# EXPERIMENTAL, like the x86-64 target. See docs/audit/20-arm64.md.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
