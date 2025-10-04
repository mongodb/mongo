/*
*  xxHash - Fast Hash algorithm
*  Copyright (C) 2012-2016, Yann Collet
*
*  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions are
*  met:
*
*  * Redistributions of source code must retain the above copyright
*  notice, this list of conditions and the following disclaimer.
*  * Redistributions in binary form must reproduce the above
*  copyright notice, this list of conditions and the following disclaimer
*  in the documentation and/or other materials provided with the
*  distribution.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
*  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
*  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
*  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
*  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
*  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
*  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
*  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*  You can contact the author at :
*  - xxHash homepage: http://www.xxhash.com
*  - xxHash source repository : https://github.com/Cyan4973/xxHash
*/


/* *************************************
*  Tuning parameters
***************************************/
/*!KXXH_FORCE_MEMORY_ACCESS :
 * By default, access to unaligned memory is controlled by `memcpy()`, which is safe and portable.
 * Unfortunately, on some target/compiler combinations, the generated assembly is sub-optimal.
 * The below switch allow to select different access method for improved performance.
 * Method 0 (default) : use `memcpy()`. Safe and portable.
 * Method 1 : `__packed` statement. It depends on compiler extension (ie, not portable).
 *            This method is safe if your compiler supports it, and *generally* as fast or faster than `memcpy`.
 * Method 2 : direct access. This method doesn't depend on compiler but violate C standard.
 *            It can generate buggy code on targets which do not support unaligned memory accesses.
 *            But in some circumstances, it's the only known way to get the most performance (ie GCC + ARMv6)
 * See http://stackoverflow.com/a/32095106/646947 for details.
 * Prefer these methods in priority order (0 > 1 > 2)
 */
#ifndef KXXH_FORCE_MEMORY_ACCESS   /* can be defined externally, on command line for example */
#  if defined(__GNUC__) && ( defined(__ARM_ARCH_6__) || defined(__ARM_ARCH_6J__) \
                        || defined(__ARM_ARCH_6K__) || defined(__ARM_ARCH_6Z__) \
                        || defined(__ARM_ARCH_6ZK__) || defined(__ARM_ARCH_6T2__) )
#    define KXXH_FORCE_MEMORY_ACCESS 2
#  elif (defined(__INTEL_COMPILER) && !defined(_WIN32)) || \
  (defined(__GNUC__) && ( defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__) \
                    || defined(__ARM_ARCH_7R__) || defined(__ARM_ARCH_7M__) \
                    || defined(__ARM_ARCH_7S__) ))
#    define KXXH_FORCE_MEMORY_ACCESS 1
#  endif
#endif

/*!KXXH_ACCEPT_NULL_INPUT_POINTER :
 * If input pointer is NULL, xxHash default behavior is to dereference it, triggering a segfault.
 * When this macro is enabled, xxHash actively checks input for null pointer.
 * It it is, result for null input pointers is the same as a null-length input.
 */
#ifndef KXXH_ACCEPT_NULL_INPUT_POINTER   /* can be defined externally */
#  define KXXH_ACCEPT_NULL_INPUT_POINTER 0
#endif

/*!KXXH_FORCE_NATIVE_FORMAT :
 * By default, xxHash library provides endian-independent Hash values, based on little-endian convention.
 * Results are therefore identical for little-endian and big-endian CPU.
 * This comes at a performance cost for big-endian CPU, since some swapping is required to emulate little-endian format.
 * Should endian-independence be of no importance for your application, you may set the #define below to 1,
 * to improve speed for Big-endian CPU.
 * This option has no impact on Little_Endian CPU.
 */
#ifndef KXXH_FORCE_NATIVE_FORMAT   /* can be defined externally */
#  define KXXH_FORCE_NATIVE_FORMAT 0
#endif

/*!KXXH_FORCE_ALIGN_CHECK :
 * This is a minor performance trick, only useful with lots of very small keys.
 * It means : check for aligned/unaligned input.
 * The check costs one initial branch per hash;
 * set it to 0 when the input is guaranteed to be aligned,
 * or when alignment doesn't matter for performance.
 */
#ifndef KXXH_FORCE_ALIGN_CHECK /* can be defined externally */
#  if defined(__i386) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#    define KXXH_FORCE_ALIGN_CHECK 0
#  else
#    define KXXH_FORCE_ALIGN_CHECK 1
#  endif
#endif


/* *************************************
*  Includes & Memory related functions
***************************************/
/*! Modify the local functions below should you wish to use some other memory routines
*   for malloc(), free() */
#include <stdlib.h>
static void* KXXH_malloc(size_t s) { return malloc(s); }
static void  KXXH_free  (void* p)  { free(p); }
/*! and for memcpy() */
#include <string.h>
static void* KXXH_memcpy(void* dest, const void* src, size_t size) { return memcpy(dest,src,size); }

