/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __evict_dirty_validate(WT_CONNECTION_IMPL *);
static int  __evict_file(WT_SESSION_IMPL *, int);
static int  __evict_file_request_walk(WT_SESSION_IMPL *);
static void __evict_init_candidate(
    WT_SESSION_IMPL *, WT_EVICT_ENTRY *, WT_PAGE *);
static int  __evict_lru(WT_SESSION_IMPL *, int);
static int  __evict_lru_cmp(const void *, const void *);
static int  __evict_walk(WT_SESSION_IMPL *, uint32_t *, int);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *, int);
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

	/* Always prioritize pages selected by force. */
	if (page->ref->state == WT_REF_EVICT_FORCE)
		return (0);

	read_gen = page->read_gen + entry->btree->evict_priority;

	/*
	 * Skew the read generation a for internal pages that are not split
	 * merge pages.  We want to consider leaf pages in preference to real
	 * internal pages, but merges are relatively cheap in-memory operations
	 * that make reads faster, so don't make them too unlikely.
	 */
	if ((page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_COL_INT) &&
	    (page->modify == NULL ||
	    !F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE)))
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
		/*
		 * If the page has been locked to assist with eviction,
		 * clear the locked state when removing it from the eviction
		 * queue.
		 */
		(void)WT_ATOMIC_CAS(e->page->ref->state,
		    WT_REF_EVICT_FORCE, WT_REF_MEM);
	}
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __evict_list_clr_range --
 *	Clear entries in the LRU eviction list, from a lower-bound to the end.
 */
static inline void
__evict_list_clr_range(WT_SESSION_IMPL *session, u_int start)
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
	    page->ref->state == WT_REF_EVICT_WALK ||
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
 * __wt_evict_forced_page --
 *	If a page matches the force criteria,try to add it to the eviction
 *	queue and trigger the eviction server.  Best effort only, so no error
 *	is returned if the page is busy.
 */
void
__wt_evict_forced_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_PAGE *top;
	u_int levels;

	conn = S2C(session);
	cache = conn->cache;

	/* Don't queue a page for forced eviction if we already have one. */
	if (F_ISSET(cache, WT_EVICT_FORCE_PASS))
		return;

	/*
	 * Check if the page we have been asked to forcefully evict is at the
	 * bottom of a stack of split-merge pages.  If so, lock the top of the
	 * stack instead.
	 */
	for (top = page, levels = 0;
	    !WT_PAGE_IS_ROOT(top) && top->parent->modify != NULL &&
	    F_ISSET(top->parent->modify, WT_PM_REC_SPLIT_MERGE);
	    ++levels)
		top = top->parent;

	if (levels > WT_MERGE_STACK_MIN)
		page = top;

	/*
	 * Try to lock the page.  If this succeeds, we're going to queue
	 * it for forced eviction.  We don't go right to the EVICT_FORCED
	 * state, because that is cleared by __wt_evict_list_clr_page.
	 */
	if (!WT_ATOMIC_CAS(page->ref->state, WT_REF_MEM, WT_REF_LOCKED))
		return;

	/* If the page is already queued for ordinary eviction, clear it. */
	__wt_evict_list_clr_page(session, page);

	__wt_spin_lock(session, &cache->evict_lock);

	/* Add the page to the head of the eviction queue. */
	__evict_init_candidate(session, cache->evict, page);

	/* Set the location in the eviction queue to the new entry. */
	cache->evict_current = cache->evict;
	/*
	 * If the candidate list was empty we are adding a candidate, in all
	 * other cases we are replacing an existing candidate.
	 */
	if (cache->evict_candidates == 0)
		cache->evict_candidates++;

	/*
	 * Lock the page so other threads cannot get new read locks on the
	 * page - which makes it more likely that the next pass of the eviction
	 * server will successfully evict the page.
	 */
	WT_PUBLISH(page->ref->state, WT_REF_EVICT_FORCE);

	__wt_spin_unlock(session, &cache->evict_lock);

	/* Wake the server, but don't worry if that fails. */
	F_SET(cache, WT_EVICT_FORCE_PASS);
	(void)__wt_evict_server_wake(session);
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

	cache->evict_entries = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	WT_ERR(__wt_calloc_def(session, cache->evict_entries, &cache->evict));

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
		if (__wt_cache_bytes_inuse(cache) != 0) {
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64 " pages, "
			    "%" PRIu64 " bytes in use",
			    __wt_cache_pages_inuse(cache),
			    __wt_cache_bytes_inuse(cache));
		}
	} else
