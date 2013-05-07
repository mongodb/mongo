/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __evict_init_candidate(
    WT_SESSION_IMPL *, WT_EVICT_ENTRY *, WT_PAGE *);
static int  __evict_lru(WT_SESSION_IMPL *, int);
static int  __evict_lru_cmp(const void *, const void *);
static int  __evict_walk(WT_SESSION_IMPL *, uint32_t *, int);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *, int);
static int  __evict_worker(WT_SESSION_IMPL *);

/*
 * __evict_read_gen --
 *	Get the adjusted read generation for an eviction entry.
 */
static inline uint64_t
__evict_read_gen(const WT_EVICT_ENTRY *entry)
{
	WT_PAGE *page;
	uint64_t read_gen;

	page = entry->page;

	/* Never prioritize empty slots. */
	if (page == NULL)
		return (UINT64_MAX);

	read_gen = page->read_gen + entry->btree->evict_priority;

	/*
	 * Skew the read generation for internal pages that aren't split merge
	 * pages.  We want to consider leaf pages in preference to real internal
	 * pages, but merges are relatively cheap in-memory operations that make
	 * reads faster, so don't make them too unlikely.
	 */
	if ((page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_COL_INT) &&
	    !__wt_btree_mergeable(page))
		read_gen += WT_EVICT_INT_SKEW;

	return (read_gen);
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort the eviction array.
 */
static int
__evict_lru_cmp(const void *a, const void *b)
{
	uint64_t a_lru, b_lru;

	a_lru = __evict_read_gen(a);
	b_lru = __evict_read_gen(b);

	return ((a_lru < b_lru) ? -1 : (a_lru == b_lru) ? 0 : 1);
}

/*
 * __evict_list_clr --
 *	Clear an entry in the LRU eviction list.
 */
static inline void
__evict_list_clr(WT_SESSION_IMPL *session, WT_EVICT_ENTRY *e)
{
	if (e->page != NULL) {
		WT_ASSERT(session, F_ISSET_ATOMIC(e->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->page, WT_PAGE_EVICT_LRU);
	}
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __wt_evict_list_clr_page --
 *	Make sure a page is not in the LRU eviction list.  This called from the
 * page eviction code to make sure there is no attempt to evict a child page
 * multiple times.
 */
void
__wt_evict_list_clr_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t i, elem;

	WT_ASSERT(session, WT_PAGE_IS_ROOT(page) ||
	    page->ref->page != page ||
	    page->ref->state == WT_REF_EVICT_WALK ||
	    page->ref->state == WT_REF_LOCKED);

	/* Fast path: if the page isn't on the queue, don't bother searching. */
	if (!F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, &cache->evict_lock);

	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++)
		if (evict->page == page) {
			__evict_list_clr(session, evict);
			break;
		}

	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU));

	__wt_spin_unlock(session, &cache->evict_lock);
}

/*
 * __wt_evict_server_wake --
 *	Wake the eviction server thread.
 */
int
__wt_evict_server_wake(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max;

	conn = S2C(session);
	cache = conn->cache;
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = conn->cache_size;

	WT_VERBOSE_RET(session, evictserver,
	    "waking, bytes inuse %s max (%" PRIu64 "MB %s %" PRIu64 "MB)",
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_inuse / WT_MEGABYTE,
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_max / WT_MEGABYTE);

	return (__wt_cond_signal(session, cache->evict_cond));
}

/*
 * __wt_cache_evict_server --
 *	Thread to evict pages from the cache.
 */
void *
__wt_cache_evict_server(void *arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)) {
		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));

		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;

		WT_VERBOSE_ERR(session, evictserver, "sleeping");
		/* Don't rely on signals: check periodically. */
		WT_ERR(__wt_cond_wait(session, cache->evict_cond, 100000));
		WT_VERBOSE_ERR(session, evictserver, "waking");
	}

	WT_VERBOSE_ERR(session, evictserver, "exiting");

	if (ret == 0) {
		if (cache->pages_inmem != cache->pages_evict)
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64 " pages in "
			    "memory and %" PRIu64 " pages evicted",
			    cache->pages_inmem, cache->pages_evict);
		if (cache->bytes_inmem != cache->bytes_evict)
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64 " bytes in "
			    "memory and %" PRIu64 " bytes evicted",
			    cache->bytes_inmem, cache->bytes_evict);
		if (cache->bytes_dirty != 0 || cache->pages_dirty != 0)
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64
			    " bytes dirty and %" PRIu64 " pages dirty",
			    cache->bytes_dirty, cache->pages_dirty);
	} else