#include <assert.h>   /* assert */

#define KXXH_STATIC_LINKING_ONLY
#include "rdxxhash.h"


/* *************************************
*  Compiler Specific Options
***************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#  define FORCE_INLINE static __forceinline
#else
#  if defined (__cplusplus) || defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
#    ifdef __GNUC__
#      define FORCE_INLINE static inline __attribute__((always_inline))
#    else
#      define FORCE_INLINE static inline
#    endif
#  else
#    define FORCE_INLINE static
#  endif /* __STDC_VERSION__ */
#endif


/* *************************************
*  Basic Types
***************************************/
#ifndef MEM_MODULE
# if !defined (__VMS) \
  && (defined (__cplusplus) \
  || (defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */) )
#   include <stdint.h>
    typedef uint8_t  BYTE;
    typedef uint16_t U16;
    typedef uint32_t U32;
# else
    typedef unsigned char      BYTE;
    typedef unsigned short     U16;
    typedef unsigned int       U32;
# endif
#endif

#if (defined(KXXH_FORCE_MEMORY_ACCESS) && (KXXH_FORCE_MEMORY_ACCESS==2))

/* Force direct memory access. Only works on CPU which support unaligned memory access in hardware */
static U32 KXXH_read32(const void* memPtr) { return *(const U32*) memPtr; }

#elif (defined(KXXH_FORCE_MEMORY_ACCESS) && (KXXH_FORCE_MEMORY_ACCESS==1))

/* __pack instructions are safer, but compiler specific, hence potentially problematic for some compilers */
/* currently only defined for gcc and icc */
typedef union { U32 u32; } __attribute__((packed)) unalign;
static U32 KXXH_read32(const void* ptr) { return ((const unalign*)ptr)->u32; }

#else

/* portable and safe solution. Generally efficient.
 * see : http://stackoverflow.com/a/32095106/646947
 */
static U32 KXXH_read32(const void* memPtr)
{
    U32 val;
    memcpy(&val, memPtr, sizeof(val));
    return val;
}

#endif   /* KXXH_FORCE_DIRECT_MEMORY_ACCESS */


/* ****************************************
*  Compiler-specific Functions and Macros
******************************************/
#define KXXH_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

/* Note : although _rotl exists for minGW (GCC under windows), performance seems poor */
#if defined(_MSC_VER)
#  define KXXH_rotl32(x,r) _rotl(x,r)
#  define KXXH_rotl64(x,r) _rotl64(x,r)
#else
#  define KXXH_rotl32(x,r) ((x << r) | (x >> (32 - r)))
#  define KXXH_rotl64(x,r) ((x << r) | (x >> (64 - r)))
#endif

#if defined(_MSC_VER)     /* Visual Studio */
#  define KXXH_swap32 _byteswap_ulong
#elif KXXH_GCC_VERSION >= 403
#  define KXXH_swap32 __builtin_bswap32
#else
static U32 KXXH_swap32 (U32 x)
{
    return  ((x << 24) & 0xff000000 ) |
            ((x <<  8) & 0x00ff0000 ) |
            ((x >>  8) & 0x0000ff00 ) |
            ((x >> 24) & 0x000000ff );
}
#endif


/* *************************************
*  Architecture Macros
***************************************/
typedef enum { KXXH_bigEndian=0, KXXH_littleEndian=1 } KXXH_endianess;

/* KXXH_CPU_LITTLE_ENDIAN can be defined externally, for example on the compiler command line */
#ifndef KXXH_CPU_LITTLE_ENDIAN
static int KXXH_isLittleEndian(void)
{
    const union { U32 u; BYTE c[4]; } one = { 1 };   /* don't use static : performance detrimental  */
    return one.c[0];
}
#   define KXXH_CPU_LITTLE_ENDIAN   KXXH_isLittleEndian()
#endif


/* ***************************
*  Memory reads
*****************************/
typedef enum { KXXH_aligned, KXXH_unaligned } KXXH_alignment;

FORCE_INLINE U32 KXXH_readLE32_align(const void* ptr, KXXH_endianess endian, KXXH_alignment align)
{
    if (align==KXXH_unaligned)
        return endian==KXXH_littleEndian ? KXXH_read32(ptr) : KXXH_swap32(KXXH_read32(ptr));
    else
        return endian==KXXH_littleEndian ? *(const U32*)ptr : KXXH_swap32(*(const U32*)ptr);
}

FORCE_INLINE U32 KXXH_readLE32(const void* ptr, KXXH_endianess endian)
{
    return KXXH_readLE32_align(ptr, endian, KXXH_unaligned);
}

static U32 KXXH_readBE32(const void* ptr)
{
    return KXXH_CPU_LITTLE_ENDIAN ? KXXH_swap32(KXXH_read32(ptr)) : KXXH_read32(ptr);
}


