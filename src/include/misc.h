/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/* Basic constants. */
#define	WT_BILLION	(1000000000)
#define	WT_MEGABYTE	(1048576)
#define	WT_MILLION	(1000000)

/*
 * 32-bit version of sizeof: many sizes that cannot be larger than 2**32 are
 * stored in uint32_t variables to save bytes.  To avoid size_t to uint32_t
 * conversion warnings, we use this macro for sizeof calculations in 32-bit
 * space.
 *
 * And, a similar solution for pointer arithmetic.
 */
#define	WT_SIZEOF32(e)	((uint32_t)sizeof(e))
#define	WT_PTRDIFF32(end, begin)					\
	((uint32_t)((uint8_t *)(end) - ((uint8_t *)(begin))))

/*
 * Align an unsigned value of any type to a specified power-of-2, including the
 * offset result of a pointer subtraction.  Do the calculation using the largest
 * unsigned integer type available, which results in conversion complaints; cast
 * the result to a uint32_t because that's the size of a piece of data in the
 * WiredTiger engine.
 */
#define	WT_ALIGN(n, v)							\
	((uint32_t)((((uintmax_t)(n)) + ((v) - 1)) & ~(((uintmax_t)(v)) - 1)))

/* Min, max. */
#define	WT_MIN(a, b)	((a) < (b) ? (a) : (b))
#define	WT_MAX(a, b)	((a) < (b) ? (b) : (a))

/* Elements in an array. */
#define	WT_ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

/*
 * Flag check for API functions.
 * Explicitly cast the hex bit mask to an unsigned value to avoid complaints
 * about implicit conversions of integers.  Using the largest unsigned type,
 * there's no defined bit mask type or maximum value.
 */
#define	WT_CONN_FCHK_RET(conn, name, f, mask, ret)			\
	if ((f) & ~((uintmax_t)(mask)))					\
		ret = __wt_api_args(&(conn)->default_session, (name));
#define	WT_CONN_FCHK(conn, name, f, mask)				\
	if ((f) & ~((uintmax_t)(mask)))					\
		return (__wt_api_args(&(conn)->default_session, (name)));
#define	WT_DB_FCHK(btree, name, f, mask)				\
	WT_CONN_FCHK((btree)->conn, (name), (f), (mask))

/* Read-only file check. */
#define	WT_DB_RDONLY(btree, name)					\
	if (F_ISSET((btree), WT_RDONLY))				\
		return (__wt_file_readonly((btree), (name)));

/* Column- and row-only file check. */
#define	WT_DB_ROW_ONLY(btree, name)					\
	if (F_ISSET((btree), WT_COLUMN))				\
		return (__wt_file_method_type((btree), (name), 1));
#define	WT_DB_COL_ONLY(btree, name)					\
	if (!F_ISSET((btree), WT_COLUMN))				\
		return (__wt_file_method_type((btree), (name), 0));

/*
 * Flag set, clear and test.
 *
 * They come in 3 flavors: F_XXX (handles a field named "flags" in the structure
 * referenced by its argument), LF_XXX (handles a local variable named "flags"),
 * and FLD_XXX (handles any variable, anywhere.
 *
 * Flags are unsigned 32-bit values -- we cast to keep the compiler quiet (the
 * hex constant might be a negative integer), and to ensure the hex constant is
 * the correct size before applying the bitwise not operator.
 */
#define	F_CLR(p, mask)		((p)->flags &= ~((uint32_t)(mask)))
#define	F_ISSET(p, mask)	((p)->flags & ((uint32_t)(mask)))
#define	F_SET(p, mask)		((p)->flags |= ((uint32_t)(mask)))

#define	LF_CLR(mask)		((flags) &= ~((uint32_t)(mask)))
#define	LF_ISSET(mask)		((flags) & ((uint32_t)(mask)))
#define	LF_SET(mask)		((flags) |= ((uint32_t)(mask)))

#define	FLD_CLR(field, mask)	((field) &= ~((uint32_t)(mask)))
#define	FLD_ISSET(field, mask)	((field) & ((uint32_t)(mask)))
#define	FLD_SET(field, mask)	((field) |= ((uint32_t)(mask)))

/* Output a verbose message. */
#ifdef HAVE_VERBOSE
#define	WT_VERBOSE(conn, f, msg) do {					\
	if (FLD_ISSET((conn)->verbose, WT_VERB_ALL | (f)))		\
		__wt_msg msg;						\
} while (0)
#else
#define	WT_VERBOSE(conn, f, msg)
#endif

/* Clear a structure. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

#define	WT_ILLEGAL_FORMAT(btree)					\
	default:							\
		return (__wt_file_format((btree)))
#define	WT_ILLEGAL_FORMAT_ERR(btree, ret)				\
	default:							\
		ret = __wt_file_format((btree));			\
		goto err

/*
 * Macros to handle standard return values and optionally branch to an error
 * label.
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

/*
 * There are lots of places where we release a reference to a page, unless it's
 * the root page, which remains pinned for the life of the table handle.  It's
 * common enough to need a macro.
 */
#define	WT_PAGE_OUT(session, p)						\
	if ((p) != NULL && (p) != (session)->btree->root_page.page)	\
		__wt_hazard_clear((session), (p));

#if defined(__cplusplus)
}
#endif
