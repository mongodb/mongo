/*    Copyright 2014 MongoDB Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/* fix for MSVC ...evil! */
#ifdef _MSC_VER
#define CONST64(n) n##ui64
typedef unsigned __int64 ulong64;
#else
#define CONST64(n) n##ULL
typedef unsigned long long ulong64;
#endif

/* this is the "32-bit at least" data type
 * Re-define it to suit your platform but it must be at least 32-bits
 */
#if defined(__x86_64__) || (defined(__sparc__) && defined(__arch64__))
typedef unsigned ulong32;
#else
typedef unsigned long ulong32;
#endif

/* ---- HELPER MACROS ---- */
#ifdef ENDIAN_NEUTRAL

#define STORE32L(x, y)                               \
    {                                                \
        (y)[3] = (unsigned char)(((x) >> 24) & 255); \
        (y)[2] = (unsigned char)(((x) >> 16) & 255); \
        (y)[1] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[0] = (unsigned char)((x)&255);           \
    }

#define LOAD32L(x, y)                                                                       \
    {                                                                                       \
        x = ((unsigned long)((y)[3] & 255) << 24) | ((unsigned long)((y)[2] & 255) << 16) | \
            ((unsigned long)((y)[1] & 255) << 8) | ((unsigned long)((y)[0] & 255));         \
    }

#define STORE64L(x, y)                               \
    {                                                \
        (y)[7] = (unsigned char)(((x) >> 56) & 255); \
        (y)[6] = (unsigned char)(((x) >> 48) & 255); \
        (y)[5] = (unsigned char)(((x) >> 40) & 255); \
        (y)[4] = (unsigned char)(((x) >> 32) & 255); \
        (y)[3] = (unsigned char)(((x) >> 24) & 255); \
        (y)[2] = (unsigned char)(((x) >> 16) & 255); \
        (y)[1] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[0] = (unsigned char)((x)&255);           \
    }

#define LOAD64L(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[7] & 255)) << 56) | (((ulong64)((y)[6] & 255)) << 48) | \
            (((ulong64)((y)[5] & 255)) << 40) | (((ulong64)((y)[4] & 255)) << 32) | \
            (((ulong64)((y)[3] & 255)) << 24) | (((ulong64)((y)[2] & 255)) << 16) | \
            (((ulong64)((y)[1] & 255)) << 8) | (((ulong64)((y)[0] & 255)));         \
    }

#define STORE32H(x, y)                               \
    {                                                \
        (y)[0] = (unsigned char)(((x) >> 24) & 255); \
        (y)[1] = (unsigned char)(((x) >> 16) & 255); \
        (y)[2] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[3] = (unsigned char)((x)&255);           \
    }

#define LOAD32H(x, y)                                                                       \
    {                                                                                       \
        x = ((unsigned long)((y)[0] & 255) << 24) | ((unsigned long)((y)[1] & 255) << 16) | \
            ((unsigned long)((y)[2] & 255) << 8) | ((unsigned long)((y)[3] & 255));         \
    }

#define STORE64H(x, y)                               \
    {                                                \
        (y)[0] = (unsigned char)(((x) >> 56) & 255); \
        (y)[1] = (unsigned char)(((x) >> 48) & 255); \
        (y)[2] = (unsigned char)(((x) >> 40) & 255); \
        (y)[3] = (unsigned char)(((x) >> 32) & 255); \
        (y)[4] = (unsigned char)(((x) >> 24) & 255); \
        (y)[5] = (unsigned char)(((x) >> 16) & 255); \
        (y)[6] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[7] = (unsigned char)((x)&255);           \
    }

#define LOAD64H(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[0] & 255)) << 56) | (((ulong64)((y)[1] & 255)) << 48) | \
            (((ulong64)((y)[2] & 255)) << 40) | (((ulong64)((y)[3] & 255)) << 32) | \
            (((ulong64)((y)[4] & 255)) << 24) | (((ulong64)((y)[5] & 255)) << 16) | \
            (((ulong64)((y)[6] & 255)) << 8) | (((ulong64)((y)[7] & 255)));         \
    }

