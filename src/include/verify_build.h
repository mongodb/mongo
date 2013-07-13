/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * NOTE: If you see a compile failure in this file, your compiler is laying out
 * structs in memory in a way WiredTiger does not expect.  Please refer to the
 * build instructions in the documentation (docs/html/install.html) for more
 * information.
 */

/*
 * Compile time assertions.
 *
 * If the argument to STATIC_ASSERT is zero, the macro evaluates to:
 *
 *	(void)sizeof(char[-1])
 *
 * which fails to compile (which is what we want, the assertion failed).
 * If the value of the argument to STATIC_ASSERT is non-zero, then the macro
 * evaluates to:
 *
 *	(void)sizeof(char[1]);
 *
 * which compiles with no warnings, and produces no code.
 *
 * For more details about why this works, see
 * http://scaryreasoner.wordpress.com/2009/02/28/
 */
#define	STATIC_ASSERT(cond)	(void)sizeof(char[1 - 2 * !(cond)])

#define	SIZE_CHECK(type, e)	do {					\
	char __check_##type[1 - 2 * !(sizeof(type) == (e))];		\
	(void)__check_##type;						\
} while (0)

#define	ALIGN_CHECK(type, a)						\
	STATIC_ASSERT(WT_ALIGN(sizeof(type), (a)) == sizeof(type))

static inline void
__wt_verify_build(void)
{
	WT_REF ref;

	/* Check specific structures weren't padded. */
	SIZE_CHECK(WT_BLOCK_DESC, WT_BLOCK_DESC_SIZE);
	SIZE_CHECK(WT_REF, WT_REF_SIZE);

	/*
	 * There's magic in the WT_REF.k field layout, check nothing bad
	 * happened.
	 */
	STATIC_ASSERT((void *)&ref.key.page.offset == (void *)&ref.key.ikey);

	/*
	 * We mix-and-match 32-bit unsigned values and size_t's, mostly because
	 * we allocate and handle 32-bit objects, and lots of the underlying C
	 * library expects size_t values for the length of memory objects.  We
	 * check, just to be sure.
	 */
	STATIC_ASSERT(sizeof(size_t) >= sizeof(uint32_t));

	/*
	 * We require an off_t fit into an 8B chunk because 8B is the largest
	 * integral value we can encode into an address cookie.
	 *
	 * WiredTiger has never been tested on a system with 4B off_t types,
	 * disallow them for now.
	 */
	STATIC_ASSERT(sizeof(off_t) == sizeof(int64_t));
}

#undef ALIGN_CHECK