/* *************************************
*  Macros
***************************************/
#define KXXH_STATIC_ASSERT(c)  { enum { KXXH_sa = 1/(int)(!!(c)) }; }  /* use after variable declarations */
KXXH_PUBLIC_API unsigned KXXH_versionNumber (void) { return KXXH_VERSION_NUMBER; }


/* *******************************************************************
*  32-bit hash functions
*********************************************************************/
static const U32 PRIME32_1 = 2654435761U;
static const U32 PRIME32_2 = 2246822519U;
static const U32 PRIME32_3 = 3266489917U;
static const U32 PRIME32_4 =  668265263U;
static const U32 PRIME32_5 =  374761393U;

static U32 KXXH32_round(U32 seed, U32 input)
{
    seed += input * PRIME32_2;
    seed  = KXXH_rotl32(seed, 13);
    seed *= PRIME32_1;
    return seed;
}

/* mix all bits */
static U32 KXXH32_avalanche(U32 h32)
{
    h32 ^= h32 >> 15;
    h32 *= PRIME32_2;
    h32 ^= h32 >> 13;
    h32 *= PRIME32_3;
    h32 ^= h32 >> 16;
    return(h32);
}

#define KXXH_get32bits(p) KXXH_readLE32_align(p, endian, align)

static U32
KXXH32_finalize(U32 h32, const void* ptr, size_t len,
                KXXH_endianess endian, KXXH_alignment align)

{
    const BYTE* p = (const BYTE*)ptr;

#define PROCESS1               \
    h32 += (*p++) * PRIME32_5; \
    h32 = KXXH_rotl32(h32, 11) * PRIME32_1 ;

#define PROCESS4                         \
    h32 += KXXH_get32bits(p) * PRIME32_3; \
    p+=4;                                \
    h32  = KXXH_rotl32(h32, 17) * PRIME32_4 ;

    switch(len&15)  /* or switch(bEnd - p) */
    {
      case 12:      PROCESS4;
                    /* fallthrough */
      case 8:       PROCESS4;
                    /* fallthrough */
      case 4:       PROCESS4;
                    return KXXH32_avalanche(h32);

      case 13:      PROCESS4;
                    /* fallthrough */
      case 9:       PROCESS4;
                    /* fallthrough */
      case 5:       PROCESS4;
                    PROCESS1;
                    return KXXH32_avalanche(h32);

      case 14:      PROCESS4;
                    /* fallthrough */
      case 10:      PROCESS4;
                    /* fallthrough */
      case 6:       PROCESS4;
                    PROCESS1;
                    PROCESS1;
                    return KXXH32_avalanche(h32);

      case 15:      PROCESS4;
                    /* fallthrough */
      case 11:      PROCESS4;
                    /* fallthrough */
      case 7:       PROCESS4;
                    /* fallthrough */
      case 3:       PROCESS1;
                    /* fallthrough */
      case 2:       PROCESS1;
                    /* fallthrough */
      case 1:       PROCESS1;
                    /* fallthrough */
      case 0:       return KXXH32_avalanche(h32);
    }
    assert(0);
    return h32;   /* reaching this point is deemed impossible */
}


FORCE_INLINE U32
KXXH32_endian_align(const void* input, size_t len, U32 seed,
                    KXXH_endianess endian, KXXH_alignment align)
{
    const BYTE* p = (const BYTE*)input;
    const BYTE* bEnd = p + len;
    U32 h32;

#if defined(KXXH_ACCEPT_NULL_INPUT_POINTER) && (KXXH_ACCEPT_NULL_INPUT_POINTER>=1)
    if (p==NULL) {
        len=0;
        bEnd=p=(const BYTE*)(size_t)16;
    }
#endif

    if (len>=16) {
        const BYTE* const limit = bEnd - 15;
        U32 v1 = seed + PRIME32_1 + PRIME32_2;
        U32 v2 = seed + PRIME32_2;
        U32 v3 = seed + 0;
        U32 v4 = seed - PRIME32_1;

        do {
            v1 = KXXH32_round(v1, KXXH_get32bits(p)); p+=4;
            v2 = KXXH32_round(v2, KXXH_get32bits(p)); p+=4;
            v3 = KXXH32_round(v3, KXXH_get32bits(p)); p+=4;
            v4 = KXXH32_round(v4, KXXH_get32bits(p)); p+=4;
        } while (p < limit);

        h32 = KXXH_rotl32(v1, 1)  + KXXH_rotl32(v2, 7)
            + KXXH_rotl32(v3, 12) + KXXH_rotl32(v4, 18);
    } else {
        h32  = seed + PRIME32_5;
    }

    h32 += (U32)len;

    return KXXH32_finalize(h32, p, len&15, endian, align);
}


