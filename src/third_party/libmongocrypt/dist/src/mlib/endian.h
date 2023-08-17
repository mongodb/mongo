#ifndef MLIB_ENDIAN_H_INCLUDED
#define MLIB_ENDIAN_H_INCLUDED

#ifdef __has_include
// Some platforms require including other headers that will define the necessary
// macros
#if __has_include(<endian.h>)
// This may recursively include our own file in a pathological case, but that
// won't cause an issue. The default will include the system's version instead:
#include <endian.h>
#endif
#if __has_include(<sys/param.h>)
#include <sys/param.h>
#endif
#endif

#include "./macros.h"

enum mlib_endian_kind {
#ifdef _MSC_VER // MSVC only targets little-endian arches at the moment
    MLIB_ENDIAN_LITTLE = 1234,
    MLIB_ENDIAN_BIG = 4321,
    MLIB_ENDIAN_NATIVE = MLIB_ENDIAN_LITTLE,
#elif defined(__BYTE_ORDER__) // Commonly built-in defined in newer compilers
    MLIB_ENDIAN_LITTLE = __ORDER_LITTLE_ENDIAN__,
    MLIB_ENDIAN_BIG = __ORDER_BIG_ENDIAN__,
    MLIB_ENDIAN_NATIVE = __BYTE_ORDER__,
#elif defined(__BYTE_ORDER)   // Common in <sys/param.h> or <endian.h>
    MLIB_ENDIAN_LITTLE = __LITTLE_ENDIAN,
    MLIB_ENDIAN_BIG = __BIG_ENDIAN,
    MLIB_ENDIAN_NATIVE = __BYTE_ORDER,
#else
#error This compiler does not define an endianness macro.
#endif
};

enum {
    MLIB_IS_LITTLE_ENDIAN = MLIB_ENDIAN_NATIVE == MLIB_ENDIAN_LITTLE,
    MLIB_IS_BIG_ENDIAN = MLIB_ENDIAN_NATIVE == MLIB_ENDIAN_BIG,
};

#endif // MLIB_ENDIAN_H_INCLUDED
