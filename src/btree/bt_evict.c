/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_file(WT_SESSION_IMPL *, WT_EVICT_REQ *);
static int  __evict_lru(WT_SESSION_IMPL *);
static int  __evict_lru_cmp(const void *, const void *);
static void __evict_lru_sort(WT_SESSION_IMPL *);
static void __evict_pages(WT_SESSION_IMPL *);
static int  __evict_request_walk(WT_SESSION_IMPL *);
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
 *	Walk a list of eviction requests.
 */
#define	WT_EVICT_REQ_FOREACH(er, er_end, cache)				\
	for ((er) = (cache)->evict_request,				\
	    (er_end) = (er) + (cache)->max_evict_request;		\
	    (er) < (er_end); ++(er))

/*
 * __evict_clr --
 *	Clear an entry in the eviction list.
 */
static inline void
__evict_clr(WT_SESSION_IMPL *session, WT_EVICT_LIST *e)
{
	if (e->page != NULL) {
		WT_ASSERT(session, F_ISSET_ATOMIC(e->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->page, WT_PAGE_EVICT_LRU);
	}
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __evict_clr_all --
 *	Clear all entries in the eviction list.
 */
static inline void
__evict_clr_all(WT_SESSION_IMPL *session, u_int start)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	uint32_t i, elem;

	cache = S2C(session)->cache;

	elem = cache->evict_entries;
	for (i = start, evict = cache->evict + i; i < elem; i++, evict++)
		__evict_clr(session, evict);
}

/*
 * __wt_evict_clr_page --
 *	Make sure a page is not in the eviction request list.  This called
 *	from inside __rec_review to make sure there is no attempt to evict
 *	child pages multiple times.
 */
void
__wt_evict_clr_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	uint32_t i, elem;

	WT_ASSERT(session, WT_PAGE_IS_ROOT(page) ||
	    page->ref->page != page ||
	    page->ref->state == WT_REF_LOCKED);

	/* Fast path: if the page isn't on the queue, don't bother searching. */
	if (!F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, &cache->lru_lock);

	elem = cache->evict_entries;
	for (evict = cache->evict, i = 0; i < elem; i++, evict++)
		if (evict->page == page) {
			__evict_clr(session, evict);
			break;
		}

	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU));

	__wt_spin_unlock(session, &cache->lru_lock);
}

/*
 * __evict_req_set --
 *	Set an entry in the eviction request list.
 */
static inline void
__evict_req_set(
    WT_SESSION_IMPL *session, WT_EVICT_REQ *r, WT_PAGE *page, int fileop)
{
					/* Should be empty */
	WT_ASSERT(session, r->session == NULL);

	WT_CLEAR(*r);
	r->btree = session->btree;
	r->page = page;
	r->fileop = fileop;

	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(r->session, session);
}

/*
 * __evict_req_clr --
 *	Clear an entry in the eviction request list.
 */
static inline void
__evict_req_clr(WT_SESSION_IMPL *session, WT_EVICT_REQ *r)
{
	WT_UNUSED(session);

	/*
	 * Publish; there must be a barrier to ensure the structure fields are
	 * set before the entry is made available for re-use.
	 */
	WT_PUBLISH(r->session, NULL);
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

	WT_VERBOSE(session, evictserver,
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
	WT_EVICT_REQ *er, *er_end;
	int fileop;

	__wt_sync_file_unpack(session, &fileop);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->session == NULL) {
			__evict_req_set(session, er, NULL, fileop);
			return;
		}

	__wt_errx(session, "eviction server request table full");
	__wt_session_serialize_wrapup(session, NULL, WT_ERROR);
}

/*
 * __wt_evict_page_request --
 *	Schedule a page for forced eviction due to a high volume of inserts or
 *	updates.
 *
 *	NOTE: this function is called from inside serialized functions, so it
 *	is holding the serial lock.
 */
int
__wt_evict_page_request(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int first;

	cache = S2C(session)->cache;
	first = 1;

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
	if (!WT_ATOMIC_CAS(page->ref->state, WT_REF_MEM, WT_REF_EVICT_FORCE))
		return (0);

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->session == NULL) {
			/* Always leave one empty slot */
			if (first) {
				first = 0;
				continue;
			}
			__evict_req_set(session, er, page, 0);
			__wt_evict_server_wake(session);
			return (0);
		}

	/*
	 * The request table is full, that's okay for page requests: another
	 * thread will see this later.
	 */
	WT_VERBOSE(session, evictserver, "eviction server request table full");
	page->ref->state = WT_REF_MEM;
	return (WT_RESTART);
}

/*
 * __wt_cache_evict_server --
 *	Thread to evict pages from the cache.
 */