KXXH_PUBLIC_API unsigned int KXXH32 (const void* input, size_t len, unsigned int seed)
{
#if 0
    /* Simple version, good for code maintenance, but unfortunately slow for small inputs */
    KXXH32_state_t state;
    KXXH32_reset(&state, seed);
    KXXH32_update(&state, input, len);
    return KXXH32_digest(&state);
#else
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if (KXXH_FORCE_ALIGN_CHECK) {
        if ((((size_t)input) & 3) == 0) {   /* Input is 4-bytes aligned, leverage the speed benefit */
            if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
                return KXXH32_endian_align(input, len, seed, KXXH_littleEndian, KXXH_aligned);
            else
                return KXXH32_endian_align(input, len, seed, KXXH_bigEndian, KXXH_aligned);
    }   }

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH32_endian_align(input, len, seed, KXXH_littleEndian, KXXH_unaligned);
    else
        return KXXH32_endian_align(input, len, seed, KXXH_bigEndian, KXXH_unaligned);
#endif
}



/*======   Hash streaming   ======*/

KXXH_PUBLIC_API KXXH32_state_t* KXXH32_createState(void)
{
    return (KXXH32_state_t*)KXXH_malloc(sizeof(KXXH32_state_t));
}
KXXH_PUBLIC_API KXXH_errorcode KXXH32_freeState(KXXH32_state_t* statePtr)
{
    KXXH_free(statePtr);
    return KXXH_OK;
}

KXXH_PUBLIC_API void KXXH32_copyState(KXXH32_state_t* dstState, const KXXH32_state_t* srcState)
{
    memcpy(dstState, srcState, sizeof(*dstState));
}

KXXH_PUBLIC_API KXXH_errorcode KXXH32_reset(KXXH32_state_t* statePtr, unsigned int seed)
{
    KXXH32_state_t state;   /* using a local state to memcpy() in order to avoid strict-aliasing warnings */
    memset(&state, 0, sizeof(state));
    state.v1 = seed + PRIME32_1 + PRIME32_2;
    state.v2 = seed + PRIME32_2;
    state.v3 = seed + 0;
    state.v4 = seed - PRIME32_1;
    /* do not write into reserved, planned to be removed in a future version */
    memcpy(statePtr, &state, sizeof(state) - sizeof(state.reserved));
    return KXXH_OK;
}


FORCE_INLINE KXXH_errorcode
KXXH32_update_endian(KXXH32_state_t* state, const void* input, size_t len, KXXH_endianess endian)
{
    if (input==NULL)
#if defined(KXXH_ACCEPT_NULL_INPUT_POINTER) && (KXXH_ACCEPT_NULL_INPUT_POINTER>=1)
        return KXXH_OK;
#else
        return KXXH_ERROR;
#endif

    {   const BYTE* p = (const BYTE*)input;
        const BYTE* const bEnd = p + len;

        state->total_len_32 += (unsigned)len;
        state->large_len |= (len>=16) | (state->total_len_32>=16);

        if (state->memsize + len < 16)  {   /* fill in tmp buffer */
            KXXH_memcpy((BYTE*)(state->mem32) + state->memsize, input, len);
            state->memsize += (unsigned)len;
            return KXXH_OK;
        }

        if (state->memsize) {   /* some data left from previous update */
            KXXH_memcpy((BYTE*)(state->mem32) + state->memsize, input, 16-state->memsize);
            {   const U32* p32 = state->mem32;
                state->v1 = KXXH32_round(state->v1, KXXH_readLE32(p32, endian)); p32++;
                state->v2 = KXXH32_round(state->v2, KXXH_readLE32(p32, endian)); p32++;
                state->v3 = KXXH32_round(state->v3, KXXH_readLE32(p32, endian)); p32++;
                state->v4 = KXXH32_round(state->v4, KXXH_readLE32(p32, endian));
            }
            p += 16-state->memsize;
            state->memsize = 0;
        }

        if (p <= bEnd-16) {
            const BYTE* const limit = bEnd - 16;
            U32 v1 = state->v1;
            U32 v2 = state->v2;
            U32 v3 = state->v3;
            U32 v4 = state->v4;

            do {
                v1 = KXXH32_round(v1, KXXH_readLE32(p, endian)); p+=4;
                v2 = KXXH32_round(v2, KXXH_readLE32(p, endian)); p+=4;
                v3 = KXXH32_round(v3, KXXH_readLE32(p, endian)); p+=4;
                v4 = KXXH32_round(v4, KXXH_readLE32(p, endian)); p+=4;
            } while (p<=limit);

            state->v1 = v1;
            state->v2 = v2;
            state->v3 = v3;
            state->v4 = v4;
        }

        if (p < bEnd) {
            KXXH_memcpy(state->mem32, p, (size_t)(bEnd-p));
            state->memsize = (unsigned)(bEnd-p);
        }
    }

    return KXXH_OK;
}