#endif /* ENDIAN_NEUTRAL */

#ifdef ENDIAN_LITTLE

#if !defined(LTC_NO_BSWAP) &&                                                                    \
    (defined(INTEL_CC) ||                                                                        \
     (defined(__GNUC__) && (defined(__DJGPP__) || defined(__CYGWIN__) || defined(__MINGW32__) || \
                            defined(__i386__) || defined(__x86_64__))))

#define STORE32H(x, y)                 \
    asm __volatile__(                  \
        "bswapl %0     \n\t"           \
        "movl   %0,(%1)\n\t"           \
        "bswapl %0     \n\t" ::"r"(x), \
        "r"(y));

#define LOAD32H(x, y)      \
    asm __volatile__(      \
        "movl (%1),%0\n\t" \
        "bswapl %0\n\t"    \
        : "=r"(x)          \
        : "r"(y));

#else

#define STORE32H(x, y)                               \
    {                                                \
        (y)[0] = (unsigned char)(((x) >> 24) & 255); \
        (y)[1] = (unsigned char)(((x) >> 16) & 255); \
        (y)[2] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[3] = (unsigned char)((x)&255);           \
    }

#define LOAD32H(x, y)                                                                       \
    {                                                                                       \
        x = ((unsigned long)((y)[0] & 255) << 24) | ((unsigned long)((y)[1] & 255) << 16) | \
            ((unsigned long)((y)[2] & 255) << 8) | ((unsigned long)((y)[3] & 255));         \
    }

#endif


/* x86_64 processor */
#if !defined(LTC_NO_BSWAP) && (defined(__GNUC__) && defined(__x86_64__))

#define STORE64H(x, y)                 \
    asm __volatile__(                  \
        "bswapq %0     \n\t"           \
        "movq   %0,(%1)\n\t"           \
        "bswapq %0     \n\t" ::"r"(x), \
        "r"(y));

#define LOAD64H(x, y)      \
    asm __volatile__(      \
        "movq (%1),%0\n\t" \
        "bswapq %0\n\t"    \
        : "=r"(x)          \
        : "r"(y));

#else

#define STORE64H(x, y)                               \
    {                                                \
        (y)[0] = (unsigned char)(((x) >> 56) & 255); \
        (y)[1] = (unsigned char)(((x) >> 48) & 255); \
        (y)[2] = (unsigned char)(((x) >> 40) & 255); \
        (y)[3] = (unsigned char)(((x) >> 32) & 255); \
        (y)[4] = (unsigned char)(((x) >> 24) & 255); \
        (y)[5] = (unsigned char)(((x) >> 16) & 255); \
        (y)[6] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[7] = (unsigned char)((x)&255);           \
    }

#define LOAD64H(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[0] & 255)) << 56) | (((ulong64)((y)[1] & 255)) << 48) | \
            (((ulong64)((y)[2] & 255)) << 40) | (((ulong64)((y)[3] & 255)) << 32) | \
            (((ulong64)((y)[4] & 255)) << 24) | (((ulong64)((y)[5] & 255)) << 16) | \
            (((ulong64)((y)[6] & 255)) << 8) | (((ulong64)((y)[7] & 255)));         \
    }

#endif

#ifdef ENDIAN_32BITWORD

#define STORE32L(x, y)       \
    {                        \
        ulong32 __t = (x);   \
        XMEMCPY(y, &__t, 4); \
    }

#define LOAD32L(x, y) XMEMCPY(&(x), y, 4);

#define STORE64L(x, y)                               \
    {                                                \
        (y)[7] = (unsigned char)(((x) >> 56) & 255); \
        (y)[6] = (unsigned char)(((x) >> 48) & 255); \
        (y)[5] = (unsigned char)(((x) >> 40) & 255); \
        (y)[4] = (unsigned char)(((x) >> 32) & 255); \
        (y)[3] = (unsigned char)(((x) >> 24) & 255); \
        (y)[2] = (unsigned char)(((x) >> 16) & 255); \
        (y)[1] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[0] = (unsigned char)((x)&255);           \
    }