err:		WT_PANIC_ERR(session, ret, "eviction server error");

	/* Close the eviction session and free its hazard array. */
	(void)session->iface.close(&session->iface, NULL);
	__wt_free(conn->default_session, session->hazard);

	return (NULL);
}

/*
 * __evict_worker --
 *	Evict pages from memory.
 */
static int
__evict_worker(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;
	int clean, loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/*
		 * Keep evicting until we hit the target cache usage and the
		 * target dirty percentage.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		dirty_inuse = __wt_cache_bytes_dirty(cache);
		bytes_max = conn->cache_size;
		if (bytes_inuse < (cache->eviction_target * bytes_max) / 100 &&
		    dirty_inuse <
		    (cache->eviction_dirty_target * bytes_max) / 100)
			break;

		WT_VERBOSE_RET(session, evictserver,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64,
		    bytes_max, bytes_inuse, dirty_inuse);

		/*
		 * Either the cache is too large or there are too many dirty
		 * pages (or both).  Ignore clean pages unless the cache is
		 * too large.
		 */
		clean = 0;
		if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
			clean = 1;

		/*
		 * Track whether pages are being evicted.  This will be cleared
		 * by the next thread to successfully evict a page.
		 */
		F_SET(cache, WT_EVICT_NO_PROGRESS);
		WT_RET(__evict_lru(session, clean));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, mark the cache "stuck" and go back to
		 * sleep, it's not something we can fix.
		 */
		if (F_ISSET(cache, WT_EVICT_NO_PROGRESS)) {
			if (loop == 100) {
				F_SET(cache, WT_EVICT_STUCK);
				WT_CSTAT_INCR(session, cache_eviction_slow);
				WT_VERBOSE_RET(session, evictserver,
				    "unable to reach eviction goal");
				break;
			}
		} else
			loop = 0;
	}
	return (0);
}

/*
 * __wt_evict_clear_tree_walk --
 *	Clear the tree's current eviction point, acquiring the eviction lock.
 */
void
__wt_evict_clear_tree_walk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_REF *ref;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	__wt_spin_lock(session, &cache->evict_walk_lock);

	/* If no page stack specified, clear the standard eviction stack. */
	if (page == NULL) {
		page = btree->evict_page;
		btree->evict_page = NULL;
	}

	/* Clear the current eviction point. */
	while (page != NULL && !WT_PAGE_IS_ROOT(page)) {
		ref = page->ref;
		page = page->parent;
		WT_ASSERT(session, page != btree->evict_page);
		if (ref->state == WT_REF_EVICT_WALK)
			ref->state = WT_REF_MEM;
	}

	__wt_spin_unlock(session, &cache->evict_walk_lock);
}

/*
 * __wt_evict_page --
 *	Evict a given page.
 */
int
__wt_evict_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_DECL_RET;
	WT_TXN saved_txn, *txn;
	int was_running;

	/* Fast path for clean pages. */
	if (!__wt_page_is_modified(page))
		return (__wt_rec_evict(session, page, 0));

	/*
	 * We have to take care when evicting pages not to write a change that:
	 *  (a) is not yet committed; or
	 *  (b) is committed more recently than an in-progress checkpoint.
	 *
	 * We handle both of these cases by setting up the transaction context
	 * before evicting, using the oldest reading ID in the system to create
	 * the snapshot.  If a transaction is in progress in the evicting
	 * session, we save and restore its state.
	 */
	txn = &session->txn;
	saved_txn = *txn;
	was_running = (F_ISSET(txn, TXN_RUNNING) != 0);

	if (was_running)
		WT_RET(__wt_txn_init(session));

	__wt_txn_get_evict_snapshot(session);
	txn->isolation = TXN_ISO_READ_COMMITTED;
	ret = __wt_rec_evict(session, page, 0);

	if (was_running) {
		WT_ASSERT(session, txn->snapshot == NULL ||
		    txn->snapshot != saved_txn.snapshot);
		__wt_txn_destroy(session);
	} else
		__wt_txn_release_snapshot(session);

	/* If the oldest transaction was updated, keep the newer value. */
	saved_txn.oldest_snap_min = txn->oldest_snap_min;

	*txn = saved_txn;
	return (ret);
}

