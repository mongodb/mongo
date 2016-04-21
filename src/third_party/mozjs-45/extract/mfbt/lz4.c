/*
   LZ4 - Fast LZ compression algorithm
   Copyright (C) 2011-2014, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - LZ4 source repository : http://code.google.com/p/lz4/
   - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/**************************************
   Tuning parameters
**************************************/
/*
 * HEAPMODE :
 * Select how default compression functions will allocate memory for their hash table,
 * in memory stack (0:default, fastest), or in memory heap (1:requires memory allocation (malloc)).
 */
#define HEAPMODE 0


/**************************************
   CPU Feature Detection
**************************************/
/* 32 or 64 bits ? */
#if (defined(__x86_64__) || defined(_M_X64) || defined(_WIN64) \
  || defined(__powerpc64__) || defined(__powerpc64le__) \
  || defined(__ppc64__) || defined(__ppc64le__) \
  || defined(__PPC64__) || defined(__PPC64LE__) \
  || defined(__ia64) || defined(__itanium__) || defined(_M_IA64) \
  || (defined(__mips64) && defined(_ABI64)))   /* Detects 64 bits mode */
#  define LZ4_ARCH64 1
#else
#  define LZ4_ARCH64 0
#endif

/*
 * Little Endian or Big Endian ?
 * Overwrite the #define below if you know your architecture endianess
 */
#include <stdlib.h>   /* Apparently required to detect endianess */
#if defined (__GLIBC__)
#  include <endian.h>
#  if (__BYTE_ORDER == __BIG_ENDIAN)
#     define LZ4_BIG_ENDIAN 1
#  endif
#elif (defined(__BIG_ENDIAN__) || defined(__BIG_ENDIAN) || defined(_BIG_ENDIAN)) && !(defined(__LITTLE_ENDIAN__) || defined(__LITTLE_ENDIAN) || defined(_LITTLE_ENDIAN))
#  define LZ4_BIG_ENDIAN 1
#elif defined(__sparc) || defined(__sparc__) \
   || defined(__powerpc__) || defined(__ppc__) || defined(__PPC__) \
   || defined(__hpux)  || defined(__hppa) \
   || defined(_MIPSEB) || defined(__s390__)
#  define LZ4_BIG_ENDIAN 1
#else
/* Little Endian assumed. PDP Endian and other very rare endian format are unsupported. */
#endif

/*
 * Unaligned memory access is automatically enabled for "common" CPU, such as x86.
 * For others CPU, such as ARM, the compiler may be more cautious, inserting unnecessary extra code to ensure aligned access property
 * If you know your target CPU supports unaligned memory access, you want to force this option manually to improve performance
 */
#if defined(__ARM_FEATURE_UNALIGNED)
#  define LZ4_FORCE_UNALIGNED_ACCESS 1
#endif

/* Define this parameter if your target system or compiler does not support hardware bit count */
#if defined(_MSC_VER) && defined(_WIN32_WCE)   /* Visual Studio for Windows CE does not support Hardware bit count */
#  define LZ4_FORCE_SW_BITCOUNT
#endif

/*
 * BIG_ENDIAN_NATIVE_BUT_INCOMPATIBLE :
 * This option may provide a small boost to performance for some big endian cpu, although probably modest.
 * You may set this option to 1 if data will remain within closed environment.
 * This option is useless on Little_Endian CPU (such as x86)
 */

/* #define BIG_ENDIAN_NATIVE_BUT_INCOMPATIBLE 1 */


/**************************************
 Compiler Options
**************************************/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
/* "restrict" is a known keyword */
#else
#  define restrict /* Disable restrict */
#endif

#ifdef _MSC_VER    /* Visual Studio */
#  define FORCE_INLINE static __forceinline
#  include <intrin.h>                    /* For Visual 2005 */
#  if LZ4_ARCH64   /* 64-bits */
#    pragma intrinsic(_BitScanForward64) /* For Visual 2005 */
#    pragma intrinsic(_BitScanReverse64) /* For Visual 2005 */
#  else            /* 32-bits */
#    pragma intrinsic(_BitScanForward)   /* For Visual 2005 */
#    pragma intrinsic(_BitScanReverse)   /* For Visual 2005 */
#  endif
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#else
#  ifdef __GNUC__
#    define FORCE_INLINE static inline __attribute__((always_inline))
#  else
#    define FORCE_INLINE static inline
#  endif
#endif

#ifdef _MSC_VER  /* Visual Studio */
#  define lz4_bswap16(x) _byteswap_ushort(x)
#else
#  define lz4_bswap16(x) ((unsigned short int) ((((x) >> 8) & 0xffu) | (((x) & 0xffu) << 8)))
#endif

#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#if (GCC_VERSION >= 302) || (__INTEL_COMPILER >= 800) || defined(__clang__)
#  define expect(expr,value)    (__builtin_expect ((expr),(value)) )
#else
#  define expect(expr,value)    (expr)
#endif

#define likely(expr)     expect((expr) != 0, 1)
#define unlikely(expr)   expect((expr) != 0, 0)


/**************************************
   Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(n,s) calloc(n,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy */
#define MEM_INIT       memset


/**************************************
   Includes
**************************************/
#include "lz4.h"


/**************************************
   Basic Types
**************************************/
#if defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
# include <stdint.h>
  typedef  uint8_t BYTE;
  typedef uint16_t U16;
  typedef uint32_t U32;
  typedef  int32_t S32;
  typedef uint64_t U64;
