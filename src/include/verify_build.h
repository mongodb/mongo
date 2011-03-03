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

#define	SIZE_CHECK(type, e)	do {					\
	char __check_##type[1 - 2 * !(sizeof(type) == (e))];		\
	(void)__check_##type;						\
} while (0)

#define	ALIGN_CHECK(type, a)						\
	STATIC_ASSERT(WT_ALIGN(sizeof(type), (a)) == sizeof(type))

static inline void
__wt_verify_build(void)
{
	/*
	 * The compiler had better not have padded our structures -- make sure
	 * the page header structure is exactly what we expect.
	 */
	SIZE_CHECK(WT_COL, WT_COL_SIZE);
	SIZE_CHECK(WT_CELL, WT_CELL_SIZE);
	SIZE_CHECK(WT_OFF, WT_OFF_SIZE);
	SIZE_CHECK(WT_OFF_RECORD, WT_OFF_RECORD_SIZE);
	SIZE_CHECK(WT_OVFL, WT_OVFL_SIZE);
	SIZE_CHECK(WT_PAGE, WT_PAGE_SIZE);
	SIZE_CHECK(WT_PAGE_DESC, WT_PAGE_DESC_SIZE);
	SIZE_CHECK(WT_ROW, WT_ROW_SIZE);

	/*
	 * The page header is special: the compiler will pad it to a multiple
	 * of 8 bytes because it has 64-bit fields that need alignment.  We
	 * use WT_PAGE_DISK_SIZE everywhere instead of sizeof to avoid writing
	 * 4 extra bytes to the file.
	 */
	SIZE_CHECK(WT_PAGE_DISK, WT_ALIGN(WT_PAGE_DISK_SIZE, sizeof(void *)));

	/* There are also structures that must be aligned correctly. */
	ALIGN_CHECK(WT_OFF, sizeof(uint32_t));
	ALIGN_CHECK(WT_OVFL, sizeof(uint32_t));
	ALIGN_CHECK(WT_PAGE_DISK, sizeof(uint32_t));
	ALIGN_CHECK(SESSION_BUFFER, sizeof(uint32_t));

	/*
	 * We mix-and-match 32-bit unsigned values and size_t's, mostly because
	 * we allocate and handle 32-bit objects, and lots of the underlying C
	 * library expects size_t values for the length of memory objects.  We
	 * check, just to be sure.
	 */
	STATIC_ASSERT(sizeof(size_t) >= sizeof(uint32_t));
}

#undef ALIGN_CHECK
