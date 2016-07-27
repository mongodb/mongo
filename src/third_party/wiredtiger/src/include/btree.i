/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_ref_is_root --
 *	Return if the page reference is for the root page.
 */
static inline bool
__wt_ref_is_root(WT_REF *ref)
{
	return (ref->home == NULL);
}

/*
 * __wt_page_is_empty --
 *	Return if the page is empty.
 */
static inline bool
__wt_page_is_empty(WT_PAGE *page)
{
	return (page->modify != NULL &&
	    page->modify->rec_result == WT_PM_REC_EMPTY);
}

/*
 * __wt_page_is_modified --
 *	Return if the page is dirty.
 */
static inline bool
__wt_page_is_modified(WT_PAGE *page)
{
	return (page->modify != NULL && page->modify->write_gen != 0);
}

/*
 * __wt_btree_block_free --
 *	Helper function to free a block from the current tree.
 */
static inline int
__wt_btree_block_free(
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BM *bm;
	WT_BTREE *btree;

	btree = S2BT(session);
	bm = btree->bm;

	return (bm->free(bm, session, addr, addr_size));
}

/*
 * __wt_cache_page_inmem_incr --
 *	Increment a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_incr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	WT_ASSERT(session, size < WT_EXABYTE);

	cache = S2C(session)->cache;
	(void)__wt_atomic_add64(&cache->bytes_inmem, size);
	(void)__wt_atomic_addsize(&page->memory_footprint, size);
	if (__wt_page_is_modified(page)) {
		(void)__wt_atomic_add64(&cache->bytes_dirty, size);
		(void)__wt_atomic_addsize(&page->modify->bytes_dirty, size);
	}
	/* Track internal and overflow size in cache. */
	if (WT_PAGE_IS_INTERNAL(page))
		(void)__wt_atomic_add64(&cache->bytes_internal, size);
	else if (page->type == WT_PAGE_OVFL)
		(void)__wt_atomic_add64(&cache->bytes_overflow, size);
}

/*
 * __wt_cache_decr_check_size --
 *	Decrement a size_t cache value and check for underflow.
 */
static inline void
__wt_cache_decr_check_size(
    WT_SESSION_IMPL *session, size_t *vp, size_t v, const char *fld)
{
	if (__wt_atomic_subsize(vp, v) < WT_EXABYTE)
		return;

#ifdef HAVE_DIAGNOSTIC
	(void)__wt_atomic_addsize(vp, v);

	{
	static bool first = true;

	if (!first)
		return;
	__wt_errx(session, "%s underflow: decrementing %" WT_SIZET_FMT, fld, v);
	first = false;
	}
#else
	WT_UNUSED(fld);
	WT_UNUSED(session);
#endif
}

/*
 * __wt_cache_decr_check_uint64 --
 *	Decrement a uint64_t cache value and check for underflow.
 */
static inline void
__wt_cache_decr_check_uint64(
    WT_SESSION_IMPL *session, uint64_t *vp, size_t v, const char *fld)
{
	if (__wt_atomic_sub64(vp, v) < WT_EXABYTE)
		return;

#ifdef HAVE_DIAGNOSTIC
	(void)__wt_atomic_add64(vp, v);

	{
	static bool first = true;

	if (!first)
		return;
	__wt_errx(session, "%s underflow: decrementing %" WT_SIZET_FMT, fld, v);
	first = false;
	}
#else
	WT_UNUSED(fld);
	WT_UNUSED(session);
#endif
}

/*
 * __wt_cache_page_byte_dirty_decr --
 *	Decrement the page's dirty byte count, guarding from underflow.
 */
static inline void
__wt_cache_page_byte_dirty_decr(
    WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;
	size_t decr, orig;
	int i;

	cache = S2C(session)->cache;

	/*
	 * We don't have exclusive access and there are ways of decrementing the
	 * page's dirty byte count by a too-large value. For example:
	 *	T1: __wt_cache_page_inmem_incr(page, size)
	 *		page is clean, don't increment dirty byte count
	 *	T2: mark page dirty
	 *	T1: __wt_cache_page_inmem_decr(page, size)
	 *		page is dirty, decrement dirty byte count
	 * and, of course, the reverse where the page is dirty at the increment
	 * and clean at the decrement.
	 *
	 * The page's dirty-byte value always reflects bytes represented in the
	 * cache's dirty-byte count, decrement the page/cache as much as we can
	 * without underflow. If we can't decrement the dirty byte counts after
	 * few tries, give up: the cache's value will be wrong, but consistent,
	 * and we'll fix it the next time this page is marked clean, or evicted.
	 */
	for (i = 0; i < 5; ++i) {
		/*
		 * Take care to read the dirty-byte count only once in case
		 * we're racing with updates.
		 */
		orig = page->modify->bytes_dirty;
		decr = WT_MIN(size, orig);
		if (__wt_atomic_cassize(
		    &page->modify->bytes_dirty, orig, orig - decr)) {
			__wt_cache_decr_check_uint64(session,
			    &cache->bytes_dirty, decr, "WT_CACHE.bytes_dirty");
			break;
		}
	}
}

/*
 * __wt_cache_page_inmem_decr --
 *	Decrement a page's memory footprint in the cache.
 */
static inline void
__wt_cache_page_inmem_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	WT_ASSERT(session, size < WT_EXABYTE);

	__wt_cache_decr_check_uint64(
	    session, &cache->bytes_inmem, size, "WT_CACHE.bytes_inmem");
	__wt_cache_decr_check_size(
	    session, &page->memory_footprint, size, "WT_PAGE.memory_footprint");
	if (__wt_page_is_modified(page))
		__wt_cache_page_byte_dirty_decr(session, page, size);
	/* Track internal and overflow size in cache. */
	if (WT_PAGE_IS_INTERNAL(page))
		__wt_cache_decr_check_uint64(session,
		    &cache->bytes_internal, size, "WT_CACHE.bytes_internal");
	else if (page->type == WT_PAGE_OVFL)
		__wt_cache_decr_check_uint64(session,
		    &cache->bytes_overflow, size, "WT_CACHE.bytes_overflow");
}

/*
 * __wt_cache_dirty_incr --
 *	Page switch from clean to dirty: increment the cache dirty page/byte
 * counts.
 */