#else
  typedef unsigned char       BYTE;
  typedef unsigned short      U16;
  typedef unsigned int        U32;
  typedef   signed int        S32;
  typedef unsigned long long  U64;
#endif

#if defined(__GNUC__)  && !defined(LZ4_FORCE_UNALIGNED_ACCESS)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

#if !defined(LZ4_FORCE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  if defined(__IBMC__) || defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    pragma pack(1)
#  else
#    pragma pack(push, 1)
#  endif
#endif

typedef struct { U16 v; }  _PACKED U16_S;
typedef struct { U32 v; }  _PACKED U32_S;
typedef struct { U64 v; }  _PACKED U64_S;
typedef struct {size_t v;} _PACKED size_t_S;

#if !defined(LZ4_FORCE_UNALIGNED_ACCESS) && !defined(__GNUC__)
#  if defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    pragma pack(0)
#  else
#    pragma pack(pop)
#  endif
#endif

#define A16(x)   (((U16_S *)(x))->v)
#define A32(x)   (((U32_S *)(x))->v)
#define A64(x)   (((U64_S *)(x))->v)
#define AARCH(x) (((size_t_S *)(x))->v)


/**************************************
   Constants
**************************************/
#define LZ4_HASHLOG   (LZ4_MEMORY_USAGE-2)
#define HASHTABLESIZE (1 << LZ4_MEMORY_USAGE)
#define HASH_SIZE_U32 (1 << LZ4_HASHLOG)

#define MINMATCH 4

#define COPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT (COPYLENGTH+MINMATCH)
static const int LZ4_minLength = (MFLIMIT+1);

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define LZ4_64KLIMIT ((64 KB) + (MFLIMIT-1))
#define SKIPSTRENGTH 6   /* Increasing this value will make the compression run slower on incompressible data */

#define MAXD_LOG 16
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)

#define ML_BITS  4
#define ML_MASK  ((1U<<ML_BITS)-1)
#define RUN_BITS (8-ML_BITS)
#define RUN_MASK ((1U<<RUN_BITS)-1)


/**************************************
   Structures and local types
**************************************/
typedef struct {
    U32  hashTable[HASH_SIZE_U32];
    U32  currentOffset;
    U32  initCheck;
    const BYTE* dictionary;
    const BYTE* bufferStart;
    U32  dictSize;
} LZ4_stream_t_internal;

typedef enum { notLimited = 0, limitedOutput = 1 } limitedOutput_directive;
typedef enum { byPtr, byU32, byU16 } tableType_t;

typedef enum { noDict = 0, withPrefix64k, usingExtDict } dict_directive;
typedef enum { noDictIssue = 0, dictSmall } dictIssue_directive;

typedef enum { endOnOutputSize = 0, endOnInputSize = 1 } endCondition_directive;
typedef enum { full = 0, partial = 1 } earlyEnd_directive;


/**************************************
   Architecture-specific macros
**************************************/
#define STEPSIZE                  sizeof(size_t)
#define LZ4_COPYSTEP(d,s)         { AARCH(d) = AARCH(s); d+=STEPSIZE; s+=STEPSIZE; }
#define LZ4_COPY8(d,s)            { LZ4_COPYSTEP(d,s); if (STEPSIZE<8) LZ4_COPYSTEP(d,s); }

#if (defined(LZ4_BIG_ENDIAN) && !defined(BIG_ENDIAN_NATIVE_BUT_INCOMPATIBLE))
#  define LZ4_READ_LITTLEENDIAN_16(d,s,p) { U16 v = A16(p); v = lz4_bswap16(v); d = (s) - v; }
#  define LZ4_WRITE_LITTLEENDIAN_16(p,i)  { U16 v = (U16)(i); v = lz4_bswap16(v); A16(p) = v; p+=2; }
#else      /* Little Endian */
#  define LZ4_READ_LITTLEENDIAN_16(d,s,p) { d = (s) - A16(p); }
#  define LZ4_WRITE_LITTLEENDIAN_16(p,v)  { A16(p) = v; p+=2; }
#endif


/**************************************
   Macros
**************************************/
#define LZ4_STATIC_ASSERT(c)    { enum { LZ4_static_assert = 1/(!!(c)) }; }   /* use only *after* variable declarations */
#if LZ4_ARCH64 || !defined(__GNUC__)
#  define LZ4_WILDCOPY(d,s,e)   { do { LZ4_COPY8(d,s) } while (d<e); }        /* at the end, d>=e; */
#else
#  define LZ4_WILDCOPY(d,s,e)   { if (likely(e-d <= 8)) LZ4_COPY8(d,s) else do { LZ4_COPY8(d,s) } while (d<e); }
#endif


/****************************
   Private local functions
****************************/
#if LZ4_ARCH64

