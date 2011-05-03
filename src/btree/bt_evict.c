/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int  __evict(SESSION *);
static void __evict_dup_remove(SESSION *);
static int  __evict_file_close(SESSION *);
static int  __evict_file_sync(SESSION *);
static int  __evict_lru_cmp(const void *, const void *);
static int  __evict_page(SESSION *);
static int  __evict_page_cmp(const void *, const void *);
static int  __evict_request_walk(SESSION *);
static int  __evict_walk(SESSION *);
static int  __evict_walk_file(SESSION *, BTREE *, u_int);
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
 * WT_EVICT_REF_CLR --
 *	Clear an eviction list entry.
 */
#define	WT_EVICT_CLR(p) do {						\
	(p)->ref = NULL;						\
	(p)->btree = WT_DEBUG_POINT;					\
} while (0)

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
	BTREE *btree;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int close_method;

	__wt_evict_file_unpack(session, btree, close_method);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the eviction request. */
	er = cache->evict_request;
	er_end = er + WT_ELEMENTS(cache->evict_request);
	for (; er < er_end; ++er)
		if (WT_EVICT_REQ_ISEMPTY(er)) {
			WT_EVICT_REQ_SET(er, session, btree, close_method);
			return (0);
		}
	__wt_err(session, 0, "eviction server request table full");
	return (WT_RESTART);
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

	/*
	 * Allocate memory for a copy of the hazard references -- it's a fixed
	 * size so doesn't need run-time adjustments.
	 */
	WT_ERR(__wt_calloc_def(session,
	    conn->session_size * conn->hazard_size, &cache->hazard));

	for (;;) {
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server sleeping"));
		cache->evict_sleeping = 1;
		__wt_lock(session, cache->mtx_evict);
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server waking"));

		/* Walk the eviction-request queue. */
		WT_ERR(__evict_request_walk(session));

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
	if (cache->hazard != NULL)
		__wt_free(session, cache->hazard);

	if (session != &conn->default_session)
		WT_TRET(__wt_session_close(session));

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
	er = cache->evict_request;
	er_end = er + WT_ELEMENTS(cache->evict_request);
	for (; er < er_end; ++er) {
		if ((request_session = er->session) == NULL)
			continue;

		/*
		 * The eviction queue might reference pages we are about to
		 * discard; clear it.
		 */
		memset(cache->evict, 0, cache->evict_len);

		/* Reference the correct BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, request_session->btree);

		ret = er->close_method ?
		    __evict_file_close(session) : __evict_file_sync(session);
		WT_EVICT_REQ_CLR(er);

		__wt_session_serialize_wrapup(request_session, NULL, ret);
	}
	return (0);
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

		bytes_start = bytes_inuse;
		WT_RET(__evict(session));
		bytes_inuse = __wt_cache_bytes_inuse(cache);

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		if (bytes_start == bytes_inuse && ++loop == 10) {
			__wt_errx(session,
			    "cache server: unable to evict pages from the "
			    "cache");
			break;
		}
	}
	return (0);
}

/*
 * __evict_file_close --
 *	Flush pages for a specific file as part of a close operation.
 */
static int
__evict_file_close(SESSION *session)
{
	BTREE *btree;
	WT_REF *ref;
	int ret;

	btree = session->btree;
	ret = 0;

	/*
	 * Walk the tree.  It doesn't matter if we are already walking the tree,
	 * __wt_walk_begin restarts the process.
	 */
	WT_RET(__wt_walk_begin(session, &btree->root_page, &btree->evict_walk));

	for (;;) {
		WT_ERR(__wt_walk_next(
		    session, &btree->evict_walk, WT_WALK_CACHE, &ref));
		if (ref == NULL)
			break;

		/*
		 * We're discarding all of the file's pages from the cache and
		 * reconciliation is how we do that.
		 */
		WT_ERR(
		    __wt_page_reconcile(session, ref->page, 0, WT_REC_CLOSE));
	}

err:	/* End the walk cleanly. */
	__wt_walk_end(session, &btree->evict_walk);

	return (ret);
}

/*
 * __evict_file_sync --
 *	Flush pages for a specific file as part of a sync operation.
 */
static int
__evict_file_sync(SESSION *session)
{
	BTREE *btree;
	WT_REF *ref;
	int ret;

	btree = session->btree;
	ret = 0;

	/*
	 * Walk the tree.  It doesn't matter if we are already walking the tree,
	 * __wt_walk_begin restarts the process.
	 */
	WT_RET(__wt_walk_begin(session, &btree->root_page, &btree->evict_walk));

	for (;;) {
		WT_ERR(__wt_walk_next(
		    session, &btree->evict_walk, WT_WALK_CACHE, &ref));
		if (ref == NULL)
			break;

		/* Only dirty pages need be reconciled. */
		if (WT_PAGE_IS_MODIFIED(ref->page))
			WT_ERR(__wt_page_reconcile(
			    session, ref->page, 0, WT_REC_SYNC));
	}

err:	/* End the walk cleanly. */
	__wt_walk_end(session, &btree->evict_walk);

	return (ret);
}

/*
 * __evict --
 *	Evict pages from the cache.
 */
static int
__evict(SESSION *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session));

	/* Remove duplicates from the list. */
	__evict_dup_remove(session);

	/* Sort the array by LRU. */
	qsort(cache->evict, (size_t)cache->evict_elem,
	    sizeof(WT_EVICT_LIST), __evict_lru_cmp);

	/* Reconcile and discard the pages. */
	WT_RET(__evict_page(session));

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(SESSION *session)
{
	BTREE *btree;
	CONNECTION *conn;
	WT_CACHE *cache;
	u_int elem, i;
	int ret;

	conn = S2C(session);
	cache = conn->cache;

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
	if (elem <= cache->evict_elem || (ret = __wt_realloc(session,
	    &cache->evict_len,
	    elem * sizeof(WT_EVICT_LIST), &cache->evict)) == 0) {
		cache->evict_elem = elem;

		i = WT_EVICT_WALK_BASE;
		TAILQ_FOREACH(btree, &conn->dbqh, q) {
			WT_ERR(__evict_walk_file(session, btree, i));
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
__evict_walk_file(SESSION *session, BTREE *btree, u_int slot)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_REF *ref;
	int i, restarted_once;

	cache = S2C(session)->cache;

	/*
	 * Tricky little loop that restarts the walk as necessary, without
	 * resetting the count of pages retrieved.
	 */
	i = restarted_once = 0;

	/* If we haven't yet started this walk, do so. */
	if (btree->evict_walk.tree == NULL)
restart:	WT_RET(__wt_walk_begin(session,
		    &btree->root_page, &btree->evict_walk));

	/* Get the next WT_EVICT_WALK_PER_TABLE entries. */
	while (i < WT_EVICT_WALK_PER_TABLE) {
		WT_RET(__wt_walk_next(session,
		    &btree->evict_walk, WT_WALK_CACHE, &ref));

		/*
		 * Restart the walk as necessary,  but only once (after one
		 * restart we've already visited all of the in-memory pages,
		 * we could loop infinitely on a tree with too few pages).
		 */
		if (ref == NULL) {
			if (restarted_once++)
				break;
			goto restart;
		}
		page = ref->page;

		/* Pinned pages can't be evicted. */
		if (F_ISSET(page, WT_PAGE_PINNED))
			continue;

		/* During verification, we can only evict clean pages. */
		if (cache->only_evict_clean && WT_PAGE_IS_MODIFIED(page))
			continue;

		cache->evict[slot].ref = ref;
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
	elem = cache->evict_elem;
	qsort(evict,
	    (size_t)elem, sizeof(WT_EVICT_LIST), __evict_page_cmp);
	for (i = 0; i < elem; i = j)
		for (j = i + 1; j < elem; ++j) {
			/*
			 * If the leading pointer hits a NULL, we're done, the
			 * NULLs all sorted to the top of the array.
			 */
			if (evict[j].ref == NULL)
				return;

			/* Delete the second and any subsequent duplicates. */
			if (evict[i].ref == evict[j].ref)
				WT_EVICT_CLR(&evict[j]);
			else
				break;
		}
}

/*
 * __evict_page --
 *	Reconcile and discard cache pages.
 */
static int
__evict_page(SESSION *session)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE *page;
	WT_REF *ref;
	u_int i;

	conn = S2C(session);
	cache = conn->cache;

	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		page = ref->page;

		/* Reference the correct BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, evict->btree);

		/*
		 * Paranoia: remove the entry so we never try and reconcile
		 * the same page on reconciliation error.
		 */
		WT_EVICT_CLR(evict);

		WT_RET(__wt_page_reconcile(session, page, 0, WT_REC_EVICT));
	}
	return (0);
}

/*
 * __evict_page_cmp --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's address.
 */
static int
__evict_page_cmp(const void *a, const void *b)
{
	WT_REF *a_ref, *b_ref;
	WT_PAGE *a_page, *b_page;

	/*
	 * There may be NULL references in the array; sort them as greater than
	 * anything else so they migrate to the end of the array.
	 */
	a_ref = ((WT_EVICT_LIST *)a)->ref;
	b_ref = ((WT_EVICT_LIST *)b)->ref;
	if (a_ref == NULL)
		return (b_ref == NULL ? 0 : 1);
	if (b_ref == NULL)
		return (-1);

	/* Sort the page address in ascending order. */
	a_page = a_ref->page;
	b_page = b_ref->page;
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
	WT_REF *a_ref, *b_ref;
	uint64_t a_lru, b_lru;

	/*
	 * There may be NULL references in the array; sort them as greater than
	 * anything else so they migrate to the end of the array.
	 */
	a_ref = ((WT_EVICT_LIST *)a)->ref;
	b_ref = ((WT_EVICT_LIST *)b)->ref;
	if (a_ref == NULL)
		return (b_ref == NULL ? 0 : 1);
	if (b_ref == NULL)
		return (-1);

	/* Sort the LRU in ascending order. */
	a_lru = a_ref->page->read_gen;
	b_lru = b_ref->page->read_gen;
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
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_MBUF mb;
	u_int n;
	int sep;

	cache = S2C(session)->cache;

	__wt_mb_init(session, &mb);
	__wt_mb_add(&mb, "eviction list");

	for (sep = ':', n = 0; n < cache->evict_elem; ++n) {
		evict = &cache->evict[n];
		if (evict->ref == NULL)
			continue;
		__wt_mb_add(&mb, "%c %lu", sep, (u_long)evict->ref->page->addr);
		sep = ',';
	}
	__wt_mb_discard(&mb);
}
#endif
