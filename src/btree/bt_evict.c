/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __evict_dirty_validate(WT_CONNECTION_IMPL *conn);
static int  __evict_file_request_walk(WT_SESSION_IMPL *);
static int  __evict_lru(WT_SESSION_IMPL *, uint32_t);
static int  __evict_lru_cmp(const void *, const void *);
static int  __evict_walk(WT_SESSION_IMPL *, uint32_t *, uint32_t);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *, uint32_t);
static int  __evict_worker(WT_SESSION_IMPL *);

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some
 * number of pages from each file's in-memory tree for each page we evict.
 */
#define	WT_EVICT_INT_SKEW  (1<<20)	/* Prefer leaf pages over internal
					   pages by this many increments of the
					   read generation. */
#define	WT_EVICT_WALK_PER_FILE	 5	/* Pages to visit per file */
#define	WT_EVICT_WALK_BASE     100	/* Pages tracked across file visits */
#define	WT_EVICT_WALK_INCR     100	/* Pages added each walk */

/* Flags used to pass state to eviction functions. */
#define	WT_EVICT_CLEAN		0x01	/* Try to evict clean pages */
#define	WT_EVICT_DIRTY		0x02	/* Try to evict dirty pages */

/*
 * __evict_read_gen --
 *	Get the adjusted read generation for an eviction entry.
 */
static inline uint64_t
__evict_read_gen(const WT_EVICT_ENTRY *entry)
{
	uint64_t read_gen;

	if (entry->page == NULL)
		return (UINT64_MAX);

	read_gen = entry->page->read_gen + entry->btree->evict_priority;
	if (entry->page->type == WT_PAGE_ROW_INT ||
	    entry->page->type == WT_PAGE_COL_INT)
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
 * __evict_list_clr_all --
 *	Clear all entries in the LRU eviction list.
 */
static inline void
__evict_list_clr_all(WT_SESSION_IMPL *session, u_int start)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t i, elem;

	cache = S2C(session)->cache;

	elem = cache->evict_entries;
	for (i = start, evict = cache->evict + i; i < elem; i++, evict++)
		__evict_list_clr(session, evict);
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
	    page->ref->state == WT_REF_LOCKED);

	/* Fast path: if the page isn't on the queue, don't bother searching. */
	if (!F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, &cache->evict_lock);

	elem = cache->evict_entries;
	for (evict = cache->evict, i = 0; i < elem; i++, evict++)
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
	    "waking, bytes inuse %s max (%" PRIu64 "MB %s %" PRIu64 "MB), ",
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_inuse / WT_MEGABYTE,
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_max / WT_MEGABYTE);

	return (__wt_cond_signal(session, cache->evict_cond));
}

/*
 * __sync_file_serial_func --
 *	Eviction serialization function called when a tree is being flushed
 *	or closed.
 */
