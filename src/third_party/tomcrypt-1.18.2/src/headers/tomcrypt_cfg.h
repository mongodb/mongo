/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
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
#elif !defined(LTC_CALL)
   #define LTC_CALL
#endif

#ifndef LTC_EXPORT
   #define LTC_EXPORT
#endif

/* certain platforms use macros for these, making the prototypes broken */
#ifndef LTC_NO_PROTOTYPES

/* you can change how memory allocation works ... */
LTC_EXPORT void * LTC_CALL XMALLOC(size_t n);
LTC_EXPORT void * LTC_CALL XREALLOC(void *p, size_t n);
LTC_EXPORT void * LTC_CALL XCALLOC(size_t n, size_t s);
LTC_EXPORT void LTC_CALL XFREE(void *p);

LTC_EXPORT void LTC_CALL XQSORT(void *base, size_t nmemb, size_t size, int(*compar)(const void *, const void *));


/* change the clock function too */
LTC_EXPORT clock_t LTC_CALL XCLOCK(void);

/* various other functions */
LTC_EXPORT void * LTC_CALL XMEMCPY(void *dest, const void *src, size_t n);
LTC_EXPORT int   LTC_CALL XMEMCMP(const void *s1, const void *s2, size_t n);
LTC_EXPORT void * LTC_CALL XMEMSET(void *s, int c, size_t n);

LTC_EXPORT int   LTC_CALL XSTRCMP(const char *s1, const char *s2);

#endif

/* some compilers do not like "inline" (or maybe "static inline"), namely: HP cc, IBM xlc */
#if defined(__HP_cc) || defined(__xlc__)
   #define LTC_INLINE
#elif defined(_MSC_VER)
   #define LTC_INLINE __inline
#else
   #define LTC_INLINE inline
#endif

/* type of argument checking, 0=default, 1=fatal and 2=error+continue, 3=nothing */
#ifndef ARGTYPE
   #define ARGTYPE  0
#endif

#undef LTC_ENCRYPT
#define LTC_ENCRYPT 0
#undef LTC_DECRYPT
#define LTC_DECRYPT 1

/* Controls endianess and size of registers.  Leave uncommented to get platform neutral [slower] code
 *
 * Note: in order to use the optimized macros your platform must support unaligned 32 and 64 bit read/writes.
 * The x86 platforms allow this but some others [ARM for instance] do not.  On those platforms you **MUST**
 * use the portable [slower] macros.
 */
/* detect x86/i386 32bit */
#if defined(__i386__) || defined(__i386) || defined(_M_IX86)
   #define ENDIAN_LITTLE
   #define ENDIAN_32BITWORD
   #define LTC_FAST
#endif

/* detect amd64/x64 */
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
   #define ENDIAN_LITTLE
   #define ENDIAN_64BITWORD
   #define LTC_FAST
#endif

/* detect PPC32 */
#if defined(LTC_PPC32)
   #define ENDIAN_BIG
   #define ENDIAN_32BITWORD
   #define LTC_FAST
#endif

/* detects MIPS R5900 processors (PS2) */
#if (defined(__R5900) || defined(R5900) || defined(__R5900__)) && (defined(_mips) || defined(__mips__) || defined(mips))
   #define ENDIAN_64BITWORD
   #if defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__)
     #define ENDIAN_BIG
   #endif
     #define ENDIAN_LITTLE
   #endif
#endif

/* detect AIX */
#if defined(_AIX) && defined(_BIG_ENDIAN)
  #define ENDIAN_BIG
  #if defined(__LP64__) || defined(_ARCH_PPC64)
    #define ENDIAN_64BITWORD
  #else
    #define ENDIAN_32BITWORD
  #endif
#endif

/* detect HP-UX */
#if defined(__hpux) || defined(__hpux__)
  #define ENDIAN_BIG
  #if defined(__ia64) || defined(__ia64__) || defined(__LP64__)
    #define ENDIAN_64BITWORD
  #else
    #define ENDIAN_32BITWORD
  #endif
#endif

/* detect Apple OS X */
#if defined(__APPLE__) && defined(__MACH__)
  #if defined(__LITTLE_ENDIAN__) || defined(__x86_64__)
    #define ENDIAN_LITTLE
  #else
    #define ENDIAN_BIG
  #endif
  #if defined(__LP64__) || defined(__x86_64__)
    #define ENDIAN_64BITWORD
  #else
    #define ENDIAN_32BITWORD
  #endif
#endif

/* detect SPARC and SPARC64 */
#if defined(__sparc__) || defined(__sparc)
  #define ENDIAN_BIG
  #if defined(__arch64__) || defined(__sparcv9) || defined(__sparc_v9__)
    #define ENDIAN_64BITWORD
  #else
    #define ENDIAN_32BITWORD
  #endif
#endif

/* detect IBM S390(x) */
#if defined(__s390x__) || defined(__s390__)
  #define ENDIAN_BIG
  #if defined(__s390x__)
    #define ENDIAN_64BITWORD
  #else
    #define ENDIAN_32BITWORD
  #endif
#endif

