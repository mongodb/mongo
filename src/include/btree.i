/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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
 * __wt_eviction_page_force --
 *	Check if a page matches the criteria for forced eviction.
 *	Returns how many times larger than the configured maximum a page is.
 *	We only split pages in memory when they are a multiple larger than
 *	the configured maxiumum.
 */
static inline int
__wt_eviction_page_force(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/*
	 * Ignore internal pages (check read-only information first to the
	 * extent possible, this is shared data).
	 */
	if (page->type != WT_PAGE_ROW_INT && page->type != WT_PAGE_COL_INT &&
	    !F_ISSET(btree, WT_BTREE_NO_EVICTION) &&
	    __wt_page_is_modified(page) &&
	    page->memory_footprint > btree->maxmempage)
		return (page->memory_footprint / btree->maxmempage);

	return (0);
}

/*
 * Estimate the per-allocation overhead.  All implementations of malloc / free
 * have some kind of header and pad for alignment.  We can't know for sure what
 * that adds up to, but this is an estimate based on some measurements of heap
 * size versus bytes in use.
 */
#define	WT_ALLOC_OVERHEAD      32

/*
 * __wt_cache_page_inmem_incr --
 *	Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_incr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	size += WT_ALLOC_OVERHEAD;

	cache = S2C(session)->cache;
	(void)WT_ATOMIC_ADD(cache->bytes_inmem, size);
	(void)WT_ATOMIC_ADD(page->memory_footprint, WT_STORE_SIZE(size));
	if (__wt_page_is_modified(page))
		(void)WT_ATOMIC_ADD(cache->bytes_dirty, size);
}

/*
 * __wt_cache_page_inmem_decr --
 *	Decrement a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	size += WT_ALLOC_OVERHEAD;

	cache = S2C(session)->cache;
	(void)WT_ATOMIC_SUB(cache->bytes_inmem, size);
	(void)WT_ATOMIC_SUB(page->memory_footprint, WT_STORE_SIZE(size));
	if (__wt_page_is_modified(page))
		(void)WT_ATOMIC_SUB(cache->bytes_dirty, size);
}

/*
 * __wt_cache_dirty_decr --
 *	Decrement a page's memory footprint from the cache dirty count. Will
 *	be called after a reconciliation leaves a page clean.
 */
static inline void
__wt_cache_dirty_decr(WT_SESSION_IMPL *session, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;
	if (cache->bytes_dirty < size || cache->pages_dirty == 0) {
		if (WT_VERBOSE_ISSET(session, evictserver))
			(void)__wt_verbose(session,
			    "Cache dirty decrement failed: %" PRIu64
			    " pages dirty, %" PRIu64
			    " bytes dirty, decrement size %" PRIuMAX,
			    cache->pages_dirty,
			    cache->bytes_dirty, (uintmax_t)size);
		cache->bytes_dirty = 0;
		cache->pages_dirty = 0;
	} else {
		(void)WT_ATOMIC_SUB(cache->bytes_dirty, size);
		(void)WT_ATOMIC_SUB(cache->pages_dirty, 1);
	}
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

	(void)WT_ATOMIC_ADD(cache->pages_evict, 1);
	(void)WT_ATOMIC_ADD(cache->bytes_evict, page->memory_footprint);

	page->memory_footprint = 0;
}

static inline uint64_t
__wt_cache_read_gen(WT_SESSION_IMPL *session)
{
	return (S2C(session)->cache->read_gen);
}

static inline uint64_t
__wt_cache_read_gen_set(WT_SESSION_IMPL *session)
{
	/*
	 * We return read-generations from the future (where "the future" is
	 * measured by increments of the global read generation).  The reason
	 * is because when acquiring a new hazard reference on a page, we can
	 * check its read generation, and if the read generation isn't less
	 * than the current global generation, we don't bother updating the
	 * page.  In other words, the goal is to avoid some number of updates
	 * immediately after each update we have to make.
	 */
	return (++S2C(session)->cache->read_gen + WT_READ_GEN_STEP);
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
	pages_in = cache->pages_inmem;
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
	bytes_in = cache->bytes_inmem;
	bytes_out = cache->bytes_evict;
	return (bytes_in > bytes_out ? bytes_in - bytes_out : 0);
}

/*
 * __wt_cache_bytes_dirty --
 *	Return the number of bytes in cache marked dirty.
 */
static inline uint64_t
__wt_cache_bytes_dirty(WT_CACHE *cache)
{
	return (cache->bytes_dirty);
}

/*
 * __wt_cache_pages_dirty --
 *	Return the number of pages in cache marked dirty.
 */