int
__wt_sync_file_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_CACHE *cache;
	int syncop;

	__wt_sync_file_unpack(args, &syncop);

	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(session->syncop, syncop);

	/* We're serialized at this point, no lock needed. */
	cache = S2C(session)->cache;
	++cache->sync_request;

	return (0);
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

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));

		if (!F_ISSET(conn, WT_CONN_SERVER_RUN))
			break;

		WT_VERBOSE_ERR(session, evictserver, "sleeping");
		/* Don't rely on signals: check periodically. */
		WT_ERR(__wt_cond_wait(session, cache->evict_cond, 100000));
		WT_VERBOSE_ERR(session, evictserver, "waking");
	}

	WT_VERBOSE_ERR(session, evictserver, "exiting");

	if (ret == 0) {
		if (__wt_cache_bytes_inuse(cache) != 0) {
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64 " pages, "
			    "%" PRIu64 " bytes in use",
			    __wt_cache_pages_inuse(cache),
			    __wt_cache_bytes_inuse(cache));
		}
	} else {
err:		__wt_err(session, ret, "eviction server error");
		(void)__wt_panic(session);
	}

	__wt_free(session, cache->evict);

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
	WT_DECL_RET;
	uint32_t flags;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;
	int loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/* Clear eviction flags for each pass. */
		flags = 0;
		/*
		 * Block out concurrent eviction while we are handling requests.
		 */
		__wt_spin_lock(session, &cache->evict_lock);

		/* If there is a file sync request, satisfy it. */
		while (ret == 0 && cache->sync_complete != cache->sync_request)
			ret = __evict_file_request_walk(session);

		__wt_spin_unlock(session, &cache->evict_lock);
		WT_RET(ret);

		/*
		 * Keep evicting until we hit the target cache usage and the
		 * target dirty percentage.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		dirty_inuse = __wt_cache_dirty_bytes(cache);
		bytes_max = conn->cache_size;
		if (bytes_inuse < (cache->eviction_target * bytes_max) / 100 &&
		    dirty_inuse <
		    (cache->eviction_dirty_target * bytes_max) / 100)
			break;

		/* Figure out how much we will focus on dirty pages. */
		if (dirty_inuse >
		    (cache->eviction_dirty_target * bytes_max) / 100)
			LF_SET(WT_EVICT_DIRTY);
		if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
			LF_SET(WT_EVICT_CLEAN | WT_EVICT_DIRTY);
		LF_CLR(cache->disabled_eviction);
		if (!LF_ISSET(WT_EVICT_CLEAN) && !LF_ISSET(WT_EVICT_DIRTY))
			break;

		WT_VERBOSE_RET(session, evictserver,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64,
		    bytes_max, bytes_inuse, dirty_inuse);
		WT_RET(__evict_lru(session, flags));

		__evict_dirty_validate(conn);
		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		if (LF_ISSET(WT_EVICT_CLEAN) &&
		    __wt_cache_bytes_inuse(cache) >= bytes_inuse) {
			if (loop == 10) {
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
	WT_CACHE *cache;
	WT_REF *ref;

	cache = S2C(session)->cache;

	__wt_spin_lock(session, &cache->evict_walk_lock);

	/* If no page stack specified, clear the standard eviction stack. */
	if (page == NULL) {
		page = session->btree->evict_page;
		session->btree->evict_page = NULL;
	}

	/* Clear the current eviction point. */
	while (page != NULL && !WT_PAGE_IS_ROOT(page)) {
		ref = page->ref;
		page = page->parent;
		if (ref->state == WT_REF_EVICT_WALK)
			ref->state = WT_REF_MEM;
	}

	__wt_spin_unlock(session, &cache->evict_walk_lock);
}

/*
 * __evict_page --
 *	Evict a given page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_DECL_RET;
	WT_TXN saved_txn, *txn;
	int was_running;

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
	saved_txn.oldest_snap_min = txn->oldest_snap_min;
	txn->isolation = TXN_ISO_READ_COMMITTED;
	ret = __wt_rec_evict(session, page, 0);

	/* Keep count of any failures. */
	saved_txn.eviction_fails = txn->eviction_fails;

	if (was_running) {
		WT_ASSERT(session, txn->snapshot == NULL ||
		    txn->snapshot != saved_txn.snapshot);
		__wt_txn_destroy(session);
	} else
		__wt_txn_release_snapshot(session);

	*txn = saved_txn;
	return (ret);
}

/*
 * __evict_file_request_walk --
 *	Walk the session list looking for sync/close requests.  If we find a
 * request, perform it, clear the request, and wake up the requesting thread.
 */
