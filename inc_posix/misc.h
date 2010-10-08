/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
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

/* Elements in an array. */
#define	WT_ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

/* Flag check for API functions. */
#define	WT_ENV_FCHK_RET(env, name, f, mask, ret)			\
	if ((f) & ~(mask))						\
		ret = __wt_api_args(env, name);
#define	WT_ENV_FCHK(env, name, f, mask)					\
	if ((f) & ~(mask))						\
		return (__wt_api_args(env, name));
#define	WT_ENV_FCHK_ASSERT(env, name, f, mask)				\
	WT_ASSERT(env, ((f) & ~(mask)) == 0)
#define	WT_DB_FCHK(db, name, f, mask)					\
	WT_ENV_FCHK((db)->env, name, f, mask)

/* Read-only database check. */
#define	WT_DB_RDONLY(db, name)						\
	if (F_ISSET((db)->idb, WT_RDONLY))				\
		return (__wt_database_readonly(db, name));

/* Column- and row-only database check. */
#define	WT_DB_ROW_ONLY(db, name)					\
	if (F_ISSET((db)->idb, WT_COLUMN))				\
		return (__wt_database_method_type(db, name, 1));
#define	WT_DB_COL_ONLY(db, name)					\
	if (!F_ISSET((db)->idb, WT_COLUMN))				\
		return (__wt_database_method_type(db, name, 0));

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

/* Output a verbose message. */
#ifdef HAVE_VERBOSE
#define	WT_VERBOSE(env, f, msg) do {					\
	if (FLD_ISSET((env)->verbose, WT_VERB_ALL | (f)))		\
		__wt_msg msg;						\
} while (0)
#else
#define	WT_VERBOSE(env, f, msg)
#endif

/* Clear a chunk of memory. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

#define	WT_ILLEGAL_FORMAT(db)						\
	default:							\
		return (__wt_database_format(db))
#define	WT_ILLEGAL_FORMAT_ERR(db, ret)					\
	default:							\
		ret = __wt_database_format(db);				\
		goto err

/*
 * Macros to handle standard return values and optionally branch to an error
 * label.  One pair returns WT_RESTART, one set retries WT_RESTART: if you
 * have valid information, that is, if you know your addr/size pair is good
 * (for example, because it's taken from a pinned page), you can retry, else
 * you have to return the failure.
 *
 * Generally, addr/size pair locations are not declared volatile, they live
 * on random Btree pages; for that reason, use a memory barrier instruction
 * before retrying the call.  (In other words, some other thread modified
 * the addr/size pair, and caused a WT_RESTART to be returned to us -- the
 * memory barrier is to ensure we get the new values, by flushing our saved
 * information.)
 */
#define	WT_ERR(a) do {							\
	if ((ret = (a)) != 0)						\
		goto err;						\
} while (0)
#define	WT_ERR_RESTART(a) do {						\
	int __ret;							\
	while ((__ret = (a)) != 0 && __ret == WT_RESTART)		\
		WT_MEMORY_FLUSH;					\
	if (__ret != 0)							\
		goto err;						\
} while (0)
#define	WT_RET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0)						\
		return (__ret);						\
} while (0)
#define	WT_RET_RESTART(a) do {						\
	int __ret;							\
	while ((__ret = (a)) != 0 && __ret == WT_RESTART)		\
		WT_MEMORY_FLUSH;					\
	if (__ret != 0)							\
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
