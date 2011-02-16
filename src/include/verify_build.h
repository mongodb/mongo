/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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

static inline void
__wt_verify_build(void)
{
	/*
	 * The compiler had better not have padded our structures -- make sure
	 * the page header structure is exactly what we expect.
	 */
	STATIC_ASSERT(sizeof(WT_COL) == WT_COL_SIZE);
	STATIC_ASSERT(sizeof(WT_ITEM) == WT_ITEM_SIZE);
	STATIC_ASSERT(sizeof(WT_OFF) == WT_OFF_SIZE);
	STATIC_ASSERT(sizeof(WT_OFF_RECORD) == WT_OFF_RECORD_SIZE);
	STATIC_ASSERT(sizeof(WT_OVFL) == WT_OVFL_SIZE);
	STATIC_ASSERT(sizeof(WT_PAGE) == WT_PAGE_SIZE);
	STATIC_ASSERT(sizeof(WT_PAGE_DESC) == WT_PAGE_DESC_SIZE);
	STATIC_ASSERT(sizeof(WT_ROW) == WT_ROW_SIZE);

	/*
	 * The page header is special: the compiler will pad it to a multiple
	 * of 8 bytes because it has 64-bit fields that need alignment.  We
	 * use WT_PAGE_DISK_SIZE everywhere instead of sizeof(WT_PAGE_DISK)
	 * to avoid writing 4 extra bytes to the file.
	 */
	STATIC_ASSERT(sizeof(WT_PAGE_DISK) ==
	    WT_ALIGN(WT_PAGE_DISK_SIZE, sizeof (void *)));

	/* There are also structures that must be aligned correctly. */
#define	ALIGN_CHECK(s, a)	STATIC_ASSERT(WT_ALIGN((s), (a)) == (s))
	ALIGN_CHECK(sizeof(WT_OFF), sizeof(uint32_t));
	ALIGN_CHECK(sizeof(WT_OVFL), sizeof(uint32_t));
	ALIGN_CHECK(sizeof(WT_PAGE_DISK), sizeof(uint32_t));
	ALIGN_CHECK(sizeof(WT_TOC_UPDATE), sizeof(uint32_t));

	/*
	 * We mix-and-match 32-bit unsigned values and size_t's, mostly because
	 * we allocate and handle 32-bit objects, and lots of the underlying C
	 * library expects size_t values for the length of memory objects.  We
	 * check, just to be sure.
	 */
	STATIC_ASSERT(sizeof(size_t) >= sizeof(uint32_t));
}

#undef ALIGN_CHECK
#undef STATIC_ASSERT
