/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Basic constants. */
#define	WT_MILLION	(1000000)
#define	WT_BILLION	(1000000000)

#define	WT_KILOBYTE	(1024)
#define	WT_MEGABYTE	(1048576)
#define	WT_GIGABYTE	(1073741824)
#define	WT_TERABYTE	((uint64_t)1099511627776)
#define	WT_PETABYTE	((uint64_t)1125899906842624)

/*
 * Size to build an entry prefix with a pathname.
 */
#define	ENTRY_SIZE	128

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
 * Align an unsigned value of any type to a specified power-of-2, including the
 * offset result of a pointer subtraction; do the calculation using the largest
 * unsigned integer type available.
 */
#define	WT_ALIGN(n, v)							\
	((((uintmax_t)(n)) + ((v) - 1)) & ~(((uintmax_t)(v)) - 1))

/* Min, max. */
#define	WT_MIN(a, b)	((a) < (b) ? (a) : (b))
#define	WT_MAX(a, b)	((a) < (b) ? (b) : (a))

/* Elements in an array. */
#define	WT_ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

/* 10 level skip lists, 1/2 have a link to the next element. */
#define	WT_SKIP_MAXDEPTH	10
#define	WT_SKIP_PROBABILITY	(UINT32_MAX >> 2)

/* The number of hazard pointers that can be in use is grown dynamically. */
#define	WT_HAZARD_INCR		10

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
#define	__wt_calloc_def(session, number, addr)				\
	__wt_calloc(session, (size_t)(number), sizeof(**(addr)), addr)
/*
 * Our internal free function clears the underlying address atomically so there
 * is a smaller chance of racing threads seeing intermediate results while a
 * structure is being free'd.   (That would be a bug, of course, but I'd rather
 * not drop core, just the same.)  That's a non-standard "free" API, and the
 * resulting bug is a mother to find -- make sure we get it right, don't make
 * the caller remember to put the & operator on the pointer.
 */
#define	__wt_free(session, p)		__wt_free_int(session, &(p))
#ifdef HAVE_DIAGNOSTIC
#define	__wt_overwrite_and_free(session, p) do {			\
	memset(p, WT_DEBUG_BYTE, sizeof(*(p)));				\
	__wt_free(session, p);						\
} while (0)
#else
#define	__wt_overwrite_and_free(session, p)	__wt_free(session, p)
#endif

/*
 * Flag set, clear and test.
 *
 * They come in 3 flavors: F_XXX (handles a field named "flags" in the structure
 * referenced by its argument), LF_XXX (handles a local variable named "flags"),
 * and FLD_XXX (handles any variable, anywhere).
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

/* Verbose messages. */
#define	WT_VERBOSE_ISSET(session, f)					\
	(FLD_ISSET(S2C(session)->verbose, WT_VERB_##f))
#define	WT_VERBOSE_ERR(session, f, ...) do {				\
	if (WT_VERBOSE_ISSET(session, f))				\
		WT_ERR(__wt_verbose(session, #f ": " __VA_ARGS__));	\
} while (0)
#define	WT_VERBOSE_RET(session, f, ...) do {				\
	if (WT_VERBOSE_ISSET(session, f))				\
		WT_RET(__wt_verbose(session, #f ": " __VA_ARGS__));	\
} while (0)
#define	WT_VERBOSE_TRET(session, f, ...) do {				\
	if (WT_VERBOSE_ISSET(session, f))				\
		WT_TRET(__wt_verbose(session, #f ": " __VA_ARGS__));	\
} while (0)

/* Clear a structure. */
#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

/* Check if a string matches a prefix. */
#define	WT_PREFIX_MATCH(str, pre)					\
	(strncmp((str), (pre), strlen(pre)) == 0)

/* Check if a string matches a prefix, and move past it. */
#define	WT_PREFIX_SKIP(str, pre)					\
	((strncmp((str), (pre), strlen(pre)) == 0) ?			\
	    ((str) += strlen(pre), 1) : 0)

/* Check if a string matches a byte string of len bytes. */
#define	WT_STRING_MATCH(str, bytes, len)				\
	(strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0')

/* Function return value and scratch buffer declaration and initialization. */
#define	WT_DECL_ITEM(i)	WT_ITEM *i = NULL
#define	WT_DECL_RET	int ret = 0

/*
 * Skip the default configuration string in an list of configurations. The
 * default config is always the first entry in the array, and the array always
 * has an explicit NULL terminator, so this is safe.
 */
#define	WT_SKIP_DEFAULT_CONFIG(c) &(c)[1]

/*
 * In diagnostic mode we track the locations from which hazard pointers and
 * scratch buffers were acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_scr_alloc(session, size, scratchp)				\
	__wt_scr_alloc_func(session, size, scratchp, __FILE__, __LINE__)
#define	__wt_page_in(session, parent, ref)				\
	__wt_page_in_func(session, parent, ref, __FILE__, __LINE__)
#define	__wt_page_swap(session, out, in, inref)				\
	__wt_page_swap_func(session, out, in, inref, __FILE__, __LINE__)
#else
#define	__wt_scr_alloc(session, size, scratchp)				\
	__wt_scr_alloc_func(session, size, scratchp)
#define	__wt_page_in(session, parent, ref)				\
	__wt_page_in_func(session, parent, ref)
#define	__wt_page_swap(session, out, in, inref)				\
	__wt_page_swap_func(session, out, in, inref)
#endif