#define LOAD64L(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[7] & 255)) << 56) | (((ulong64)((y)[6] & 255)) << 48) | \
            (((ulong64)((y)[5] & 255)) << 40) | (((ulong64)((y)[4] & 255)) << 32) | \
            (((ulong64)((y)[3] & 255)) << 24) | (((ulong64)((y)[2] & 255)) << 16) | \
            (((ulong64)((y)[1] & 255)) << 8) | (((ulong64)((y)[0] & 255)));         \
    }

#else /* 64-bit words then  */

#define STORE32L(x, y)       \
    {                        \
        ulong32 __t = (x);   \
        XMEMCPY(y, &__t, 4); \
    }

#define LOAD32L(x, y)        \
    {                        \
        XMEMCPY(&(x), y, 4); \
        x &= 0xFFFFFFFF;     \
    }

#define STORE64L(x, y)       \
    {                        \
        ulong64 __t = (x);   \
        XMEMCPY(y, &__t, 8); \
    }

#define LOAD64L(x, y) \
    { XMEMCPY(&(x), y, 8); }

#endif /* ENDIAN_64BITWORD */

#endif /* ENDIAN_LITTLE */

#ifdef ENDIAN_BIG
#define STORE32L(x, y)                               \
    {                                                \
        (y)[3] = (unsigned char)(((x) >> 24) & 255); \
        (y)[2] = (unsigned char)(((x) >> 16) & 255); \
        (y)[1] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[0] = (unsigned char)((x)&255);           \
    }

#define LOAD32L(x, y)                                                                       \
    {                                                                                       \
        x = ((unsigned long)((y)[3] & 255) << 24) | ((unsigned long)((y)[2] & 255) << 16) | \
            ((unsigned long)((y)[1] & 255) << 8) | ((unsigned long)((y)[0] & 255));         \
    }

#define STORE64L(x, y)                               \
    {                                                \
        (y)[7] = (unsigned char)(((x) >> 56) & 255); \
        (y)[6] = (unsigned char)(((x) >> 48) & 255); \
        (y)[5] = (unsigned char)(((x) >> 40) & 255); \
        (y)[4] = (unsigned char)(((x) >> 32) & 255); \
        (y)[3] = (unsigned char)(((x) >> 24) & 255); \
        (y)[2] = (unsigned char)(((x) >> 16) & 255); \
        (y)[1] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[0] = (unsigned char)((x)&255);           \
    }

#define LOAD64L(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[7] & 255)) << 56) | (((ulong64)((y)[6] & 255)) << 48) | \
            (((ulong64)((y)[5] & 255)) << 40) | (((ulong64)((y)[4] & 255)) << 32) | \
            (((ulong64)((y)[3] & 255)) << 24) | (((ulong64)((y)[2] & 255)) << 16) | \
            (((ulong64)((y)[1] & 255)) << 8) | (((ulong64)((y)[0] & 255)));         \
    }

#ifdef ENDIAN_32BITWORD

#define STORE32H(x, y)       \
    {                        \
        ulong32 __t = (x);   \
        XMEMCPY(y, &__t, 4); \
    }

#define LOAD32H(x, y) XMEMCPY(&(x), y, 4);

#define STORE64H(x, y)                               \
    {                                                \
        (y)[0] = (unsigned char)(((x) >> 56) & 255); \
        (y)[1] = (unsigned char)(((x) >> 48) & 255); \
        (y)[2] = (unsigned char)(((x) >> 40) & 255); \
        (y)[3] = (unsigned char)(((x) >> 32) & 255); \
        (y)[4] = (unsigned char)(((x) >> 24) & 255); \
        (y)[5] = (unsigned char)(((x) >> 16) & 255); \
        (y)[6] = (unsigned char)(((x) >> 8) & 255);  \
        (y)[7] = (unsigned char)((x)&255);           \
    }