KXXH_PUBLIC_API KXXH_errorcode KXXH32_update (KXXH32_state_t* state_in, const void* input, size_t len)
{
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH32_update_endian(state_in, input, len, KXXH_littleEndian);
    else
        return KXXH32_update_endian(state_in, input, len, KXXH_bigEndian);
}


FORCE_INLINE U32
KXXH32_digest_endian (const KXXH32_state_t* state, KXXH_endianess endian)
{
    U32 h32;

    if (state->large_len) {
        h32 = KXXH_rotl32(state->v1, 1)
            + KXXH_rotl32(state->v2, 7)
            + KXXH_rotl32(state->v3, 12)
            + KXXH_rotl32(state->v4, 18);
    } else {
        h32 = state->v3 /* == seed */ + PRIME32_5;
    }

    h32 += state->total_len_32;

    return KXXH32_finalize(h32, state->mem32, state->memsize, endian, KXXH_aligned);
}


KXXH_PUBLIC_API unsigned int KXXH32_digest (const KXXH32_state_t* state_in)
{
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH32_digest_endian(state_in, KXXH_littleEndian);
    else
        return KXXH32_digest_endian(state_in, KXXH_bigEndian);
}


/*======   Canonical representation   ======*/

/*! Default KXXH result types are basic unsigned 32 and 64 bits.
*   The canonical representation follows human-readable write convention, aka big-endian (large digits first).
*   These functions allow transformation of hash result into and from its canonical format.
*   This way, hash values can be written into a file or buffer, remaining comparable across different systems.
*/

KXXH_PUBLIC_API void KXXH32_canonicalFromHash(KXXH32_canonical_t* dst, KXXH32_hash_t hash)
{
    KXXH_STATIC_ASSERT(sizeof(KXXH32_canonical_t) == sizeof(KXXH32_hash_t));
    if (KXXH_CPU_LITTLE_ENDIAN) hash = KXXH_swap32(hash);
    memcpy(dst, &hash, sizeof(*dst));
}

KXXH_PUBLIC_API KXXH32_hash_t KXXH32_hashFromCanonical(const KXXH32_canonical_t* src)
{
    return KXXH_readBE32(src);
}


#ifndef KXXH_NO_LONG_LONG

/* *******************************************************************
*  64-bit hash functions
*********************************************************************/

/*======   Memory access   ======*/

#ifndef MEM_MODULE
# define MEM_MODULE
# if !defined (__VMS) \
  && (defined (__cplusplus) \
  || (defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */) )
#   include <stdint.h>
    typedef uint64_t U64;
# else
    /* if compiler doesn't support unsigned long long, replace by another 64-bit type */
    typedef unsigned long long U64;
# endif
#endif


#if (defined(KXXH_FORCE_MEMORY_ACCESS) && (KXXH_FORCE_MEMORY_ACCESS==2))

/* Force direct memory access. Only works on CPU which support unaligned memory access in hardware */
static U64 KXXH_read64(const void* memPtr) { return *(const U64*) memPtr; }

#elif (defined(KXXH_FORCE_MEMORY_ACCESS) && (KXXH_FORCE_MEMORY_ACCESS==1))

/* __pack instructions are safer, but compiler specific, hence potentially problematic for some compilers */
/* currently only defined for gcc and icc */
typedef union { U32 u32; U64 u64; } __attribute__((packed)) unalign64;
static U64 KXXH_read64(const void* ptr) { return ((const unalign64*)ptr)->u64; }

#else

/* portable and safe solution. Generally efficient.
 * see : http://stackoverflow.com/a/32095106/646947
 */

static U64 KXXH_read64(const void* memPtr)
{
    U64 val;
    memcpy(&val, memPtr, sizeof(val));
    return val;
}

#endif   /* KXXH_FORCE_DIRECT_MEMORY_ACCESS */

#if defined(_MSC_VER)     /* Visual Studio */
#  define KXXH_swap64 _byteswap_uint64
#elif KXXH_GCC_VERSION >= 403
#  define KXXH_swap64 __builtin_bswap64
#else
static U64 KXXH_swap64 (U64 x)
{
    return  ((x << 56) & 0xff00000000000000ULL) |
            ((x << 40) & 0x00ff000000000000ULL) |
            ((x << 24) & 0x0000ff0000000000ULL) |
            ((x << 8)  & 0x000000ff00000000ULL) |
            ((x >> 8)  & 0x00000000ff000000ULL) |
            ((x >> 24) & 0x0000000000ff0000ULL) |
            ((x >> 40) & 0x000000000000ff00ULL) |
            ((x >> 56) & 0x00000000000000ffULL);
}
#endif

