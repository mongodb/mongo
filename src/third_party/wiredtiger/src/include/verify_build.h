/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#undef ALIGN_CHECK
#undef SIZE_CHECK

/*
 * NOTE: If you see a compile failure in this file, your compiler is laying out
 * structs in memory in a way WiredTiger does not expect.  Please refer to the
 * build instructions in the documentation (docs/html/install.html) for more
 * information.
 */

/*
 * Compile time assertions.
 *
 * If the argument to WT_STATIC_ASSERT is zero, the macro evaluates to:
 *
 *	(void)sizeof(char[-1])
 *
 * which fails to compile (which is what we want, the assertion failed).
 * If the value of the argument to WT_STATIC_ASSERT is non-zero, then the
 * macro evaluates to:
 *
 *	(void)sizeof(char[1]);
 *
 * which compiles with no warnings, and produces no code.
 *
 * For more details about why this works, see
 * http://scaryreasoner.wordpress.com/2009/02/28/
 */
#define	WT_STATIC_ASSERT(cond)	(void)sizeof(char[1 - 2 * !(cond)])

#define	SIZE_CHECK(type, e)	do {					\
	char __check_##type[1 - 2 * !(sizeof(type) == (e))];		\
	(void)__check_##type;						\
} while (0)

#define	ALIGN_CHECK(type, a)						\
	WT_STATIC_ASSERT(WT_ALIGN(sizeof(type), (a)) == sizeof(type))

/*
 * __wt_verify_build --
 *      This function is never called: it exists so there is a place for code
 *      that checks build-time conditions.
 */
static inline void
__wt_verify_build(void)
{
	/* Check specific structures weren't padded. */
	SIZE_CHECK(WT_BLOCK_DESC, WT_BLOCK_DESC_SIZE);
	SIZE_CHECK(WT_REF, WT_REF_SIZE);

	/*
	 * The btree code encodes key/value pairs in size_t's, and requires at
	 * least 8B size_t's.
	 */
	WT_STATIC_ASSERT(sizeof(size_t) >= 8);

	/*
	 * We require a wt_off_t fit into an 8B chunk because 8B is the largest
	 * integral value we can encode into an address cookie.
	 *
	 * WiredTiger has never been tested on a system with 4B file offsets,
	 * disallow them for now.
	 */
	WT_STATIC_ASSERT(sizeof(wt_off_t) == 8);
}

#undef ALIGN_CHECK
#undef SIZE_CHECK
