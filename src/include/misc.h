/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Quiet compiler warnings about unused function parameters and variables,
 * and unused function return values.
 */
#define	WT_UNUSED(var)		(void)(var)

#define	WT_DIVIDER	"=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

/* Basic constants. */
#define	WT_THOUSAND	(1000)
#define	WT_MILLION	(1000000)
#define	WT_BILLION	(1000000000)

#define	WT_KILOBYTE	(1024)
#define	WT_MEGABYTE	(1048576)
#define	WT_GIGABYTE	(1073741824)
#define	WT_TERABYTE	((uint64_t)1099511627776)
#define	WT_PETABYTE	((uint64_t)1125899906842624)
#define	WT_EXABYTE	((uint64_t)1152921504606846976)

/*
 * Sizes that cannot be larger than 2**32 are stored in uint32_t fields in
 * common structures to save space.  To minimize conversions from size_t to
 * uint32_t through the code, we use the following macros.
 */
#define	WT_STORE_SIZE(s)	((uint32_t)(s))
#define	WT_PTRDIFF(end, begin)						\
	((size_t)((const uint8_t *)(end) - (const uint8_t *)(begin)))
#define	WT_PTRDIFF32(end, begin)					\
	WT_STORE_SIZE(WT_PTRDIFF((end), (begin)))
#define	WT_BLOCK_FITS(p, len, begin, maxlen)				\
	((const uint8_t *)(p) >= (const uint8_t *)(begin) &&		\
	((const uint8_t *)(p) + (len) <= (const uint8_t *)(begin) + (maxlen)))
#define	WT_PTR_IN_RANGE(p, begin, maxlen)				\
	WT_BLOCK_FITS((p), 1, (begin), (maxlen))

/*
 * Align an unsigned value of any type to a specified power-of-2, including the
 * offset result of a pointer subtraction; do the calculation using the largest
 * unsigned integer type available.
 */
#define	WT_ALIGN(n, v)							\
	((((uintmax_t)(n)) + ((v) - 1)) & ~(((uintmax_t)(v)) - 1))

#define	WT_ALIGN_NEAREST(n, v)						\
	((((uintmax_t)(n)) + ((v) / 2)) & ~(((uintmax_t)(v)) - 1))

/* Min, max. */
#define	WT_MIN(a, b)	((a) < (b) ? (a) : (b))
#define	WT_MAX(a, b)	((a) < (b) ? (b) : (a))

/* Elements in an array. */
#define	WT_ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

/* 10 level skip lists, 1/4 have a link to the next element. */
#define	WT_SKIP_MAXDEPTH	10
#define	WT_SKIP_PROBABILITY	(UINT32_MAX >> 2)

/*
 * Encryption needs to know its original length before either the
 * block or logging subsystems pad.  Constant value.
 */
#define	WT_ENCRYPT_LEN_SIZE	sizeof(uint32_t)

/*
 * __wt_calloc_def, __wt_calloc_one --
 *	Most calloc calls don't need separate count or sizeof arguments.
 */
#define	__wt_calloc_def(session, number, addr)				\
	__wt_calloc(session, (size_t)(number), sizeof(**(addr)), addr)
#define	__wt_calloc_one(session, addr)					\
	__wt_calloc(session, (size_t)1, sizeof(**(addr)), addr)

/*
 * __wt_realloc_def --
 *	Common case allocate-and-grow function.
 *	Starts by allocating the requested number of items (at least 10), then
 *	doubles each time the list needs to grow.
 */
#define	__wt_realloc_def(session, sizep, number, addr)			\
	(((number) * sizeof(**(addr)) <= *(sizep)) ? 0 :		\
	    __wt_realloc(session, sizep, WT_MAX(*(sizep) * 2,		\
		WT_MAX(10, (number)) * sizeof(**(addr))), addr))
/*
 * Our internal free function clears the underlying address atomically so there
 * is a smaller chance of racing threads seeing intermediate results while a
 * structure is being free'd.	(That would be a bug, of course, but I'd rather
 * not drop core, just the same.)  That's a non-standard "free" API, and the
 * resulting bug is a mother to find -- make sure we get it right, don't make
 * the caller remember to put the & operator on the pointer.
 */
#define	__wt_free(session, p) do {					\
	void *__p = &(p);						\
	if (*(void **)__p != NULL)					\
		__wt_free_int(session, __p);				\
} while (0)
#ifdef HAVE_DIAGNOSTIC
#define	__wt_overwrite_and_free(session, p) do {			\
	memset(p, WT_DEBUG_BYTE, sizeof(*(p)));				\
	__wt_free(session, p);						\
} while (0)
#define	__wt_overwrite_and_free_len(session, p, len) do {		\
	memset(p, WT_DEBUG_BYTE, len);					\
	__wt_free(session, p);						\
} while (0)
#else
#define	__wt_overwrite_and_free(session, p)		__wt_free(session, p)
#define	__wt_overwrite_and_free_len(session, p, len)	__wt_free(session, p)
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
#define	FLD_CLR(field, mask)	        ((void)((field) &= ~(uint32_t)(mask)))
#define	FLD_MASK(field, mask)	        ((field) & (uint32_t)(mask))
#define	FLD_ISSET(field, mask)	        (FLD_MASK(field, mask) != 0)
#define	FLD64_ISSET(field, mask)	(((field) & (uint64_t)(mask)) != 0)
#define	FLD_SET(field, mask)	        ((void)((field) |= (uint32_t)(mask)))