err:		WT_PANIC_ERR(session, ret, "eviction server error");

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
	uint64_t bytes_inuse, bytes_max, dirty_inuse;
	int clean, force, loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/*
		 * Block out concurrent eviction while we are handling requests.
		 */
		__wt_spin_lock(session, &cache->evict_lock);

		/* If there is a file sync request, satisfy it. */
		while (ret == 0 && cache->sync_complete != cache->sync_request)
			ret = __evict_file_request_walk(session);

		/* Check for forced eviction while we hold the lock. */
		force = F_ISSET(cache, WT_EVICT_FORCE_PASS) ? 1 : 0;
		F_CLR(cache, WT_EVICT_FORCE_PASS);

		__wt_spin_unlock(session, &cache->evict_lock);
		WT_RET(ret);

		/*
		 * If we've been awoken for forced eviction, just try to evict
		 * the first page in the queue: don't do a walk and sort first.
		 * Sometimes the page won't be available for eviction because
		 * there is a reader still holding a hazard reference. Give up
		 * in that case, the application thread can add it again.
		 */
		if (force)
			(void)__wt_evict_lru_page(session, 0);

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

		WT_RET(__evict_lru(session, clean));

		__evict_dirty_validate(conn);
		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		if (clean && __wt_cache_bytes_inuse(cache) >= bytes_inuse) {
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
	const char *msg;

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
	 * errors later).  No publish is required, all we care about is
	 * that we see it change.
	 */
	syncop = request_session->syncop;
	request_session->syncop = 0;

	switch (syncop) {
	case WT_SYNC_DISCARD:
		msg = "sync-discard";
		break;
	case WT_SYNC_DISCARD_NOWRITE:
		msg = "sync-discard-nowrite";
		break;
	WT_ILLEGAL_VALUE(session);
	}
	WT_VERBOSE_RET(
	    session, evictserver, "eviction server request: %s", msg);

	/*
	 * The eviction candidate list might reference pages we are
	 * about to discard; clear it.
	 */
	__evict_list_clr_range(session, 0);

	/* Wait for LRU eviction activity to drain. */
	while (request_session->btree->lru_count > 0) {
		__wt_spin_unlock(session, &cache->evict_lock);
		__wt_yield();
		__wt_spin_lock(session, &cache->evict_lock);
	}

	/*
	 * Handle the request and publish the result: there must be a barrier
	 * to ensure the return value is set before the requesting thread
	 * wakes.
	 */
	WT_PUBLISH(request_session->syncop_ret,
	    __evict_file(request_session, syncop));
	return (__wt_cond_signal(request_session, request_session->cond));
}

/*
 * __evict_file --
 *	Flush pages for a specific file as part of a close or compact operation.
 */