static inline void
__wt_cache_dirty_incr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	size_t size;

	cache = S2C(session)->cache;
	(void)__wt_atomic_add64(&cache->pages_dirty, 1);

	/*
	 * Take care to read the memory_footprint once in case we are racing
	 * with updates.
	 */
	size = page->memory_footprint;
	(void)__wt_atomic_add64(&cache->bytes_dirty, size);
	(void)__wt_atomic_addsize(&page->modify->bytes_dirty, size);
}

/*
 * __wt_cache_dirty_decr --
 *	Page switch from dirty to clean: decrement the cache dirty page/byte
 * counts.
 */
static inline void
__wt_cache_dirty_decr(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_PAGE_MODIFY *modify;

	cache = S2C(session)->cache;

	if (cache->pages_dirty < 1) {
		__wt_errx(session,
		   "cache eviction dirty-page decrement failed: dirty page"
		   "count went negative");
		cache->pages_dirty = 0;
	} else
		(void)__wt_atomic_sub64(&cache->pages_dirty, 1);

	modify = page->modify;
	if (modify != NULL && modify->bytes_dirty != 0)
		__wt_cache_page_byte_dirty_decr(
		    session, page, modify->bytes_dirty);
}

/*
 * __wt_cache_page_evict --
 *	Evict pages from the cache.
 */
static inline void
__wt_cache_page_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_PAGE_MODIFY *modify;

	cache = S2C(session)->cache;
	modify = page->modify;

	/* Update the bytes in-memory to reflect the eviction. */
	__wt_cache_decr_check_uint64(session,
	    &cache->bytes_inmem,
	    page->memory_footprint, "WT_CACHE.bytes_inmem");

	/* Update the bytes_internal value to reflect the eviction */
	if (WT_PAGE_IS_INTERNAL(page))
		__wt_cache_decr_check_uint64(session,
		    &cache->bytes_internal,
		    page->memory_footprint, "WT_CACHE.bytes_internal");

	/* Update the cache's dirty-byte count. */
	if (modify != NULL && modify->bytes_dirty != 0) {
		if (cache->bytes_dirty < modify->bytes_dirty) {
			__wt_errx(session,
			   "cache eviction dirty-bytes decrement failed: "
			   "dirty byte count went negative");
			cache->bytes_dirty = 0;
		} else
			__wt_cache_decr_check_uint64(session,
			    &cache->bytes_dirty,
			    modify->bytes_dirty, "WT_CACHE.bytes_dirty");
	}

	/* Update pages and bytes evicted. */
	(void)__wt_atomic_add64(&cache->bytes_evict, page->memory_footprint);
	(void)__wt_atomic_add64(&cache->pages_evict, 1);
}

/*
 * __wt_update_list_memsize --
 *      The size in memory of a list of updates.
 */
static inline size_t
__wt_update_list_memsize(WT_UPDATE *upd)
{
	size_t upd_size;

	for (upd_size = 0; upd != NULL; upd = upd->next)
		upd_size += WT_UPDATE_MEMSIZE(upd);

	return (upd_size);
}

/*
 * __wt_page_evict_soon --
 *      Set a page to be evicted as soon as possible.
 */
static inline void
__wt_page_evict_soon(WT_PAGE *page)
{
	page->read_gen = WT_READGEN_OLDEST;
}

/*
 * __wt_page_modify_init --
 *	A page is about to be modified, allocate the modification structure.
 */
static inline int
__wt_page_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	return (page->modify == NULL ?
	    __wt_page_modify_alloc(session, page) : 0);
}

/*
 * __wt_page_only_modify_set --
 *	Mark the page (but only the page) dirty.
 */
static inline void
__wt_page_only_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	uint64_t last_running;

	WT_ASSERT(session, !F_ISSET(session->dhandle, WT_DHANDLE_DEAD));

	last_running = 0;
	if (page->modify->write_gen == 0)
		last_running = S2C(session)->txn_global.last_running;

	/*
	 * We depend on atomic-add being a write barrier, that is, a barrier to
	 * ensure all changes to the page are flushed before updating the page
	 * write generation and/or marking the tree dirty, otherwise checkpoints
	 * and/or page reconciliation might be looking at a clean page/tree.
	 *
	 * Every time the page transitions from clean to dirty, update the cache
	 * and transactional information.
	 */
	if (__wt_atomic_add32(&page->modify->write_gen, 1) == 1) {
		__wt_cache_dirty_incr(session, page);

		/*
		 * We won the race to dirty the page, but another thread could
		 * have committed in the meantime, and the last_running field
		 * been updated past it.  That is all very unlikely, but not
		 * impossible, so we take care to read the global state before
		 * the atomic increment.
		 *
		 * If the page was dirty on entry, then last_running == 0. The
		 * page could have become clean since then, if reconciliation
		 * completed. In that case, we leave the previous value for
		 * first_dirty_txn rather than potentially racing to update it,
		 * at worst, we'll unnecessarily write a page in a checkpoint.
		 */
		if (last_running != 0)
			page->modify->first_dirty_txn = last_running;
	}

	/* Check if this is the largest transaction ID to update the page. */
	if (WT_TXNID_LT(page->modify->update_txn, session->txn.id))
		page->modify->update_txn = session->txn.id;
}

/*
 * __wt_page_modify_clear --
 *	Clean a modified page.
 */
static inline void
__wt_page_modify_clear(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * The page must be held exclusive when this call is made, this call
	 * can only be used when the page is owned by a single thread.
	 *
	 * Allow the call to be made on clean pages.
	 */
	if (__wt_page_is_modified(page)) {
		page->modify->write_gen = 0;
		__wt_cache_dirty_decr(session, page);
	}
}

/*
 * __wt_page_modify_set --
 *	Mark the page and tree dirty.
 */
