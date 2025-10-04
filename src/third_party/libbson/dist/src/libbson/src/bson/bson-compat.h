/*
 * Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/bson-prelude.h>


#ifndef BSON_COMPAT_H
#define BSON_COMPAT_H


#if defined(__MINGW32__)
#if defined(__USE_MINGW_ANSI_STDIO)
#if __USE_MINGW_ANSI_STDIO < 1
#error "__USE_MINGW_ANSI_STDIO > 0 is required for correct PRI* macros"
#endif
#else
#define __USE_MINGW_ANSI_STDIO 1
#endif
#endif

#include <bson/bson-config.h>
#include <bson/bson-macros.h>


#ifdef BSON_OS_WIN32
#if defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0601)
#undef _WIN32_WINNT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#else
#include <windows.h>
#endif
#include <direct.h>
#include <io.h>
#endif


#ifdef BSON_OS_UNIX
#include <unistd.h>
#include <sys/time.h>
#endif


#include <bson/bson-macros.h>


#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>


BSON_BEGIN_DECLS

#if !defined(_MSC_VER) || (_MSC_VER >= 1800)
#include <inttypes.h>
#endif
#ifdef _MSC_VER
#ifndef __cplusplus
/* benign redefinition of type */
#pragma warning(disable : 4142)
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef SIZE_T size_t;
#endif
#pragma warning(default : 4142)
#else
/*
 * MSVC++ does not include ssize_t, just size_t.
 * So we need to synthesize that as well.
 */
#pragma warning(disable : 4142)
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#pragma warning(default : 4142)
#endif
#ifndef PRIi32
#define PRIi32 "d"
#endif
#ifndef PRId32
#define PRId32 "d"
#endif
#ifndef PRIu32
#define PRIu32 "u"
#endif
#ifndef PRIi64
#define PRIi64 "I64i"
#endif
#ifndef PRId64
#define PRId64 "I64i"
#endif
#ifndef PRIu64
#define PRIu64 "I64u"
#endif
#endif

/* Derive the maximum representable value of signed integer type T using the
 * formula 2^(N - 1) - 1 where N is the number of bits in type T. This assumes
 * T is represented using two's complement. */
#define BSON_NUMERIC_LIMITS_MAX_SIGNED(T) ((T) ((((size_t) 0x01u) << (sizeof (T) * (size_t) CHAR_BIT - 1u)) - 1u))

/* Derive the minimum representable value of signed integer type T as one less
 * than the negation of its maximum representable value. This assumes T is
 * represented using two's complement. */
#define BSON_NUMERIC_LIMITS_MIN_SIGNED(T, max) ((T) ((-(max)) - 1))

/* Derive the maximum representable value of unsigned integer type T by flipping
 * all its bits to 1. */
#define BSON_NUMERIC_LIMITS_MAX_UNSIGNED(T) ((T) (~((T) 0)))

#ifndef SSIZE_MAX
#define SSIZE_MAX BSON_NUMERIC_LIMITS_MAX_SIGNED (ssize_t)
#endif

#ifndef SSIZE_MIN
#define SSIZE_MIN BSON_NUMERIC_LIMITS_MIN_SIGNED (ssize_t, SSIZE_MAX)
#endif

#if defined(__MINGW32__) && !defined(INIT_ONCE_STATIC_INIT)
#define INIT_ONCE_STATIC_INIT RTL_RUN_ONCE_INIT
typedef RTL_RUN_ONCE INIT_ONCE;
#endif

#ifdef BSON_HAVE_STDBOOL_H
#include <stdbool.h>
#elif !defined(__bool_true_false_are_defined)
#ifndef __cplusplus
typedef signed char bool;
#define false 0
#define true 1
#endif
#define __bool_true_false_are_defined 1
#endif


#if defined(__GNUC__)
#if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
#define bson_sync_synchronize() __sync_synchronize ()
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__) || defined(__x86_64__)
#define bson_sync_synchronize() asm volatile ("mfence" ::: "memory")
#else
#define bson_sync_synchronize() asm volatile ("sync" ::: "memory")
#endif
#elif defined(_MSC_VER)
#define bson_sync_synchronize() MemoryBarrier ()
#endif


#if !defined(va_copy) && defined(__va_copy)
#define va_copy(dst, src) __va_copy (dst, src)
#endif


#if !defined(va_copy)
#define va_copy(dst, src) ((dst) = (src))
#endif


#ifdef _MSC_VER
/** Expands the arguments if compiling with MSVC, otherwise empty */
#define BSON_IF_MSVC(...) __VA_ARGS__
/** Expands the arguments if compiling with GCC or Clang, otherwise empty */
#define BSON_IF_GNU_LIKE(...)
#elif defined(__GNUC__) || defined(__clang__)
/** Expands the arguments if compiling with MSVC, otherwise empty */
#define BSON_IF_MSVC(...)
/** Expands the arguments if compiling with GCC or Clang, otherwise empty */
#define BSON_IF_GNU_LIKE(...) __VA_ARGS__
#endif

#ifdef BSON_OS_WIN32
/** Expands the arguments if compiling for Windows, otherwise empty */
#define BSON_IF_WINDOWS(...) __VA_ARGS__
/** Expands the arguments if compiling for POSIX, otherwise empty */
#define BSON_IF_POSIX(...)
#elif defined(BSON_OS_UNIX)
/** Expands the arguments if compiling for Windows, otherwise empty */
#define BSON_IF_WINDOWS(...)
/** Expands the arguments if compiling for POSIX, otherwise empty */
#define BSON_IF_POSIX(...) __VA_ARGS__
#endif


BSON_END_DECLS


#endif /* BSON_COMPAT_H */
