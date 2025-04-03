/***************************************************************************************************

  Zyan Core Library (Zyan-C)

  Original Author : Florian Bernd, Joel Hoener

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Includes and defines some default data types.
 */

#ifndef ZYCORE_TYPES_H
#define ZYCORE_TYPES_H

#include "zydis/Zycore/Defines.h"

/* ============================================================================================== */
/* Integer types                                                                                  */
/* ============================================================================================== */

#if defined(ZYAN_NO_LIBC) || \
    (defined(ZYAN_MSVC) && defined(ZYAN_KERNEL)) // The WDK LibC lacks stdint.h.
    // No LibC mode, use compiler built-in types / macros.
#   if defined(ZYAN_MSVC) || defined(ZYAN_ICC)
        typedef unsigned __int8  ZyanU8;
        typedef unsigned __int16 ZyanU16;
        typedef unsigned __int32 ZyanU32;
        typedef unsigned __int64 ZyanU64;
        typedef   signed __int8  ZyanI8;
        typedef   signed __int16 ZyanI16;
        typedef   signed __int32 ZyanI32;
        typedef   signed __int64 ZyanI64;
#       if _WIN64
           typedef ZyanU64       ZyanUSize;
           typedef ZyanI64       ZyanISize;
           typedef ZyanU64       ZyanUPointer;
           typedef ZyanI64       ZyanIPointer;
#       else
           typedef ZyanU32       ZyanUSize;
           typedef ZyanI32       ZyanISize;
           typedef ZyanU32       ZyanUPointer;
           typedef ZyanI32       ZyanIPointer;
#       endif
#   elif defined(ZYAN_GNUC)
        typedef __UINT8_TYPE__   ZyanU8;
        typedef __UINT16_TYPE__  ZyanU16;
        typedef __UINT32_TYPE__  ZyanU32;
        typedef __UINT64_TYPE__  ZyanU64;
        typedef __INT8_TYPE__    ZyanI8;
        typedef __INT16_TYPE__   ZyanI16;
        typedef __INT32_TYPE__   ZyanI32;
        typedef __INT64_TYPE__   ZyanI64;
        typedef __SIZE_TYPE__    ZyanUSize;
        typedef __PTRDIFF_TYPE__ ZyanISize;
        typedef __UINTPTR_TYPE__ ZyanUPointer;
        typedef __INTPTR_TYPE__  ZyanIPointer;
#   else
#       error "Unsupported compiler for no-libc mode."
#   endif

#   if defined(ZYAN_MSVC)
#       define ZYAN_INT8_MIN     (-127i8 - 1)
#       define ZYAN_INT16_MIN    (-32767i16 - 1)
#       define ZYAN_INT32_MIN    (-2147483647i32 - 1)
#       define ZYAN_INT64_MIN    (-9223372036854775807i64 - 1)
#       define ZYAN_INT8_MAX     127i8
#       define ZYAN_INT16_MAX    32767i16
#       define ZYAN_INT32_MAX    2147483647i32
#       define ZYAN_INT64_MAX    9223372036854775807i64
#       define ZYAN_UINT8_MAX    0xffui8
#       define ZYAN_UINT16_MAX   0xffffui16
#       define ZYAN_UINT32_MAX   0xffffffffui32
#       define ZYAN_UINT64_MAX   0xffffffffffffffffui64
#   else
#       define ZYAN_INT8_MAX     __INT8_MAX__
#       define ZYAN_INT8_MIN     (-ZYAN_INT8_MAX - 1)
#       define ZYAN_INT16_MAX    __INT16_MAX__
#       define ZYAN_INT16_MIN    (-ZYAN_INT16_MAX - 1)
#       define ZYAN_INT32_MAX    __INT32_MAX__
#       define ZYAN_INT32_MIN    (-ZYAN_INT32_MAX - 1)
#       define ZYAN_INT64_MAX    __INT64_MAX__
#       define ZYAN_INT64_MIN    (-ZYAN_INT64_MAX - 1)
#       define ZYAN_UINT8_MAX    __UINT8_MAX__
#       define ZYAN_UINT16_MAX   __UINT16_MAX__
#       define ZYAN_UINT32_MAX   __UINT32_MAX__
#       define ZYAN_UINT64_MAX   __UINT64_MAX__
#   endif
#else
    // If is LibC present, we use stdint types.
#   include <stdint.h>
#   include <stddef.h>
    typedef uint8_t   ZyanU8;
    typedef uint16_t  ZyanU16;
    typedef uint32_t  ZyanU32;
    typedef uint64_t  ZyanU64;
    typedef int8_t    ZyanI8;
    typedef int16_t   ZyanI16;
    typedef int32_t   ZyanI32;
    typedef int64_t   ZyanI64;
    typedef size_t    ZyanUSize;
    typedef ptrdiff_t ZyanISize;
    typedef uintptr_t ZyanUPointer;
    typedef intptr_t  ZyanIPointer;

