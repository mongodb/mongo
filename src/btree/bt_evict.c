/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static inline void __evict_clr(WT_EVICT_LIST *);
static inline void __evict_req_clr(SESSION *, WT_EVICT_REQ *);
static inline void __evict_req_set(SESSION *, WT_EVICT_REQ *, int);

static void __evict_dup_remove(SESSION *);
static int  __evict_file(SESSION *, WT_EVICT_REQ *);
static int  __evict_lru(SESSION *);
static int  __evict_lru_cmp(const void *, const void *);
static void __evict_page(SESSION *);
static int  __evict_page_cmp(const void *, const void *);
static int  __evict_request_retry(SESSION *);
static int  __evict_request_walk(SESSION *);
static int  __evict_walk(SESSION *);
static int  __evict_walk_file(SESSION *, u_int);
static int  __evict_worker(SESSION *);

/*
 * Tuning constants -- I hesitate to call this tuning, but we should review some
 * number of pages from each file's in-memory tree for each page we evict, and
 * we should amortize the comparison of the hazard references across some number
 * of eviction candidates.
 */
#define	WT_EVICT_GROUP		10	/* Evict N pages at a time */
#define	WT_EVICT_WALK_PER_TABLE	5	/* Pages to visit per file */
#define	WT_EVICT_WALK_BASE	25	/* Pages tracked across file visits */

/*
 * WT_EVICT_FOREACH --
 *	Walk a list of eviction candidates.
 */
#define	WT_EVICT_FOREACH(cache, p, i)					\
	for ((i) = 0, (p) = (cache)->evict; (i) < WT_EVICT_GROUP; ++(i), ++(p))

/*
 * WT_EVICT_REQ_FOREACH --
 *	Walk a list of eviction requests.
 */
#define	WT_EVICT_REQ_FOREACH(er, er_end, cache)				\
	for ((er) = (cache)->evict_request,				\
	    (er_end) = (er) + WT_ELEMENTS((cache)->evict_request);	\
	    (er) < (er_end); ++(er))

/*
 * __evict_clr --
 *	Clear an entry in the eviction list.
 */
