#ifndef AWS_COMMON_CONFIG_H
#define AWS_COMMON_CONFIG_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * This header exposes compiler feature test results determined during cmake
 * configure time to inline function implementations. The macros defined here
 * should be considered to be an implementation detail, and can change at any
 * time.
 */
#define AWS_HAVE_GCC_OVERFLOW_MATH_EXTENSIONS
#define AWS_HAVE_GCC_INLINE_ASM
/* #undef AWS_HAVE_MSVC_INTRINSICS_X64 */
#define AWS_HAVE_POSIX_LARGE_FILE_SUPPORT
#define AWS_HAVE_EXECINFO
/* #undef AWS_HAVE_WINAPI_DESKTOP */
#define AWS_HAVE_LINUX_IF_LINK_H
#define AWS_HAVE_AVX2_INTRINSICS
#define AWS_HAVE_AVX512_INTRINSICS
#define AWS_HAVE_MM256_EXTRACT_EPI64
#define AWS_HAVE_CLMUL
/* #undef AWS_HAVE_ARM32_CRC */
/* #undef AWS_HAVE_ARMv8_1 */
/* #undef AWS_ARCH_ARM64 */
#define AWS_ARCH_INTEL
#define AWS_ARCH_INTEL_X64
#define AWS_USE_CPU_EXTENSIONS

#endif