static inline void
__wt_page_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * Mark the tree dirty (even if the page is already marked dirty), newly
	 * created pages to support "empty" files are dirty, but the file isn't
	 * marked dirty until there's a real change needing to be written. Test
	 * before setting the dirty flag, it's a hot cache line.
	 *
	 * The tree's modified flag is cleared by the checkpoint thread: set it
	 * and insert a barrier before dirtying the page.  (I don't think it's
	 * a problem if the tree is marked dirty with all the pages clean, it
	 * might result in an extra checkpoint that doesn't do any work but it
	 * shouldn't cause problems; regardless, let's play it safe.)
	 */
	if (S2BT(session)->modified == 0) {
		/* Assert we never dirty a checkpoint handle. */
		WT_ASSERT(session, session->dhandle->checkpoint == NULL);

		S2BT(session)->modified = 1;
		WT_FULL_BARRIER();
	}

	__wt_page_only_modify_set(session, page);
}

/*
 * __wt_page_parent_modify_set --
 *	Mark the parent page, and optionally the tree, dirty.
 */
static inline int
__wt_page_parent_modify_set(
    WT_SESSION_IMPL *session, WT_REF *ref, bool page_only)
{
	WT_PAGE *parent;

	/*
	 * This function exists as a place to stash this comment.  There are a
	 * few places where we need to dirty a page's parent.  The trick is the
	 * page's parent might split at any point, and the page parent might be
	 * the wrong parent at any particular time.  We ignore this and dirty
	 * whatever page the page's reference structure points to.  This is safe
	 * because if we're pointing to the wrong parent, that parent must have
	 * split, deepening the tree, which implies marking the original parent
	 * and all of the newly-created children as dirty.  In other words, if
	 * we have the wrong parent page, everything was marked dirty already.
	 */
	parent = ref->home;
	WT_RET(__wt_page_modify_init(session, parent));
	if (page_only)
		__wt_page_only_modify_set(session, parent);
	else
		__wt_page_modify_set(session, parent);
	return (0);
}

/*
 * __wt_off_page --
 *	Return if a pointer references off-page data.
 */
static inline bool
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
 * __wt_ref_addr_free --
 *	Free the address in a reference, if necessary.
 */
static inline void
__wt_ref_addr_free(WT_SESSION_IMPL *session, WT_REF *ref)
{
	if (ref->addr == NULL)
		return;

	if (ref->home == NULL || __wt_off_page(ref->home, ref->addr)) {
		__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
		__wt_free(session, ref->addr);
	}
	ref->addr = NULL;
}

/*
 * __wt_ref_key --
 *	Return a reference to a row-store internal page key as cheaply as
 * possible.
 */
static inline void
__wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, size_t *sizep)
{
	uintptr_t v;

	/*
	 * An internal page key is in one of two places: if we instantiated the
	 * key (for example, when reading the page), WT_REF.ref_ikey references
	 * a WT_IKEY structure, otherwise WT_REF.ref_ikey references an on-page
	 * key offset/length pair.
	 *
	 * Now the magic: allocated memory must be aligned to store any standard
	 * type, and we expect some standard type to require at least quad-byte
	 * alignment, so allocated memory should have some clear low-order bits.
	 * On-page objects consist of an offset/length pair: the maximum page
	 * size currently fits into 29 bits, so we use the low-order bits of the
	 * pointer to mark the other bits of the pointer as encoding the key's
	 * location and length.  This breaks if allocated memory isn't aligned,
	 * of course.
	 *
	 * In this specific case, we use bit 0x01 to mark an on-page key, else
	 * it's a WT_IKEY reference.  The bit pattern for internal row-store
	 * on-page keys is:
	 *	32 bits		key length
	 *	31 bits		page offset of the key's bytes,
	 *	 1 bits		flags
	 */
#define	WT_IK_FLAG			0x01
#define	WT_IK_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 32)
#define	WT_IK_DECODE_KEY_LEN(v)		((v) >> 32)
#define	WT_IK_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 1)
#define	WT_IK_DECODE_KEY_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 1)
	v = (uintptr_t)ref->ref_ikey;
	if (v & WT_IK_FLAG) {
		*(void **)keyp =
		    WT_PAGE_REF_OFFSET(page, WT_IK_DECODE_KEY_OFFSET(v));
		*sizep = WT_IK_DECODE_KEY_LEN(v);
	} else {
		*(void **)keyp = WT_IKEY_DATA(ref->ref_ikey);
		*sizep = ((WT_IKEY *)ref->ref_ikey)->size;
	}
}

/*
 * __wt_ref_key_onpage_set --
 *	Set a WT_REF to reference an on-page key.
 */
static inline void
__wt_ref_key_onpage_set(WT_PAGE *page, WT_REF *ref, WT_CELL_UNPACK *unpack)
{
	uintptr_t v;

	/*
	 * See the comment in __wt_ref_key for an explanation of the magic.
	 */
	v = WT_IK_ENCODE_KEY_LEN(unpack->size) |
	    WT_IK_ENCODE_KEY_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->data)) |
	    WT_IK_FLAG;
	ref->ref_ikey = (void *)v;
}

/*
 * __wt_ref_key_instantiated --
 *	Return if a WT_REF key is instantiated.
 */
static inline WT_IKEY *
__wt_ref_key_instantiated(WT_REF *ref)
{
	uintptr_t v;

	/*
	 * See the comment in __wt_ref_key for an explanation of the magic.
	 */
	v = (uintptr_t)ref->ref_ikey;
	return (v & WT_IK_FLAG ? NULL : ref->ref_ikey);
}

/*
 * __wt_ref_key_clear --
 *	Clear a WT_REF key.
 */
static inline void
__wt_ref_key_clear(WT_REF *ref)
{
	/*
	 * The key union has 2 8B fields; this is equivalent to:
	 *
	 *	ref->ref_recno = WT_RECNO_OOB;
	 *	ref->ref_ikey = NULL;
	 */
	ref->ref_recno = 0;
}

/*
 * __wt_row_leaf_key_info --
 *	Return a row-store leaf page key referenced by a WT_ROW if it can be
 * had without unpacking a cell, and information about the cell, if the key
 * isn't cheaply available.
 */