static int
__evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_DECL_RET;
	WT_PAGE *next_page, *page;

	/* Clear any existing LRU eviction walk, we're discarding the tree. */
	__wt_evict_clear_tree_walk(session, NULL);

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
				session->btree->root_page = NULL;
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
	uint32_t flags;

	btree = session->btree;
	cache = S2C(session)->cache;
	page = NULL;

	switch (syncop) {
	case WT_SYNC_CHECKPOINT:
		/*
		 * The first pass walks all cache leaf pages, waiting for
		 * concurrent activity in a page to be resolved, acquiring
		 * hazard references to prevent eviction.
		 */
		flags = WT_TREE_CACHE | WT_TREE_SKIP_INTL | WT_TREE_WAIT;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			/* Write dirty pages. */
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(session, page, NULL, 0));
			WT_ERR(__wt_tree_walk(session, &page, flags));
		}

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
	uint64_t cutoff;
	uint32_t i, candidates;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session, &candidates, clean));

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

	while (candidates > 0 && cache->evict[candidates - 1].page == NULL)
		--candidates;

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

	__evict_list_clr_range(session, WT_EVICT_WALK_BASE);

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->evict_lock);

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
	WT_DECL_RET;
	u_int file_count, i, retries;

	conn = S2C(session);
	cache = S2C(session)->cache;
	retries = 0;

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
		ret = __evict_walk_file(session, &i, clean);
		WT_CLEAR_BTREE_IN_SESSION(session);

		__wt_spin_unlock(session, &cache->evict_walk_lock);

		if (ret != 0 || i == cache->evict_entries)
			break;
	}
	cache->evict_file_next = (btree == NULL) ? 0 : file_count;

	/* Walk the files a few times if we don't find enough pages. */
	if (ret == 0 && i < cache->evict_entries && retries++ < 3)
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
	if (evict->page != NULL)
		__evict_list_clr(session, evict);
	evict->page = page;
	evict->btree = session->btree;

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
	WT_PAGE *page, *parent;
	int modified, restarts, splits;

	btree = session->btree;
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = start + WT_EVICT_WALK_PER_FILE;
	if (end > cache->evict + cache->evict_entries)
		end = cache->evict + cache->evict_entries;

	/*
	 * Get some more eviction candidate pages.
	 */
	for (evict = start, restarts = splits = 0;
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
		 * If this page has never been considered for eviction, set its
		 * read generation to a little bit in the future and move on,
		 * give readers a chance to start updating the read generation.
		 */
		if (page->read_gen == WT_READ_GEN_NOTSET) {
			page->read_gen =
			    WT_READ_GEN_STEP + __wt_cache_read_gen(session);
			continue;
		}

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		/*
		 * Skip root pages, and split-merge pages that have split-merge
		 * pages as their parents (we're only interested in the top-most
		 * split-merge page of deep trees).
		 *
		 * Don't skip empty or split pages: updates after their last
		 * reconciliation may have changed their state and only the
		 * reconciliation/eviction code can confirm if they should be
		 * skipped.
		 */
		if (WT_PAGE_IS_ROOT(page))
			continue;

		if (page->modify != NULL &&
		    F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE)) {
			parent = page->parent;
			if (++splits < WT_MERGE_STACK_MIN ||
			    (parent->modify != NULL &&
			    F_ISSET(parent->modify, WT_PM_REC_SPLIT_MERGE)))
				continue;
		} else
			splits = 0;

		/*
		 * If the file is being checkpointed, there's a period of time
		 * where we can't discard any page with a modification
		 * structure because it might race with the checkpointing
		 * thread.
		 *
		 * During this phase, there is little point trying to evict
		 * dirty pages: we might be lucky and find an internal page
		 * that has not yet been checkpointed, but much more likely is
		 * that we will waste effort considering dirty leaf pages that
		 * cannot be evicted because they have modifications more
		 * recent than the checkpoint.
		 */
		modified = __wt_page_is_modified(page);
		if (modified && btree->checkpointing)
			continue;

		/* Optionally ignore clean pages. */
		if (!modified && !clean)
			continue;

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

		/*
		 * Switch pages from the evict force state to locked - the
		 * logic for forced and regular eviction is identical from here
		 * on, and having reconciliation be able to use a single
		 * locked state simplifies that code.
		 */
		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED) &&
		    !WT_ATOMIC_CAS(
		    ref->state, WT_REF_EVICT_FORCE, WT_REF_LOCKED)) {
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

	bytes_baseline = __wt_cache_bytes_dirty(cache);

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

	if (WT_VERBOSE_ISSET(session, evictserver) &&
	    (ret == 0 || ret == WT_NOTFOUND) &&
	    bytes != 0 &&
	    (bytes < WT_MIN(bytes_baseline, __wt_cache_bytes_dirty(cache)) ||
	    bytes > WT_MAX(bytes_baseline, __wt_cache_bytes_dirty(cache))))
		(void)__wt_verbose(session,
		    "Cache dirty count mismatch. Expected a value between: %"
		    PRIu64 " and %" PRIu64 " got: %" PRIu64,
		    bytes_baseline, __wt_cache_bytes_dirty(cache), bytes);
#else
	WT_UNUSED(conn);
#endif
}