int LZ4_NbCommonBytes (register U64 val)
{
# if defined(LZ4_BIG_ENDIAN)
#   if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanReverse64( &r, val );
    return (int)(r>>3);
#   elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_clzll(val) >> 3);
#   else
    int r;
    if (!(val>>32)) { r=4; } else { r=0; val>>=32; }
    if (!(val>>16)) { r+=2; val>>=8; } else { val>>=24; }
    r += (!val);
    return r;
#   endif
# else
#   if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanForward64( &r, val );
    return (int)(r>>3);
#   elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_ctzll(val) >> 3);
#   else
    static const int DeBruijnBytePos[64] = { 0, 0, 0, 0, 0, 1, 1, 2, 0, 3, 1, 3, 1, 4, 2, 7, 0, 2, 3, 6, 1, 5, 3, 5, 1, 3, 4, 4, 2, 5, 6, 7, 7, 0, 1, 2, 3, 3, 4, 6, 2, 6, 5, 5, 3, 4, 5, 6, 7, 1, 2, 4, 6, 4, 4, 5, 7, 2, 6, 5, 7, 6, 7, 7 };
    return DeBruijnBytePos[((U64)((val & -(long long)val) * 0x0218A392CDABBD3FULL)) >> 58];
#   endif
# endif
}

#else

int LZ4_NbCommonBytes (register U32 val)
{
# if defined(LZ4_BIG_ENDIAN)
#   if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r = 0;
    _BitScanReverse( &r, val );
    return (int)(r>>3);
#   elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_clz(val) >> 3);
#   else
    int r;
    if (!(val>>16)) { r=2; val>>=8; } else { r=0; val>>=24; }
    r += (!val);
    return r;
#   endif
# else
#   if defined(_MSC_VER) && !defined(LZ4_FORCE_SW_BITCOUNT)
    unsigned long r;
    _BitScanForward( &r, val );
    return (int)(r>>3);
#   elif defined(__GNUC__) && (GCC_VERSION >= 304) && !defined(LZ4_FORCE_SW_BITCOUNT)
    return (__builtin_ctz(val) >> 3);
#   else
    static const int DeBruijnBytePos[32] = { 0, 0, 3, 0, 3, 1, 3, 0, 3, 2, 2, 1, 3, 2, 0, 1, 3, 3, 1, 2, 2, 2, 2, 0, 3, 1, 2, 0, 1, 0, 1, 1 };
    return DeBruijnBytePos[((U32)((val & -(S32)val) * 0x077CB531U)) >> 27];
#   endif
# endif
}

#endif


/********************************
   Compression functions
********************************/
int LZ4_compressBound(int isize)  { return LZ4_COMPRESSBOUND(isize); }

static int LZ4_hashSequence(U32 sequence, tableType_t tableType)
{
    if (tableType == byU16)
        return (((sequence) * 2654435761U) >> ((MINMATCH*8)-(LZ4_HASHLOG+1)));
    else
        return (((sequence) * 2654435761U) >> ((MINMATCH*8)-LZ4_HASHLOG));
}

static int LZ4_hashPosition(const BYTE* p, tableType_t tableType) { return LZ4_hashSequence(A32(p), tableType); }

static void LZ4_putPositionOnHash(const BYTE* p, U32 h, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    switch (tableType)
    {
    case byPtr: { const BYTE** hashTable = (const BYTE**) tableBase; hashTable[h] = p; break; }
    case byU32: { U32* hashTable = (U32*) tableBase; hashTable[h] = (U32)(p-srcBase); break; }
    case byU16: { U16* hashTable = (U16*) tableBase; hashTable[h] = (U16)(p-srcBase); break; }
    }
}

static void LZ4_putPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ4_hashPosition(p, tableType);
    LZ4_putPositionOnHash(p, h, tableBase, tableType, srcBase);
}

static const BYTE* LZ4_getPositionOnHash(U32 h, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    if (tableType == byPtr) { const BYTE** hashTable = (const BYTE**) tableBase; return hashTable[h]; }
    if (tableType == byU32) { U32* hashTable = (U32*) tableBase; return hashTable[h] + srcBase; }
    { U16* hashTable = (U16*) tableBase; return hashTable[h] + srcBase; }   /* default, to ensure a return */
}

static const BYTE* LZ4_getPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ4_hashPosition(p, tableType);
    return LZ4_getPositionOnHash(h, tableBase, tableType, srcBase);
}

static unsigned LZ4_count(const BYTE* pIn, const BYTE* pRef, const BYTE* pInLimit)
{
    const BYTE* const pStart = pIn;

    while (likely(pIn<pInLimit-(STEPSIZE-1)))
    {
        size_t diff = AARCH(pRef) ^ AARCH(pIn);
        if (!diff) { pIn+=STEPSIZE; pRef+=STEPSIZE; continue; }
        pIn += LZ4_NbCommonBytes(diff);
        return (unsigned)(pIn - pStart);
    }
    if (sizeof(void*)==8) if ((pIn<(pInLimit-3)) && (A32(pRef) == A32(pIn))) { pIn+=4; pRef+=4; }
    if ((pIn<(pInLimit-1)) && (A16(pRef) == A16(pIn))) { pIn+=2; pRef+=2; }
    if ((pIn<pInLimit) && (*pRef == *pIn)) pIn++;

    return (unsigned)(pIn - pStart);
}