static inline bool
__wt_row_leaf_key_info(WT_PAGE *page, void *copy,
    WT_IKEY **ikeyp, WT_CELL **cellp, void *datap, size_t *sizep)
{
	WT_IKEY *ikey;
	uintptr_t v;

	v = (uintptr_t)copy;

	/*
	 * A row-store leaf page key is in one of two places: if instantiated,
	 * the WT_ROW pointer references a WT_IKEY structure, otherwise, it
	 * references an on-page offset.  Further, on-page keys are in one of
	 * two states: if the key is a simple key (not an overflow key, prefix
	 * compressed or Huffman encoded, all of which are likely), the key's
	 * offset/size is encoded in the pointer.  Otherwise, the offset is to
	 * the key's on-page cell.
	 *
	 * Now the magic: allocated memory must be aligned to store any standard
	 * type, and we expect some standard type to require at least quad-byte
	 * alignment, so allocated memory should have some clear low-order bits.
	 * On-page objects consist of an offset/length pair: the maximum page
	 * size currently fits into 29 bits, so we use the low-order bits of the
	 * pointer to mark the other bits of the pointer as encoding the key's
	 * location and length.  This breaks if allocated memory isn't aligned,
	 * of course.
	 *
	 * In this specific case, we use bit 0x01 to mark an on-page cell, bit
	 * 0x02 to mark an on-page key, 0x03 to mark an on-page key/value pair,
	 * otherwise it's a WT_IKEY reference. The bit pattern for on-page cells
	 * is:
	 *	29 bits		page offset of the key's cell,
	 *	 2 bits		flags
	 *
	 * The bit pattern for on-page keys is:
	 *	32 bits		key length,
	 *	29 bits		page offset of the key's bytes,
	 *	 2 bits		flags
	 *
	 * But, while that allows us to skip decoding simple key cells, we also
	 * want to skip decoding the value cell in the case where the value cell
	 * is also simple/short.  We use bit 0x03 to mark an encoded on-page key
	 * and value pair.  The bit pattern for on-page key/value pairs is:
	 *	 9 bits		key length,
	 *	13 bits		value length,
	 *	20 bits		page offset of the key's bytes,
	 *	20 bits		page offset of the value's bytes,
	 *	 2 bits		flags
	 *
	 * These bit patterns are in-memory only, of course, so can be modified
	 * (we could even tune for specific workloads).  Generally, the fields
	 * are larger than the anticipated values being stored (512B keys, 8KB
	 * values, 1MB pages), hopefully that won't be necessary.
	 *
	 * This function returns a list of things about the key (instantiation
	 * reference, cell reference and key/length pair).  Our callers know
	 * the order in which we look things up and the information returned;
	 * for example, the cell will never be returned if we are working with
	 * an on-page key.
	 */
#define	WT_CELL_FLAG			0x01
#define	WT_CELL_ENCODE_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_CELL_DECODE_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 2)

#define	WT_K_FLAG			0x02
#define	WT_K_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 32)
#define	WT_K_DECODE_KEY_LEN(v)		((v) >> 32)
#define	WT_K_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_K_DECODE_KEY_OFFSET(v)	(((v) & 0xFFFFFFFF) >> 2)

#define	WT_KV_FLAG			0x03
#define	WT_KV_ENCODE_KEY_LEN(v)		((uintptr_t)(v) << 55)
#define	WT_KV_DECODE_KEY_LEN(v)		((v) >> 55)
#define	WT_KV_MAX_KEY_LEN		(0x200 - 1)
#define	WT_KV_ENCODE_VALUE_LEN(v)	((uintptr_t)(v) << 42)
#define	WT_KV_DECODE_VALUE_LEN(v)	(((v) & 0x007FFC0000000000) >> 42)
#define	WT_KV_MAX_VALUE_LEN		(0x2000 - 1)
#define	WT_KV_ENCODE_KEY_OFFSET(v)	((uintptr_t)(v) << 22)
#define	WT_KV_DECODE_KEY_OFFSET(v)	(((v) & 0x000003FFFFC00000) >> 22)
#define	WT_KV_MAX_KEY_OFFSET		(0x100000 - 1)
#define	WT_KV_ENCODE_VALUE_OFFSET(v)	((uintptr_t)(v) << 2)
#define	WT_KV_DECODE_VALUE_OFFSET(v)	(((v) & 0x00000000003FFFFC) >> 2)
#define	WT_KV_MAX_VALUE_OFFSET		(0x100000 - 1)
	switch (v & 0x03) {
	case WT_CELL_FLAG:
		/* On-page cell: no instantiated key. */
		if (ikeyp != NULL)
			*ikeyp = NULL;
		if (cellp != NULL)
			*cellp =
			    WT_PAGE_REF_OFFSET(page, WT_CELL_DECODE_OFFSET(v));
		return (false);
	case WT_K_FLAG:
		/* Encoded key: no instantiated key, no cell. */
		if (cellp != NULL)
			*cellp = NULL;
		if (ikeyp != NULL)
			*ikeyp = NULL;
		if (datap != NULL) {
			*(void **)datap =
			    WT_PAGE_REF_OFFSET(page, WT_K_DECODE_KEY_OFFSET(v));
			*sizep = WT_K_DECODE_KEY_LEN(v);
			return (true);
		}
		return (false);
	case WT_KV_FLAG:
		/* Encoded key/value pair: no instantiated key, no cell. */
		if (cellp != NULL)
			*cellp = NULL;
		if (ikeyp != NULL)
			*ikeyp = NULL;
		if (datap != NULL) {
			*(void **)datap = WT_PAGE_REF_OFFSET(
			    page, WT_KV_DECODE_KEY_OFFSET(v));
			*sizep = WT_KV_DECODE_KEY_LEN(v);
			return (true);
		}
		return (false);

	}

	/* Instantiated key. */
	ikey = copy;
	if (ikeyp != NULL)
		*ikeyp = copy;
	if (cellp != NULL)
		*cellp = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
	if (datap != NULL) {
		*(void **)datap = WT_IKEY_DATA(ikey);
		*sizep = ikey->size;
		return (true);
	}
	return (false);
}

/*
 * __wt_row_leaf_key_set_cell --
 *	Set a WT_ROW to reference an on-page row-store leaf cell.
 */
static inline void
__wt_row_leaf_key_set_cell(WT_PAGE *page, WT_ROW *rip, WT_CELL *cell)
{
	uintptr_t v;

	/*
	 * See the comment in __wt_row_leaf_key_info for an explanation of the
	 * magic.
	 */
	v = WT_CELL_ENCODE_OFFSET(WT_PAGE_DISK_OFFSET(page, cell)) |
	    WT_CELL_FLAG;
	WT_ROW_KEY_SET(rip, v);
}