#define LOAD64H(x, y)                                                               \
    {                                                                               \
        x = (((ulong64)((y)[0] & 255)) << 56) | (((ulong64)((y)[1] & 255)) << 48) | \
            (((ulong64)((y)[2] & 255)) << 40) | (((ulong64)((y)[3] & 255)) << 32) | \
            (((ulong64)((y)[4] & 255)) << 24) | (((ulong64)((y)[5] & 255)) << 16) | \
            (((ulong64)((y)[6] & 255)) << 8) | (((ulong64)((y)[7] & 255)));         \
    }

#else /* 64-bit words then  */

#define STORE32H(x, y)       \
    {                        \
        ulong32 __t = (x);   \
        XMEMCPY(y, &__t, 4); \
    }

#define LOAD32H(x, y)        \
    {                        \
        XMEMCPY(&(x), y, 4); \
        x &= 0xFFFFFFFF;     \
    }

#define STORE64H(x, y)       \
    {                        \
        ulong64 __t = (x);   \
        XMEMCPY(y, &__t, 8); \
    }

#define LOAD64H(x, y) \
    { XMEMCPY(&(x), y, 8); }

#endif /* ENDIAN_64BITWORD */
#endif /* ENDIAN_BIG */

#define BSWAP(x)                                                                           \
    (((x >> 24) & 0x000000FFUL) | ((x << 24) & 0xFF000000UL) | ((x >> 8) & 0x0000FF00UL) | \
     ((x << 8) & 0x00FF0000UL))


/* 32-bit Rotates */
#if defined(_MSC_VER)

/* instrinsic rotate */
#include <stdlib.h>
#pragma intrinsic(_lrotr, _lrotl)
#define ROR(x, n) _lrotr(x, n)
#define ROL(x, n) _lrotl(x, n)
#define RORc(x, n) _lrotr(x, n)
#define ROLc(x, n) _lrotl(x, n)

#elif !defined(__STRICT_ANSI__) && defined(__GNUC__) && \
    (defined(__i386__) || defined(__x86_64__)) && !defined(INTEL_CC) && !defined(LTC_NO_ASM)

static inline unsigned ROL(unsigned word, int i) {
    asm("roll %%cl,%0" : "=r"(word) : "0"(word), "c"(i));
    return word;
}

static inline unsigned ROR(unsigned word, int i) {
    asm("rorl %%cl,%0" : "=r"(word) : "0"(word), "c"(i));
    return word;
}

#ifndef LTC_NO_ROLC

static inline unsigned ROLc(unsigned word, const int i) {
    asm("roll %2,%0" : "=r"(word) : "0"(word), "I"(i));
    return word;
}

static inline unsigned RORc(unsigned word, const int i) {
    asm("rorl %2,%0" : "=r"(word) : "0"(word), "I"(i));
    return word;
}

#else

#define ROLc ROL
#define RORc ROR

#endif

#elif !defined(__STRICT_ANSI__) && defined(LTC_PPC32)

static inline unsigned ROL(unsigned word, int i) {
    asm("rotlw %0,%0,%2" : "=r"(word) : "0"(word), "r"(i));
    return word;
}

static inline unsigned ROR(unsigned word, int i) {
    asm("rotlw %0,%0,%2" : "=r"(word) : "0"(word), "r"(32 - i));
    return word;
}

#ifndef LTC_NO_ROLC

static inline unsigned ROLc(unsigned word, const int i) {
    asm("rotlwi %0,%0,%2" : "=r"(word) : "0"(word), "I"(i));
    return word;
}

static inline unsigned RORc(unsigned word, const int i) {
    asm("rotrwi %0,%0,%2" : "=r"(word) : "0"(word), "I"(i));
    return word;
}

#else

#define ROLc ROL
#define RORc ROR

#endif


#else