static int LZ4_compress_generic(
                 void* ctx,
                 const char* source,
                 char* dest,
                 int inputSize,
                 int maxOutputSize,

                 limitedOutput_directive outputLimited,
                 tableType_t tableType,
                 dict_directive dict,
                 dictIssue_directive dictIssue)
{
    LZ4_stream_t_internal* const dictPtr = (LZ4_stream_t_internal*)ctx;

    const BYTE* ip = (const BYTE*) source;
    const BYTE* base;
    const BYTE* lowLimit;
    const BYTE* const lowRefLimit = ip - dictPtr->dictSize;
    const BYTE* const dictionary = dictPtr->dictionary;
    const BYTE* const dictEnd = dictionary + dictPtr->dictSize;
    const size_t dictDelta = dictEnd - (const BYTE*)source;
    const BYTE* anchor = (const BYTE*) source;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;

    BYTE* op = (BYTE*) dest;
    BYTE* const olimit = op + maxOutputSize;

    const int skipStrength = SKIPSTRENGTH;
    U32 forwardH;
    size_t refDelta=0;

    /* Init conditions */
    if ((U32)inputSize > (U32)LZ4_MAX_INPUT_SIZE) return 0;          /* Unsupported input size, too large (or negative) */
    switch(dict)
    {
    case noDict:
    default:
        base = (const BYTE*)source;
        lowLimit = (const BYTE*)source;
        break;
    case withPrefix64k:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source - dictPtr->dictSize;
        break;
    case usingExtDict:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source;
        break;
    }
    if ((tableType == byU16) && (inputSize>=(int)LZ4_64KLIMIT)) return 0;   /* Size too large (not within 64K limit) */
    if (inputSize<LZ4_minLength) goto _last_literals;                       /* Input too small, no compression (all literals) */

    /* First Byte */
    LZ4_putPosition(ip, ctx, tableType, base);
    ip++; forwardH = LZ4_hashPosition(ip, tableType);

    /* Main Loop */
    for ( ; ; )
    {
        const BYTE* ref;
        BYTE* token;
        {
            const BYTE* forwardIp = ip;
            unsigned step=1;
            unsigned searchMatchNb = (1U << skipStrength);

            /* Find a match */
            do {
                U32 h = forwardH;
                ip = forwardIp;
                forwardIp += step;
                step = searchMatchNb++ >> skipStrength;
                //if (step>8) step=8;   // required for valid forwardIp ; slows down uncompressible data a bit

                if (unlikely(forwardIp > mflimit)) goto _last_literals;

                ref = LZ4_getPositionOnHash(h, ctx, tableType, base);
                if (dict==usingExtDict)
                {
                    if (ref<(const BYTE*)source)
                    {
                        refDelta = dictDelta;
                        lowLimit = dictionary;
                    }
                    else
                    {
                        refDelta = 0;
                        lowLimit = (const BYTE*)source;
                    }
                }
                forwardH = LZ4_hashPosition(forwardIp, tableType);
                LZ4_putPositionOnHash(ip, h, ctx, tableType, base);

            } while ( ((dictIssue==dictSmall) ? (ref < lowRefLimit) : 0)
                || ((tableType==byU16) ? 0 : (ref + MAX_DISTANCE < ip))
                || (A32(ref+refDelta) != A32(ip)) );
        }

        /* Catch up */
        while ((ip>anchor) && (ref+refDelta > lowLimit) && (unlikely(ip[-1]==ref[refDelta-1]))) { ip--; ref--; }

        {
            /* Encode Literal length */
            unsigned litLength = (unsigned)(ip - anchor);
            token = op++;
            if ((outputLimited) && (unlikely(op + litLength + (2 + 1 + LASTLITERALS) + (litLength/255) > olimit)))
                return 0;   /* Check output limit */
            if (litLength>=RUN_MASK)
            {
                int len = (int)litLength-RUN_MASK;
                *token=(RUN_MASK<<ML_BITS);
                for(; len >= 255 ; len-=255) *op++ = 255;
                *op++ = (BYTE)len;
            }
            else *token = (BYTE)(litLength<<ML_BITS);

            /* Copy Literals */
            { BYTE* end = op+litLength; LZ4_WILDCOPY(op,anchor,end); op=end; }
        }

_next_match:
        /* Encode Offset */
        LZ4_WRITE_LITTLEENDIAN_16(op, (U16)(ip-ref));

        /* Encode MatchLength */
        {
            unsigned matchLength;

            if ((dict==usingExtDict) && (lowLimit==dictionary))
            {
                const BYTE* limit;
                ref += refDelta;
                limit = ip + (dictEnd-ref);
                if (limit > matchlimit) limit = matchlimit;
                matchLength = LZ4_count(ip+MINMATCH, ref+MINMATCH, limit);
                ip += MINMATCH + matchLength;
                if (ip==limit)
                {
                    unsigned more = LZ4_count(ip, (const BYTE*)source, matchlimit);
                    matchLength += more;
                    ip += more;
                }
            }
            else
            {
                matchLength = LZ4_count(ip+MINMATCH, ref+MINMATCH, matchlimit);
                ip += MINMATCH + matchLength;
            }

            if (matchLength>=ML_MASK)
            {
                if ((outputLimited) && (unlikely(op + (1 + LASTLITERALS) + (matchLength>>8) > olimit)))
                    return 0;    /* Check output limit */
                *token += ML_MASK;
                matchLength -= ML_MASK;
                for (; matchLength >= 510 ; matchLength-=510) { *op++ = 255; *op++ = 255; }
                if (matchLength >= 255) { matchLength-=255; *op++ = 255; }
                *op++ = (BYTE)matchLength;
            }
            else *token += (BYTE)(matchLength);
        }

        anchor = ip;

        /* Test end of chunk */
        if (ip > mflimit) break;

        /* Fill table */
        LZ4_putPosition(ip-2, ctx, tableType, base);

        /* Test next position */
        ref = LZ4_getPosition(ip, ctx, tableType, base);
        if (dict==usingExtDict)
        {
            if (ref<(const BYTE*)source)
            {
                refDelta = dictDelta;
                lowLimit = dictionary;
            }
            else
            {
                refDelta = 0;
                lowLimit = (const BYTE*)source;
            }
        }
        LZ4_putPosition(ip, ctx, tableType, base);
        if ( ((dictIssue==dictSmall) ? (ref>=lowRefLimit) : 1)
            && (ref+MAX_DISTANCE>=ip)
            && (A32(ref+refDelta)==A32(ip)) )
        { token=op++; *token=0; goto _next_match; }

        /* Prepare next loop */
        forwardH = LZ4_hashPosition(++ip, tableType);
    }

_last_literals:
    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
        if ((outputLimited) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize))
            return 0;   /* Check output limit */
        if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun >= 255 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
        else *op++ = (BYTE)(lastRun<<ML_BITS);
        memcpy(op, anchor, iend - anchor);
        op += iend-anchor;
    }

    /* End */
    return (int) (((char*)op)-dest);
}