/*
 * __wt_evict_file --
 *	Flush pages for a specific file as part of a close or compact operation.
 */
int
__wt_evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	WT_PAGE *next_page, *page;
	u_int i, elem;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*
	 * We need exclusive access to the file -- disable ordinary eviction.
	 *
	 * Hold the walk lock to set the "no eviction" flag: no new pages will
	 * be queued for eviction after this point.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	F_SET(btree, WT_BTREE_NO_EVICTION);
	__wt_spin_unlock(session, &cache->evict_walk_lock);

	/* Hold the evict lock to remove any queued pages from this file. */
	__wt_spin_lock(session, &cache->evict_lock);

	/* Clear any existing LRU eviction walk, we're discarding the tree. */
	__wt_evict_clear_tree_walk(session, NULL);

	/*
	 * The eviction candidate list might reference pages we are about to
	 * discard; clear it.
	 */
	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++)
		if (evict->btree == btree)
			__evict_list_clr(session, evict);
	__wt_spin_unlock(session, &cache->evict_lock);

	/*
	 * We have disabled further eviction: wait for concurrent LRU eviction
	 * activity to drain.
	 */
	while (btree->lru_count > 0)
		__wt_yield();

	/*
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always walk one page ahead of the page being evicted.
	 */
	next_page = NULL;
	WT_RET(__wt_tree_walk(session, &next_page, WT_TREE_EVICT));
	while ((page = next_page) != NULL) {
		WT_ERR(__wt_tree_walk(session, &next_page, WT_TREE_EVICT));

		switch (syncop) {
		case WT_SYNC_DISCARD:
			/*
			 * Eviction can fail when a page in the evicted page's
			 * subtree switches state.  For example, if we don't
			 * evict a page marked empty, because we expect it to
			 * be merged into its parent, it might no longer be
			 * empty after it's reconciled, in which case eviction
			 * of its parent would fail.  We can either walk the
			 * tree multiple times, until it's eventually empty,
			 * or immediately reconcile the page to get it to its
			 * final state before considering if it's an eviction
			 * target.
			 *
			 * We could limit this test to empty pages (only empty
			 * pages can switch state this way, split pages always
			 * merge into their parent, no matter what), but I see
			 * no reason to do that now.
			 */
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(
				    session, page, NULL, WT_SKIP_UPDATE_ERR));

			/*
			 * Evict the page.
			 * Do not attempt to evict pages expected to be merged
			 * into their parents, with the single exception that
			 * the root page can't be merged into anything, it must
			 * be written.
			 */
			if (WT_PAGE_IS_ROOT(page) || page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_EMPTY |
			    WT_PM_REC_SPLIT | WT_PM_REC_SPLIT_MERGE))
				WT_ERR(__wt_rec_evict(session, page, 1));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			/*
			 * Discard the page, whether clean or dirty.
			 * Before we discard the root page, clear the reference
			 * from the btree handle.  This is necessary so future
			 * evictions don't see the handle's root page reference
			 * pointing to freed memory.
			 */
			if (WT_PAGE_IS_ROOT(page))
				btree->root_page = NULL;
			if (__wt_page_is_modified(page))
				__wt_cache_dirty_decr(
				    session, page->memory_footprint);
			__wt_page_out(session, &page);
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_page != NULL)
			__wt_evict_clear_tree_walk(session, next_page);
	}
	WT_ASSERT(session, btree->evict_page == NULL);
	F_CLR(btree, WT_BTREE_NO_EVICTION);
	return (ret);
}

/*
 * __wt_sync_file --
 *	Flush pages for a specific file as part of a checkpoint or compaction
 * operation.
 */