#define	F_CLR(p, mask)		        FLD_CLR((p)->flags, mask)
#define	F_ISSET(p, mask)	        FLD_ISSET((p)->flags, mask)
#define	F_MASK(p, mask)	                FLD_MASK((p)->flags, mask)
#define	F_SET(p, mask)		        FLD_SET((p)->flags, mask)

#define	LF_CLR(mask)		        FLD_CLR(flags, mask)
#define	LF_ISSET(mask)		        FLD_ISSET(flags, mask)
#define	LF_MASK(mask)		        FLD_MASK(flags, mask)
#define	LF_SET(mask)		        FLD_SET(flags, mask)

/*
 * Insertion sort, for sorting small sets of values.
 *
 * The "compare_lt" argument is a function or macro that returns true when
 * its first argument is less than its second argument.
 */
#define	WT_INSERTION_SORT(arrayp, n, value_type, compare_lt) do {	\
	value_type __v;							\
	int __i, __j, __n = (int)(n);					\
	if (__n == 2) {							\
		__v = (arrayp)[1];					\
		if (compare_lt(__v, (arrayp)[0])) {			\
			(arrayp)[1] = (arrayp)[0];			\
			(arrayp)[0] = __v;				\
		}							\
	}								\
	if (__n > 2) {							\
		for (__i = 1; __i < __n; ++__i) {			\
			__v = (arrayp)[__i];				\
			for (__j = __i - 1; __j >= 0 &&			\
			    compare_lt(__v, (arrayp)[__j]); --__j)	\
				(arrayp)[__j + 1] = (arrayp)[__j];	\
			(arrayp)[__j + 1] = __v;			\
		}							\
	}								\
} while (0)

/*
 * Binary search for an integer key.
 */
#define	WT_BINARY_SEARCH(key, arrayp, n, found) do {			\
	uint32_t __base, __indx, __limit;				\
	found = false;							\
	for (__base = 0, __limit = (n); __limit != 0; __limit >>= 1) {	\
		__indx = __base + (__limit >> 1);			\
		if ((arrayp)[__indx] < key) {				\
			__base = __indx + 1;				\
			--__limit;					\
		} else if ((arrayp)[__indx] == key) {			\
			found = true;					\
			break;						\
		}							\
	}								\
} while (0)

/* Verbose messages. */
#ifdef HAVE_VERBOSE
#define	WT_VERBOSE_ISSET(session, f)					\
	(FLD_ISSET(S2C(session)->verbose, f))
#else
#define	WT_VERBOSE_ISSET(session, f)	0
#endif

#define	WT_CLEAR(s)							\
	memset(&(s), 0, sizeof(s))

/* Check if a string matches a prefix. */
#define	WT_PREFIX_MATCH(str, pfx)					\
	(((const char *)(str))[0] == ((const char *)pfx)[0] &&		\
	    strncmp((str), (pfx), strlen(pfx)) == 0)

/* Check if a string matches a prefix, and move past it. */
#define	WT_PREFIX_SKIP(str, pfx)					\
	(WT_PREFIX_MATCH(str, pfx) ? ((str) += strlen(pfx), 1) : 0)

/*
 * Check if a variable string equals a constant string.  Inline the common
 * case for WiredTiger of a single byte string.  This is required because not
 * all compilers optimize this case in strcmp (e.g., clang).
 */
#define	WT_STREQ(s, cs)							\
	(sizeof(cs) == 2 ? (s)[0] == (cs)[0] && (s)[1] == '\0' :	\
	strcmp(s, cs) == 0)

/* Check if a string matches a byte string of len bytes. */
#define	WT_STRING_MATCH(str, bytes, len)				\
	(((const char *)str)[0] == ((const char *)bytes)[0] &&		\
	    strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0')

/*
 * Macro that produces a string literal that isn't wrapped in quotes, to avoid
 * tripping up spell checkers.
 */
#define	WT_UNCHECKED_STRING(str) #str

/* Function return value and scratch buffer declaration and initialization. */
#define	WT_DECL_ITEM(i)	WT_ITEM *i = NULL
#define	WT_DECL_RET	int ret = 0

/* If a WT_ITEM data field points somewhere in its allocated memory. */
#define	WT_DATA_IN_ITEM(i)						\
	((i)->mem != NULL && (i)->data >= (i)->mem &&			\
	    WT_PTRDIFF((i)->data, (i)->mem) < (i)->memsize)

/* Copy the data and size fields of an item. */
#define	WT_ITEM_SET(dst, src) do {					\
	(dst).data = (src).data;					\
	(dst).size = (src).size;					\
} while (0)

/*
 * In diagnostic mode we track the locations from which hazard pointers and
 * scratch buffers were acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_scr_alloc(session, size, scratchp)				\
	__wt_scr_alloc_func(session, size, scratchp, __FILE__, __LINE__)
#define	__wt_page_in(session, ref, flags)				\
	__wt_page_in_func(session, ref, flags, __FILE__, __LINE__)
#define	__wt_page_swap(session, held, want, flags)			\
	__wt_page_swap_func(session, held, want, flags, __FILE__, __LINE__)
#else
#define	__wt_scr_alloc(session, size, scratchp)				\
	__wt_scr_alloc_func(session, size, scratchp)
#define	__wt_page_in(session, ref, flags)				\
	__wt_page_in_func(session, ref, flags)
#define	__wt_page_swap(session, held, want, flags)			\
	__wt_page_swap_func(session, held, want, flags)
#endif

/* Random number generator state. */
union __wt_rand_state {
	uint64_t v;
	struct {
		uint32_t w, z;
	} x;
};

/* Shared array for converting to hex */
extern const u_char __wt_hex[];