int LZ4_compress(const char* source, char* dest, int inputSize)
{
#if (HEAPMODE)
    void* ctx = ALLOCATOR(LZ4_STREAMSIZE_U32, 4);   /* Aligned on 4-bytes boundaries */
#else
    U32 ctx[LZ4_STREAMSIZE_U32] = {0};      /* Ensure data is aligned on 4-bytes boundaries */
#endif
    int result;

    if (inputSize < (int)LZ4_64KLIMIT)
        result = LZ4_compress_generic((void*)ctx, source, dest, inputSize, 0, notLimited, byU16, noDict, noDictIssue);
    else
        result = LZ4_compress_generic((void*)ctx, source, dest, inputSize, 0, notLimited, (sizeof(void*)==8) ? byU32 : byPtr, noDict, noDictIssue);

#if (HEAPMODE)
    FREEMEM(ctx);
#endif
    return result;
}

int LZ4_compress_limitedOutput(const char* source, char* dest, int inputSize, int maxOutputSize)
{
#if (HEAPMODE)
    void* ctx = ALLOCATOR(LZ4_STREAMSIZE_U32, 4);   /* Aligned on 4-bytes boundaries */
#else
    U32 ctx[LZ4_STREAMSIZE_U32] = {0};      /* Ensure data is aligned on 4-bytes boundaries */
#endif
    int result;

    if (inputSize < (int)LZ4_64KLIMIT)
        result = LZ4_compress_generic((void*)ctx, source, dest, inputSize, maxOutputSize, limitedOutput, byU16, noDict, noDictIssue);
    else
        result = LZ4_compress_generic((void*)ctx, source, dest, inputSize, maxOutputSize, limitedOutput, (sizeof(void*)==8) ? byU32 : byPtr, noDict, noDictIssue);

#if (HEAPMODE)
    FREEMEM(ctx);
#endif
    return result;
}


/*****************************************
   Experimental : Streaming functions
*****************************************/

void* LZ4_createStream()
{
    void* lz4s = ALLOCATOR(4, LZ4_STREAMSIZE_U32);
    MEM_INIT(lz4s, 0, LZ4_STREAMSIZE);
    return lz4s;
}

int LZ4_free (void* LZ4_stream)
{
    FREEMEM(LZ4_stream);
    return (0);
}


int LZ4_loadDict (void* LZ4_dict, const char* dictionary, int dictSize)
{
    LZ4_stream_t_internal* dict = (LZ4_stream_t_internal*) LZ4_dict;
    const BYTE* p = (const BYTE*)dictionary;
    const BYTE* const dictEnd = p + dictSize;
    const BYTE* base;

    LZ4_STATIC_ASSERT(LZ4_STREAMSIZE >= sizeof(LZ4_stream_t_internal));      /* A compilation error here means LZ4_STREAMSIZE is not large enough */
    if (dict->initCheck) MEM_INIT(dict, 0, sizeof(LZ4_stream_t_internal));   /* Uninitialized structure detected */

    if (dictSize < MINMATCH)
    {
        dict->dictionary = NULL;
        dict->dictSize = 0;
        return 1;
    }

    if (p <= dictEnd - 64 KB) p = dictEnd - 64 KB;
    base = p - dict->currentOffset;
    dict->dictionary = p;
    dict->dictSize = (U32)(dictEnd - p);
    dict->currentOffset += dict->dictSize;

    while (p <= dictEnd-MINMATCH)
    {
        LZ4_putPosition(p, dict, byU32, base);
        p+=3;
    }

    return 1;
}


void LZ4_renormDictT(LZ4_stream_t_internal* LZ4_dict, const BYTE* src)
{
    if ((LZ4_dict->currentOffset > 0x80000000) ||
        ((size_t)LZ4_dict->currentOffset > (size_t)src))   /* address space overflow */
    {
        /* rescale hash table */
        U32 delta = LZ4_dict->currentOffset - 64 KB;
        const BYTE* dictEnd = LZ4_dict->dictionary + LZ4_dict->dictSize;
        int i;
        for (i=0; i<HASH_SIZE_U32; i++)
        {
            if (LZ4_dict->hashTable[i] < delta) LZ4_dict->hashTable[i]=0;
            else LZ4_dict->hashTable[i] -= delta;
        }
        LZ4_dict->currentOffset = 64 KB;
        if (LZ4_dict->dictSize > 64 KB) LZ4_dict->dictSize = 64 KB;
        LZ4_dict->dictionary = dictEnd - LZ4_dict->dictSize;
    }
}


