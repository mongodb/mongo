#ifndef PRIVATE_H

#define PRIVATE_H

/*
** This file is in the public domain, so clarified as of
** 1996-06-05 by Arthur David Olson.
*/

/*
** This header is for use ONLY with the time conversion code.
** There is no guarantee that it will remain unchanged,
** or that it will remain at all.
** Do NOT copy it to any system include directory.
** Thank you!
*/

#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#endif

#define GRANDPARENTED	"Local time zone must be set--see zic manual page"

/*
** Nested includes
*/

#include "errno.h"
#include "string.h"
#include "limits.h"	/* for CHAR_BIT et al. */
#include "time.h"

/* Unlike <ctype.h>'s isdigit, this also works if c < 0 | c > UCHAR_MAX. */
#define is_digit(c) ((unsigned)(c) - '0' <= 9)

/*
** Define HAVE_STDINT_H's default value here, rather than at the
** start, since __GLIBC__'s value depends on previously-included
** files.
** (glibc 2.1 and later have stdint.h, even with pre-C99 compilers.)
*/
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H \
	(199901 <= __STDC_VERSION__ || \
	2 < (__GLIBC__ + (0 < __GLIBC_MINOR__)))
#endif /* !defined HAVE_STDINT_H */

#if HAVE_STDINT_H
#include "stdint.h"
#endif /* !HAVE_STDINT_H */

#ifndef HAVE_INTTYPES_H
# define HAVE_INTTYPES_H HAVE_STDINT_H
#endif
#if HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#ifndef INT_FAST64_MAX
/* Pre-C99 GCC compilers define __LONG_LONG_MAX__ instead of LLONG_MAX.  */
#if defined LLONG_MAX || defined __LONG_LONG_MAX__
typedef long long	int_fast64_t;
# ifdef LLONG_MAX
#  define INT_FAST64_MIN LLONG_MIN
#  define INT_FAST64_MAX LLONG_MAX
# else
#  define INT_FAST64_MIN __LONG_LONG_MIN__
#  define INT_FAST64_MAX __LONG_LONG_MAX__
# endif
# define SCNdFAST64 "lld"
#else /* ! (defined LLONG_MAX || defined __LONG_LONG_MAX__) */
#if (LONG_MAX >> 31) < 0xffffffff
Please use a compiler that supports a 64-bit integer type (or wider);
you may need to compile with "-DHAVE_STDINT_H".
#endif /* (LONG_MAX >> 31) < 0xffffffff */
typedef long		int_fast64_t;
# define INT_FAST64_MIN LONG_MIN
# define INT_FAST64_MAX LONG_MAX
# define SCNdFAST64 "ld"
#endif /* ! (defined LLONG_MAX || defined __LONG_LONG_MAX__) */
#endif /* !defined INT_FAST64_MAX */

#ifndef INT_FAST32_MAX
# if INT_MAX >> 31 == 0
typedef long int_fast32_t;
# else
typedef int int_fast32_t;
# endif
#endif

#ifndef INTMAX_MAX
# if defined LLONG_MAX || defined __LONG_LONG_MAX__
typedef long long intmax_t;
#  define strtoimax strtoll
#  define PRIdMAX "lld"
#  ifdef LLONG_MAX
#   define INTMAX_MAX LLONG_MAX
#   define INTMAX_MIN LLONG_MIN
#  else
#   define INTMAX_MAX __LONG_LONG_MAX__
#   define INTMAX_MIN __LONG_LONG_MIN__
#  endif
# else
typedef long intmax_t;
#  define strtoimax strtol
#  define PRIdMAX "ld"
#  define INTMAX_MAX LONG_MAX
#  define INTMAX_MIN LONG_MIN
# endif
#endif

#ifndef UINTMAX_MAX
# if defined ULLONG_MAX || defined __LONG_LONG_MAX__
typedef unsigned long long uintmax_t;
#  define PRIuMAX "llu"
# else
typedef unsigned long uintmax_t;
#  define PRIuMAX "lu"
# endif
#endif

#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif /* !defined INT32_MAX */
#ifndef INT32_MIN
#define INT32_MIN (-1 - INT32_MAX)
#endif /* !defined INT32_MIN */

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t) -1)
#endif

#if 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
# define ATTRIBUTE_CONST __attribute__ ((const))
# define ATTRIBUTE_PURE __attribute__ ((__pure__))
# define ATTRIBUTE_FORMAT(spec) __attribute__ ((__format__ spec))
#else
# define ATTRIBUTE_CONST /* empty */
# define ATTRIBUTE_PURE /* empty */
# define ATTRIBUTE_FORMAT(spec) /* empty */
#endif

#if !defined _Noreturn && __STDC_VERSION__ < 201112
# if 2 < __GNUC__ + (8 <= __GNUC_MINOR__)
#  define _Noreturn __attribute__ ((__noreturn__))
# else
#  define _Noreturn
# endif
#endif

#if __STDC_VERSION__ < 199901 && !defined restrict
# define restrict /* empty */
#endif

#ifndef TRUE
#define TRUE	1
#endif /* !defined TRUE */

#ifndef FALSE
#define FALSE	0
#endif /* !defined FALSE */

#ifndef TYPE_BIT
#define TYPE_BIT(type)	(sizeof (type) * CHAR_BIT)
#endif /* !defined TYPE_BIT */

#ifndef TYPE_SIGNED
#define TYPE_SIGNED(type) (((type) -1) < 0)
#endif /* !defined TYPE_SIGNED */

/* The minimum and maximum finite time values.  */
static time_t const time_t_min =
    (TYPE_SIGNED(time_t)
        ? (time_t) -1 << (CHAR_BIT * sizeof (time_t) - 1)
        : 0);
static time_t const time_t_max =
    (TYPE_SIGNED(time_t)
        ? - (~ 0 < 0) - ((time_t) -1 << (CHAR_BIT * sizeof (time_t) - 1))
        : -1);

#endif /* !defined PRIVATE_H */