static int
__evict_file_request_walk(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *request_session;
	uint32_t i, session_cnt;
	int syncop;

	conn = S2C(session);
	cache = conn->cache;

	/* Make progress, regardless of success or failure. */
	++cache->sync_complete;

	/*
	 * No lock is required because the session array is fixed size, but it
	 * it may contain inactive entries.
	 *
	 * If we don't find a request, something went wrong; complain, but don't
	 * return an error code, the eviction thread doesn't need to exit.
	 */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (request_session = conn->sessions,
	    i = 0; i < session_cnt; ++request_session, ++i)
		if (request_session->active && request_session->syncop != 0)
			break;
	if (i == session_cnt) {
		__wt_errx(session,
		    "failed to find handle's sync operation request");
		return (0);
	}

	/*
	 * Clear the session's request (we don't want to find it again
	 * on our next walk, and doing it now should help avoid coding
	 * errors later.  No publish is required, all we care about is
	 * that we see it change.
	 */
	syncop = request_session->syncop;
	request_session->syncop = 0;

	WT_VERBOSE_RET(session, evictserver,
	    "file request: %s",
	    (syncop == WT_SYNC ? "sync" : (syncop == WT_SYNC_DISCARD ?
	    "sync-discard" : "sync-discard-nowrite")));

	/*
	 * The eviction candidate list might reference pages we are
	 * about to discard; clear it.
	 */
	__evict_list_clr_all(session, 0);

	/*
	 * Wait for LRU eviction activity to drain.  It is much easier
	 * to reason about checkpoints if we know there are no other
	 * threads evicting in the tree.
	 */
	while (request_session->btree->lru_count > 0) {
		__wt_spin_unlock(session, &cache->evict_lock);
		__wt_yield();
		__wt_spin_lock(session, &cache->evict_lock);
	}

	/*
	 * Publish: there must be a barrier to ensure the return value is set
	 * before the requesting thread wakes.
	 */
	WT_PUBLISH(request_session->syncop_ret,
	    __wt_evict_file(request_session, syncop));
	return (__wt_cond_signal(request_session, request_session->cond));
}

/*
 * __wt_evict_readonly --
 *	Switch on/off read-only eviction.
 */
void
__wt_evict_readonly(WT_SESSION_IMPL *session, int readonly)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	if (readonly)
		FLD_SET(cache->disabled_eviction, WT_EVICT_DIRTY);
	else
		FLD_CLR(cache->disabled_eviction, WT_EVICT_DIRTY);
}

/*
 * __wt_evict_file --
 *	Flush pages for a specific file as part of a close/sync operation.
 */
int
__wt_evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_DECL_RET;
	WT_PAGE *next_page, *page;
	uint32_t walk_flags = WT_TREE_EVICT;

	/*
	 * Checkpoints need to wait for any concurrent activity in a
	 * page to be resolved.
	 */
	if (syncop == WT_SYNC) {
		walk_flags |= WT_TREE_WAIT;
	}

	/*
	 * Clear any existing LRU walk, we may be about to discard the
	 * tree or we may be taking a checkpoint that would block.
	 */
	__wt_evict_clear_tree_walk(session, NULL);

	/*
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	next_page = NULL;
	WT_RET(__wt_tree_walk(session, &next_page, walk_flags));
	for (;;) {
		if ((page = next_page) == NULL)
			break;
		if (syncop != WT_SYNC && syncop != WT_SYNC_FUZZY)
			WT_ERR(__wt_tree_walk(session, &next_page, walk_flags));

		switch (syncop) {
		case WT_SYNC_COMPACT:
			WT_ERR(__wt_compact_evict(session, page));
			break;
		case WT_SYNC_FUZZY:
			/* Skip internal pages in the fuzzy sync. */
			if (page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_COL_INT)
				break;
			/* FALLTHROUGH */
		case WT_SYNC:
		case WT_SYNC_DISCARD:
			/* Write dirty pages for sync and sync with discard. */
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(session, page, NULL, 1));
			if (syncop == WT_SYNC || syncop == WT_SYNC_FUZZY)
				break;

			/*
			 * Evict the page for sync with discard.
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
			 * Simply discard the page for discard alone.  When we
			 * discard the root page, clear the reference from the
			 * btree handle.  It is important to do this here, so
			 * that future eviction doesn't see root_page pointing
			 * to freed memory.
			 */
			if (WT_PAGE_IS_ROOT(page))
				session->btree->root_page = NULL;
			__wt_page_out(session, &page, 0);
			break;
		}

		if (syncop == WT_SYNC || syncop == WT_SYNC_FUZZY)
			WT_ERR(__wt_tree_walk(session, &next_page, walk_flags));
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_page != NULL)
			__wt_evict_clear_tree_walk(session, next_page);
	}
	return (ret);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CACHE *cache;
	uint64_t cutoff;
	uint32_t i, candidates;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session, &candidates, flags));

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);
	while (candidates > 0 && cache->evict[candidates - 1].page == NULL)
		--candidates;
	if (candidates == 0) {
		__wt_spin_unlock(session, &cache->evict_lock);
		return (0);
	}

	qsort(cache->evict,
	    candidates, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	/* Find the bottom 25% */
	while (candidates > 0 && cache->evict[candidates - 1].page == NULL)
		--candidates;

	cutoff = (3 * __evict_read_gen(&cache->evict[0]) +
	    __evict_read_gen(&cache->evict[candidates - 1])) / 4;

	/*
	 * Don't take more than half, regardless.  That said, if there is only
	 * one candidate page, which is normal when populating an empty file,
	 * don't exclude it.
	 */
	for (i = 0; i < candidates / 2; i++)
		if (cache->evict[i].page->read_gen > cutoff)
			break;
	cache->evict_candidates = i + 1;

	__evict_list_clr_all(session, WT_EVICT_WALK_BASE);

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->evict_lock);

	/* Reconcile and discard some pages. */
	while (__wt_evict_lru_page(session, 0) == 0)
		;

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session, u_int *entriesp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	u_int elem, file_count, i, retries;

	conn = S2C(session);
	cache = S2C(session)->cache;
	retries = 0;

	/*
	 * Resize the array in which we're tracking pages, as necessary, then
	 * get some pages from each underlying file.  In practice, a realloc
	 * is rarely needed, so it is worth avoiding the LRU lock.
	 */
	elem = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	if (elem > cache->evict_entries) {
		__wt_spin_lock(session, &cache->evict_lock);
		/* Save the offset of the eviction point. */
		i = (u_int)(cache->evict_current - cache->evict);
		WT_ERR(__wt_realloc(session, &cache->evict_allocated,
		    elem * sizeof(WT_EVICT_ENTRY), &cache->evict));
		cache->evict_entries = elem;
		if (cache->evict_current != NULL)
			cache->evict_current = cache->evict + i;
		__wt_spin_unlock(session, &cache->evict_lock);
	}

	/*
	 * NOTE: we don't hold the schema lock: files can't be removed without
	 * the eviction server being involved, and when we're here, we aren't
	 * servicing eviction requests.
	 */
	i = WT_EVICT_WALK_BASE;