void *
__wt_cache_evict_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	WT_CACHE *cache;
	int read_lockout, ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/*
	 * We need a session handle because we're reading/writing pages.
	 * Start with the default session to keep error handling simple.
	 */
	session = &conn->default_session;
	WT_ERR(__wt_open_session(conn, 1, NULL, NULL, &session));

	while (F_ISSET(conn, WT_SERVER_RUN)) {
		/*
		 * Use the same logic as application threads to decide whether
		 * there is work to do.
		 */
		__wt_eviction_check(session, &read_lockout, 0);

		if (!read_lockout) {
			WT_VERBOSE(session, evictserver, "sleeping");
			__wt_cond_wait(session, cache->evict_cond);
		}

		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(session, evictserver, "waking");

		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));
	}

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

	WT_VERBOSE(session, evictserver, "exiting");

	__wt_free(session, cache->evict);

	if (session != &conn->default_session)
		(void)session->iface.close(&session->iface, NULL);

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
		/* Walk the eviction-request queue. */
		WT_RET(__evict_request_walk(session));

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
				WT_VERBOSE(session, evictserver,
				    "unable to reach eviction goal");
				break;
			}
		} else
			loop = 0;
	}
	return (0);
}

/*
 * __evict_request_walk --
 *	Walk the eviction request queue.
 */
static int
__evict_request_walk(WT_SESSION_IMPL *session)
{
	WT_SESSION_IMPL *request_session;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	WT_PAGE *page;
	WT_REF *ref;
	int ret;

	cache = S2C(session)->cache;

	/*
	 * Walk the eviction request queue, looking for sync/close or page flush
	 * requests.  If we find a request, perform it, clear the request slot
	 * and wake up the requesting thread if necessary.
	 */
	WT_EVICT_REQ_FOREACH(er, er_end, cache) {
		if ((request_session = er->session) == NULL)
			continue;

		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, er->btree);

		/*
		 * Block out concurrent eviction while we are handling this
		 * request.
		 */
		__wt_spin_lock(session, &cache->lru_lock);

		/*
		 * The eviction candidate list might reference pages we are
		 * about to discard; clear it.
		 */
		__evict_clr_all(session, 0);

		/* Clear the current eviction point. */
		page = session->btree->evict_page;
		while (page != NULL && !WT_PAGE_IS_ROOT(page)) {
			ref = page->ref;
			page = page->parent;
			if (ref->state == WT_REF_EVICT_WALK)
				ref->state = WT_REF_MEM;
		}
		session->btree->evict_page = NULL;

		/*
		 * Wait for LRU eviction activity to drain.  It is much easier
		 * to reason about sync or forced eviction if we can be sure
		 * there are no other threads evicting in the tree.
		 */
		while (session->btree->lru_count > 0)
			__wt_yield();

		if (er->page == NULL) {
			WT_VERBOSE(session, evictserver,
			    "file request: %s",
			    (er->fileop == WT_SYNC ? "sync" :
			    (er->fileop == WT_SYNC_DISCARD ?
			    "sync-discard" : "sync-discard-nowrite")));

			/*
			 * If we're about to do a walk of the file tree (and
			 * possibly close the file), any page we're referencing
			 * won't be useful; Discard any page we're holding and
			 * we can restart our walk as needed.
			 */
			ret = __evict_file(session, er);
		} else {
			WT_VERBOSE(session, evictserver,
			    "forcing eviction of page %p", er->page);

			ref = er->page->ref;
			WT_ASSERT(session, ref->page == er->page);
			WT_ASSERT(session, ref->state == WT_REF_EVICT_FORCE);
			ref->state = WT_REF_LOCKED;

			/*
			 * At this point, the page is locked, which stalls new
			 * readers.  Pause before attempting to evict it to
			 * give existing readers a chance to drop their
			 * references.
			 */
			__wt_yield();

			/*
			 * If eviction fails, it will free up the page: hope it
			 * works next time.  Application threads may be holding
			 * a reference while trying to get another (e.g., if
			 * they have two cursors open), so blocking
			 * indefinitely leads to deadlock.
			 */
			ret = __wt_rec_evict(session, er->page, 0);
		}

		__wt_spin_unlock(session, &cache->lru_lock);

		/* Clear the reference to the btree handle. */
		WT_CLEAR_BTREE_IN_SESSION(session);

		/*
		 * Resolve the request and clear the slot.
		 *
		 * !!!
		 * Page eviction is special: the requesting thread is already
		 * inside wrapup.
		 */
		if (er->page == NULL)
			__wt_session_serialize_wrapup(
			    request_session, NULL, ret);

		__evict_req_clr(session, er);
	}
	return (0);
}

/*
 * __evict_file --
 *	Flush pages for a specific file as part of a close/sync operation.
 */
