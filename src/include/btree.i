/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cache_page_inmem_incr --
 *	Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_incr(
    WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ATOMIC_ADD(cache->bytes_inmem, size);
	WT_ATOMIC_ADD(page->memory_footprint, WT_STORE_SIZE(size));
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

	WT_ATOMIC_ADD(cache->pages_read, 1);
	WT_ATOMIC_ADD(cache->bytes_read, size);
	WT_ATOMIC_ADD(page->memory_footprint, WT_STORE_SIZE(size));
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

	WT_ATOMIC_ADD(cache->pages_evict, 1);
	WT_ATOMIC_ADD(cache->bytes_evict, page->memory_footprint);

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
	bytes_in = cache->bytes_read + cache->bytes_inmem;
	bytes_out = cache->bytes_evict;
	return (bytes_in > bytes_out ? bytes_in - bytes_out : 0);
}

/*
 * __wt_page_modify_init --
 *	A page is about to be modified, allocate the modification structure.
 */
static inline int
__wt_page_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *modify;

	if (page->modify != NULL)
		return (0);

	WT_RET(__wt_calloc_def(session, 1, &modify));

	/*
	 * Multiple threads of control may be searching and deciding to modify
	 * a page, if we don't do the update, discard the memory.
	 */
	if (!WT_ATOMIC_CAS(page->modify, NULL, modify))
		__wt_free(session, modify);
	return (0);
}

/*
 * __wt_page_modify_set --
 *	Mark the page dirty.
 */
static inline void
__wt_page_modify_set(WT_PAGE *page)
{
	/*
	 * Publish: there must be a barrier to ensure all changes to the page
	 * are flushed before we update the page's write generation, otherwise
	 * a thread searching the page might see the page's write generation
	 * update before the changes to the page, which breaks the protocol.
	 */
	WT_WRITE_BARRIER();

	/* The page is dirty if the disk and write generations differ. */
	++page->modify->write_gen;
}

/*
 * __wt_page_is_modified --
 *	Return if the page is dirty.
 */
static inline int
__wt_page_is_modified(WT_PAGE *page)
{
	return (page->modify != NULL &&
	    page->modify->write_gen != page->modify->disk_gen ? 1 : 0);
}

/*
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
static inline int
__wt_page_write_gen_check(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen)
{
	if (page->modify->write_gen == write_gen)
		return (0);

	WT_BSTAT_INCR(session, file_write_conflicts);
	return (WT_RESTART);
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
 * __wt_get_addr --
 *	Return the addr/size pair for a reference.
 */
static inline void
__wt_get_addr(
    WT_PAGE *page, WT_REF *ref, const uint8_t **addrp, uint32_t *sizep)
{
	WT_CELL_UNPACK *unpack, _unpack;

	unpack = &_unpack;

	/*
	 * If NULL, there is no location.
	 * If off-page, the pointer references a WT_ADDR structure.
	 * If on-page, the pointer references a cell.
	 */
	if (ref->addr == NULL) {
		*addrp = NULL;
		*sizep = 0;
	} else if (__wt_off_page(page, ref->addr)) {
		*addrp = ((WT_ADDR *)(ref->addr))->addr;
		*sizep = ((WT_ADDR *)(ref->addr))->size;
	} else {
		__wt_cell_unpack(ref->addr, unpack);
		*addrp = unpack->data;
		*sizep = unpack->size;
	}
}

/*
 * __wt_page_release --
 *	Release a reference to a page.
 */
static inline void
__wt_page_release(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* We never acquired a hazard reference on the root page. */
	if (page != NULL && !WT_PAGE_IS_ROOT(page))
		__wt_hazard_clear(session, page);
}

/*
 * __wt_page_hazard_check --
 *	Return if there's a hazard reference to the page in the system.
 */
static inline WT_HAZARD *
__wt_page_hazard_check(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	WT_SESSION_IMPL *s;
	uint32_t i, session_cnt;

	conn = S2C(session);

	/*
	 * No lock is required because the session array is fixed size, but it
	 * it may contain inactive entries.  We must review any active session
	 * that might contain a hazard reference, so insert a barrier before
	 * reading the active session count.  That way, no matter what sessions
	 * come or go, we'll check the slots for all of the sessions that could
	 * have been active when we started our check.
	 */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
		if (!s->active)
			continue;
		for (hp = s->hazard; hp < s->hazard + conn->hazard_size; ++hp)
			if (hp->page == page)
				return (hp);
	}
	return (NULL);
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
	(bt)->collator->compare((bt)->collator, &(s)->iface,            \
	    (k1), (k2), &(cmp)))
