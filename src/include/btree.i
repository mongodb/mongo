/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#undef	STATIN
#define	STATIN	static inline

STATIN uint64_t __wt_cache_bytes_inuse(WT_CACHE *);
STATIN void	__wt_cache_page_evict(WT_SESSION_IMPL *, WT_PAGE *);
STATIN void	__wt_cache_page_read(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
STATIN void	__wt_cache_page_workq(WT_SESSION_IMPL *);
STATIN uint64_t __wt_cache_pages_inuse(WT_CACHE *);
STATIN uint64_t __wt_cache_read_gen(WT_SESSION_IMPL *);
STATIN uint8_t	__wt_fix_getv(WT_BTREE *, uint32_t, uint8_t *);
STATIN uint8_t	__wt_fix_getv_recno(WT_BTREE *, WT_PAGE *, uint64_t);
STATIN void	__wt_fix_setv(WT_BTREE *, uint32_t, uint8_t *, uint8_t);
STATIN void	__wt_fix_setv_recno(WT_BTREE *, WT_PAGE *, uint64_t, uint8_t);
STATIN int	__wt_off_page(WT_PAGE *, const void *);
STATIN void	__wt_page_out(WT_SESSION_IMPL *, WT_PAGE *);
STATIN int	__wt_page_reconcile(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
STATIN int	__wt_page_write_gen_check(WT_PAGE *, uint32_t);

/*
 * __wt_cache_page_workq --
 *	Create pages into the cache.
 */
static inline void
__wt_cache_page_workq(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	++cache->pages_workq;
}

/*
 * __wt_cache_page_workq_incr --
 *	Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_workq_incr(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	cache->bytes_workq += size;
	page->memory_footprint += size;
}

/*
 * __wt_cache_page_read --
 *	Read pages into the cache.
 */
static inline void
__wt_cache_page_read(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ASSERT(session, size != 0);
	WT_ASSERT(session, page->memory_footprint == 0);

	++cache->pages_read;
	cache->bytes_read += size;

	page->memory_footprint = size;
}

/*
 * __wt_cache_page_evict --
 *	Evict pages from the cache.
 */
static inline void
__wt_cache_page_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ASSERT(session, page->memory_footprint != 0);

	++cache->pages_evict;
	cache->bytes_evict += page->memory_footprint;

	page->memory_footprint = 0;
}

static inline uint64_t
__wt_cache_read_gen(WT_SESSION_IMPL *session)
{
	return (++S2C(session)->cache->read_gen);
}

/*
 * __wt_cache_pages_inuse --
 *	Return the number of pages in use.
 */
static inline uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
	uint64_t pages_in, pages_out;

	/*
	 * Reading 64-bit fields, potentially on 32-bit machines, and other
	 * threads of control may be modifying them.  Check them for sanity
	 * (although "interesting" corruption is vanishingly unlikely, these
	 * values just increment over time).
	 */
	pages_in = cache->pages_read + cache->pages_workq;
	pages_out = cache->pages_evict;
	return (pages_in > pages_out ? pages_in - pages_out : 0);
}

/*
 * __wt_cache_bytes_inuse --
 *	Return the number of bytes in use.
 */
static inline uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
	uint64_t bytes_in, bytes_out;

	/*
	 * Reading 64-bit fields, potentially on 32-bit machines, and other
	 * threads of control may be modifying them.  Check them for sanity
	 * (although "interesting" corruption is vanishingly unlikely, these
	 * values just increment over time).
	 */
	bytes_in = cache->bytes_read + cache->bytes_workq;
	bytes_out = cache->bytes_evict;
	return (bytes_in > bytes_out ? bytes_in - bytes_out : 0);
}

/*
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
static inline int
__wt_page_write_gen_check(WT_PAGE *page, uint32_t write_gen)
{
	return (page->write_gen == write_gen ? 0 : WT_RESTART);
}

/*
 * __wt_off_page --
 *	Return if a pointer references off-page data.
 */