FORCE_INLINE int LZ4_compress_continue_generic (void* LZ4_stream, const char* source, char* dest, int inputSize,
                                                int maxOutputSize, limitedOutput_directive limit)
{
    LZ4_stream_t_internal* streamPtr = (LZ4_stream_t_internal*)LZ4_stream;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = (const BYTE*) source;
    if (streamPtr->initCheck) return 0;   /* Uninitialized structure detected */
    if ((streamPtr->dictSize>0) && (smallest>dictEnd)) smallest = dictEnd;
    LZ4_renormDictT(streamPtr, smallest);

    /* Check overlapping input/dictionary space */
    {
        const BYTE* sourceEnd = (const BYTE*) source + inputSize;
        if ((sourceEnd > streamPtr->dictionary) && (sourceEnd < dictEnd))
        {
            streamPtr->dictSize = (U32)(dictEnd - sourceEnd);
            if (streamPtr->dictSize > 64 KB) streamPtr->dictSize = 64 KB;
            if (streamPtr->dictSize < 4) streamPtr->dictSize = 0;
            streamPtr->dictionary = dictEnd - streamPtr->dictSize;
        }
    }

    /* prefix mode : source data follows dictionary */
    if (dictEnd == (const BYTE*)source)
    {
        int result;
        if ((streamPtr->dictSize < 64 KB) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ4_compress_generic(LZ4_stream, source, dest, inputSize, maxOutputSize, limit, byU32, withPrefix64k, dictSmall);
        else
            result = LZ4_compress_generic(LZ4_stream, source, dest, inputSize, maxOutputSize, limit, byU32, withPrefix64k, noDictIssue);
        streamPtr->dictSize += (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }

    /* external dictionary mode */
    {
        int result;
        if ((streamPtr->dictSize < 64 KB) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ4_compress_generic(LZ4_stream, source, dest, inputSize, maxOutputSize, limit, byU32, usingExtDict, dictSmall);
        else
            result = LZ4_compress_generic(LZ4_stream, source, dest, inputSize, maxOutputSize, limit, byU32, usingExtDict, noDictIssue);
        streamPtr->dictionary = (const BYTE*)source;
        streamPtr->dictSize = (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }
}


int LZ4_compress_continue (void* LZ4_stream, const char* source, char* dest, int inputSize)
{
    return LZ4_compress_continue_generic(LZ4_stream, source, dest, inputSize, 0, notLimited);
}

int LZ4_compress_limitedOutput_continue (void* LZ4_stream, const char* source, char* dest, int inputSize, int maxOutputSize)
{
    return LZ4_compress_continue_generic(LZ4_stream, source, dest, inputSize, maxOutputSize, limitedOutput);
}


// Hidden debug function, to force separate dictionary mode
int LZ4_compress_forceExtDict (LZ4_stream_t* LZ4_dict, const char* source, char* dest, int inputSize)
{
    LZ4_stream_t_internal* streamPtr = (LZ4_stream_t_internal*)LZ4_dict;
    int result;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = dictEnd;
    if (smallest > (const BYTE*) source) smallest = (const BYTE*) source;
    LZ4_renormDictT((LZ4_stream_t_internal*)LZ4_dict, smallest);

    result = LZ4_compress_generic(LZ4_dict, source, dest, inputSize, 0, notLimited, byU32, usingExtDict, noDictIssue);

    streamPtr->dictionary = (const BYTE*)source;
    streamPtr->dictSize = (U32)inputSize;
    streamPtr->currentOffset += (U32)inputSize;

    return result;
}


int LZ4_saveDict (void* LZ4_dict, char* safeBuffer, int dictSize)
{
    LZ4_stream_t_internal* dict = (LZ4_stream_t_internal*) LZ4_dict;
    const BYTE* previousDictEnd = dict->dictionary + dict->dictSize;

    if ((U32)dictSize > 64 KB) dictSize = 64 KB;   /* useless to define a dictionary > 64 KB */
    if ((U32)dictSize > dict->dictSize) dictSize = dict->dictSize;

    memcpy(safeBuffer, previousDictEnd - dictSize, dictSize);

    dict->dictionary = (const BYTE*)safeBuffer;
    dict->dictSize = (U32)dictSize;

    return 1;
}



/****************************
   Decompression functions
****************************/
/*
 * This generic decompression function cover all use cases.
 * It shall be instanciated several times, using different sets of directives
 * Note that it is essential this generic function is really inlined,
 * in order to remove useless branches during compilation optimisation.
 */
FORCE_INLINE int LZ4_decompress_generic(
                 const char* source,
                 char* dest,
                 int inputSize,
                 int outputSize,         /* If endOnInput==endOnInputSize, this value is the max size of Output Buffer. */

                 int endOnInput,         /* endOnOutputSize, endOnInputSize */
                 int partialDecoding,    /* full, partial */
                 int targetOutputSize,   /* only used if partialDecoding==partial */
                 int dict,               /* noDict, withPrefix64k, usingExtDict */
                 const char* dictStart,  /* only if dict==usingExtDict */
                 int dictSize            /* note : = 0 if noDict */
                 )
{
    /* Local Variables */
    const BYTE* restrict ip = (const BYTE*) source;
    const BYTE* ref;
    const BYTE* const iend = ip + inputSize;

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + outputSize;
    BYTE* cpy;
    BYTE* oexit = op + targetOutputSize;
    const BYTE* const lowLimit = (const BYTE*)dest - dictSize;

    const BYTE* const dictEnd = (const BYTE*)dictStart + dictSize;
//#define OLD
#ifdef OLD
    const size_t dec32table[] = {0, 3, 2, 3, 0, 0, 0, 0};   /* static reduces speed for LZ4_decompress_safe() on GCC64 */
#else
    const size_t dec32table[] = {4-0, 4-3, 4-2, 4-3, 4-0, 4-0, 4-0, 4-0};   /* static reduces speed for LZ4_decompress_safe() on GCC64 */
#endif
    static const size_t dec64table[] = {0, 0, 0, (size_t)-1, 0, 1, 2, 3};

    const int checkOffset = (endOnInput) && (dictSize < (int)(64 KB));


    /* Special cases */
    if ((partialDecoding) && (oexit> oend-MFLIMIT)) oexit = oend-MFLIMIT;                        /* targetOutputSize too high => decode everything */
    if ((endOnInput) && (unlikely(outputSize==0))) return ((inputSize==1) && (*ip==0)) ? 0 : -1;   /* Empty output buffer */
    if ((!endOnInput) && (unlikely(outputSize==0))) return (*ip==0?1:-1);


    /* Main Loop */
    while (1)
    {
        unsigned token;
        size_t length;

        /* get runlength */
        token = *ip++;
        if ((length=(token>>ML_BITS)) == RUN_MASK)
        {
            unsigned s;
            do
            {
                s = *ip++;
                length += s;
            }
            while (likely((endOnInput)?ip<iend-RUN_MASK:1) && (s==255));
            //if ((sizeof(void*)==4) && unlikely(length>LZ4_MAX_INPUT_SIZE)) goto _output_error;   /* overflow detection */
            if ((sizeof(void*)==4) && unlikely((size_t)(op+length)<(size_t)(op))) goto _output_error;   /* quickfix issue 134 */
            if ((endOnInput) && (sizeof(void*)==4) && unlikely((size_t)(ip+length)<(size_t)(ip))) goto _output_error;   /* quickfix issue 134 */
        }

        /* copy literals */
        cpy = op+length;
        if (((endOnInput) && ((cpy>(partialDecoding?oexit:oend-MFLIMIT)) || (ip+length>iend-(2+1+LASTLITERALS))) )
            || ((!endOnInput) && (cpy>oend-COPYLENGTH)))
        {
            if (partialDecoding)
            {
                if (cpy > oend) goto _output_error;                           /* Error : write attempt beyond end of output buffer */
                if ((endOnInput) && (ip+length > iend)) goto _output_error;   /* Error : read attempt beyond end of input buffer */
            }
            else
            {
                if ((!endOnInput) && (cpy != oend)) goto _output_error;       /* Error : block decoding must stop exactly there */
                if ((endOnInput) && ((ip+length != iend) || (cpy > oend))) goto _output_error;   /* Error : input must be consumed */
            }
            memcpy(op, ip, length);
            ip += length;
            op += length;
            break;                                       /* Necessarily EOF, due to parsing restrictions */
        }
        LZ4_WILDCOPY(op, ip, cpy); ip -= (op-cpy); op = cpy;

        /* get offset */
        LZ4_READ_LITTLEENDIAN_16(ref,cpy,ip); ip+=2;
        if ((checkOffset) && (unlikely(ref < lowLimit))) goto _output_error;   /* Error : offset outside destination buffer */

        /* get matchlength */
        if ((length=(token&ML_MASK)) == ML_MASK)
        {
            unsigned s;
            do
            {
                if ((endOnInput) && (ip > iend-LASTLITERALS)) goto _output_error;
                s = *ip++;
                length += s;
            } while (s==255);
            //if ((sizeof(void*)==4) && unlikely(length>LZ4_MAX_INPUT_SIZE)) goto _output_error;   /* overflow detection */
            if ((sizeof(void*)==4) && unlikely((size_t)(op+length)<(size_t)op)) goto _output_error;   /* quickfix issue 134 */
        }

        /* check external dictionary */
        if ((dict==usingExtDict) && (ref < (BYTE* const)dest))
        {
            if (unlikely(op+length+MINMATCH > oend-LASTLITERALS)) goto _output_error;

            if (length+MINMATCH <= (size_t)(dest-(char*)ref))
            {
                ref = dictEnd - (dest-(char*)ref);
                memcpy(op, ref, length+MINMATCH);
                op += length+MINMATCH;
            }
            else
            {
                size_t copySize = (size_t)(dest-(char*)ref);
                memcpy(op, dictEnd - copySize, copySize);
                op += copySize;
                copySize = length+MINMATCH - copySize;
                if (copySize > (size_t)((char*)op-dest))   /* overlap */
                {
                    BYTE* const cpy = op + copySize;
                    const BYTE* ref = (BYTE*)dest;
                    while (op < cpy) *op++ = *ref++;
                }
                else
                {
                    memcpy(op, dest, copySize);
                    op += copySize;
                }
            }
            continue;
        }

        /* copy repeated sequence */
        if (unlikely((op-ref)<(int)STEPSIZE))
        {
            const size_t dec64 = dec64table[(sizeof(void*)==4) ? 0 : op-ref];
            op[0] = ref[0];
            op[1] = ref[1];
            op[2] = ref[2];
            op[3] = ref[3];
#ifdef OLD
            op += 4, ref += 4; ref -= dec32table[op-ref];
            A32(op) = A32(ref);
            op += STEPSIZE-4; ref -= dec64;
#else
            ref += dec32table[op-ref];
            A32(op+4) = A32(ref);
            op += STEPSIZE; ref -= dec64;
#endif
        } else { LZ4_COPYSTEP(op,ref); }
        cpy = op + length - (STEPSIZE-4);

        if (unlikely(cpy>oend-COPYLENGTH-(STEPSIZE-4)))
        {
            if (cpy > oend-LASTLITERALS) goto _output_error;    /* Error : last 5 bytes must be literals */
            if (op<oend-COPYLENGTH) LZ4_WILDCOPY(op, ref, (oend-COPYLENGTH));
            while(op<cpy) *op++=*ref++;
            op=cpy;
            continue;
        }
        LZ4_WILDCOPY(op, ref, cpy);
        op=cpy;   /* correction */
    }

    /* end of decoding */
    if (endOnInput)
       return (int) (((char*)op)-dest);     /* Nb of output bytes decoded */
    else
       return (int) (((char*)ip)-source);   /* Nb of input bytes read */

    /* Overflow error detected */
_output_error:
    return (int) (-(((char*)ip)-source))-1;
}


int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    return LZ4_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, noDict, NULL, 0);
}

int LZ4_decompress_safe_partial(const char* source, char* dest, int compressedSize, int targetOutputSize, int maxOutputSize)
{
    return LZ4_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, partial, targetOutputSize, noDict, NULL, 0);
}

int LZ4_decompress_fast(const char* source, char* dest, int originalSize)
{
    return LZ4_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, withPrefix64k, NULL, 0);
}