/*
 * __wt_row_leaf_key_set --
 *	Set a WT_ROW to reference an on-page row-store leaf key.
 */
static inline void
__wt_row_leaf_key_set(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK *unpack)
{
	uintptr_t v;

	/*
	 * See the comment in __wt_row_leaf_key_info for an explanation of the
	 * magic.
	 */
	v = WT_K_ENCODE_KEY_LEN(unpack->size) |
	    WT_K_ENCODE_KEY_OFFSET(WT_PAGE_DISK_OFFSET(page, unpack->data)) |
	    WT_K_FLAG;
	WT_ROW_KEY_SET(rip, v);
}

/*
 * __wt_row_leaf_value_set --
 *	Set a WT_ROW to reference an on-page row-store leaf value.
 */
static inline void
__wt_row_leaf_value_set(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK *unpack)
{
	uintptr_t key_len, key_offset, value_offset, v;

	v = (uintptr_t)WT_ROW_KEY_COPY(rip);

	/*
	 * See the comment in __wt_row_leaf_key_info for an explanation of the
	 * magic.
	 */
	if (!(v & WT_K_FLAG))			/* Already an encoded key */
		return;

	key_len = WT_K_DECODE_KEY_LEN(v);	/* Key length */
	if (key_len > WT_KV_MAX_KEY_LEN)
		return;
	if (unpack->size > WT_KV_MAX_VALUE_LEN)	/* Value length */
		return;

	key_offset = WT_K_DECODE_KEY_OFFSET(v);	/* Page offsets */
	if (key_offset > WT_KV_MAX_KEY_OFFSET)
		return;
	value_offset = WT_PAGE_DISK_OFFSET(page, unpack->data);
	if (value_offset > WT_KV_MAX_VALUE_OFFSET)
		return;

	v = WT_KV_ENCODE_KEY_LEN(key_len) |
	    WT_KV_ENCODE_VALUE_LEN(unpack->size) |
	    WT_KV_ENCODE_KEY_OFFSET(key_offset) |
	    WT_KV_ENCODE_VALUE_OFFSET(value_offset) | WT_KV_FLAG;
	WT_ROW_KEY_SET(rip, v);
}

/*
 * __wt_row_leaf_key --
 *	Set a buffer to reference a row-store leaf page key as cheaply as
 * possible.
 */
static inline int
__wt_row_leaf_key(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_ROW *rip, WT_ITEM *key, bool instantiate)
{
	void *copy;

	/*
	 * A front-end for __wt_row_leaf_key_work, here to inline fast paths.
	 *
	 * The row-store key can change underfoot; explicitly take a copy.
	 */
	copy = WT_ROW_KEY_COPY(rip);

	/*
	 * All we handle here are on-page keys (which should be a common case),
	 * and instantiated keys (which start out rare, but become more common
	 * as a leaf page is searched, instantiating prefix-compressed keys).
	 */
	if (__wt_row_leaf_key_info(
	    page, copy, NULL, NULL, &key->data, &key->size))
		return (0);

	/*
	 * The alternative is an on-page cell with some kind of compressed or
	 * overflow key that's never been instantiated.  Call the underlying
	 * worker function to figure it out.
	 */
	return (__wt_row_leaf_key_work(session, page, rip, key, instantiate));
}

/*
 * __wt_cursor_row_leaf_key --
 *	Set a buffer to reference a cursor-referenced row-store leaf page key.
 */
static inline int
__wt_cursor_row_leaf_key(WT_CURSOR_BTREE *cbt, WT_ITEM *key)
{
	WT_PAGE *page;
	WT_ROW *rip;
	WT_SESSION_IMPL *session;

	/*
	 * If the cursor references a WT_INSERT item, take the key from there,
	 * else take the key from the original page.
	 */
	if (cbt->ins == NULL) {
		session = (WT_SESSION_IMPL *)cbt->iface.session;
		page = cbt->ref->page;
		rip = &page->u.row.d[cbt->slot];
		WT_RET(__wt_row_leaf_key(session, page, rip, key, false));
	} else {
		key->data = WT_INSERT_KEY(cbt->ins);
		key->size = WT_INSERT_KEY_SIZE(cbt->ins);
	}
	return (0);
}

/*
 * __wt_row_leaf_value_cell --
 *	Return a pointer to the value cell for a row-store leaf page key, or
 * NULL if there isn't one.
 */
static inline WT_CELL *
__wt_row_leaf_value_cell(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK *kpack)
{
	WT_CELL *kcell, *vcell;
	WT_CELL_UNPACK unpack;
	void *copy, *key;
	size_t size;

	/* If we already have an unpacked key cell, use it. */
	if (kpack != NULL)
		vcell = (WT_CELL *)
		    ((uint8_t *)kpack->cell + __wt_cell_total_len(kpack));
	else {
		/*
		 * The row-store key can change underfoot; explicitly take a
		 * copy.
		 */
		copy = WT_ROW_KEY_COPY(rip);

		/*
		 * Figure out where the key is, step past it to the value cell.
		 * The test for a cell not being set tells us that we have an
		 * on-page key, otherwise we're looking at an instantiated key
		 * or on-page cell, both of which require an unpack of the key's
		 * cell to find the value cell that follows.
		 */
		if (__wt_row_leaf_key_info(
		    page, copy, NULL, &kcell, &key, &size) && kcell == NULL)
			vcell = (WT_CELL *)((uint8_t *)key + size);
		else {
			__wt_cell_unpack(kcell, &unpack);
			vcell = (WT_CELL *)((uint8_t *)
			    unpack.cell + __wt_cell_total_len(&unpack));
		}
	}

	return (__wt_cell_leaf_value_parse(page, vcell));
}

/*
 * __wt_row_leaf_value --
 *	Return the value for a row-store leaf page encoded key/value pair.
 */
static inline bool
__wt_row_leaf_value(WT_PAGE *page, WT_ROW *rip, WT_ITEM *value)
{
	uintptr_t v;

	/* The row-store key can change underfoot; explicitly take a copy. */
	v = (uintptr_t)WT_ROW_KEY_COPY(rip);

	/*
	 * See the comment in __wt_row_leaf_key_info for an explanation of the
	 * magic.
	 */
	if ((v & 0x03) == WT_KV_FLAG) {
		value->data =
		    WT_PAGE_REF_OFFSET(page, WT_KV_DECODE_VALUE_OFFSET(v));
		value->size = WT_KV_DECODE_VALUE_LEN(v);
		return (true);
	}
	return (false);
}