int
__wt_sync_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_TXN *txn;
	uint32_t flags;

	btree = S2BT(session);
	cache = S2C(session)->cache;
	page = NULL;
	txn = &session->txn;

	switch (syncop) {
	case WT_SYNC_CHECKPOINT:
	case WT_SYNC_WRITE_LEAVES:
		/*
		 * The first pass walks all cache leaf pages, waiting for
		 * concurrent activity in a page to be resolved, acquiring
		 * hazard references to prevent eviction.
		 */
		flags = WT_TREE_CACHE | WT_TREE_SKIP_INTL;
		if (syncop == WT_SYNC_CHECKPOINT)
			flags |= WT_TREE_WAIT;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			/* Write dirty pages if nobody beat us to it. */
			if (__wt_page_is_modified(page)) {
				if (txn->isolation == TXN_ISO_READ_COMMITTED)
					__wt_txn_get_snapshot(session,
					    WT_TXN_NONE, WT_TXN_NONE, 0);
				ret = __wt_rec_write(session, page, NULL, 0);
				if (txn->isolation == TXN_ISO_READ_COMMITTED)
					__wt_txn_release_snapshot(session);
				WT_ERR(ret);
			}

			WT_ERR(__wt_tree_walk(session, &page, flags));
		}

		if (syncop == WT_SYNC_WRITE_LEAVES)
			break;

		/*
		 * Pages cannot disappear from underneath internal pages when
		 * internal pages are being reconciled by checkpoint; also,
		 * pages in a checkpoint cannot be freed until the block lists
		 * for the checkpoint are stable.  Eviction is disabled in the
		 * subtree of any internal page being reconciled, including,
		 * eventually, the whole tree when the root page is written.
		 *
		 * Set the checkpointing flag, it is checked in __rec_review
		 * before any page is evicted.
		 *
		 * If any thread is already in the progress of evicting a page,
		 * it will have switched the ref state to WT_REF_LOCKED while
		 * holding evict_lock inside __evict_get_page, and the
		 * checkpoint will notice and wait for eviction to complete
		 * before proceeding.
		 */
		__wt_spin_lock(session, &cache->evict_lock);
		btree->checkpointing = 1;
		__wt_spin_unlock(session, &cache->evict_lock);

		/*
		 * The second pass walks all cache internal pages, waiting for
		 * concurrent activity to be resolved.  We don't acquire hazard
		 * references in this pass, using the EVICT_WALK state prevents
		 * eviction from getting underneath an internal page that is
		 * being evicted.
		 */
		flags = WT_TREE_EVICT | WT_TREE_SKIP_LEAF | WT_TREE_WAIT;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			/* Write dirty pages. */
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(session, page, NULL, 0));
			WT_ERR(__wt_tree_walk(session, &page, flags));
		}
		break;
	case WT_SYNC_COMPACT:
		/*
		 * Compaction requires only a single pass (we don't have to turn
		 * eviction off when visiting internal nodes, so we don't bother
		 * breaking the work into two separate passes).   Wait for
		 * concurrent activity in a page to be resolved, acquire hazard
		 * references to prevent eviction.
		 */
		flags = WT_TREE_CACHE | WT_TREE_WAIT;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			WT_ERR(__wt_compact_evict(session, page));
			WT_ERR(__wt_tree_walk(session, &page, flags));
		}
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	/* On error, clear any left-over tree walk. */
	if (page != NULL)
		__wt_evict_clear_tree_walk(session, page);

	if (btree->checkpointing) {
		btree->checkpointing = 0;

		/*
		 * Wake the eviction server, in case application threads have
		 * stalled while the eviction server decided it couldn't make
		 * progress.  Without this, application threads will be stalled
		 * until the eviction server next wakes.
		 */
		WT_TRET(__wt_evict_server_wake(session));
	}

	return (ret);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(WT_SESSION_IMPL *session, int clean)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	uint64_t cutoff;
	uint32_t i, candidates;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session, &candidates, clean));

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);

	qsort(cache->evict,
	    candidates, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	while (candidates > 0 && cache->evict[candidates - 1].page == NULL)
		--candidates;

	cache->evict_entries = candidates;

	if (candidates == 0) {
		__wt_spin_unlock(session, &cache->evict_lock);
		return (0);
	}

	/* Find the bottom 25% of read generations. */
	cutoff = (3 * __evict_read_gen(&cache->evict[0]) +
	    __evict_read_gen(&cache->evict[candidates - 1])) / 4;

	/*
	 * Don't take less than 10% or more than 50% of candidates, regardless.
	 * That said, if there is only one candidate page, which is normal when
	 * populating an empty file, don't exclude it.
	 */
	for (i = candidates / 10; i < candidates / 2; i++)
		if (__evict_read_gen(&cache->evict[i]) > cutoff)
			break;
	cache->evict_candidates = i + 1;

	/* If we have more than the minimum number of entries, clear them. */
	if (cache->evict_entries > WT_EVICT_WALK_BASE) {
		for (i = WT_EVICT_WALK_BASE, evict = cache->evict + i;
		    i < cache->evict_entries;
		    i++, evict++)
			__evict_list_clr(session, evict);
		cache->evict_entries = WT_EVICT_WALK_BASE;
	}

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->evict_lock);

	/*
	 * Signal any application threads waiting for the eviction queue to
	 * have candidates.
	 */
	WT_RET(__wt_cond_signal(session, cache->evict_waiter_cond));

	/*
	 * Reconcile and discard some pages: EBUSY is returned if a page fails
	 * eviction because it's unavailable, continue in that case.
	 */
	while ((ret = __wt_evict_lru_page(session, 0)) == 0 || ret == EBUSY)
		;
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session, u_int *entriesp, int clean)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int file_count, i, max_entries, retries;

	conn = S2C(session);
	cache = S2C(session)->cache;
	retries = 0;

	/* Update the oldest transaction ID -- we use it to filter pages. */
	__wt_txn_get_oldest(session);

	/*
	 * NOTE: we don't hold the schema lock, so we have to take care
	 * that the handles we see are open and valid.
	 */
	i = cache->evict_entries;
	max_entries = i + WT_EVICT_WALK_INCR;