/* streaming decompression functions */

//#define LZ4_STREAMDECODESIZE_U32 4
//#define LZ4_STREAMDECODESIZE     (LZ4_STREAMDECODESIZE_U32 * sizeof(unsigned int))
//typedef struct { unsigned int table[LZ4_STREAMDECODESIZE_U32]; } LZ4_streamDecode_t;
typedef struct
{
    const char* dictionary;
    int dictSize;
} LZ4_streamDecode_t_internal;

/*
 * If you prefer dynamic allocation methods,
 * LZ4_createStreamDecode()
 * provides a pointer (void*) towards an initialized LZ4_streamDecode_t structure.
 */
void* LZ4_createStreamDecode()
{
    void* lz4s = ALLOCATOR(sizeof(U32), LZ4_STREAMDECODESIZE_U32);
    MEM_INIT(lz4s, 0, LZ4_STREAMDECODESIZE);
    return lz4s;
}

/*
 * LZ4_setDictDecode
 * Use this function to instruct where to find the dictionary
 * This function is not necessary if previous data is still available where it was decoded.
 * Loading a size of 0 is allowed (same effect as no dictionary).
 * Return : 1 if OK, 0 if error
 */
int LZ4_setDictDecode (void* LZ4_streamDecode, const char* dictionary, int dictSize)
{
    LZ4_streamDecode_t_internal* lz4sd = (LZ4_streamDecode_t_internal*) LZ4_streamDecode;
    lz4sd->dictionary = dictionary;
    lz4sd->dictSize = dictSize;
    return 1;
}