/*
 * __wt_ref_info --
 *	Return the addr/size and type triplet for a reference.
 */
static inline void
__wt_ref_info(WT_REF *ref, const uint8_t **addrp, size_t *sizep, u_int *typep)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK *unpack, _unpack;

	addr = ref->addr;
	unpack = &_unpack;

	/*
	 * If NULL, there is no location.
	 * If off-page, the pointer references a WT_ADDR structure.
	 * If on-page, the pointer references a cell.
	 *
	 * The type is of a limited set: internal, leaf or no-overflow leaf.
	 */
	if (addr == NULL) {
		*addrp = NULL;
		*sizep = 0;
		if (typep != NULL)
			*typep = 0;
	} else if (__wt_off_page(ref->home, addr)) {
		*addrp = addr->addr;
		*sizep = addr->size;
		if (typep != NULL)
			switch (addr->type) {
			case WT_ADDR_INT:
				*typep = WT_CELL_ADDR_INT;
				break;
			case WT_ADDR_LEAF:
				*typep = WT_CELL_ADDR_LEAF;
				break;
			case WT_ADDR_LEAF_NO:
				*typep = WT_CELL_ADDR_LEAF_NO;
				break;
			default:
				*typep = 0;
				break;
			}
	} else {
		__wt_cell_unpack((WT_CELL *)addr, unpack);
		*addrp = unpack->data;
		*sizep = unpack->size;
		if (typep != NULL)
			*typep = unpack->type;
	}
}

/*
 * __wt_ref_block_free --
 *	Free the on-disk block for a reference and clear the address.
 */
static inline int
__wt_ref_block_free(WT_SESSION_IMPL *session, WT_REF *ref)
{
	const uint8_t *addr;
	size_t addr_size;

	if (ref->addr == NULL)
		return (0);

	__wt_ref_info(ref, &addr, &addr_size, NULL);
	WT_RET(__wt_btree_block_free(session, addr, addr_size));

	/* Clear the address (so we don't free it twice). */
	__wt_ref_addr_free(session, ref);
	return (0);
}

/*
 * __wt_leaf_page_can_split --
 *	Check whether a page can be split in memory.
 */
static inline bool
__wt_leaf_page_can_split(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT_HEAD *ins_head;
	WT_INSERT *ins;
	size_t size;
	int count;

	btree = S2BT(session);

	/*
	 * Only split a page once, otherwise workloads that update in the middle
	 * of the page could continually split without benefit.
	 */
	if (F_ISSET_ATOMIC(page, WT_PAGE_SPLIT_INSERT))
		return (false);

	/*
	 * Check for pages with append-only workloads. A common application
	 * pattern is to have multiple threads frantically appending to the
	 * tree. We want to reconcile and evict this page, but we'd like to
	 * do it without making the appending threads wait. See if it's worth
	 * doing a split to let the threads continue before doing eviction.
	 *
	 * Ignore anything other than large, dirty leaf pages. We depend on the
	 * page being dirty for correctness (the page must be reconciled again
	 * before being evicted after the split, information from a previous
	 * reconciliation will be wrong, so we can't evict immediately).
	 */
	if (page->memory_footprint < btree->splitmempage)
		return (false);
	if (WT_PAGE_IS_INTERNAL(page))
		return (false);
	if (!__wt_page_is_modified(page))
		return (false);

	/*
	 * There is no point doing an in-memory split unless there is a lot of
	 * data in the last skiplist on the page.  Split if there are enough
	 * items and the skiplist does not fit within a single disk page.
	 *
	 * Rather than scanning the whole list, walk a higher level, which
	 * gives a sample of the items -- at level 0 we have all the items, at
	 * level 1 we have 1/4 and at level 2 we have 1/16th.  If we see more
	 * than 30 items and more data than would fit in a disk page, split.
	 */
#define	WT_MIN_SPLIT_DEPTH	2
#define	WT_MIN_SPLIT_COUNT	30
#define	WT_MIN_SPLIT_MULTIPLIER 16      /* At level 2, we see 1/16th entries */

	ins_head = page->type == WT_PAGE_ROW_LEAF ?
	    (page->pg_row_entries == 0 ?
	    WT_ROW_INSERT_SMALLEST(page) :
	    WT_ROW_INSERT_SLOT(page, page->pg_row_entries - 1)) :
	    WT_COL_APPEND(page);
	if (ins_head == NULL)
		return (false);
	for (count = 0, size = 0, ins = ins_head->head[WT_MIN_SPLIT_DEPTH];
	    ins != NULL; ins = ins->next[WT_MIN_SPLIT_DEPTH]) {
		count += WT_MIN_SPLIT_MULTIPLIER;
		size += WT_MIN_SPLIT_MULTIPLIER *
		    (WT_INSERT_KEY_SIZE(ins) + WT_UPDATE_MEMSIZE(ins->upd));
		if (count > WT_MIN_SPLIT_COUNT &&
		    size > (size_t)btree->maxleafpage) {
			WT_STAT_FAST_CONN_INCR(session, cache_inmem_splittable);
			WT_STAT_FAST_DATA_INCR(session, cache_inmem_splittable);
			return (true);
		}
	}
	return (false);
}

/*
 * __wt_page_can_evict --
 *	Check whether a page can be evicted.
 */