FORCE_INLINE U64 KXXH_readLE64_align(const void* ptr, KXXH_endianess endian, KXXH_alignment align)
{
    if (align==KXXH_unaligned)
        return endian==KXXH_littleEndian ? KXXH_read64(ptr) : KXXH_swap64(KXXH_read64(ptr));
    else
        return endian==KXXH_littleEndian ? *(const U64*)ptr : KXXH_swap64(*(const U64*)ptr);
}

FORCE_INLINE U64 KXXH_readLE64(const void* ptr, KXXH_endianess endian)
{
    return KXXH_readLE64_align(ptr, endian, KXXH_unaligned);
}

static U64 KXXH_readBE64(const void* ptr)
{
    return KXXH_CPU_LITTLE_ENDIAN ? KXXH_swap64(KXXH_read64(ptr)) : KXXH_read64(ptr);
}


/*======   xxh64   ======*/

static const U64 PRIME64_1 = 11400714785074694791ULL;
static const U64 PRIME64_2 = 14029467366897019727ULL;
static const U64 PRIME64_3 =  1609587929392839161ULL;
static const U64 PRIME64_4 =  9650029242287828579ULL;
static const U64 PRIME64_5 =  2870177450012600261ULL;

static U64 KXXH64_round(U64 acc, U64 input)
{
    acc += input * PRIME64_2;
    acc  = KXXH_rotl64(acc, 31);
    acc *= PRIME64_1;
    return acc;
}

static U64 KXXH64_mergeRound(U64 acc, U64 val)
{
    val  = KXXH64_round(0, val);
    acc ^= val;
    acc  = acc * PRIME64_1 + PRIME64_4;
    return acc;
}

static U64 KXXH64_avalanche(U64 h64)
{
    h64 ^= h64 >> 33;
    h64 *= PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= PRIME64_3;
    h64 ^= h64 >> 32;
    return h64;
}


#define KXXH_get64bits(p) KXXH_readLE64_align(p, endian, align)

static U64
KXXH64_finalize(U64 h64, const void* ptr, size_t len,
               KXXH_endianess endian, KXXH_alignment align)
{
    const BYTE* p = (const BYTE*)ptr;

#define PROCESS1_64            \
    h64 ^= (*p++) * PRIME64_5; \
    h64 = KXXH_rotl64(h64, 11) * PRIME64_1;

#define PROCESS4_64          \
    h64 ^= (U64)(KXXH_get32bits(p)) * PRIME64_1; \
    p+=4;                    \
    h64 = KXXH_rotl64(h64, 23) * PRIME64_2 + PRIME64_3;

#define PROCESS8_64 {        \
    U64 const k1 = KXXH64_round(0, KXXH_get64bits(p)); \
    p+=8;                    \
    h64 ^= k1;               \
    h64  = KXXH_rotl64(h64,27) * PRIME64_1 + PRIME64_4; \
}

    switch(len&31) {
      case 24: PROCESS8_64;
                    /* fallthrough */
      case 16: PROCESS8_64;
                    /* fallthrough */
      case  8: PROCESS8_64;
               return KXXH64_avalanche(h64);

      case 28: PROCESS8_64;
                    /* fallthrough */
      case 20: PROCESS8_64;
                    /* fallthrough */
      case 12: PROCESS8_64;
                    /* fallthrough */
      case  4: PROCESS4_64;
               return KXXH64_avalanche(h64);

      case 25: PROCESS8_64;
                    /* fallthrough */
      case 17: PROCESS8_64;
                    /* fallthrough */
      case  9: PROCESS8_64;
               PROCESS1_64;
               return KXXH64_avalanche(h64);

      case 29: PROCESS8_64;
                    /* fallthrough */
      case 21: PROCESS8_64;
                    /* fallthrough */
      case 13: PROCESS8_64;
                    /* fallthrough */
      case  5: PROCESS4_64;
               PROCESS1_64;
               return KXXH64_avalanche(h64);

      case 26: PROCESS8_64;
                    /* fallthrough */
      case 18: PROCESS8_64;
                    /* fallthrough */
      case 10: PROCESS8_64;
               PROCESS1_64;
               PROCESS1_64;
               return KXXH64_avalanche(h64);

      case 30: PROCESS8_64;
                    /* fallthrough */
      case 22: PROCESS8_64;
                    /* fallthrough */
      case 14: PROCESS8_64;
                    /* fallthrough */
      case  6: PROCESS4_64;
               PROCESS1_64;
               PROCESS1_64;
               return KXXH64_avalanche(h64);

      case 27: PROCESS8_64;
                    /* fallthrough */
      case 19: PROCESS8_64;
                    /* fallthrough */
      case 11: PROCESS8_64;
               PROCESS1_64;
               PROCESS1_64;
               PROCESS1_64;
               return KXXH64_avalanche(h64);

      case 31: PROCESS8_64;
                    /* fallthrough */
      case 23: PROCESS8_64;
                    /* fallthrough */
      case 15: PROCESS8_64;
                    /* fallthrough */
      case  7: PROCESS4_64;
                    /* fallthrough */
      case  3: PROCESS1_64;
                    /* fallthrough */
      case  2: PROCESS1_64;
                    /* fallthrough */
      case  1: PROCESS1_64;
                    /* fallthrough */
      case  0: return KXXH64_avalanche(h64);
    }

    /* impossible to reach */
    assert(0);
    return 0;  /* unreachable, but some compilers complain without it */
}

