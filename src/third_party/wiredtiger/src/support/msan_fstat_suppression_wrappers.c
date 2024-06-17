/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <sys/stat.h>
#include "string.h"

/*
 * FIXME-WT-13103 If you add this file to the list of compiled sources and build with the
 * `-Wl,--wrap=XXX` linker flag the compiler will wrap calls to function XXX with the respective
 * __wrap_XXX function defined below. We need this because MSan reports false positives when setting
 * memory via *stat() calls, so we've defined __wrap_*stat functions that first zero the memory to
 * suppress the false positive. Once WiredTiger's minimum supported LLVM version is 14 this fix
 * (https://github.com/llvm/llvm-project/commit/4e1a6c07052b466a2a1cd0c3ff150e4e89a6d87a) is
 * available and this file can be deleted.
 */

extern int __real_stat(const char *path, struct stat *buf);
extern int __real_fstat(int fd, struct stat *statbuf);

/*
 * MSan is only supported on Clang and these __wrap_* functions should only be used when WiredTiger
 * is built with MSan.
 */
#ifndef __clang__
#error "msan_fstat_suppression_wrappers.c should only be compiled with Clang"
#elif !__has_feature(memory_sanitizer)
#error "msan_fstat_suppression_wrappers.c should only be compiled with MSan"
#endif

/*
 * We've already checked that we're building with Clang above. LLVM and Clang versions are identical
 * so Clang 14+ implies we're building with LLVM 14+.
 */
#if __clang_major__ >= 14
#pragma message( \
  "Building with Clang 14 or later. Please check if we can close WT-13103 and delete this file.")
#endif

int __wrap_stat(const char *path, struct stat *buf);
int __wrap_fstat(int fd, struct stat *buf);

/*
 * __wrap_stat --
 *     Zero out memory before setting it with stat(). This suppresses an MSan false positive.
 */
int
__wrap_stat(const char *path, struct stat *buf)
{
    memset(buf, 0, sizeof(*buf));
    return (__real_stat(path, buf));
}

/*
 * __wrap_fstat --
 *     Zero out memory before setting it with fstat(). This suppresses an MSan false positive.
 */
int
__wrap_fstat(int fd, struct stat *buf)
{
    memset(buf, 0, sizeof(*buf));
    return (__real_fstat(fd, buf));
}
