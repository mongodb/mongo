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

/* This is the build config file.
 *
 * With this you can setup what to inlcude/exclude automatically during any build.  Just comment
 * out the line that #define's the word for the thing you want to remove.  phew!
 */

#ifndef TOMCRYPT_CFG_H
#define TOMCRYPT_CFG_H

#if defined(_WIN32) || defined(_MSC_VER)
#define LTC_CALL __cdecl
#else
#ifndef LTC_CALL
#define LTC_CALL
#endif
#endif

#ifndef LTC_EXPORT
#define LTC_EXPORT
#endif

/* certain platforms use macros for these, making the prototypes broken */
#ifndef LTC_NO_PROTOTYPES

/* you can change how memory allocation works ... */
LTC_EXPORT void* LTC_CALL XMALLOC(size_t n);
LTC_EXPORT void* LTC_CALL XREALLOC(void* p, size_t n);
LTC_EXPORT void* LTC_CALL XCALLOC(size_t n, size_t s);
LTC_EXPORT void LTC_CALL XFREE(void* p);

LTC_EXPORT void LTC_CALL XQSORT(void* base,
                                size_t nmemb,
                                size_t size,
                                int (*compar)(const void*, const void*));


/* change the clock function too */
LTC_EXPORT clock_t LTC_CALL XCLOCK(void);

/* various other functions */
LTC_EXPORT void* LTC_CALL XMEMCPY(void* dest, const void* src, size_t n);
LTC_EXPORT int LTC_CALL XMEMCMP(const void* s1, const void* s2, size_t n);
LTC_EXPORT void* LTC_CALL XMEMSET(void* s, int c, size_t n);

LTC_EXPORT int LTC_CALL XSTRCMP(const char* s1, const char* s2);

#endif

/* type of argument checking, 0=default, 1=fatal and 2=error+continue, 3=nothing */
#ifndef ARGTYPE
#define ARGTYPE 0
#endif

/* Controls endianess and size of registers.  Leave uncommented to get platform neutral [slower]
 * code
 *
 * Note: in order to use the optimized macros your platform must support unaligned 32 and 64 bit
 * read/writes. The x86 platforms allow this but some others [ARM for instance] do not.  On those
 * platforms you **MUST** use the portable [slower] macros.
 */

/* detect x86-32 machines somewhat */
#if !defined(__STRICT_ANSI__) &&                                   \
    (defined(INTEL_CC) || (defined(_MSC_VER) && defined(WIN32)) || \
     (defined(__GNUC__) &&                                         \
      (defined(__DJGPP__) || defined(__CYGWIN__) || defined(__MINGW32__) || defined(__i386__))))
#define ENDIAN_LITTLE
#define ENDIAN_32BITWORD
#define LTC_FAST
#define LTC_FAST_TYPE unsigned long
#endif

/* detects MIPS R5900 processors (PS2) */
#if (defined(__R5900) || defined(R5900) || defined(__R5900__)) && \
    (defined(_mips) || defined(__mips__) || defined(mips))
#define ENDIAN_LITTLE
#define ENDIAN_64BITWORD
#endif

/* detect amd64 */
#if !defined(__STRICT_ANSI__) && defined(__x86_64__)
#define ENDIAN_LITTLE
#define ENDIAN_64BITWORD
#define LTC_FAST
#define LTC_FAST_TYPE unsigned long
#endif

/* detect PPC32 */
#if !defined(__STRICT_ANSI__) && defined(LTC_PPC32)
#define ENDIAN_BIG
#define ENDIAN_32BITWORD
#define LTC_FAST
#define LTC_FAST_TYPE unsigned long
#endif

/* detect sparc and sparc64 */
#if defined(__sparc__)
#define ENDIAN_BIG
#if defined(__arch64__)
#define ENDIAN_64BITWORD
#else
#define ENDIAN_32BITWORD
#endif
#endif


#ifdef LTC_NO_FAST
#ifdef LTC_FAST
#undef LTC_FAST
#endif
#endif

/* No asm is a quick way to disable anything "not portable" */
#ifdef LTC_NO_ASM
#undef ENDIAN_LITTLE
#undef ENDIAN_BIG
#undef ENDIAN_32BITWORD
#undef ENDIAN_64BITWORD
#undef LTC_FAST
#undef LTC_FAST_TYPE
#define LTC_NO_ROLC
#define LTC_NO_BSWAP
#endif

/* #define ENDIAN_LITTLE */
/* #define ENDIAN_BIG */

/* #define ENDIAN_32BITWORD */
/* #define ENDIAN_64BITWORD */

#if (defined(ENDIAN_BIG) || defined(ENDIAN_LITTLE)) && \
    !(defined(ENDIAN_32BITWORD) || defined(ENDIAN_64BITWORD))
#error You must specify a word size as well as endianess in tomcrypt_cfg.h
#endif

#if !(defined(ENDIAN_BIG) || defined(ENDIAN_LITTLE))
#define ENDIAN_NEUTRAL
#endif

#endif


/* $Source: /cvs/libtom/libtomcrypt/src/headers/tomcrypt_cfg.h,v $ */
/* $Revision: 1.19 $ */
/* $Date: 2006/12/04 02:19:48 $ */