retry:	file_count = 0;
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;
		if (file_count++ < cache->evict_file_next)
			continue;
		btree = dhandle->handle;

		/*
		 * Skip files that aren't open or don't have a root page.
		 *
		 * Also skip files marked as cache-resident, and files
		 * potentially involved in a bulk load.  The real problem is
		 * eviction doesn't want to be walking the file as it converts
		 * to a bulk-loaded object, and empty trees aren't worth trying
		 * to evict, anyway.
		 */
		if (btree->root_page == NULL ||
		    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
		    btree->bulk_load_ok)
			continue;

		__wt_spin_lock(session, &cache->evict_walk_lock);

		/*
		 * Re-check the "no eviction" flag -- it is used to enforce
		 * exclusive access when a handle is being closed.
		 */
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION))
			WT_WITH_BTREE(session, btree,
			    ret = __evict_walk_file(session, &i, clean));

		__wt_spin_unlock(session, &cache->evict_walk_lock);

		if (ret != 0 || i == max_entries)
			break;
	}
	cache->evict_file_next = (dhandle == NULL) ? 0 : file_count;

	/* Walk the files a few times if we don't find enough pages. */
	if (ret == 0 && i < cache->evict_slots && retries++ < 10)
		goto retry;

	*entriesp = i;
	return (ret);
}

/*
 * __evict_init_candidate --
 *	Initialize a WT_EVICT_ENTRY structure with a given page.
 */