#   define ZYAN_INT8_MIN         INT8_MIN
#   define ZYAN_INT16_MIN        INT16_MIN
#   define ZYAN_INT32_MIN        INT32_MIN
#   define ZYAN_INT64_MIN        INT64_MIN
#   define ZYAN_INT8_MAX         INT8_MAX
#   define ZYAN_INT16_MAX        INT16_MAX
#   define ZYAN_INT32_MAX        INT32_MAX
#   define ZYAN_INT64_MAX        INT64_MAX
#   define ZYAN_UINT8_MAX        UINT8_MAX
#   define ZYAN_UINT16_MAX       UINT16_MAX
#   define ZYAN_UINT32_MAX       UINT32_MAX
#   define ZYAN_UINT64_MAX       UINT64_MAX
#endif

// Verify size assumptions.
ZYAN_STATIC_ASSERT(sizeof(ZyanU8      ) == 1            );
ZYAN_STATIC_ASSERT(sizeof(ZyanU16     ) == 2            );
ZYAN_STATIC_ASSERT(sizeof(ZyanU32     ) == 4            );
ZYAN_STATIC_ASSERT(sizeof(ZyanU64     ) == 8            );
ZYAN_STATIC_ASSERT(sizeof(ZyanI8      ) == 1            );
ZYAN_STATIC_ASSERT(sizeof(ZyanI16     ) == 2            );
ZYAN_STATIC_ASSERT(sizeof(ZyanI32     ) == 4            );
ZYAN_STATIC_ASSERT(sizeof(ZyanI64     ) == 8            );
ZYAN_STATIC_ASSERT(sizeof(ZyanUSize   ) == sizeof(void*)); // TODO: This one is incorrect!
ZYAN_STATIC_ASSERT(sizeof(ZyanISize   ) == sizeof(void*)); // TODO: This one is incorrect!
ZYAN_STATIC_ASSERT(sizeof(ZyanUPointer) == sizeof(void*));
ZYAN_STATIC_ASSERT(sizeof(ZyanIPointer) == sizeof(void*));

// Verify signedness assumptions (relies on size checks above).
ZYAN_STATIC_ASSERT((ZyanI8 )-1 >> 1 < (ZyanI8 )((ZyanU8 )-1 >> 1));
ZYAN_STATIC_ASSERT((ZyanI16)-1 >> 1 < (ZyanI16)((ZyanU16)-1 >> 1));
ZYAN_STATIC_ASSERT((ZyanI32)-1 >> 1 < (ZyanI32)((ZyanU32)-1 >> 1));
ZYAN_STATIC_ASSERT((ZyanI64)-1 >> 1 < (ZyanI64)((ZyanU64)-1 >> 1));

/* ============================================================================================== */
/* Pointer                                                                                        */
/* ============================================================================================== */

/**
 * Defines the `ZyanVoidPointer` data-type.
 */
typedef void* ZyanVoidPointer;

/**
 * Defines the `ZyanConstVoidPointer` data-type.
 */
typedef const void* ZyanConstVoidPointer;

#define ZYAN_NULL ((void*)0)

/* ============================================================================================== */
/* Logic types                                                                                    */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Boolean                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#define ZYAN_FALSE 0u
#define ZYAN_TRUE  1u

/**
 * Defines the `ZyanBool` data-type.
 *
 * Represents a default boolean data-type where `0` is interpreted as `false` and all other values
 * as `true`.
 */
typedef ZyanU8 ZyanBool;

/* ---------------------------------------------------------------------------------------------- */
/* Ternary                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZyanTernary` data-type.
 *
 * The `ZyanTernary` is a balanced ternary type that uses three truth values indicating `true`,
 * `false` and an indeterminate third value.
 */
typedef ZyanI8 ZyanTernary;

#define ZYAN_TERNARY_FALSE    (-1)
#define ZYAN_TERNARY_UNKNOWN  0x00
#define ZYAN_TERNARY_TRUE     0x01

/* ============================================================================================== */
/* String types                                                                                   */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* C-style strings                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZyanCharPointer` data-type.
 *
 * This type is most often used to represent null-terminated strings aka. C-style strings.
 */
typedef char* ZyanCharPointer;

/**
 * Defines the `ZyanConstCharPointer` data-type.
 *
 * This type is most often used to represent null-terminated strings aka. C-style strings.
 */
typedef const char* ZyanConstCharPointer;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#endif /* ZYCORE_TYPES_H */
