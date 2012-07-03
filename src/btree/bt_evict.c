/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __evict_clear_tree_walk(WT_SESSION_IMPL *, WT_PAGE *);
static int  __evict_file_request(WT_SESSION_IMPL *, int);
static int  __evict_file_request_walk(WT_SESSION_IMPL *);
static int  __evict_lru(WT_SESSION_IMPL *);
static int  __evict_lru_cmp(const void *, const void *);
static void __evict_lru_sort(WT_SESSION_IMPL *);
static int  __evict_page_request_walk(WT_SESSION_IMPL *);
static int  __evict_walk(WT_SESSION_IMPL *);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *);
static int  __evict_worker(WT_SESSION_IMPL *);

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some
 * number of pages from each file's in-memory tree for each page we evict.
 */
#define	WT_EVICT_GROUP		30	/* Consider N pages as LRU candidates */
#define	WT_EVICT_WALK_PER_TABLE	35	/* Pages to visit per file */
#define	WT_EVICT_WALK_BASE	50	/* Pages tracked across file visits */

/*
 * WT_EVICT_REQ_FOREACH --
 *	Walk the list of forced page eviction requests.
 */
#define	WT_EVICT_REQ_FOREACH(er, er_end, cache)				\
	for ((er) = (cache)->evict_request,				\
	    (er_end) = (er) + (cache)->max_evict_request;		\
	    (er) < (er_end); ++(er))

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
 * __evict_req_set --
 *	Set an entry in the forced page eviction request list.
 */
static inline void
__evict_req_set(WT_EVICT_ENTRY *r, WT_BTREE *btree, WT_PAGE *page)
{
	r->btree = btree;
	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(r->page, page);
}

/*
 * __evict_req_clr --
 *	Clear an entry in the forced page eviction request list.
 */
static inline void
__evict_req_clr(WT_EVICT_ENTRY *r)
{
	r->btree = NULL;
	r->page = NULL;
	/*
	 * No publication necessary, all we care about is the page value and
	 * whenever it's cleared is fine.
	 */
}

/*
 * __wt_evict_server_wake --
 *	Wake the eviction server thread.
 */
void
__wt_evict_server_wake(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max;

	conn = S2C(session);
	cache = conn->cache;
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = conn->cache_size;

	WT_VERBOSE_VOID(session, evictserver,
	    "waking, bytes inuse %s max (%" PRIu64 "MB %s %" PRIu64 "MB), ",
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_inuse / WT_MEGABYTE,
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_max / WT_MEGABYTE);

	__wt_cond_signal(session, cache->evict_cond);
}

/*
 * __sync_file_serial_func --
 *	Eviction serialization function called when a tree is being flushed
 *	or closed.
 */
void
__wt_sync_file_serial_func(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	int syncop;

	__wt_sync_file_unpack(session, &syncop);

	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(session->syncop, syncop);

	/* We're serialized at this point, no lock needed. */
	cache = S2C(session)->cache;
	++cache->sync_request;
}

/*
 * __wt_evict_page_request --
 *	Schedule a page for forced eviction due to a high volume of inserts or
 *	updates.
 */