static inline uint64_t
__wt_cache_pages_dirty(WT_CACHE *cache)
{
	return (cache->pages_dirty);
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
	 * a page.  If our modify structure is used, update the page's memory
	 * footprint, else discard the modify structure, another thread did the
	 * work.
	 */
	if (WT_ATOMIC_CAS(page->modify, NULL, modify))
		__wt_cache_page_inmem_incr(session, page, sizeof(*modify));
	else
		__wt_free(session, modify);
	return (0);
}

/*
 * __wt_page_modify_set --
 *	Mark the page dirty.
 */
static inline void
__wt_page_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (!__wt_page_is_modified(page)) {
		(void)WT_ATOMIC_ADD(S2C(session)->cache->pages_dirty, 1);
		(void)WT_ATOMIC_ADD(
		    S2C(session)->cache->bytes_dirty, page->memory_footprint);

		/*
		 * The page can never end up with changes older than the oldest
		 * running transaction.
		 */
		if (F_ISSET(&session->txn, TXN_RUNNING))
			page->modify->disk_snap_min = session->txn.snap_min;
	}

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
 * __wt_page_and_tree_modify_set --
 *	Mark both the page and tree dirty.
 */
static inline void
__wt_page_and_tree_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/*
	 * A memory barrier is required for setting the tree's modified value,
	 * we depend on the barrier called in setting the page's modified value.
	 */
	btree->modified = 1;

	__wt_page_modify_set(session, page);
}

/*
 * __wt_page_write_gen_wrapped_check --
 *	Confirm the page's write generation number hasn't wrapped.
 */
static inline int
__wt_page_write_gen_wrapped_check(WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;

	mod = page->modify;

	/* 
	 * If the page's write generation has wrapped and caught up with the
	 * disk generation (wildly unlikely but technically possible as it
	 * implies 4B updates between page reconciliations), fail the update.
	 */
	return (mod->write_gen + 1 == mod->disk_gen ? WT_RESTART : 0);
}

/*
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
static inline int
__wt_page_write_gen_check(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t write_gen)
{
	WT_PAGE_MODIFY *mod;

	WT_RET(__wt_page_write_gen_wrapped_check(page));

	/*
	 * If the page's write generation matches the search generation, we can
	 * proceed.  Except, if the page's write generation has wrapped and
	 * caught up with the disk generation (wildly unlikely but technically
	 * possible as it implies 4B updates between page reconciliations), fail
	 * the update.
	 */
	mod = page->modify;
	if (mod->write_gen == write_gen)
		return (0);

	WT_DSTAT_INCR(session, txn_write_conflict);
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
	 */
	return (page->dsk == NULL ||
	    p < (void *)page->dsk ||
	    p >= (void *)((uint8_t *)page->dsk + page->dsk->mem_size));
}

/*
 * __wt_ref_key --
 *	Return a reference to a row-store internal page key as cheaply as
 * possible.
 */
static inline void
__wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, uint32_t *sizep)
{
	/*
	 * An internal page key is in one of two places: if we instantiated the
	 * key (for example, when reading the page), WT_REF.key.ikey references
	 * a WT_IKEY structure, otherwise, WT_REF.key.pkey references an on-page
	 * key.
	 *
	 * Now the magic: Any allocated memory will have a low-order bit of 0
	 * (the return from malloc must be aligned to store any standard type,
	 * and we assume there's always going to be a standard type requiring
	 * even-byte alignment).  An on-page key consists of an offset/length
	 * pair.  We can fit the maximum page size into 31 bits, so we use the
	 * low-order bit in the on-page value to flag the next 31 bits as a
	 * page offset and the other 32 bits as the key's length, not a WT_IKEY
	 * pointer.  This breaks if allocation chunks aren't even-byte aligned
	 * or pointers and uint64_t's don't always map their low-order bits to
	 * the same location.
	 */
	if (ref->key.pkey & 0x01) {
		*(void **)keyp =
		    WT_PAGE_REF_OFFSET(page, (ref->key.pkey & 0xFFFFFFFF) >> 1);
		*sizep = (uint32_t)(ref->key.pkey >> 32);
	} else {
		*(void **)keyp = WT_IKEY_DATA(ref->key.ikey);
		*sizep = ((WT_IKEY *)ref->key.ikey)->size;
	}
}

/*
 * __wt_ref_key_onpage_set --
 *	Set a WT_REF to reference an on-page key.
 */