static inline int
__wt_off_page(WT_PAGE *page, const void *p)
{
	/*
	 * There may be no underlying page, in which case the reference is
	 * off-page by definition.
	 *
	 * We use the page's disk size, not the page parent's reference disk
	 * size for a reason: the page may already be disconnected from the
	 * parent reference (when being discarded), or not yet be connected
	 * to the parent reference (when being created).
	 */
	return (page->dsk == NULL ||
	    p < (void *)page->dsk ||
	    p >= (void *)((uint8_t *)page->dsk + page->dsk->size));
}

/*
 * __wt_page_reconcile --
 *	Standard version of page reconciliation.
 */
static inline int
__wt_page_reconcile(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	/*
	 * There's an internal version of page reconciliation that salvage uses,
	 * everybody else just calls with a value of NULL as the 3rd argument.
	 */
	return (__wt_page_reconcile_int(session, page, NULL, flags));
}

/*
 * __wt_page_out --
 *	Release a reference to a page, unless it's the root page, which remains
 * pinned for the life of the table handle.
 */
static inline void
__wt_page_out(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (page != NULL && !WT_PAGE_IS_ROOT(page))
		__wt_hazard_clear(session, page);
}

/*
 * __wt_fix_getv --
 *	Get a bit-field and return it.
 */
static inline uint8_t
__wt_fix_getv(WT_BTREE *btree, uint32_t entry, uint8_t *bitf)
{
	uint8_t value;
	int bit;

#define	__BIT_GET(len, mask)						\
	case len:							\
		if (bit_test(bitf, bit))				\
			value |= mask;					\
		++bit							\
		/* FALLTHROUGH */

	bit = entry * btree->bitcnt;
	value = 0;
	switch (btree->bitcnt) {
	__BIT_GET(8, 0x80);
	__BIT_GET(7, 0x40);
	__BIT_GET(6, 0x20);
	__BIT_GET(5, 0x10);
	__BIT_GET(4, 0x08);
	__BIT_GET(3, 0x04);
	__BIT_GET(2, 0x02);
	__BIT_GET(1, 0x01);
	}
	return (value);
}

/*
 * __wt_fix_getv_recno --
 *	Get the record number's bit-field from a page and return it.
 */
static inline uint8_t
__wt_fix_getv_recno(WT_BTREE *btree, WT_PAGE *page, uint64_t recno)
{
	return (__wt_fix_getv(btree,
	    (uint32_t)(recno - page->u.col_leaf.recno),
	    page->u.col_leaf.bitf));
}

/*
 * __wt_fix_setv --
 *	Set a bit-field to a value.
 */
static inline void
__wt_fix_setv(WT_BTREE *btree, uint32_t entry, uint8_t *bitf, uint8_t value)
{
	int bit;

#define	__BIT_SET(len, mask)						\
	case len:							\
		if (value & (mask))					\
			bit_set(bitf, bit);				\
		else							\
			bit_clear(bitf, bit);				\
		++bit							\
		/* FALLTHROUGH */

	bit = entry * btree->bitcnt;
	switch (btree->bitcnt) {
	__BIT_SET(8, 0x80);
	__BIT_SET(7, 0x40);
	__BIT_SET(6, 0x20);
	__BIT_SET(5, 0x10);
	__BIT_SET(4, 0x08);
	__BIT_SET(3, 0x04);
	__BIT_SET(2, 0x02);
	__BIT_SET(1, 0x01);
	}
}

/*
 * __wt_fix_setv_recno --
 *	Set the record number's bit-field to a value.
 */
static inline void
__wt_fix_setv_recno(
    WT_BTREE *btree, WT_PAGE *page, uint64_t recno, uint8_t value)
{
	return (__wt_fix_setv(btree,
	    (uint32_t)(recno - page->u.col_leaf.recno),
	    page->u.col_leaf.bitf, value));
}