void
__wt_evict_page_request(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *er, *er_end;
	int set;

	cache = S2C(session)->cache;

	/* Do a cheap test before acquiring the lock. */
	if (page->ref->state != WT_REF_MEM)
		return;

	__wt_spin_lock(session, &cache->evict_lock);

	/*
	 * Application threads request forced eviction of pages when they
	 * become too big.  The application thread must hold a hazard reference
	 * when this function is called, which protects it from being freed.
	 *
	 * However, it is possible (but unlikely) that the page is already part
	 * way through the process of being evicted: a thread may have selected
	 * it from the LRU list but not yet checked its hazard references.
	 *
	 * To avoid that race, we try to atomically switch the page state to
	 * WT_REF_EVICT_FORCE.  Since only one thread can do that successfully,
	 * this prevents a page from being evicted twice.  Threads looking for
	 * a page to evict on the ordinary LRU eviction queue will ignore this
	 * page and it will be evicted by the main eviction thread.
	 *
	 * If the state is not WT_REF_MEM, some other thread is already
	 * evicting this page, which is fine, and in that case we don't want to
	 * put it on the request queue because the memory may be freed by the
	 * time the eviction thread sees it.
	 */
	if (!WT_ATOMIC_CAS(page->ref->state, WT_REF_MEM, WT_REF_EVICT_FORCE)) {
		__wt_spin_unlock(session, &cache->evict_lock);
		return;
	}

	set = 0;

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->page == NULL) {
			__evict_req_set(er, session->btree, page);
			set = 1;
			break;
		}

	if (!set) {
		/*
		 * The request table is full, that's okay for page requests:
		 * another thread will see this later.
		 */
		WT_VERBOSE_VOID(session, evictserver,
		    "page eviction request table is full");
		page->ref->state = WT_REF_MEM;
	}

	__wt_spin_unlock(session, &cache->evict_lock);
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
	int read_lockout;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_SERVER_RUN)) {
		/*
		 * Use the same logic as application threads to decide whether
		 * there is work to do.
		 */
		__wt_eviction_check(session, &read_lockout, 0);

		if (!read_lockout) {
			WT_VERBOSE_ERR(session, evictserver, "sleeping");
			__wt_cond_wait(session, cache->evict_cond);
		}

		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE_ERR(session, evictserver, "waking");

		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));
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
err:		__wt_err(session, ret, "eviction server error");

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
	uint64_t bytes_start, bytes_inuse, bytes_max;
	int loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/*
		 * Block out concurrent eviction while we are handling requests.
		 */
		__wt_spin_lock(session, &cache->evict_lock);

		/*
		 * Walk the eviction-request queue.  It is important to do this
		 * before closing files, in case a page schedule for eviction
		 * is freed by closing a file.
		 */
		WT_RET(__evict_page_request_walk(session));

		/* If there is a file sync request, satisfy it. */
		while (cache->sync_complete != cache->sync_request)
			WT_RET(__evict_file_request_walk(session));

		__wt_spin_unlock(session, &cache->evict_lock);

		/*
		 * Keep evicting until we hit the target cache usage.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
		if (bytes_inuse < cache->eviction_target * (bytes_max / 100))
			break;

		WT_RET(__evict_lru(session));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		bytes_start = bytes_inuse;
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		if (bytes_start == bytes_inuse) {
			if (loop == 10) {
				WT_STAT_INCR(conn->stats, cache_evict_slow);
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
 * __evict_clear_tree_walk --
 *	Clear the tree's current eviction point.
 */
static void
__evict_clear_tree_walk(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *ref;

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
}

/*
 * __evict_page --
 *	Evict a given page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN saved_txn, *txn;
	int was_running;

	/*
	 * We have to take care when evicting pages not to write a change that:
	 *  (a) is not yet committed; or
	 *  (b) is committed more recently than an in-progress checkpoint.
	 *
	 * We handle both of these cases by setting up the transaction context
	 * before evicting.  If a checkpoint is in progress, copy the
	 * checkpoint's transaction.  Otherwise, we need a snapshot to avoid
	 * uncommitted changes.  If a transaction is in progress in the
	 * evicting session, we save and restore its state.
	 */
	txn = &session->txn;
	saved_txn = *txn;
	was_running = (F_ISSET(txn, TXN_RUNNING) != 0);

	txn_global = &S2C(session)->txn_global;
	if (was_running)
		WT_RET(__wt_txn_init(session));

	WT_ERR(__wt_txn_get_snapshot(session, txn_global->ckpt_txnid));

	ret = __wt_rec_evict(session, page, 0);

err:	if (was_running) {
		WT_ASSERT(session, txn->snapshot == NULL ||
		    txn->snapshot != saved_txn.snapshot);
		__wt_txn_destroy(session);
	}

	session->txn = saved_txn;

	return (ret);
}

/*
 * __evict_file_request_walk --
 *      Walk the session list looking for sync/close requests.  If we find a
 *      request, perform it, clear the request, and wake up the requesting
 *      thread.
 */
static int
__evict_file_request_walk(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *request_session;
	WT_DECL_RET;
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
	    (request_session->syncop == WT_SYNC ? "sync" :
	    (request_session->syncop == WT_SYNC_DISCARD ?
	    "sync-discard" : "sync-discard-nowrite")));

	/*
	 * The eviction candidate list might reference pages we are
	 * about to discard; clear it.
	 */
	__evict_list_clr_all(session, 0);

	/*
	 * Wait for LRU eviction activity to drain.  It is much easier
	 * to reason about sync or forced eviction if we know there are
	 * no other threads evicting in the tree.
	 */
	while (request_session->btree->lru_count > 0) {
		__wt_spin_unlock(session, &cache->evict_lock);
		__wt_yield();
		__wt_spin_lock(session, &cache->evict_lock);
	}

	ret = __evict_file_request(request_session, syncop);

	__wt_session_serialize_wrapup(request_session, NULL, ret);

	return (0);
}

/*
 * __evict_file_request --
 *	Flush pages for a specific file as part of a close/sync operation.
 */