static inline void
__wt_ref_key_onpage_set(WT_PAGE *page, WT_REF *ref, WT_CELL_UNPACK *unpack)
{
	/*
	 * See the comment in __wt_ref_key for an explanation of the magic.
	 */
	ref->key.pkey =
	    (uint64_t)unpack->size << 32 |
	    (uint32_t)WT_PAGE_DISK_OFFSET(page, unpack->data) << 1 |
	    0x01;
}

/*
 * __wt_ref_key_instantiated --
 *	Return an instantiated key from a WT_REF.
 */
static inline WT_IKEY *
__wt_ref_key_instantiated(WT_REF *ref)
{
	/*
	 * See the comment in __wt_ref_key for an explanation of the magic.
	 */
	return (ref->key.pkey & 0x01 ? NULL : ref->key.ikey);
}

/*
 * __wt_ref_key_clear --
 *	Clear a WT_REF key.
 */
static inline void
__wt_ref_key_clear(WT_REF *ref)
{
	/* The key union has 3 fields, all of which are 8B. */
	ref->key.recno = 0;
}

/*
 * __wt_row_leaf_key --
 *	Set a buffer to reference a row-store leaf page key as cheaply as
 * possible.
 */
static inline int
__wt_row_leaf_key(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW *rip, WT_ITEM *key, int instantiate)
{
	WT_BTREE *btree;
	WT_IKEY *ikey;
	WT_CELL_UNPACK unpack;

	btree = S2BT(session);

	/*
	 * A subset of __wt_row_leaf_key_work, that is, calling that function
	 * should give you the same results as calling this one; this function
	 * exists to inline fast-path checks for already instantiated keys and
	 * on-page uncompressed keys.
	 */
retry:	ikey = WT_ROW_KEY_COPY(rip);

	/*
	 * Key copied.
	 * If the key has been instantiated for any reason, off-page, use it.
	 */
	if (__wt_off_page(page, ikey)) {
		key->data = WT_IKEY_DATA(ikey);
		key->size = ikey->size;
		return (0);
	}

	/* If the key isn't compressed or an overflow, take it from the page. */
	if (btree->huffman_key == NULL) {
		__wt_cell_unpack((WT_CELL *)ikey, &unpack);
		if (unpack.type == WT_CELL_KEY && unpack.prefix == 0) {
			key->data = unpack.data;
			key->size = unpack.size;
			return (0);
		}
	}

	/*
	 * We have to build the key (it's never been instantiated, and it's some
	 * kind of compressed or overflow key).
	 *
	 * Magic: the row-store leaf page search loop calls us to instantiate
	 * keys, and it's not prepared to handle memory being allocated in the
	 * key's WT_ITEM.  Call __wt_row_leaf_key_work to instantiate the key
	 * with no buffer reference, then retry to pick up a simple reference
	 * to the instantiated key.
	 */
	if (instantiate) {
		WT_RET(__wt_row_leaf_key_work(session, page, rip, NULL, 1));
		goto retry;
	}

	/*
	 * If instantiate wasn't set, our caller is prepared to handle memory
	 * allocations in the key's WT_ITEM, pass the key.
	 */
	return (__wt_row_leaf_key_work(session, page, rip, key, 0));
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
static inline int
__wt_page_release(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_DECL_RET;

	/*
	 * Discard our hazard pointer.  Ignore pages we don't have and the root
	 * page, which sticks in memory, regardless.
	 */
	if (page == NULL || WT_PAGE_IS_ROOT(page))
		return (0);

	/*
	 * Try to immediately evict pages if they have the special "oldest"
	 * read generation and we have some chance of succeeding.
	 */
	if (!WT_TXN_ACTIVE(&session->txn) &&
	    page->read_gen == WT_READ_GEN_OLDEST &&
	    WT_ATOMIC_CAS(page->ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
		if ((ret = __wt_hazard_clear(session, page)) != 0) {
			page->ref->state = WT_REF_MEM;
			return (ret);
		}

		if ((ret = __wt_evict_page(session, page)) == EBUSY)
			ret = 0;
		return (ret);
	}

	return (__wt_hazard_clear(session, page));
}

/*
 * __wt_page_swap_func --
 *	Swap one page's hazard pointer for another one when hazard pointer
 * coupling up/down the tree.
 */
static inline int
__wt_page_swap_func(
    WT_SESSION_IMPL *session, WT_PAGE *out, WT_PAGE *in, WT_REF *inref
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_DECL_RET;
	int acquired;

	/*
	 * This function is here to simplify the error handling during hazard
	 * pointer coupling so we never leave a hazard pointer dangling.  The
	 * assumption is we're holding a hazard pointer on "out", and want to
	 * read page "in", acquiring a hazard pointer on it, then release page
	 * "out" and its hazard pointer.  If something fails, discard it all.
	 */
	ret = __wt_page_in_func(session, in, inref
#ifdef HAVE_DIAGNOSTIC
	    , file, line
#endif
	    );
	acquired = ret == 0;
	WT_TRET(__wt_page_release(session, out));

	if (ret != 0 && acquired)
		WT_TRET(__wt_page_release(session, inref->page));
	return (ret);
}

/*
 * __wt_page_hazard_check --
 *	Return if there's a hazard pointer to the page in the system.
 */
static inline WT_HAZARD *
__wt_page_hazard_check(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	WT_SESSION_IMPL *s;
	uint32_t i, hazard_size, session_cnt;

	conn = S2C(session);

	/*
	 * No lock is required because the session array is fixed size, but it
	 * may contain inactive entries.  We must review any active session
	 * that might contain a hazard pointer, so insert a barrier before
	 * reading the active session count.  That way, no matter what sessions
	 * come or go, we'll check the slots for all of the sessions that could
	 * have been active when we started our check.
	 */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
		if (!s->active)
			continue;
		WT_ORDERED_READ(hazard_size, s->hazard_size);
		for (hp = s->hazard; hp < s->hazard + hazard_size; ++hp)
			if (hp->page == page)
				return (hp);
	}
	return (NULL);
}

/*
 * __wt_skip_choose_depth --
 *	Randomly choose a depth for a skiplist insert.
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
 * __wt_btree_size_overflow --
 *      Check if the size of an in-memory tree with a single leaf page is
 *      over a specified maximum.  If called on anything other than a simple
 *      tree with a single leaf page, returns true so the calling code will
 *      switch to a new tree.
 */
static inline int
__wt_btree_size_overflow(WT_SESSION_IMPL *session, uint32_t maxsize)
{
	WT_BTREE *btree;
	WT_PAGE *child, *root;

	btree = S2BT(session);
	root = btree->root_page;

	if (btree == NULL || root == NULL ||
	    (child = root->u.intl.t->page) == NULL)
		return (0);

	/* Make sure this is a simple tree, or LSM should switch. */
	if (!F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
	    root->entries != 1 ||
	    root->u.intl.t->state != WT_REF_MEM ||
	    child->type != WT_PAGE_ROW_LEAF)
		return (1);

	return (child->memory_footprint > maxsize);
}

/*
 * __wt_lex_compare --
 *	Lexicographic comparison routine.
 *
 * Returns:
 *	< 0 if user_item is lexicographically < tree_item
 *	= 0 if user_item is lexicographically = tree_item
 *	> 0 if user_item is lexicographically > tree_item
 *
 * We use the names "user" and "tree" so it's clear in the btree code which
 * the application is looking at when we call its comparison func.
 */
static inline int
__wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item)
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

