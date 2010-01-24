/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

/* Basic constants. */
#define	WT_BILLION	(1000000000)
#define	WT_FRAGMENT	(512)
#define	WT_MEGABYTE	(1048576)
#define	WT_MILLION	(1000000)

/* Align a number to a specified power-of-2. */
#define	WT_ALIGN(n, v)							\
	(((n) + ((v) - 1)) & ~(((uintmax_t)(v)) - 1))

/* Min, max. */
#define	WT_MIN(a, b)	((a) < (b) ? (a) : (b))
#define	WT_MAX(a, b)	((a) < (b) ? (b) : (a))

/*
 * Convert a pointer to an unsigned long so we can print it without compiler
 * complaint.
 */
#define	WT_ADDR_TO_ULONG(addr)	((u_long)(uintptr_t)(addr))

/*
 * Flag checking for API functions.
 */
#define	WT_DB_FCHK(db, name, f, mask)					\
	if ((f) & ~(mask))						\
		return (__wt_api_args((db)->env, name));
#define	WT_DB_FCHK_NOTFATAL(db, name, f, mask, ret)			\
	if ((f) & ~(mask))						\
		(ret) = __wt_api_args((db)->env, name);
#define	WT_ENV_FCHK(env, name, f, mask)					\
	if ((f) & ~(mask))						\
		return (__wt_api_args(env, name));
#define	WT_ENV_FCHK_NOTFATAL(env, name, f, mask, ret)			\
	if ((f) & ~(mask))						\
		(ret) = __wt_api_args(env, name);

/*
 * Flag set, clear and test.  They come in 3 flavors: F_XXX (handles a
 * field named "flags" in the structure referenced by its argument),
 * LF_XXX (handles a local variable named "flags"), and FLD_XXX (handles
 * any variable, anywhere.
 */
#define	F_CLR(p, mask)		((p)->flags &= ~(mask))
#define	F_ISSET(p, mask)	((p)->flags & (mask))
#define	F_SET(p, mask)		((p)->flags |= (mask))

#define	LF_CLR(mask)		((flags) &= ~(mask))
#define	LF_ISSET(mask)		((flags) & (mask))
#define	LF_SET(mask)		((flags) |= (mask))

#define	FLD_CLR(field, mask)	((field) &= ~(mask))
#define	FLD_ISSET(field, mask)	((field) & (mask))
#define	FLD_SET(field, mask)	((field) |= (mask))

/* Check for a verbose flag setting. */
#define	WT_VERB_ISSET(env, f)						\
	FLD_ISSET((env)->verbose, WT_VERB_ALL | (f))

/* Clear a chunk of memory. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

#define	WT_DEFAULT_FORMAT(db)						\
	default:							\
		return (__wt_database_format(db))

/*
 * Standard macros to handle simple return values and optionally branch to
 *  an error label.
 */
#define	WT_ERR(a) do {							\
	if ((ret = (a)) != 0)						\
		goto err;						\
} while (0)
#define	WT_RET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0)						\
		return (__ret);						\
} while (0)
#define	WT_TRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 && ret == 0)				\
		ret = __ret;						\
} while (0)

#if defined(__cplusplus)
}
#endif
