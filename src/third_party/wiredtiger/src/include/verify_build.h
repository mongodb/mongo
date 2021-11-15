/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * NOTE: If you see a compile failure in this file, your compiler is laying out structs in memory in
 * a way WiredTiger does not expect. Please refer to the build instructions in the documentation
 * (docs/html/install.html) for more information.
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
#define WT_STATIC_ASSERT(cond) (void)sizeof(char[1 - 2 * !(cond)])

#define WT_SIZE_CHECK(type, e)                               \
    do {                                                     \
        char __check_##type[1 - 2 * !(sizeof(type) == (e))]; \
        (void)__check_##type;                                \
    } while (0)

#define WT_ALIGN_CHECK(type, a) WT_STATIC_ASSERT(WT_ALIGN(sizeof(type), (a)) == sizeof(type))

/*
 * __wt_verify_build --
 *     This function is never called: it exists so there is a place for code that checks build-time
 *     conditions.
 */
static inline void
__wt_verify_build(void)
{
    /* Check specific structures weren't padded. */
    WT_SIZE_CHECK(WT_BLKCACHE_ID, WT_BLKCACHE_ID_SIZE);
    WT_SIZE_CHECK(WT_BLOCK_DESC, WT_BLOCK_DESC_SIZE);
    WT_SIZE_CHECK(WT_REF, WT_REF_SIZE);

    /*
     * WT_UPDATE is special: we arrange fields to avoid padding within the structure but it could be
     * padded at the end depending on the timestamp size. Further check that the data field in the
     * update structure is where we expect it.
     */
    WT_SIZE_CHECK(WT_UPDATE, WT_ALIGN(WT_UPDATE_SIZE, 8));
    WT_STATIC_ASSERT(offsetof(WT_UPDATE, data) == WT_UPDATE_SIZE);

/* Check specific structures were padded. */
#define WT_PADDING_CHECK(s) \
    WT_STATIC_ASSERT(       \
      sizeof(s) > WT_CACHE_LINE_ALIGNMENT || sizeof(s) % WT_CACHE_LINE_ALIGNMENT == 0)
    WT_PADDING_CHECK(WT_LOGSLOT);
    WT_PADDING_CHECK(WT_TXN_SHARED);

    /*
     * The btree code encodes key/value pairs in size_t's, and requires at least 8B size_t's.
     */
    WT_STATIC_ASSERT(sizeof(size_t) >= 8);

    /*
     * We require a wt_off_t fit into an 8B chunk because 8B is the largest integral value we can
     * encode into an address cookie.
     *
     * WiredTiger has never been tested on a system with 4B file offsets, disallow them for now.
     */
    WT_STATIC_ASSERT(sizeof(wt_off_t) == 8);

    /*
     * We require a time_t be an integral type and fit into a uint64_t for simplicity.
     */
    WT_STATIC_ASSERT(sizeof(time_t) <= sizeof(uint64_t));
}