#define	WT_LEX_CMP(s, collator, k1, k2, cmp)				\
	((collator) == NULL ?						\
	(((cmp) = __wt_lex_compare((k1), (k2))), 0) :			\
	(collator)->compare(collator, &(s)->iface, (k1), (k2), &(cmp)))

/*
 * __wt_lex_compare_skip --
 *	Lexicographic comparison routine, but skipping leading bytes.
 *
 * Returns:
 *	< 0 if user_item is lexicographically < tree_item
 *	= 0 if user_item is lexicographically = tree_item
 *	> 0 if user_item is lexicographically > tree_item
 *
 * We use the names "user" and "tree" so it's clear in the btree code which
 * the application is looking at when we call its comparison func.
 */
static inline int
__wt_lex_compare_skip(
    const WT_ITEM *user_item, const WT_ITEM *tree_item, uint32_t *matchp)
{
	const uint8_t *userp, *treep;
	uint32_t len, usz, tsz;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz) - *matchp;

	for (userp = (uint8_t *)user_item->data + *matchp,
	    treep = (uint8_t *)tree_item->data + *matchp;
	    len > 0;
	    --len, ++userp, ++treep, ++*matchp)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

#define	WT_LEX_CMP_SKIP(s, collator, k1, k2, cmp, matchp)		\
	((collator) == NULL ?						\
	(((cmp) = __wt_lex_compare_skip((k1), (k2), matchp)), 0) :	\
	(collator)->compare(collator, &(s)->iface, (k1), (k2), &(cmp)))

/*
 * __wt_btree_mergeable --
 *      Determines whether the given page is a candidate for merging.
 */
static inline int
__wt_btree_mergeable(WT_PAGE *page)
{
	if (WT_PAGE_IS_ROOT(page) ||
	    page->modify == NULL ||
	    !F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
		return (0);

	return (!WT_PAGE_IS_ROOT(page->parent));
}
