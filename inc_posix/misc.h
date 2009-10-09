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
#define	WT_FRAGMENT	(512)
#define	WT_MEGABYTE	(1048576)

/* Align a number to a specified power-of-2. */
#define	WT_ALIGN(n, v)							\
	(((n) + ((v) - 1)) & ~(((uintmax_t)(v)) - 1))

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
		return (__wt_api_flags((db)->env, name));
#define	WT_DB_FCHK_NOTFATAL(db, name, f, mask, ret)			\
	if ((f) & ~(mask))						\
		(ret) = __wt_api_flags((db)->env, name);
#define	WT_ENV_FCHK(env, name, f, mask)					\
	if ((f) & ~(mask))						\
		return (__wt_api_flags(env, name));
#define	WT_ENV_FCHK_NOTFATAL(env, name, f, mask, ret)			\
	if ((f) & ~(mask))						\
		(ret) = __wt_api_flags(env, name);

/*
 * Flag set, clear and test.  They come in 3 flavors: F_XXX (handles a
 * field named "flags" in the structure referenced by its argument),
 * LF_XXX (handles a local variable named "flags"), and FLD_XXX (handles
 * any variable, anywhere.
 */
#define	F_CLR(p, mask)		((p)->flags &= ~(mask))
#define	F_ISSET(p, mask)	((p)->flags & (mask) ? 1 : 0)
#define	F_SET(p, mask)		((p)->flags |= (mask))

#define	LF_CLR(mask)		((flags) &= ~(mask))
#define	LF_ISSET(mask)		((flags) & (mask) ? 1 : 0)
#define	LF_SET(mask)		((flags) |= (mask))

#define	FLD_CLR(field, mask)	((field) &= ~(mask))
#define	FLD_ISSET(field, mask)	((field) & (mask) ? 1 : 0)
#define	FLD_SET(field, mask)	((field) |= (mask))

/* Clear a chunk of memory. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

/* Free memory if set. */
#define	WT_FREE_AND_CLEAR(env, p) do {					\
	if ((p) != NULL) {						\
		__wt_free(env, p);					\
		(p) = NULL;						\
	}								\
} while (0)

/* A distinguished byte pattern to overwrite memory we are done using. */
#define	OVERWRITE_BYTE	0xab

#ifdef HAVE_DIAGNOSTIC
#define	WT_ASSERT(env, e)						\
	((e) ? (void)0 : __wt_assert(env, #e, __FILE__, __LINE__))
#else
#define	WT_ASSERT(ienv, e)
#endif

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