static int
__evict_file_request(WT_SESSION_IMPL *session, int syncop)
{
	WT_DECL_RET;
	WT_PAGE *next_page, *page;

	/* Clear any existing tree walk, we may be about to discard the tree. */
	__evict_clear_tree_walk(session, NULL);

	/*
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	next_page = NULL;
	WT_RET(__wt_tree_np(session, &next_page, 1, 1));
	for (;;) {
		if ((page = next_page) == NULL)
			break;
		WT_ERR(__wt_tree_np(session, &next_page, 1, 1));

		/* Write dirty pages for sync, and sync with discard. */
		switch (syncop) {
		case WT_SYNC:
		case WT_SYNC_DISCARD:
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(session, page, NULL));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			break;
		}

		/*
		 * Evict the page for sync with discard, simply discard the page
		 * for discard alone.
		 */
		switch (syncop) {
		case WT_SYNC:
			break;
		case WT_SYNC_DISCARD:
			/*
			 * Do not attempt to evict pages expected to be merged
			 * into their parents, with the single exception that
			 * the root page can't be merged into anything, it must
			 * be written.
			 */
			if (WT_PAGE_IS_ROOT(page) || page->modify == NULL ||
			    !F_ISSET(page->modify, WT_PM_REC_EMPTY |
			    WT_PM_REC_SPLIT | WT_PM_REC_SPLIT_MERGE))
				WT_ERR(__wt_rec_evict(
				    session, page, WT_REC_SINGLE));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			__wt_page_out(session, &page, 0);
			break;
		}
	}

	return (0);

	/* On error, clear any left-over tree walk. */
err:	if (next_page != NULL)
		__evict_clear_tree_walk(session, next_page);
	return (ret);
}

/*
 * __evict_page_request_walk --
 *	Walk the forced page eviction request queue.
 */
static int
__evict_page_request_walk(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *er, *er_end;
	WT_PAGE *page;
	WT_REF *ref;

	cache = S2C(session)->cache;

	/*
	 * Walk the forced page eviction request queue: if we find a request,
	 * perform it and clear the request slot.
	 */
	WT_EVICT_REQ_FOREACH(er, er_end, cache) {
		if ((page = er->page) == NULL)
			continue;

		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, er->btree);

		WT_VERBOSE_RET(session, evictserver,
		    "forcing eviction of page %p", page);

		/*
		 * The eviction candidate list might reference pages we are
		 * about to discard; clear it.
		 */
		__evict_list_clr_all(session, 0);

		/*
		 * The eviction candidate might be part of the current tree's
		 * walk; clear it.
		 */
		__evict_clear_tree_walk(session, NULL);

		/*
		 * Wait for LRU eviction activity to drain.  It is much easier
		 * to reason about sync or forced eviction if we know there are
		 * no other threads evicting in the tree.
		 */
		while (session->btree->lru_count > 0) {
			__wt_spin_unlock(session, &cache->evict_lock);
			__wt_yield();
			__wt_spin_lock(session, &cache->evict_lock);
		}

		ref = page->ref;
		WT_ASSERT(session, ref->page == page);
		WT_ASSERT(session, ref->state == WT_REF_EVICT_FORCE);
		ref->state = WT_REF_LOCKED;

		/*
		 * If eviction fails, it will free up the page: hope it works
		 * next time.  Application threads may be holding a reference
		 * while trying to get another (e.g., if they have two cursors
		 * open), so blocking indefinitely leads to deadlock.
		 */
		(void)__evict_page(session, page);

		/* Clear the reference to the btree handle. */
		WT_CLEAR_BTREE_IN_SESSION(session);

		/* Clear the request slot. */
		__evict_req_clr(er);
	}
	return (0);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session));

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);
	__evict_lru_sort(session);
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
__evict_walk(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	u_int elem, i;

	conn = S2C(session);
	cache = S2C(session)->cache;

	/*
	 * We hold the schema lock for the entire walk -- it's slow, but
	 * (1) how often do new files get added or removed to/from the system?
	 * and (2) it's all in-memory stuff, so it's not that slow.
	 *
	 * If the schema lock is not available, don't block: another thread may
	 * be holding it and waiting on eviction (e.g., checkpoint).
	 */
	if (__wt_spin_trylock(session, &conn->schema_lock) != 0)
		return (0);

	/*
	 * Resize the array in which we're tracking pages, as necessary, then
	 * get some pages from each underlying file.  In practice, a realloc
	 * is rarely needed, so it is worth avoiding the LRU lock.
	 */
	elem = WT_EVICT_WALK_BASE + (conn->btqcnt * WT_EVICT_WALK_PER_TABLE);
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

	i = WT_EVICT_WALK_BASE;
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
			continue;

		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, btree);

		ret = __evict_walk_file(session, &i);

		WT_CLEAR_BTREE_IN_SESSION(session);

		if (ret != 0)
			break;
	}

	if (0) {
err:		__wt_spin_unlock(session, &cache->evict_lock);
	}
	__wt_spin_unlock(session, &conn->schema_lock);
	return (ret);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(WT_SESSION_IMPL *session, u_int *slotp)
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
	end = start + WT_EVICT_WALK_PER_TABLE;

	/*
	 * Get the next WT_EVICT_WALK_PER_TABLE entries.
	 *
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	for (evict = start, restarts = 0;
	    evict < end && restarts <= 1 && ret == 0;
	    ret = __wt_tree_np(session, &btree->evict_page, 1, 1)) {
		if ((page = btree->evict_page) == NULL) {
			++restarts;
			continue;
		}

		/*
		 * Root pages can't be evicted, nor can skip pages that must be
		 * merged into their parents.  Use the EVICT_LRU flag to avoid
		 * putting pages onto the list multiple times.
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
 * __evict_lru_sort --
 *	Sort the list of pages queued for eviction into LRU order.
 */