static int
__evict_file(WT_SESSION_IMPL *session, WT_EVICT_REQ *er)
{
	WT_PAGE *next_page, *page;

	/*
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	next_page = NULL;
	WT_RET(__wt_tree_np(session, &next_page, 1, 1));
	for (;;) {
		if ((page = next_page) == NULL)
			break;
		WT_RET(__wt_tree_np(session, &next_page, 1, 1));

		/* Write dirty pages for sync, and sync with discard. */
		switch (er->fileop) {
		case WT_SYNC:
		case WT_SYNC_DISCARD:
			if (__wt_page_is_modified(page))
				WT_RET(__wt_rec_write(session, page, NULL));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			break;
		}

		/*
		 * Evict the page for sync with discard, simply discard the page
		 * for discard alone.
		 */
		switch (er->fileop) {
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
				WT_RET(__wt_rec_evict(
				    session, page, WT_REC_SINGLE));
			break;
		case WT_SYNC_DISCARD_NOWRITE:
			__wt_page_out(session, &page, 0);
			break;
		}
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
	__wt_spin_lock(session, &cache->lru_lock);
	__evict_lru_sort(session);
	__evict_clr_all(session, WT_EVICT_WALK_BASE);

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->lru_lock);

	/* Reconcile and discard some pages. */
	__evict_pages(session);

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_BTREE *btree;
	WT_CACHE *cache;
	u_int elem, i;
	int ret;

	conn = S2C(session);
	cache = S2C(session)->cache;
	ret = 0;

	/*
	 * We hold a spinlock for the entire walk -- it's slow, but (1) how
	 * often do new files get added or removed to/from the system, and (2)
	 * it's all in-memory stuff, so it's not that slow.
	 */
	__wt_spin_lock(session, &conn->spinlock);

	/*
	 * Resize the array in which we're tracking pages, as necessary, then
	 * get some pages from each underlying file.  In practice, a realloc
	 * is rarely needed, so it is worth avoiding the LRU lock.
	 */
	elem = WT_EVICT_WALK_BASE + (conn->btqcnt * WT_EVICT_WALK_PER_TABLE);
	if (elem > cache->evict_entries) {
		/* Save the offset of the eviction point. */
		__wt_spin_lock(session, &cache->lru_lock);
		i = (u_int)(cache->evict_current - cache->evict);
		WT_ERR(__wt_realloc(session, &cache->evict_allocated,
		    elem * sizeof(WT_EVICT_LIST), &cache->evict));
		cache->evict_entries = elem;
		if (cache->evict_current != NULL)
			cache->evict_current = cache->evict + i;
		__wt_spin_unlock(session, &cache->lru_lock);
	}

	i = WT_EVICT_WALK_BASE;
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, btree);

		ret = __evict_walk_file(session, &i);

		WT_CLEAR_BTREE_IN_SESSION(session);

		if (ret != 0)
			break;
	}

	if (0) {
err:		__wt_spin_unlock(session, &cache->lru_lock);
	}
	__wt_spin_unlock(session, &conn->spinlock);
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
	WT_EVICT_LIST *end, *evict, *start;
	WT_PAGE *page;
	int restarts, ret;

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
	for (evict = start, restarts = ret = 0;
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

		WT_VERBOSE(session, evictserver,
		    "select: %p, size %" PRIu32, page, page->memory_footprint);

		WT_ASSERT(session, evict->page == NULL);
		evict->page = page;
		evict->btree = btree;
		++evict;

		/* Mark the page on the list */
		F_SET_ATOMIC(page, WT_PAGE_EVICT_LRU);
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
	    cache->evict_entries, sizeof(WT_EVICT_LIST), __evict_lru_cmp);
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
	WT_EVICT_LIST *evict;
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
		if (__wt_spin_trylock(session, &cache->lru_lock) == 0)
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
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, including pages on the request queue for
		 * forced eviction, this operation will fail and we will move
		 * on.
		 */
		ref = evict->page->ref;
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
		__evict_clr(session, evict);
		break;
	}

	if (is_app && *pagep == NULL)
		cache->evict_current = NULL;
	__wt_spin_unlock(session, &cache->lru_lock);
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
	 * We don't care why eviction failed (maybe the page was dirty and we're
	 * out of disk space, or the page had an in-memory subtree already being
	 * evicted).  Regardless, don't pick the same page every time.
	 *
	 * We used to bump the page's read_gen only if eviction failed, but
	 * that isn't safe: at that point, eviction has already unlocked the
	 * page and some other thread may have evicted it by the time we look
	 * at it.
	 */
	page->read_gen = __wt_cache_read_gen(session);
	(void)__wt_rec_evict(session, page, 0);

	WT_ATOMIC_ADD(btree->lru_count, -1);

	WT_CLEAR_BTREE_IN_SESSION(session);
	session->btree = saved_btree;

	return (0);
}

/*
 * __evict_page --
 *	Reconcile and discard cache pages.
 */
static void
__evict_pages(WT_SESSION_IMPL *session)
{
	while (__wt_evict_lru_page(session, 0) == 0)
		;
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's read
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
	a_page = ((WT_EVICT_LIST *)a)->page;
	b_page = ((WT_EVICT_LIST *)b)->page;
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