/* detect PPC64 */
#if defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__)
   #define ENDIAN_64BITWORD
   #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      #define ENDIAN_BIG
   #elif  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      #define ENDIAN_LITTLE
   #endif
   #define LTC_FAST
#endif

/* endianness fallback */
#if !defined(ENDIAN_BIG) && !defined(ENDIAN_LITTLE)
  #if defined(_BYTE_ORDER) && _BYTE_ORDER == _BIG_ENDIAN || \
      defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN || \
      defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ || \
      defined(__BIG_ENDIAN__) || \
      defined(__ARMEB__) || defined(__THUMBEB__) || defined(__AARCH64EB__) || \
      defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__)
    #define ENDIAN_BIG
  #elif defined(_BYTE_ORDER) && _BYTE_ORDER == _LITTLE_ENDIAN || \
      defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN || \
      defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || \
      defined(__LITTLE_ENDIAN__) || \
      defined(__ARMEL__) || defined(__THUMBEL__) || defined(__AARCH64EL__) || \
      defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__)
    #define ENDIAN_LITTLE
  #else
    #error Cannot detect endianness
  #endif
#endif

/* ulong64: 64-bit data type */
#ifdef _MSC_VER
   #define CONST64(n) n ## ui64
   typedef unsigned __int64 ulong64;
#else
   #define CONST64(n) n ## ULL
   typedef unsigned long long ulong64;
#endif

/* ulong32: "32-bit at least" data type */
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__) || \
    defined(__s390x__) || defined(__arch64__) || defined(__aarch64__) || \
    defined(__sparcv9) || defined(__sparc_v9__) || defined(__sparc64__) || \
    defined(__ia64) || defined(__ia64__) || defined(__itanium__) || defined(_M_IA64) || \
    defined(__LP64__) || defined(_LP64) || defined(__64BIT__)
   typedef unsigned ulong32;
   #if !defined(ENDIAN_64BITWORD) && !defined(ENDIAN_32BITWORD)
     #define ENDIAN_64BITWORD
   #endif
#else
   typedef unsigned long ulong32;
   #if !defined(ENDIAN_64BITWORD) && !defined(ENDIAN_32BITWORD)
     #define ENDIAN_32BITWORD
   #endif
#endif

#if defined(ENDIAN_64BITWORD) && !defined(_MSC_VER)
typedef unsigned long long ltc_mp_digit;
#else
typedef unsigned long ltc_mp_digit;
#endif

/* No asm is a quick way to disable anything "not portable" */
#ifdef LTC_NO_ASM
   #define ENDIAN_NEUTRAL
   #undef ENDIAN_32BITWORD
   #undef ENDIAN_64BITWORD
   #undef LTC_FAST
   #define LTC_NO_ROLC
   #define LTC_NO_BSWAP
#endif

/* No LTC_FAST if: explicitly disabled OR non-gcc/non-clang compiler OR old gcc OR using -ansi -std=c99 */
#if defined(LTC_NO_FAST) || (__GNUC__ < 4) || defined(__STRICT_ANSI__)
   #undef LTC_FAST
#endif

#ifdef LTC_FAST
   #define LTC_FAST_TYPE_PTR_CAST(x) ((LTC_FAST_TYPE*)(void*)(x))
   #ifdef ENDIAN_64BITWORD
   typedef ulong64 __attribute__((__may_alias__)) LTC_FAST_TYPE;
   #else
   typedef ulong32 __attribute__((__may_alias__)) LTC_FAST_TYPE;
   #endif
#endif

#if !defined(ENDIAN_NEUTRAL) && (defined(ENDIAN_BIG) || defined(ENDIAN_LITTLE)) && !(defined(ENDIAN_32BITWORD) || defined(ENDIAN_64BITWORD))
   #error You must specify a word size as well as endianess in tomcrypt_cfg.h
#endif

#if !(defined(ENDIAN_BIG) || defined(ENDIAN_LITTLE))
   #define ENDIAN_NEUTRAL
#endif

#if (defined(ENDIAN_32BITWORD) && defined(ENDIAN_64BITWORD))
   #error Cannot be 32 and 64 bit words...
#endif

/* gcc 4.3 and up has a bswap builtin; detect it by gcc version.
 * clang also supports the bswap builtin, and although clang pretends
 * to be gcc (macro-wise, anyway), clang pretends to be a version
 * prior to gcc 4.3, so we can't detect bswap that way.  Instead,
 * clang has a __has_builtin mechanism that can be used to check
 * for builtins:
 * http://clang.llvm.org/docs/LanguageExtensions.html#feature_check */
#ifndef __has_builtin
   #define __has_builtin(x) 0
#endif
#if !defined(LTC_NO_BSWAP) && defined(__GNUC__) &&                      \
   ((__GNUC__ * 100 + __GNUC_MINOR__ >= 403) ||                         \
    (__has_builtin(__builtin_bswap32) && __has_builtin(__builtin_bswap64)))
   #define LTC_HAVE_BSWAP_BUILTIN
#endif


/* ref:         HEAD -> master, tag: v1.18.2 */
/* git commit:  7e7eb695d581782f04b24dc444cbfde86af59853 */
/* commit time: 2018-07-01 22:49:01 +0200 */