static void
__evict_init_candidate(
    WT_SESSION_IMPL *session, WT_EVICT_ENTRY *evict, WT_PAGE *page)
{
	WT_CACHE *cache;
	u_int slot;

	cache = S2C(session)->cache;

	/* Keep track of the maximum slot we are using. */
	slot = (u_int)(evict - cache->evict);
	if (slot >= cache->evict_max)
		cache->evict_max = slot + 1;

	if (evict->page != NULL)
		__evict_list_clr(session, evict);
	evict->page = page;
	evict->btree = S2BT(session);

	/* Mark the page on the list */
	F_SET_ATOMIC(page, WT_PAGE_EVICT_LRU);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(WT_SESSION_IMPL *session, u_int *slotp, int clean)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	wt_txnid_t oldest_txn;
	int modified, restarts, levels;

	btree = S2BT(session);
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = start + WT_EVICT_WALK_PER_FILE;
	if (end > cache->evict + cache->evict_slots)
		end = cache->evict + cache->evict_slots;
	oldest_txn = session->txn.oldest_snap_min;

	WT_ASSERT(session, btree->evict_page == NULL ||
	    WT_PAGE_IS_ROOT(btree->evict_page) ||
	    btree->evict_page->ref->state == WT_REF_EVICT_WALK);

	/*
	 * Get some more eviction candidate pages.
	 */
	for (evict = start, restarts = 0;
	    evict < end && ret == 0;
	    ret = __wt_tree_walk(session, &btree->evict_page, WT_TREE_EVICT)) {
		if ((page = btree->evict_page) == NULL) {
			/*
			 * Take care with terminating this loop.
			 *
			 * Don't make an extra call to __wt_tree_walk: that
			 * will leave a page in the WT_REF_EVICT_WALK state,
			 * unable to be evicted, which may prevent any work
			 * from being done.
			 */
			if (++restarts == 2)
				break;
			continue;
		}

		WT_CSTAT_INCR(session, cache_eviction_walk);

		/* Ignore root pages entirely. */
		if (WT_PAGE_IS_ROOT(page))
			continue;

		/* Look for a split-merge (grand)parent page to merge. */
		levels = 0;
		if (__wt_btree_mergeable(page))
			for (levels = 1;
			    levels < WT_MERGE_STACK_MIN &&
			    __wt_btree_mergeable(page->parent);
			    page = page->parent, levels++)
				;
		else if (page->modify != NULL &&
		    F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
			continue;

		/*
		 * Only look for a parent at exactly the right height above: if
		 * the stack is deep enough, we'll find it eventually, and we
		 * don't want to do too much work on every level.
		 *
		 * !!!
		 * Don't restrict ourselves to only the top-most page (that is,
		 * don't require that page->parent is not mergeable).  If there
		 * is a big, busy enough split-merge tree, the top-level merge
		 * will only happen if we can lock the whole subtree
		 * exclusively.  Consider smaller merges in case locking the
		 * whole tree fails.
		 */
		if (levels != 0 && levels != WT_MERGE_STACK_MIN)
			continue;

		/*
		 * If this page has never been considered for eviction, set its
		 * read generation to a little bit in the future and move on,
		 * give readers a chance to start updating the read generation.
		 */
		if (page->read_gen == WT_READ_GEN_NOTSET) {
			page->read_gen = __wt_cache_read_gen_set(session);
			continue;
		}

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		/* The following checks apply to eviction but not merges. */
		if (levels == 0) {
			/*
			 * If the file is being checkpointed, there's a period
			 * of time where we can't discard any page with a
			 * modification structure because it might race with
			 * the checkpointing thread.
			 *
			 * During this phase, there is little point trying to
			 * evict dirty pages: we might be lucky and find an
			 * internal page that has not yet been checkpointed,
			 * but much more likely is that we will waste effort
			 * considering dirty leaf pages that cannot be evicted
			 * because they have modifications more recent than the
			 * checkpoint.
			 */
			modified = __wt_page_is_modified(page);
			if (modified && btree->checkpointing)
				continue;

			/* Optionally ignore clean pages. */
			if (!modified && !clean)
				continue;

			/*
			 * If the oldest transaction hasn't changed since the
			 * last time this page was written, it's unlikely that
			 * we can make progress.  This is a heuristic that
			 * saves repeated attempts to evict the same page.
			 *
			 * That said, if eviction is stuck, try anyway: maybe a
			 * transaction that were running last time we wrote the
			 * page has since rolled back.
			 */
			if (modified &&
			    TXNID_LE(oldest_txn, page->modify->disk_txn) &&
			    !F_ISSET(cache, WT_EVICT_STUCK))
				continue;
		}

		WT_ASSERT(session, evict->page == NULL);
		__evict_init_candidate(session, evict, page);
		++evict;

		WT_VERBOSE_RET(session, evictserver,
		    "select: %p, size %" PRIu32, page, page->memory_footprint);
	}

	*slotp += (u_int)(evict - start);
	return (ret);
}

/*
 * __evict_get_page --
 *	Get a page for eviction.
 */
static int
__evict_get_page(
    WT_SESSION_IMPL *session, int is_app, WT_BTREE **btreep, WT_PAGE **pagep)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	WT_REF *ref;
	uint32_t candidates;

	cache = S2C(session)->cache;
	*btreep = NULL;
	*pagep = NULL;

	/*
	 * A pathological case: if we're the oldest transaction in the system
	 * and the eviction server is stuck trying to find space, abort the
	 * transaction to give up all hazard references before trying again.
	 */
	if (is_app && F_ISSET(cache, WT_EVICT_STUCK) &&
	    __wt_txn_am_oldest(session)) {
		F_CLR(cache, WT_EVICT_STUCK);
		WT_CSTAT_INCR(session, txn_fail_cache);
		return (WT_DEADLOCK);
	}

	candidates = cache->evict_candidates;
	/* The eviction server only considers half of the entries. */
	if (!is_app && candidates > 1)
		candidates /= 2;

	/*
	 * Avoid the LRU lock if no pages are available.  If there are pages
	 * available, spin until we get the lock.  If this function returns
	 * without getting a page to evict, application threads assume there
	 * are no more pages available and will attempt to wake the eviction
	 * server.
	 */
	for (;;) {
		if (cache->evict_current == NULL ||
		    cache->evict_current >= cache->evict + candidates)
			return (WT_NOTFOUND);
		if (__wt_spin_trylock(session, &cache->evict_lock) == 0)
			break;
		__wt_yield();
	}

	/* Get the next page queued for eviction. */
	while ((evict = cache->evict_current) != NULL &&
	    evict >= cache->evict && evict < cache->evict + candidates &&
	    evict->page != NULL) {
		WT_ASSERT(session, evict->btree != NULL);

		/* Move to the next item. */
		++cache->evict_current;

		/*
		 * In case something goes wrong, don't pick the same set of
		 * pages every time.
		 *
		 * We used to bump the page's read_gen only if eviction failed,
		 * but that isn't safe: at that point, eviction has already
		 * unlocked the page and some other thread may have evicted it
		 * by the time we look at it.
		 */
		evict->page->read_gen = __wt_cache_read_gen_set(session);

		/*
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, this operation will fail and we will move on.
		 */
		ref = evict->page->ref;
		WT_ASSERT(session, evict->page == ref->page);

		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
			__evict_list_clr(session, evict);
			continue;
		}

		/*
		 * Increment the LRU count in the btree handle to prevent it
		 * from being closed under us.
		 */
		(void)WT_ATOMIC_ADD(evict->btree->lru_count, 1);

		*btreep = evict->btree;
		*pagep = evict->page;

		/*
		 * Remove the entry so we never try to reconcile the same page
		 * on reconciliation error.
		 */
		__evict_list_clr(session, evict);
		break;
	}

	if (is_app && *pagep == NULL)
		cache->evict_current = NULL;
	__wt_spin_unlock(session, &cache->evict_lock);

	return ((*pagep == NULL) ? WT_NOTFOUND : 0);
}

