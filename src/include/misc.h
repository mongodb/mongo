/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/* Basic constants. */
#define	WT_BILLION	(1000000000)
#define	WT_MEGABYTE	(1048576)
#define	WT_MILLION	(1000000)

/*
 * Sizes that cannot be larger than 2**32 are stored in uint32_t fields in
 * common structures to save space.  To minimize conversions from size_t to
 * uint32_t through the code, we use the following macros.
 */
#define	WT_STORE_SIZE(s)	((uint32_t)(s))
#define	WT_PTRDIFF(end, begin)						\
	((size_t)((uint8_t *)(end) - (uint8_t *)(begin)))
#define	WT_PTRDIFF32(end, begin)					\
	WT_STORE_SIZE(WT_PTRDIFF((end), (begin)))
#define	WT_BLOCK_FITS(p, len, begin, maxlen)				\
	((uint8_t *)(p) >= (uint8_t *)(begin) &&			\
	((uint8_t *)(p) + (len) <= (uint8_t *)(begin) + (maxlen)))
#define	WT_PTR_IN_RANGE(p, begin, maxlen)				\
	WT_BLOCK_FITS((p), 1, (begin), (maxlen))

/*
 * XXX
 * The server threads use their own WT_SESSION_IMPL handles because they may
 * want to block (for example, the eviction server calls reconciliation, and
 * some of the reconciliation diagnostic code reads pages), and the user's
 * session handle is already blocking on a server thread.  The problem is the
 * server thread needs to reference the correct btree handle, and that's
 * hanging off the application's thread of control.  For now, I'm just making
 * it obvious where that's getting done.
 */
#define	WT_SET_BTREE_IN_SESSION(s, b)					\
	((s)->btree = (b))

#define	WT_CLEAR_BTREE_IN_SESSION(s)					\
	((s)->btree = NULL)

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
 * Quiet compiler warnings about unused parameters.
 */
#define	WT_UNUSED(var)	(void)(var)

/* Add GCC-specific attributes to types and function declarations. */
#ifdef __GNUC__
#define	WT_GCC_ATTRIBUTE(x)	__attribute__(x)
#else
#define	WT_GCC_ATTRIBUTE(x)
#endif

/*
 * Attribute are only permitted on function declarations, not definitions.
 * This macro is a marker for function definitions that is rewritten by
 * dist/s_prototypes to create extern.h.
 */
#define	WT_GCC_FUNC_ATTRIBUTE(x)

/*
 * __wt_calloc_def --
 *	Simple calls don't need separate sizeof arguments.
 */
#define	__wt_calloc_def(a, b, c)					\
	__wt_calloc(a, (size_t)(b), sizeof(**(c)), c)
/*
 * Our internal free function clears the underlying address atomically so there
 * is a smaller chance of racing threads seeing intermediate results while a
 * structure is being free'd.   (That would be a bug, of course, but I'd rather
 * not drop core, just the same.)  That's a non-standard "free" API, and the
 * resulting bug is a mother to find -- make sure we get it right, don't make
 * the caller remember to put the & operator on the pointer.
 */
#define	__wt_free(a, b)			__wt_free_int(a, &(b))

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
#define	WT_VERBOSE_ISSET(session, f)					\
	(FLD_ISSET(S2C(session)->verbose, WT_VERB_##f))
#define	WT_VERBOSE(session, f, ...) do {				\
	if (WT_VERBOSE_ISSET(session, f))				\
		__wt_msg(session, __VA_ARGS__);				\
} while (0)
#else
#define	WT_VERBOSE_ISSET(session, f)	0
#define	WT_VERBOSE(session, f, ...)
#endif

/* Clear a structure. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

/* Standard error cases for switch statements. */
#define	WT_ILLEGAL_FORMAT(session)					\
	default:							\
		return (__wt_file_format(session))
#define	WT_ILLEGAL_FORMAT_ERR(session)					\
	default:							\
		ret = __wt_file_format(session);			\
		goto err

/*
 * Macros to handle standard return values and optionally branch to an error
 * label.
 */
#define	WT_ERR(a) do {							\
	if ((ret = (a)) != 0)						\
		goto err;						\
} while (0)
#define	WT_ERR_TEST(a, v) do {						\
	if (a) {							\
		ret = (v);						\
		goto err;						\
	}								\
} while (0)
#define	WT_RET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0)						\
		return (__ret);						\
} while (0)
#define	WT_RET_TEST(a, v) do {						\
	if (a)								\
		return (v);						\
} while (0)
#define	WT_TRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 && ret == 0)				\
		ret = __ret;						\
} while (0)

/* Macros to check whether a string matches a prefix. */

#define	WT_PREFIX_MATCH(str, pre)					\
	(strncmp((str), (pre), strlen(pre)) == 0)

#define	WT_PREFIX_SKIP(str, pre)					\
	((strncmp((str), (pre), strlen(pre)) == 0) ?			\
	    ((str) += strlen(pre), 1) : 0)