static inline void
__evict_clr(WT_EVICT_LIST *e)
{
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __evict_req_set --
 *	Set an entry in the eviction request list.
 */
static inline void
__evict_req_set(SESSION *session, WT_EVICT_REQ *r, int close_method)
{
	r->close_method = close_method;
	WT_ASSERT(session, r->retry == NULL);
	WT_ASSERT(session, r->retry_next == 0);
	WT_ASSERT(session, r->retry_entries == 0);
	WT_ASSERT(session, r->retry_allocated == 0);
	WT_MEMORY_FLUSH;		/* Flush before turning entry on */

	r->session = session;
	WT_MEMORY_FLUSH;		/* Turn entry on */
}

/*
 * __evict_req_clr --
 *	Set an entry in the eviction request list.
 */
static inline void
__evict_req_clr(SESSION *session, WT_EVICT_REQ *r)
{
	if (r->retry != NULL)
		__wt_free(session, r->retry);
	r->retry_next = r->retry_entries = r->retry_allocated = 0;

	r->session = NULL;
	WT_MEMORY_FLUSH;		/* Turn entry off */
}

/*
 * __wt_workq_evict_server --
 *	See if the eviction server thread needs to be awakened.
 */
void
__wt_workq_evict_server(CONNECTION *conn, int force)
{
	SESSION *session;
	WT_CACHE *cache;
	uint64_t bytes_inuse, bytes_max;

	session = &conn->default_session;
	cache = conn->cache;

	/* If the eviction server is running, there's nothing to do. */
	if (!cache->evict_sleeping)
		return;

	/*
	 * If we're locking out reads, or over our cache limit, or forcing the
	 * issue (when closing the environment), run the eviction server.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = WT_STAT(cache->stats, cache_bytes_max);
	if (!force && !cache->read_lockout && bytes_inuse < bytes_max)
		return;

	WT_VERBOSE(conn, WT_VERB_EVICT, (session,
	    "waking eviction server: force %sset, read lockout %sset, "
	    "bytes inuse %s max (%lluMB %s %lluMB), ",
	    force ? "" : "not ", cache->read_lockout ? "" : "not ",
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    (unsigned long long)(bytes_inuse / WT_MEGABYTE),
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    (unsigned long long)(bytes_max / WT_MEGABYTE)));

	cache->evict_sleeping = 0;
	__wt_unlock(session, cache->mtx_evict);
}

/*
 * __evict_file_serial_func --
 *	Eviction serialization function called when a tree is being flushed
 *	or closed.
 */
int
__wt_evict_file_serial_func(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int close_method;

	__wt_evict_file_unpack(session, close_method);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->session == NULL) {
			__evict_req_set(session, er, close_method);
			return (0);
		}
	__wt_err(session, 0, "eviction server request table full");
	return (WT_ERROR);
}

/*
 * __wt_cache_evict_server --
 *	Thread to evict pages from the cache.
 */
void *
__wt_cache_evict_server(void *arg)
{
	CONNECTION *conn;
	SESSION *session;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/*
	 * We need a thread of control because we're reading/writing pages.
	 * Start with the default session to keep error handling simple.
	 *
	 * There is some complexity involved in using the public API, because
	 * public sessions are implicitly closed during WT_CONNECTION->close.
	 * If the eviction thread's session were to go on the public list, the
	 * eviction thread would have to be shut down before the public session
	 * handles are closed.
	 */
	session = &conn->default_session;
	WT_ERR(conn->session(conn, 0, &session));

	for (;;) {
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server sleeping"));
		cache->evict_sleeping = 1;
		__wt_lock(session, cache->mtx_evict);
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server waking"));

		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));
	}

	if (ret == 0) {
		if (__wt_cache_bytes_inuse(cache) != 0) {
			__wt_errx(session,
			    "cache server: exiting with %llu pages, %llu bytes "
			    "in use",
			    (unsigned long long)__wt_cache_pages_inuse(cache),
			    (unsigned long long)__wt_cache_bytes_inuse(cache));
			WT_ASSERT(session, 0);
		}
	} else
err:		__wt_err(session, ret, "cache eviction server error");

	WT_VERBOSE(conn, WT_VERB_EVICT,
	    (session, "cache eviction server exiting"));

	if (cache->evict != NULL)
		__wt_free(session, cache->evict);
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->retry != NULL)
			__wt_free(session, er->retry);

	if (session != &conn->default_session)
		(void)__wt_session_close(session);

	return (NULL);
}

/*
 * __wt_workq_evict_server_exit --
 *	The exit flag is set, wake the eviction server to exit.
 */
void
__wt_workq_evict_server_exit(CONNECTION *conn)
{
	SESSION *session;
	WT_CACHE *cache;

	session = &conn->default_session;
	cache = conn->cache;

	__wt_unlock(session, cache->mtx_evict);
}

/*
 * __evict_worker --
 *	Evict pages from memory.
 */