/*
*_continue() :
    These decoding functions allow decompression of multiple blocks in "streaming" mode.
    Previously decoded blocks must still be available at the memory position where they were decoded.
    If it's not possible, save the relevant part of decoded data into a safe buffer,
    and indicate where it stands using LZ4_setDictDecode()
*/
int LZ4_decompress_safe_continue (void* LZ4_streamDecode, const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    LZ4_streamDecode_t_internal* lz4sd = (LZ4_streamDecode_t_internal*) LZ4_streamDecode;
    int result;

    result = LZ4_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, usingExtDict, lz4sd->dictionary, lz4sd->dictSize);
    if (result <= 0) return result;
    if (lz4sd->dictionary + lz4sd->dictSize == dest)
    {
        lz4sd->dictSize += result;
    }
    else
    {
        lz4sd->dictionary = dest;
        lz4sd->dictSize = result;
    }

    return result;
}

int LZ4_decompress_fast_continue (void* LZ4_streamDecode, const char* source, char* dest, int originalSize)
{
    LZ4_streamDecode_t_internal* lz4sd = (LZ4_streamDecode_t_internal*) LZ4_streamDecode;
    int result;

    result = LZ4_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, usingExtDict, lz4sd->dictionary, lz4sd->dictSize);
    if (result <= 0) return result;
    if (lz4sd->dictionary + lz4sd->dictSize == dest)
    {
        lz4sd->dictSize += result;
    }
    else
    {
        lz4sd->dictionary = dest;
        lz4sd->dictSize = result;
    }

    return result;
}


/*
Advanced decoding functions :
*_usingDict() :
    These decoding functions work the same as "_continue" ones,
    the dictionary must be explicitly provided within parameters
*/

int LZ4_decompress_safe_usingDict(const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize)
{
    return LZ4_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, usingExtDict, dictStart, dictSize);
}

int LZ4_decompress_fast_usingDict(const char* source, char* dest, int originalSize, const char* dictStart, int dictSize)
{
    return LZ4_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, usingExtDict, dictStart, dictSize);
}