FORCE_INLINE U64
KXXH64_endian_align(const void* input, size_t len, U64 seed,
                KXXH_endianess endian, KXXH_alignment align)
{
    const BYTE* p = (const BYTE*)input;
    const BYTE* bEnd = p + len;
    U64 h64;

#if defined(KXXH_ACCEPT_NULL_INPUT_POINTER) && (KXXH_ACCEPT_NULL_INPUT_POINTER>=1)
    if (p==NULL) {
        len=0;
        bEnd=p=(const BYTE*)(size_t)32;
    }
#endif

    if (len>=32) {
        const BYTE* const limit = bEnd - 32;
        U64 v1 = seed + PRIME64_1 + PRIME64_2;
        U64 v2 = seed + PRIME64_2;
        U64 v3 = seed + 0;
        U64 v4 = seed - PRIME64_1;

        do {
            v1 = KXXH64_round(v1, KXXH_get64bits(p)); p+=8;
            v2 = KXXH64_round(v2, KXXH_get64bits(p)); p+=8;
            v3 = KXXH64_round(v3, KXXH_get64bits(p)); p+=8;
            v4 = KXXH64_round(v4, KXXH_get64bits(p)); p+=8;
        } while (p<=limit);

        h64 = KXXH_rotl64(v1, 1) + KXXH_rotl64(v2, 7) + KXXH_rotl64(v3, 12) + KXXH_rotl64(v4, 18);
        h64 = KXXH64_mergeRound(h64, v1);
        h64 = KXXH64_mergeRound(h64, v2);
        h64 = KXXH64_mergeRound(h64, v3);
        h64 = KXXH64_mergeRound(h64, v4);

    } else {
        h64  = seed + PRIME64_5;
    }

    h64 += (U64) len;

    return KXXH64_finalize(h64, p, len, endian, align);
}


KXXH_PUBLIC_API unsigned long long KXXH64 (const void* input, size_t len, unsigned long long seed)
{
#if 0
    /* Simple version, good for code maintenance, but unfortunately slow for small inputs */
    KXXH64_state_t state;
    KXXH64_reset(&state, seed);
    KXXH64_update(&state, input, len);
    return KXXH64_digest(&state);
#else
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if (KXXH_FORCE_ALIGN_CHECK) {
        if ((((size_t)input) & 7)==0) {  /* Input is aligned, let's leverage the speed advantage */
            if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
                return KXXH64_endian_align(input, len, seed, KXXH_littleEndian, KXXH_aligned);
            else
                return KXXH64_endian_align(input, len, seed, KXXH_bigEndian, KXXH_aligned);
    }   }

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH64_endian_align(input, len, seed, KXXH_littleEndian, KXXH_unaligned);
    else
        return KXXH64_endian_align(input, len, seed, KXXH_bigEndian, KXXH_unaligned);
#endif
}

/*======   Hash Streaming   ======*/

KXXH_PUBLIC_API KXXH64_state_t* KXXH64_createState(void)
{
    return (KXXH64_state_t*)KXXH_malloc(sizeof(KXXH64_state_t));
}
KXXH_PUBLIC_API KXXH_errorcode KXXH64_freeState(KXXH64_state_t* statePtr)
{
    KXXH_free(statePtr);
    return KXXH_OK;
}

KXXH_PUBLIC_API void KXXH64_copyState(KXXH64_state_t* dstState, const KXXH64_state_t* srcState)
{
    memcpy(dstState, srcState, sizeof(*dstState));
}

KXXH_PUBLIC_API KXXH_errorcode KXXH64_reset(KXXH64_state_t* statePtr, unsigned long long seed)
{
    KXXH64_state_t state;   /* using a local state to memcpy() in order to avoid strict-aliasing warnings */
    memset(&state, 0, sizeof(state));
    state.v1 = seed + PRIME64_1 + PRIME64_2;
    state.v2 = seed + PRIME64_2;
    state.v3 = seed + 0;
    state.v4 = seed - PRIME64_1;
     /* do not write into reserved, planned to be removed in a future version */
    memcpy(statePtr, &state, sizeof(state) - sizeof(state.reserved));
    return KXXH_OK;
}

