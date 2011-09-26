/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __wt_cache_page_workq_incr --
 *	Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_workq_incr(
    WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	cache->bytes_workq += size;
	page->memory_footprint += WT_STORE_SIZE(size);
}

/*
 * __wt_cache_page_read --
 *	Read pages into the cache.
 */
static inline void
__wt_cache_page_read(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ASSERT(session, size != 0);
	WT_ASSERT(session, page->memory_footprint == 0);

	++cache->pages_read;
	cache->bytes_read += size;

	page->memory_footprint = WT_STORE_SIZE(size);
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
	pages_in = cache->pages_read;
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
	    p >= (void *)((uint8_t *)page->dsk + page->dsk->memsize));
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
 * __wt_page_release --
 *	Release a reference to a page, unless it's pinned into memory, in which
 * case we never acquired a hazard reference.
 */
static inline void
__wt_page_release(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (page != NULL && !F_ISSET(page, WT_PAGE_PINNED))
		__wt_hazard_clear(session, page);
}

/*
 * __wt_skip_choose_depth --
 *      Randomly choose a depth for a skiplist insert.
 */
static inline u_int
__wt_skip_choose_depth(void)
{
	u_int d;

	for (d = 1; d < WT_SKIP_MAXDEPTH &&
	    __wt_random() < WT_SKIP_PROBABILITY; d++)
		;
	return (d);
}

/*
 * __wt_btree_lex_compare --
 *	Lexicographic comparison routine.
 *
 * Returns:
 *	< 0 if user_item is lexicographically < tree_item
 *	= 0 if user_item is lexicographically = tree_item
 *	> 0 if user_item is lexicographically > tree_item
 *
 * We use the names "user" and "tree" so it's clear which the application is
 * looking at when we call its comparison func.
 */
static inline int
__wt_btree_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item)
{
	const uint8_t *userp, *treep;
	uint32_t len, usz, tsz;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz);

	for (userp = user_item->data, treep = tree_item->data;
	    len > 0;
	    --len, ++userp, ++treep)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

#define	WT_BTREE_CMP(s, bt, k1, k2, cmp)				\
	(((bt)->collator == NULL) ?					\
	(((cmp) = __wt_btree_lex_compare((k1), (k2))), 0) :		\
	(bt)->collator->compare((bt)->collator, &(s)->iface,		\
	    (k1), (k2), &(cmp), NULL))