static int
__evict_worker(SESSION *session)
{
	WT_CACHE *cache;
	uint64_t bytes_start, bytes_inuse, bytes_max;
	int loop;

	cache = S2C(session)->cache;

	/* Evict pages from the cache. */
	for (loop = 0;;) {
		/* Walk the eviction-request queue. */
		WT_RET(__evict_request_walk(session));

		/*
		 * Eviction requests can temporarily fail when a tree is active,
		 * that is, we may not be able to immediately reconcile all of
		 * the file's pages.  If the pending_retry value is non-zero, it
		 * means there are pending requests we need to handle.
		 *
		 * Do general eviction even if we're just handling pending retry
		 * requests.  The problematic case is when reads are locked out
		 * because we're out of memory in the cache, a reading thread
		 * is blocked, and that thread has a hazard reference blocking
		 * us from reconciling a page that's part of a pending request.
		 * Keep pushing out blocks from the general pool as well as the
		 * pending requests until the system unjams.
		 */
		while (cache->pending_retry) {
			WT_RET(__evict_request_retry(session));
			if (cache->pending_retry)
				WT_RET(__evict_lru(session));
		}

		/*
		 * If we've locked out reads, keep evicting until we get to at
		 * least 5% under the maximum cache.  Else, quit evicting as
		 * soon as we get under the maximum cache.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = WT_STAT(cache->stats, cache_bytes_max);
		if (cache->read_lockout) {
			if (bytes_inuse <= bytes_max - (bytes_max / 20))
				break;
		} else if (bytes_inuse < bytes_max)
			break;

		WT_RET(__evict_lru(session));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		bytes_start = bytes_inuse;
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		if (bytes_start == bytes_inuse && ++loop == 10) {
			__wt_errx(session,
			    "eviction server: unable to evict cache pages");
			break;
		}
	}
	return (0);
}

/*
 * __evict_request_walk --
 *	Walk the eviction request queue.
 */
static int
__evict_request_walk(SESSION *session)
{
	SESSION *request_session;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int ret;

	cache = S2C(session)->cache;

	/*
	 * Walk the eviction request queue, looking for sync or close requests
	 * (defined by a valid SESSION handle).  If we find a request, perform
	 * it, flush the result and clear the request slot, then wake up the
	 * requesting thread.
	 */
	WT_EVICT_REQ_FOREACH(er, er_end, cache) {
		if ((request_session = er->session) == NULL)
			continue;

		/*
		 * The eviction candidate list might reference pages we are
		 * about to discard; clear it.
		 */
		memset(cache->evict, 0, cache->evict_allocated);

		/* Reference the correct BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, request_session->btree);

		ret = __evict_file(session, er);

		/*
		 * If we don't have any pages to retry, we're done, resolve the
		 * request.  If we have pages to retry, we have to wait for the
		 * main eviction loop to finish the work.
		 */
		if (er->retry == NULL) {
			__wt_session_serialize_wrapup(
			    request_session, NULL, ret);
			__evict_req_clr(session, er);
		} else
			cache->pending_retry = 1;
	}
	return (0);
}

/*
 * __evict_file --
 *	Flush pages for a specific file as part of a close/sync operation.
 */
static int
__evict_file(SESSION *session, WT_EVICT_REQ *er)
{
	BTREE *btree;
	WT_PAGE *page;
	int caller, ret;

	btree = session->btree;
	ret = 0;
	caller = er->close_method ? WT_REC_CLOSE : WT_REC_SYNC;

	/*
	 * Walk the tree.  It doesn't matter if we are already walking the tree,
	 * __wt_walk_begin restarts the process.
	 */
	WT_RET(
	    __wt_walk_begin(session, NULL, &btree->evict_walk, WT_WALK_CACHE));

	for (;;) {
		WT_ERR(__wt_walk_next(session, &btree->evict_walk, &page));
		if (page == NULL)
			break;

		/*
		 * Sync: only dirty pages need be reconciled.
		 * Close: discarding all of the file's pages from the cache,
		 * and reconciliation is how we do that.
		 */
		if (caller == WT_REC_SYNC && !WT_PAGE_IS_MODIFIED(page))
			continue;
		if (__wt_page_reconcile(session, page, 0, caller) == 0)
			continue;

		/*
		 * We weren't able to reconcile the page: possible in sync if
		 * another thread of control holds a hazard reference on the
		 * page we're reconciling (or a hazard reference on a deleted
		 * or split page in that page's subtree), is trying to read
		 * another page, and can't as the read subsystem is locked out
		 * right now.  Possible in close or sync if the file system is
		 * full.
		 *
		 * Add this page to the list of pages we'll have to retry.
		 */
		if (er->retry_next == er->retry_entries) {
			WT_ERR(__wt_realloc(session, &er->retry_allocated,
			    (er->retry_entries + 100) *
			    sizeof(*er->retry), &er->retry));
			er->retry_entries += 100;
		}
		er->retry[er->retry_next++] = page;
	}

err:	/* End the walk cleanly. */
	__wt_walk_end(session, &btree->evict_walk);

	return (ret);
}

/*
 * __evict_request_retry --
 *	Retry an eviction request.
 */
static int
__evict_request_retry(SESSION *session)
{
	SESSION *request_session;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	uint32_t i;
	int caller, pending_retry;

	cache = S2C(session)->cache;

	/* Reset the flag for pending retry requests. */
	cache->pending_retry = 0;

	/*
	 * Walk the eviction request queue, looking for sync or close requests
	 * that need to be retried (defined by a non-NULL retry reference).
	 */
	WT_EVICT_REQ_FOREACH(er, er_end, cache) {
		if (er->retry == NULL)
			continue;

		/*
		 * The eviction candidate list might reference pages we are
		 * about to discard; clear it.
		 */
		memset(cache->evict, 0, cache->evict_allocated);

		/* Reference the correct BTREE handle. */
		request_session = er->session;
		WT_SET_BTREE_IN_SESSION(session, request_session->btree);

		/*
		 * Set the reconcile caller: should never be close, but we
		 * can do the work even if it is.
		 */
		caller = er->close_method ? WT_REC_CLOSE : WT_REC_SYNC;

		/* Walk the list of retry requests. */
		for (pending_retry = 0, i = 0; i < er->retry_entries; ++i) {
			if (er->retry[i] == NULL)
				continue;
			if (__wt_page_reconcile(
			    session, er->retry[i], 0, caller) == 0)
				er->retry[i] = NULL;
			else
				pending_retry = 1;
		}

		/*
		 * If we finished, clean up and resolve the request, otherwise
		 * there's still work to do.
		 */
		if (pending_retry)
			cache->pending_retry = 1;
		else {
			__wt_session_serialize_wrapup(request_session, NULL, 0);
			__evict_req_clr(session, er);
		}
	}
	return (0);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(SESSION *session)
{
	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session));

	/* Remove duplicates from the list. */
	__evict_dup_remove(session);

	/* Reconcile and discard the pages. */
	__evict_page(session);

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(SESSION *session)
{
	CONNECTION *conn;
	BTREE *btree;
	WT_CACHE *cache;
	u_int elem, i;
	int ret;

	conn = S2C(session);
	cache = S2C(session)->cache;

	/*
	 * Resize the array in which we're tracking pages, as necessary, then
	 * get some pages from each underlying file.  We hold a mutex for the
	 * entire time -- it's slow, but (1) how often do new files get added
	 * or removed to/from the system, and (2) it's all in-memory stuff, so
	 * it's not that slow.
	 */
	ret = 0;
	__wt_lock(session, conn->mtx);
	elem = WT_EVICT_WALK_BASE + (conn->dbqcnt * WT_EVICT_WALK_PER_TABLE);
	if (elem <= cache->evict_entries || (ret = __wt_realloc(session,
	    &cache->evict_allocated,
	    elem * sizeof(WT_EVICT_LIST), &cache->evict)) == 0) {
		cache->evict_entries = elem;

		i = WT_EVICT_WALK_BASE;
		TAILQ_FOREACH(btree, &conn->dbqh, q) {
			/* Reference the correct BTREE handle. */
			WT_SET_BTREE_IN_SESSION(session, btree);

			WT_ERR(__evict_walk_file(session, i));
			i += WT_EVICT_WALK_PER_TABLE;
		}
	}
err:	__wt_unlock(session, conn->mtx);
	return (ret);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(SESSION *session, u_int slot)
{
	BTREE *btree;
	WT_CACHE *cache;
	WT_PAGE *page;
	int i, restarted_once;

	btree = session->btree;
	cache = S2C(session)->cache;

	/*
	 * Tricky little loop that restarts the walk as necessary, without
	 * resetting the count of pages retrieved.
	 */
	i = restarted_once = 0;

	/* If we haven't yet started this walk, do so. */
	if (btree->evict_walk.tree == NULL)
walk:		WT_RET(__wt_walk_begin(
		    session, NULL, &btree->evict_walk, WT_WALK_CACHE));

	/* Get the next WT_EVICT_WALK_PER_TABLE entries. */
	while (i < WT_EVICT_WALK_PER_TABLE) {
		WT_RET(__wt_walk_next(session, &btree->evict_walk, &page));

		/*
		 * Restart the walk as necessary,  but only once (after one
		 * restart we've already visited all of the in-memory pages,
		 * we could loop infinitely on a tree with too few pages).
		 */
		if (page == NULL) {
			if (restarted_once++)
				break;
			goto walk;
		}

		/*
		 * Pinned pages can't be evicted, and it's not useful to try
		 * and evict deleted or split pages.
		 */
		if (F_ISSET(page,
		    WT_PAGE_PINNED | WT_PAGE_DELETED | WT_PAGE_SPLIT))
			continue;

		/* During verification, we can only evict clean pages. */
		if (cache->only_evict_clean && WT_PAGE_IS_MODIFIED(page))
			continue;

		cache->evict[slot].page = page;
		cache->evict[slot].btree = btree;
		++slot;
		++i;
	}

	return (0);
}

/*
 * __evict_dup_remove --
 *	Discard duplicates from the list of pages we collected.
 */
static void
__evict_dup_remove(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	u_int elem, i, j;

	cache = S2C(session)->cache;

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
	evict = cache->evict;
	elem = cache->evict_entries;
	qsort(evict,
	    (size_t)elem, sizeof(WT_EVICT_LIST), __evict_page_cmp);
	for (i = 0; i < elem; i = j)
		for (j = i + 1; j < elem; ++j) {
			/*
			 * If the leading pointer hits a NULL, we're done, the
			 * NULLs all sorted to the top of the array.
			 */
			if (evict[j].page == NULL)
				return;

			/* Delete the second and any subsequent duplicates. */
			if (evict[i].page == evict[j].page)
				__evict_clr(&evict[j]);
			else
				break;
		}
}

/*
 * __evict_page --
 *	Reconcile and discard cache pages.
 */
static void
__evict_page(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE *page;
	u_int i;

	cache = S2C(session)->cache;

	/* Sort the array by LRU, then evict the most promising candidates. */
	qsort(cache->evict, (size_t)cache->evict_entries,
	    sizeof(WT_EVICT_LIST), __evict_lru_cmp);

	WT_EVICT_FOREACH(cache, evict, i) {
		if ((page = evict->page) == NULL)
			continue;

		/* Reference the correct BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, evict->btree);

		/*
		 * Paranoia: remove the entry so we never try and reconcile
		 * the same page on reconciliation error.
		 */
		__evict_clr(evict);

		/*
		 * For now, we don't care why reconciliation failed -- we expect
		 * the reason is we were unable to get exclusive access for the
		 * page, but it might be we're out of disk space.   Regardless,
		 * try not to pick the same page every time.
		 */
		if (__wt_page_reconcile(session, page, 0, WT_REC_EVICT) != 0)
			page->read_gen = __wt_cache_read_gen(session);
	}
}

/*
 * __evict_page_cmp --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's address.
 */
static int
__evict_page_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

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

	/* Sort the page address in ascending order. */
	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
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
	return (a_lru > b_lru ? 1 : (a_lru < b_lru ? -1 : 0));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __evict_dump --
 *	Display the eviction list.
 */
void
__wt_evict_dump(SESSION *session)
{
	FILE *fp;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	u_int n;
	const char *sep;

	cache = S2C(session)->cache;

	if ((fp = fopen("xx.log", "w")) == NULL)
		return;
	for (sep = "\tdump:", n = 0; n < cache->evict_entries; ++n) {
		evict = &cache->evict[n];
		if (evict->page != NULL) {
			fprintf(fp, "%s %lu", sep, (u_long)evict->page->addr);
			sep = ",";
		}
	}
	fprintf(fp, "\n");
	(void)fclose(fp);
}
#endif