/*
 * __wt_evict_lru_page --
 *	Called by both eviction and application threads to evict a page.
 */
int
__wt_evict_lru_page(WT_SESSION_IMPL *session, int is_app)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;

	WT_RET(__evict_get_page(session, is_app, &btree, &page));
	WT_ASSERT(session, page->ref->state == WT_REF_LOCKED);

	WT_WITH_BTREE(session, btree,
	    ret = __wt_evict_page(session, page));

	(void)WT_ATOMIC_SUB(btree->lru_count, 1);

	cache = S2C(session)->cache;
	if (ret == 0 && F_ISSET(cache, WT_EVICT_NO_PROGRESS | WT_EVICT_STUCK))
		F_CLR(cache, WT_EVICT_NO_PROGRESS | WT_EVICT_STUCK);

	return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump debugging information to stdout about the size of the files in the
 *	cache.
 *
 *	NOTE: this function is not called anywhere, it is intended to be called
 *	from a debugger.
 */
void
__wt_cache_dump(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_PAGE *page;
	uint64_t file_bytes, file_dirty, file_pages, total_bytes;

	conn = S2C(session);
	total_bytes = 0;

	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		btree = dhandle->handle;
		if (btree->root_page == NULL ||
		    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
		    btree->bulk_load_ok)
			continue;

		file_bytes = file_dirty = file_pages = 0;
		page = NULL;
		session->dhandle = dhandle;
		while (__wt_tree_walk(session, &page, WT_TREE_CACHE) == 0 &&
		    page != NULL) {
			++file_pages;
			file_bytes += page->memory_footprint;
			if (__wt_page_is_modified(page))
				file_dirty += page->memory_footprint;
		}
		session->dhandle = NULL;

		printf("cache dump: %s [%s]: %"
		    PRIu64 " pages, %" PRIu64 "MB, %" PRIu64 "MB dirty\n",
		    dhandle->name, dhandle->checkpoint,
		    file_pages, file_bytes >> 20, file_dirty >> 20);

		total_bytes += file_bytes;
	}
	printf("cache dump: total found = %" PRIu64 "MB"
	    " vs tracked inuse %" PRIu64 "MB\n",
	    total_bytes >> 20, __wt_cache_bytes_inuse(conn->cache) >> 20);
	fflush(stdout);
}
#endif