/* rotates the hard way */
#define ROL(x, y)                                                              \
    ((((unsigned long)(x) << (unsigned long)((y)&31)) |                        \
      (((unsigned long)(x)&0xFFFFFFFFUL) >> (unsigned long)(32 - ((y)&31)))) & \
     0xFFFFFFFFUL)
#define ROR(x, y)                                                      \
    (((((unsigned long)(x)&0xFFFFFFFFUL) >> (unsigned long)((y)&31)) | \
      ((unsigned long)(x) << (unsigned long)(32 - ((y)&31)))) &        \
     0xFFFFFFFFUL)
#define ROLc(x, y)                                                             \
    ((((unsigned long)(x) << (unsigned long)((y)&31)) |                        \
      (((unsigned long)(x)&0xFFFFFFFFUL) >> (unsigned long)(32 - ((y)&31)))) & \
     0xFFFFFFFFUL)
#define RORc(x, y)                                                     \
    (((((unsigned long)(x)&0xFFFFFFFFUL) >> (unsigned long)((y)&31)) | \
      ((unsigned long)(x) << (unsigned long)(32 - ((y)&31)))) &        \
     0xFFFFFFFFUL)

#endif


/* 64-bit Rotates */
#if !defined(__STRICT_ANSI__) && defined(__GNUC__) && defined(__x86_64__) && !defined(LTC_NO_ASM)

static inline unsigned long ROL64(unsigned long word, int i) {
    asm("rolq %%cl,%0" : "=r"(word) : "0"(word), "c"(i));
    return word;
}

static inline unsigned long ROR64(unsigned long word, int i) {
    asm("rorq %%cl,%0" : "=r"(word) : "0"(word), "c"(i));
    return word;
}

#ifndef LTC_NO_ROLC

static inline unsigned long ROL64c(unsigned long word, const int i) {
    asm("rolq %2,%0" : "=r"(word) : "0"(word), "J"(i));
    return word;
}

static inline unsigned long ROR64c(unsigned long word, const int i) {
    asm("rorq %2,%0" : "=r"(word) : "0"(word), "J"(i));
    return word;
}

#else /* LTC_NO_ROLC */

#define ROL64c ROL64
#define ROR64c ROR64

#endif

#else /* Not x86_64  */

#define ROL64(x, y)                                                      \
    ((((x) << ((ulong64)(y)&63)) |                                       \
      (((x)&CONST64(0xFFFFFFFFFFFFFFFF)) >> ((ulong64)64 - ((y)&63)))) & \
     CONST64(0xFFFFFFFFFFFFFFFF))

#define ROR64(x, y)                                                       \
    (((((x)&CONST64(0xFFFFFFFFFFFFFFFF)) >> ((ulong64)(y)&CONST64(63))) | \
      ((x) << ((ulong64)(64 - ((y)&CONST64(63)))))) &                     \
     CONST64(0xFFFFFFFFFFFFFFFF))

#define ROL64c(x, y)                                                     \
    ((((x) << ((ulong64)(y)&63)) |                                       \
      (((x)&CONST64(0xFFFFFFFFFFFFFFFF)) >> ((ulong64)64 - ((y)&63)))) & \
     CONST64(0xFFFFFFFFFFFFFFFF))

#define ROR64c(x, y)                                                      \
    (((((x)&CONST64(0xFFFFFFFFFFFFFFFF)) >> ((ulong64)(y)&CONST64(63))) | \
      ((x) << ((ulong64)(64 - ((y)&CONST64(63)))))) &                     \
     CONST64(0xFFFFFFFFFFFFFFFF))

#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

/* extract a byte portably */
#ifdef _MSC_VER
#define byte(x, n) ((unsigned char)((x) >> (8 * (n))))
#else
#define byte(x, n) (((x) >> (8 * (n))) & 255)
#endif

/* $Source: /cvs/libtom/libtomcrypt/src/headers/tomcrypt_macros.h,v $ */
/* $Revision: 1.15 $ */
/* $Date: 2006/11/29 23:43:57 $ */