static inline bool
__wt_page_can_evict(WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_splitp)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	bool modified;

	if (inmem_splitp != NULL)
		*inmem_splitp = false;

	btree = S2BT(session);
	page = ref->page;
	mod = page->modify;

	/* Pages that have never been modified can always be evicted. */
	if (mod == NULL)
		return (true);

	/*
	 * Check for in-memory splits before other eviction tests. If the page
	 * should split in-memory, return success immediately and skip more
	 * detailed eviction tests. We don't need further tests since the page
	 * won't be written or discarded from the cache.
	 */
	if (__wt_leaf_page_can_split(session, page)) {
		if (inmem_splitp != NULL)
			*inmem_splitp = true;
		return (true);
	}

	modified = __wt_page_is_modified(page);

	/*
	 * If the file is being checkpointed, we can't evict dirty pages:
	 * if we write a page and free the previous version of the page, that
	 * previous version might be referenced by an internal page already
	 * been written in the checkpoint, leaving the checkpoint inconsistent.
	 */
	if (btree->checkpointing != WT_CKPT_OFF && modified) {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_checkpoint);
		WT_STAT_FAST_DATA_INCR(session, cache_eviction_checkpoint);
		return (false);
	}

	/*
	 * We can't evict clean, multiblock row-store pages where the parent's
	 * key for the page is an overflow item, because the split into the
	 * parent frees the backing blocks for any no-longer-used overflow keys,
	 * which will corrupt the checkpoint's block management.
	 */
	if (btree->checkpointing != WT_CKPT_OFF &&
	    F_ISSET_ATOMIC(ref->home, WT_PAGE_OVERFLOW_KEYS))
		return (false);

	/*
	 * If a split created new internal pages, those newly created internal
	 * pages cannot be evicted until all threads are known to have exited
	 * the original parent page's index, because evicting an internal page
	 * discards its WT_REF array, and a thread traversing the original
	 * parent page index might see a freed WT_REF.
	 */
	if (WT_PAGE_IS_INTERNAL(page) &&
	    F_ISSET_ATOMIC(page, WT_PAGE_SPLIT_BLOCK))
		return (false);

	/*
	 * If the oldest transaction hasn't changed since the last time
	 * this page was written, it's unlikely we can make progress.
	 * Similarly, if the most recent update on the page is not yet
	 * globally visible, eviction will fail.  These heuristics
	 * attempt to avoid repeated attempts to evict the same page.
	 */
	if (modified &&
	    !F_ISSET(S2C(session)->cache, WT_CACHE_STUCK) &&
	    (mod->last_oldest_id == __wt_txn_oldest_id(session) ||
	    !__wt_txn_visible_all(session, mod->update_txn)))
		return (false);

	return (true);
}

/*
 * __wt_page_release_evict --
 *	Release a reference to a page, and attempt to immediately evict it.
 */
static inline int
__wt_page_release_evict(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	bool locked, too_big;

	btree = S2BT(session);
	page = ref->page;

	/*
	 * Take some care with order of operations: if we release the hazard
	 * reference without first locking the page, it could be evicted in
	 * between.
	 */
	locked = __wt_atomic_casv32(
	    &ref->state, WT_REF_MEM, WT_REF_LOCKED) ? true : false;
	if ((ret = __wt_hazard_clear(session, page)) != 0 || !locked) {
		if (locked)
			ref->state = WT_REF_MEM;
		return (ret == 0 ? EBUSY : ret);
	}

	(void)__wt_atomic_addv32(&btree->evict_busy, 1);

	too_big = page->memory_footprint > btree->maxmempage;
	if ((ret = __wt_evict(session, ref, false)) == 0) {
		if (too_big)
			WT_STAT_FAST_CONN_INCR(session, cache_eviction_force);
		else
			/*
			 * If the page isn't too big, we are evicting it because
			 * it had a chain of deleted entries that make traversal
			 * expensive.
			 */
			WT_STAT_FAST_CONN_INCR(
			    session, cache_eviction_force_delete);
	} else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_force_fail);

	(void)__wt_atomic_subv32(&btree->evict_busy, 1);

	return (ret);
}

/*
 * __wt_page_release --
 *	Release a reference to a page.
 */
static inline int
__wt_page_release(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = S2BT(session);

	/*
	 * Discard our hazard pointer.  Ignore pages we don't have and the root
	 * page, which sticks in memory, regardless.
	 */
	if (ref == NULL || ref->page == NULL || __wt_ref_is_root(ref))
		return (0);

	/*
	 * If hazard pointers aren't necessary for this file, we can't be
	 * evicting, we're done.
	 */
	if (F_ISSET(btree, WT_BTREE_IN_MEMORY))
		return (0);

	/*
	 * Attempt to evict pages with the special "oldest" read generation.
	 * This is set for pages that grow larger than the configured
	 * memory_page_max setting, when we see many deleted items, and when we
	 * are attempting to scan without trashing the cache.
	 *
	 * Fast checks if eviction is disabled for this handle, operation or
	 * tree, then perform a general check if eviction will be possible.
	 */
	page = ref->page;
	if (page->read_gen != WT_READGEN_OLDEST ||
	    LF_ISSET(WT_READ_NO_EVICT) ||
	    F_ISSET(session, WT_SESSION_NO_EVICTION) ||
	    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
	    !__wt_page_can_evict(session, ref, NULL))
		return (__wt_hazard_clear(session, page));

	WT_RET_BUSY_OK(__wt_page_release_evict(session, ref));
	return (0);
}

/*
 * __wt_page_swap_func --
 *	Swap one page's hazard pointer for another one when hazard pointer
 * coupling up/down the tree.
 */