retry:	file_count = 0;
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (file_count++ < cache->evict_file_next)
			continue;

		/*
		 * Skip files that aren't open or don't have a root page.
		 *
		 * Also skip files marked as cache-resident, and files
		 * potentially involved in a bulk load.  The real problem is
		 * eviction doesn't want to be walking the file as it converts
		 * to a bulk-loaded object, and empty trees aren't worth trying
		 * to evict, anyway.
		 */
		if (!F_ISSET(btree, WT_BTREE_OPEN) ||
		    btree->root_page == NULL ||
		    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
		    btree->bulk_load_ok)
			continue;

		__wt_spin_lock(session, &cache->evict_walk_lock);

		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, btree);
		ret = __evict_walk_file(session, &i, flags);
		WT_CLEAR_BTREE_IN_SESSION(session);

		__wt_spin_unlock(session, &cache->evict_walk_lock);

		if (ret != 0 || i == cache->evict_entries)
			break;
	}
	cache->evict_file_next = (btree == NULL) ? 0 : file_count;

	/* In the extreme case, all of the pages have to come from one file. */
	if (ret == 0 && i < cache->evict_entries &&
	    retries++ < WT_EVICT_WALK_INCR / WT_EVICT_WALK_PER_FILE)
		goto retry;

	*entriesp = i;
	if (0) {
err:		__wt_spin_unlock(session, &cache->evict_lock);
	}
	return (ret);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(WT_SESSION_IMPL *session, u_int *slotp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	int restarts;

	btree = session->btree;
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = start + WT_EVICT_WALK_PER_FILE;
	if (end > cache->evict + cache->evict_entries)
		end = cache->evict + cache->evict_entries;

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

		/*
		 * Root pages can't be evicted, nor can internal pages expected
		 * to be merged into their parents.  Use the EVICT_LRU flag to
		 * avoid putting pages onto the list multiple times.
		 *
		 * Don't skip pages marked WT_PM_REC_EMPTY or SPLIT: updates
		 * after their last reconciliation may have changed their state
		 * and only the reconciliation/eviction code can confirm if they
		 * should really be skipped.
		 */
		if (WT_PAGE_IS_ROOT(page) ||
		    page->ref->state != WT_REF_EVICT_WALK ||
		    F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU) ||
		    (page->modify != NULL &&
		    F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE)))
			continue;

		if (__wt_page_is_modified(page)) {
			if (!LF_ISSET(WT_EVICT_DIRTY))
				continue;
		} else if (!LF_ISSET(WT_EVICT_CLEAN))
			continue;

		WT_ASSERT(session, evict->page == NULL);
		evict->page = page;
		evict->btree = btree;
		++evict;

		/* Mark the page on the list */
		F_SET_ATOMIC(page, WT_PAGE_EVICT_LRU);

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
static void
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

	candidates = cache->evict_candidates;
	/* The eviction server only considers half of the entries. */
	if (!is_app)
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
			return;
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
		evict->page->read_gen = __wt_cache_read_gen(session);

		/*
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, this operation will fail and we will move on.
		 */
		ref = evict->page->ref;
		WT_ASSERT(session, evict->page == ref->page);
		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED))
			continue;

		/*
		 * Increment the LRU count in the btree handle to prevent it
		 * from being closed under us.
		 */
		(void)WT_ATOMIC_ADD(evict->btree->lru_count, 1);

		*btreep = evict->btree;
		*pagep = evict->page;

		/*
		 * Remove the entry so we never try and reconcile the same page
		 * on reconciliation error.
		 */
		__evict_list_clr(session, evict);
		break;
	}

	if (is_app && *pagep == NULL)
		cache->evict_current = NULL;
	__wt_spin_unlock(session, &cache->evict_lock);
}