static void
__evict_lru_sort(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	/*
	 * We have an array of page eviction references that may contain NULLs,
	 * as well as duplicate entries.
	 *
	 * First, sort the array by WT_REF address, then delete any duplicates.
	 * The reason is because we might evict the page but leave a duplicate
	 * entry in the "saved" area of the array, and that would be a NULL
	 * dereference on the next run.  (If someone ever tries to remove this
	 * duplicate cleanup for better performance, you can't fix it just by
	 * checking the WT_REF state -- that only works if you are discarding
	 * a page from a single level of the tree; if you are discarding a
	 * page and its parent, the duplicate of the page's WT_REF might have
	 * been free'd before a subsequent review of the eviction array.)
	 */
	cache = S2C(session)->cache;
	qsort(cache->evict,
	    cache->evict_entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);
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
	int candidates;

	cache = S2C(session)->cache;
	*btreep = NULL;
	*pagep = NULL;

	candidates = (is_app ? WT_EVICT_GROUP : WT_EVICT_GROUP / 2);

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
		 * being evicted, including pages on the request queue for
		 * forced eviction, this operation will fail and we will move
		 * on.
		 */
		ref = evict->page->ref;
		WT_ASSERT(session, evict->page == ref->page);
		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED))
			continue;

		/*
		 * Increment the LRU count in the btree handle to prevent it
		 * from being closed under us.
		 */
		WT_ATOMIC_ADD(evict->btree->lru_count, 1);

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
	WT_PAGE *page;

	__evict_get_page(session, is_app, &btree, &page);
	if (page == NULL)
		return (WT_NOTFOUND);

	WT_ASSERT(session, page->ref->state == WT_REF_LOCKED);

	/* Reference the correct WT_BTREE handle. */
	saved_btree = session->btree;
	WT_SET_BTREE_IN_SESSION(session, btree);

	/*
	 * We don't care why eviction failed (maybe the page was dirty and
	 * we're out of disk space, or the page had an in-memory subtree
	 * already being evicted).
	 */
	(void)__evict_page(session, page);

	WT_ATOMIC_ADD(btree->lru_count, -1);

	WT_CLEAR_BTREE_IN_SESSION(session);
	session->btree = saved_btree;

	return (0);
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort LRU eviction array based on the page's read
 *	generation.
 */
static int
__evict_lru_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;
	uint64_t a_lru, b_lru;

	/*
	 * There may be NULL references in the array; sort them as greater than
	 * anything else so they migrate to the end of the array.
	 */
	a_page = ((WT_EVICT_ENTRY *)a)->page;
	b_page = ((WT_EVICT_ENTRY *)b)->page;
	if (a_page == NULL)
		return (b_page == NULL ? 0 : 1);
	if (b_page == NULL)
		return (-1);

	/* Sort the LRU in ascending order. */
	a_lru = a_page->read_gen;
	b_lru = b_page->read_gen;

	/*
	 * Bias in favor of leaf pages.  Otherwise, we can waste time
	 * considering parent pages for eviction while their child pages are
	 * still in memory.
	 *
	 * Bump the LRU generation by a small fixed amount: the idea being that
	 * if we have enough good leaf page candidates, we should evict them
	 * first, but not completely ignore an old internal page.
	 */
	if (a_page->type == WT_PAGE_ROW_INT || a_page->type == WT_PAGE_COL_INT)
		a_lru += WT_EVICT_GROUP;
	if (b_page->type == WT_PAGE_ROW_INT || b_page->type == WT_PAGE_COL_INT)
		b_lru += WT_EVICT_GROUP;
	return (a_lru > b_lru ? 1 : (a_lru < b_lru ? -1 : 0));
}
