/* config.h for CMake builds */

/* #undef HAVE_ATTRIBUTE_UNINITIALIZED */
#define HAVE_DIRENT_H 1
#define HAVE_STRERROR 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WINDOWS_H 1

/* #undef HAVE_BCOPY */
/* #undef HAVE_MEMFD_CREATE */
#define HAVE_MEMMOVE 1
/* #undef HAVE_SECURE_GETENV */
#define HAVE_STRERROR 1

#define SUPPORT_PCRE2_8 1
/* #undef SUPPORT_PCRE2_16 */
/* #undef SUPPORT_PCRE2_32 */
/* #undef PCRE2_DEBUG */
/* #undef DISABLE_PERCENT_ZT */

/* #undef SUPPORT_LIBBZ2 */
/* #undef SUPPORT_LIBEDIT */
/* #undef SUPPORT_LIBREADLINE */
/* #undef SUPPORT_LIBZ */

/* #undef SUPPORT_JIT */
/* #undef SLJIT_PROT_EXECUTABLE_ALLOCATOR */
#define SUPPORT_PCRE2GREP_JIT 1
#define SUPPORT_PCRE2GREP_CALLOUT 1
#define SUPPORT_PCRE2GREP_CALLOUT_FORK 1
#define SUPPORT_UNICODE 1
/* #undef SUPPORT_VALGRIND */

/* #undef BSR_ANYCRLF */
/* #undef EBCDIC */
/* #undef EBCDIC_NL25 */
/* #undef HEAP_MATCH_RECURSE */
/* #undef NEVER_BACKSLASH_C */

#define LINK_SIZE		2
#define HEAP_LIMIT              20000000
#define MATCH_LIMIT		200000
#define MATCH_LIMIT_DEPTH	4000
#define NEWLINE_DEFAULT         2
#define PARENS_NEST_LIMIT       250
#define PCRE2GREP_BUFSIZE       20480
#define PCRE2GREP_MAX_BUFSIZE   1048576

#define MAX_NAME_SIZE	32
#define MAX_NAME_COUNT	10000

/* end config.h for CMake builds */
