/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Define if building universal (internal helper macro) */
//#undef AC_APPLE_UNIVERSAL_BUILD

#if defined(_WIN32)
// signed/unsigned mismatch
#pragma warning( disable : 4018 )
#endif

/* Define to 1 if the compiler supports __builtin_ctz and friends. */
#if defined(__GNUC__)
#definfe HAVE_BUILTIN_CTZ 1
#endif

/* Define to 1 if the compiler supports __builtin_expect. */
#if defined(__GNUC__)
#definfe HAVE_BUILTIN_EXPECT 1
#endif

/* Define to 1 if you have the <dlfcn.h> header file. */
#if !defined(_WIN32)
#define HAVE_DLFCN_H 1
#endif

/* Use the gflags package for command-line parsing. */
#undef HAVE_GFLAGS

/* Defined when Google Test is available. */
#undef HAVE_GTEST

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the `fastlz' library (-lfastlz). */
#undef HAVE_LIBFASTLZ

/* Define to 1 if you have the `lzf' library (-llzf). */
#undef HAVE_LIBLZF

/* Define to 1 if you have the `lzo2' library (-llzo2). */
#undef HAVE_LIBLZO2

/* Define to 1 if you have the `quicklz' library (-lquicklz). */
#undef HAVE_LIBQUICKLZ

/* Define to 1 if you have the `z' library (-lz). */
#undef HAVE_LIBZ

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <stddef.h> header file. */
#define HAVE_STDDEF_H 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/mman.h> header file. */
#if !defined(_WIN32)
#define HAVE_SYS_MMAN_H 1
#endif

/* Define to 1 if you have the <sys/resource.h> header file. */
#define HAVE_SYS_RESOURCE_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the <windows.h> header file. */
#if defined(_WIN32)
#define HAVE_WINDOWS_H 1
#endif

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR "libs/"

/* Name of package */
#define PACKAGE "snappy"

#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "snappy"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "snappy 1.0.3"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "snappy"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.0.3"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "1.0.3"

/* Define WORDS_BIGENDIAN to 1 if your processor stores words with the most
   significant byte first (like Motorola and SPARC, unlike Intel). */
#if defined(__BIG_ENDIAN__)
#define WORDS_BIGENDIAN 1
#endif