/*
 * __wt_evict_lru_page --
 *	Called by both eviction and application threads to evict a page.
 */
int
__wt_evict_lru_page(WT_SESSION_IMPL *session, int is_app)
{
	WT_BTREE *btree, *saved_btree;
	WT_DECL_RET;
	WT_PAGE *page;

	__evict_get_page(session, is_app, &btree, &page);
	if (page == NULL)
		return (WT_NOTFOUND);

	WT_ASSERT(session, page->ref->state == WT_REF_LOCKED);

	/* Reference the correct WT_BTREE handle. */
	saved_btree = session->btree;
	WT_SET_BTREE_IN_SESSION(session, btree);

	ret = __evict_page(session, page);

	(void)WT_ATOMIC_SUB(btree->lru_count, 1);

	WT_CLEAR_BTREE_IN_SESSION(session);
	session->btree = saved_btree;

	return (ret);
}

/*
 * __evict_dirty_validate --
 *	Walk the cache counting dirty entries so we can validate dirty counts.
 *	This belongs in eviction, because it's the only time we can safely
 *	traverse the btree queue without locking.
 */
static void
__evict_dirty_validate(WT_CONNECTION_IMPL *conn)
{
#ifdef HAVE_DIAGNOSTIC
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_SESSION_IMPL *session;
	uint64_t bytes, bytes_baseline;

	cache = conn->cache;
	session = conn->default_session;
	page = NULL;
	btree = NULL;
	bytes = 0;

	if (!WT_VERBOSE_ISSET(session, evictserver))
		return;

	bytes_baseline = cache->bytes_dirty;

	TAILQ_FOREACH(btree, &conn->btqh, q) {
		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, btree);
		while ((ret = __wt_tree_walk(
		    session, &page, WT_TREE_CACHE)) == 0 &&
		    page != NULL) {
			if (__wt_page_is_modified(page))
				bytes += page->memory_footprint;
		}
		WT_CLEAR_BTREE_IN_SESSION(session);
	}
	if ((ret == 0 || ret == WT_NOTFOUND) && bytes != 0) {
		if (bytes < WT_MIN(bytes_baseline, cache->bytes_dirty) ||
		    bytes > WT_MAX(bytes_baseline, cache->bytes_dirty))
			WT_VERBOSE_VOID(session, evictserver,
			    "Cache dirty count mismatch. Expected a value "
			    "between: %" PRIu64 " and %" PRIu64
			    " got: %" PRIu64,
			    bytes_baseline, cache->bytes_dirty, bytes);
	}
#else
	WT_UNUSED(conn);
#endif
}