static inline int
__wt_page_swap_func(
    WT_SESSION_IMPL *session, WT_REF *held, WT_REF *want, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_DECL_RET;
	bool acquired;

	/*
	 * This function is here to simplify the error handling during hazard
	 * pointer coupling so we never leave a hazard pointer dangling.  The
	 * assumption is we're holding a hazard pointer on "held", and want to
	 * acquire a hazard pointer on "want", releasing the hazard pointer on
	 * "held" when we're done.
	 *
	 * When walking the tree, we sometimes swap to the same page. Fast-path
	 * that to avoid thinking about error handling.
	 */
	if (held == want)
		return (0);

	/* Get the wanted page. */
	ret = __wt_page_in_func(session, want, flags
#ifdef HAVE_DIAGNOSTIC
	    , file, line
#endif
	    );

	/*
	 * Expected failures: page not found or restart. Our callers list the
	 * errors they're expecting to handle.
	 */
	if (LF_ISSET(WT_READ_NOTFOUND_OK) && ret == WT_NOTFOUND)
		return (WT_NOTFOUND);
	if (LF_ISSET(WT_READ_RESTART_OK) && ret == WT_RESTART)
		return (WT_RESTART);

	/* Discard the original held page on either success or error. */
	acquired = ret == 0;
	WT_TRET(__wt_page_release(session, held, flags));

	/* Fast-path expected success. */
	if (ret == 0)
		return (0);

	/*
	 * If there was an error at any point that our caller isn't prepared to
	 * handle, discard any page we acquired.
	 */
	if (acquired)
		WT_TRET(__wt_page_release(session, want, flags));

	/*
	 * If we're returning an error, don't let it be one our caller expects
	 * to handle as returned by page-in: the expectation includes the held
	 * page not having been released, and that's not the case.
	 */
	if (LF_ISSET(WT_READ_NOTFOUND_OK) && ret == WT_NOTFOUND)
		return (EINVAL);
	if (LF_ISSET(WT_READ_RESTART_OK) && ret == WT_RESTART)
		return (EINVAL);

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
	uint32_t i, j, hazard_size, max, session_cnt;

	conn = S2C(session);

	/*
	 * No lock is required because the session array is fixed size, but it
	 * may contain inactive entries.  We must review any active session
	 * that might contain a hazard pointer, so insert a barrier before
	 * reading the active session count.  That way, no matter what sessions
	 * come or go, we'll check the slots for all of the sessions that could
	 * have been active when we started our check.
	 */
	WT_STAT_FAST_CONN_INCR(session, cache_hazard_checks);
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0, j = 0, max = 0;
	    i < session_cnt; ++s, ++i) {
		if (!s->active)
			continue;
		WT_ORDERED_READ(hazard_size, s->hazard_size);
		if (s->hazard_size > max) {
			max = s->hazard_size;
			WT_STAT_FAST_CONN_SET(session,
			    cache_hazard_max, max);
		}
		for (hp = s->hazard; hp < s->hazard + hazard_size; ++hp) {
			++j;
			if (hp->page == page) {
				WT_STAT_FAST_CONN_INCRV(session,
				    cache_hazard_walks, j);
				return (hp);
			}
		}
	}
	WT_STAT_FAST_CONN_INCRV(session, cache_hazard_walks, j);
	return (NULL);
}

/*
 * __wt_skip_choose_depth --
 *	Randomly choose a depth for a skiplist insert.
 */
static inline u_int
__wt_skip_choose_depth(WT_SESSION_IMPL *session)
{
	u_int d;

	for (d = 1; d < WT_SKIP_MAXDEPTH &&
	    __wt_random(&session->rnd) < WT_SKIP_PROBABILITY; d++)
		;
	return (d);
}

/*
 * __wt_btree_lsm_over_size --
 *	Return if the size of an in-memory tree with a single leaf page is over
 * a specified maximum.  If called on anything other than a simple tree with a
 * single leaf page, returns true so our LSM caller will switch to a new tree.
 */
static inline bool
__wt_btree_lsm_over_size(WT_SESSION_IMPL *session, uint64_t maxsize)
{
	WT_BTREE *btree;
	WT_PAGE *child, *root;
	WT_PAGE_INDEX *pindex;
	WT_REF *first;

	btree = S2BT(session);
	root = btree->root.page;

	/* Check for a non-existent tree. */
	if (root == NULL)
		return (false);

	/* A tree that can be evicted always requires a switch. */
	if (!F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return (true);

	/* Check for a tree with a single leaf page. */
	WT_INTL_INDEX_GET(session, root, pindex);
	if (pindex->entries != 1)		/* > 1 child page, switch */
		return (true);

	first = pindex->index[0];
	if (first->state != WT_REF_MEM)		/* no child page, ignore */
		return (false);

	/*
	 * We're reaching down into the page without a hazard pointer, but
	 * that's OK because we know that no-eviction is set and so the page
	 * cannot disappear.
	 */
	child = first->page;
	if (child->type != WT_PAGE_ROW_LEAF)	/* not a single leaf page */
		return (true);

	return (child->memory_footprint > maxsize);
}

/*
 * __wt_split_descent_race --
 *	Return if we raced with an internal page split when descending the tree.
 */
static inline bool
__wt_split_descent_race(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX *saved_pindex)
{
	WT_PAGE_INDEX *pindex;

	/* No test when starting the descent (there's no home to check). */
	if (__wt_ref_is_root(ref))
		return (false);

	/*
	 * A place to hang this comment...
	 *
	 * There's a page-split race when we walk the tree: if we're splitting
	 * an internal page into its parent, we update the parent's page index
	 * before updating the split page's page index, and it's not an atomic
	 * update. A thread can read the parent page's original page index and
	 * then read the split page's replacement index.
	 *
	 * Because internal page splits work by truncating the original page to
	 * the initial part of the original page, the result of this race is we
	 * will have a search key that points past the end of the current page.
	 * This is only an issue when we search past the end of the page, if we
	 * find a WT_REF in the page with the namespace we're searching for, we
	 * don't care if the WT_REF moved or not while we were searching, we
	 * have the correct page.
	 *
	 * For example, imagine an internal page with 3 child pages, with the
	 * namespaces a-f, g-h and i-j; the first child page splits. The parent
	 * starts out with the following page-index:
	 *
	 *	| ... | a | g | i | ... |
	 *
	 * which changes to this:
	 *
	 *	| ... | a | c | e | g | i | ... |
	 *
	 * The child starts out with the following page-index:
	 *
	 *	| a | b | c | d | e | f |
	 *
	 * which changes to this:
	 *
	 *	| a | b |
	 *
	 * The thread searches the original parent page index for the key "cat",
	 * it couples to the "a" child page; if it uses the replacement child
	 * page index, it will search past the end of the page and couple to the
	 * "b" page, which is wrong.
	 *
	 * To detect the problem, we remember the parent page's page index used
	 * to descend the tree. Whenever we search past the end of a page, we
	 * check to see if the parent's page index has changed since our use of
	 * it during descent. As the problem only appears if we read the split
	 * page's replacement index, the parent page's index must already have
	 * changed, ensuring we detect the problem.
	 *
	 * It's possible for the opposite race to happen (a thread could read
	 * the parent page's replacement page index and then read the split
	 * page's original index). This isn't a problem because internal splits
	 * work by truncating the split page, so the split page search is for
	 * content the split page retains after the split, and we ignore this
	 * race.
	 */
	WT_INTL_INDEX_GET(session, ref->home, pindex);
	return (pindex != saved_pindex);
}