FORCE_INLINE KXXH_errorcode
KXXH64_update_endian (KXXH64_state_t* state, const void* input, size_t len, KXXH_endianess endian)
{
    if (input==NULL)
#if defined(KXXH_ACCEPT_NULL_INPUT_POINTER) && (KXXH_ACCEPT_NULL_INPUT_POINTER>=1)
        return KXXH_OK;
#else
        return KXXH_ERROR;
#endif

    {   const BYTE* p = (const BYTE*)input;
        const BYTE* const bEnd = p + len;

        state->total_len += len;

        if (state->memsize + len < 32) {  /* fill in tmp buffer */
            KXXH_memcpy(((BYTE*)state->mem64) + state->memsize, input, len);
            state->memsize += (U32)len;
            return KXXH_OK;
        }

        if (state->memsize) {   /* tmp buffer is full */
            KXXH_memcpy(((BYTE*)state->mem64) + state->memsize, input, 32-state->memsize);
            state->v1 = KXXH64_round(state->v1, KXXH_readLE64(state->mem64+0, endian));
            state->v2 = KXXH64_round(state->v2, KXXH_readLE64(state->mem64+1, endian));
            state->v3 = KXXH64_round(state->v3, KXXH_readLE64(state->mem64+2, endian));
            state->v4 = KXXH64_round(state->v4, KXXH_readLE64(state->mem64+3, endian));
            p += 32-state->memsize;
            state->memsize = 0;
        }

        if (p+32 <= bEnd) {
            const BYTE* const limit = bEnd - 32;
            U64 v1 = state->v1;
            U64 v2 = state->v2;
            U64 v3 = state->v3;
            U64 v4 = state->v4;

            do {
                v1 = KXXH64_round(v1, KXXH_readLE64(p, endian)); p+=8;
                v2 = KXXH64_round(v2, KXXH_readLE64(p, endian)); p+=8;
                v3 = KXXH64_round(v3, KXXH_readLE64(p, endian)); p+=8;
                v4 = KXXH64_round(v4, KXXH_readLE64(p, endian)); p+=8;
            } while (p<=limit);

            state->v1 = v1;
            state->v2 = v2;
            state->v3 = v3;
            state->v4 = v4;
        }

        if (p < bEnd) {
            KXXH_memcpy(state->mem64, p, (size_t)(bEnd-p));
            state->memsize = (unsigned)(bEnd-p);
        }
    }

    return KXXH_OK;
}

KXXH_PUBLIC_API KXXH_errorcode KXXH64_update (KXXH64_state_t* state_in, const void* input, size_t len)
{
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH64_update_endian(state_in, input, len, KXXH_littleEndian);
    else
        return KXXH64_update_endian(state_in, input, len, KXXH_bigEndian);
}

FORCE_INLINE U64 KXXH64_digest_endian (const KXXH64_state_t* state, KXXH_endianess endian)
{
    U64 h64;

    if (state->total_len >= 32) {
        U64 const v1 = state->v1;
        U64 const v2 = state->v2;
        U64 const v3 = state->v3;
        U64 const v4 = state->v4;

        h64 = KXXH_rotl64(v1, 1) + KXXH_rotl64(v2, 7) + KXXH_rotl64(v3, 12) + KXXH_rotl64(v4, 18);
        h64 = KXXH64_mergeRound(h64, v1);
        h64 = KXXH64_mergeRound(h64, v2);
        h64 = KXXH64_mergeRound(h64, v3);
        h64 = KXXH64_mergeRound(h64, v4);
    } else {
        h64  = state->v3 /*seed*/ + PRIME64_5;
    }

    h64 += (U64) state->total_len;

    return KXXH64_finalize(h64, state->mem64, (size_t)state->total_len, endian, KXXH_aligned);
}

KXXH_PUBLIC_API unsigned long long KXXH64_digest (const KXXH64_state_t* state_in)
{
    KXXH_endianess endian_detected = (KXXH_endianess)KXXH_CPU_LITTLE_ENDIAN;

    if ((endian_detected==KXXH_littleEndian) || KXXH_FORCE_NATIVE_FORMAT)
        return KXXH64_digest_endian(state_in, KXXH_littleEndian);
    else
        return KXXH64_digest_endian(state_in, KXXH_bigEndian);
}


/*====== Canonical representation   ======*/

KXXH_PUBLIC_API void KXXH64_canonicalFromHash(KXXH64_canonical_t* dst, KXXH64_hash_t hash)
{
    KXXH_STATIC_ASSERT(sizeof(KXXH64_canonical_t) == sizeof(KXXH64_hash_t));
    if (KXXH_CPU_LITTLE_ENDIAN) hash = KXXH_swap64(hash);
    memcpy(dst, &hash, sizeof(*dst));
}

KXXH_PUBLIC_API KXXH64_hash_t KXXH64_hashFromCanonical(const KXXH64_canonical_t* src)
{
    return KXXH_readBE64(src);
}

#endif  /* KXXH_NO_LONG_LONG */
