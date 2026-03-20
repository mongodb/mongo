
/* Enable RDRAND Hardware RNG Hash Seed */
/* #undef ENABLE_RDRAND */

/* Override json_c_get_random_seed() with custom code */
/* #undef OVERRIDE_GET_RANDOM_SEED */

/* Enable partial threading support */
/* #undef ENABLE_THREADING */

/* Define if .gnu.warning accepts long strings. */
/* #undef HAS_GNU_WARNING_LONG */

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H

/* Define to 1 if you have the <endian.h> header file. */
#define HAVE_ENDIAN_H

/* Define to 1 if you have the <fcntl.h> header file. */
#define HAVE_FCNTL_H

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H

/* Define to 1 if you have the <limits.h> header file. */
#define HAVE_LIMITS_H

/* Define to 1 if you have the <locale.h> header file. */
#define HAVE_LOCALE_H

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H

/* Define to 1 if you have the <stdarg.h> header file. */
#define HAVE_STDARG_H

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H

/* Define to 1 if you have the <syslog.h> header file. */
#define HAVE_SYSLOG_H 1

/* Define to 1 if you have the <sys/cdefs.h> header file. */
#define HAVE_SYS_CDEFS_H

/* Define to 1 if you have the <sys/param.h> header file. */
#define HAVE_SYS_PARAM_H 1

/* Define to 1 if you have the <sys/random.h> header file. */
#define HAVE_SYS_RANDOM_H

/* Define to 1 if you have the <sys/resource.h> header file. */
#define HAVE_SYS_RESOURCE_H

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the <xlocale.h> header file. */
/* #undef HAVE_XLOCALE_H */

/* Define to 1 if you have the <bsd/stdlib.h> header file. */
/* #undef HAVE_BSD_STDLIB_H */

/* Define to 1 if you have `arc4random' */
/* #undef HAVE_ARC4RANDOM */

/* Define to 1 if you don't have `vprintf' but do have `_doprnt.' */
/* #undef HAVE_DOPRNT */

/* Has atomic builtins */
#define HAVE_ATOMIC_BUILTINS

/* Define to 1 if you have the declaration of `INFINITY', and to 0 if you
   don't. */
#define HAVE_DECL_INFINITY

/* Define to 1 if you have the declaration of `isinf', and to 0 if you don't.
   */
#define HAVE_DECL_ISINF

/* Define to 1 if you have the declaration of `isnan', and to 0 if you don't.
   */
#define HAVE_DECL_ISNAN

/* Define to 1 if you have the declaration of `nan', and to 0 if you don't. */
#define HAVE_DECL_NAN

/* Define to 1 if you have the declaration of `_finite', and to 0 if you
   don't. */
/* #undef HAVE_DECL__FINITE */

/* Define to 1 if you have the declaration of `_isnan', and to 0 if you don't.
   */
/* #undef HAVE_DECL__ISNAN */

/* Define to 1 if you have the `open' function. */
#define HAVE_OPEN

/* Define to 1 if you have the `realloc' function. */
#define HAVE_REALLOC

/* Define to 1 if you have the `setlocale' function. */
#define HAVE_SETLOCALE

/* Define to 1 if you have the `snprintf' function. */
#define HAVE_SNPRINTF


/* Define to 1 if you have the `strcasecmp' function. */
#define HAVE_STRCASECMP 1

/* Define to 1 if you have the `strdup' function. */
#define HAVE_STRDUP

/* Define to 1 if you have the `strerror' function. */
#define HAVE_STRERROR

/* Define to 1 if you have the `strncasecmp' function. */
#define HAVE_STRNCASECMP 1

/* Define to 1 if you have the `uselocale' function. */
#define HAVE_USELOCALE

/* Define to 1 if newlocale() needs freelocale() called on it's `base` argument */
/* #undef NEWLOCALE_NEEDS_FREELOCALE */

/* Define to 1 if you have the `vasprintf' function. */
#define HAVE_VASPRINTF

/* Define to 1 if you have the `vprintf' function. */
#define HAVE_VPRINTF

/* Define to 1 if you have the `vsnprintf' function. */
#define HAVE_VSNPRINTF

/* Define to 1 if you have the `vsyslog' function. */
#define HAVE_VSYSLOG 1

/* Define if you have the `getrandom' function. */
#define HAVE_GETRANDOM

/* Define if you have the `getrusage' function. */
#define HAVE_GETRUSAGE

#define HAVE_STRTOLL
#if !defined(HAVE_STRTOLL)
#define strtoll strtoll
/* #define json_c_strtoll strtoll*/
#endif

#define HAVE_STRTOULL
#if !defined(HAVE_STRTOULL)
#define strtoull strtoull
/* #define json_c_strtoull strtoull */
#endif

/* Have __thread */
#define HAVE___THREAD

/* Public define for json_inttypes.h */
#define JSON_C_HAVE_INTTYPES_H 1

/* Name of package */
#define PACKAGE "json-c"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "json-c@googlegroups.com"

/* Define to the full name of this package. */
#define PACKAGE_NAME "json-c"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "json-c 0.17."

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "json-c"

/* Define to the home page for this package. */
#define PACKAGE_URL "https://github.com/json-c/json-c"

/* Define to the version of this package. */
#define PACKAGE_VERSION "0.17."

/* The number of bytes in type int */
#define SIZEOF_INT 4

/* The number of bytes in type int64_t */
#define SIZEOF_INT64_T 8

/* The number of bytes in type long */
#define SIZEOF_LONG 8

/* The number of bytes in type long long */
#define SIZEOF_LONG_LONG 8

/* The number of bytes in type size_t */
#define SIZEOF_SIZE_T 8

/* The number of bytes in type ssize_t */
#define SIZEOF_SSIZE_T 8

/* Specifier for __thread */
#define SPEC___THREAD __thread

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS

/* Version number of package */
#define VERSION "0.17."

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */
