/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int  __wt_evict(SESSION *);
static void __wt_evict_clean(SESSION *);
static int  __wt_evict_compare_lru(const void *a, const void *b);
static int  __wt_evict_compare_page(const void *a, const void *b);
static int  __wt_evict_dirty(SESSION *);
static void __wt_evict_hazard_check(SESSION *);
static int  __wt_evict_hazard_compare(const void *a, const void *b);
static void __wt_evict_set(SESSION *);
static void __wt_evict_state_check(SESSION *);
static int  __wt_evict_subtrees(WT_PAGE *);
static int  __wt_evict_walk(SESSION *);
static int  __wt_evict_walk_single(SESSION *, BTREE *, u_int);

#ifdef HAVE_DIAGNOSTIC
static void __wt_evict_hazard_validate(CONNECTION *, WT_PAGE *);
#endif

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
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
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
 * __wt_cache_evict_server --
 *	Thread to evict pages from the cache.
 */
void *
__wt_cache_evict_server(void *arg)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	SESSION *session;
	uint64_t bytes_inuse, bytes_max;
	int ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/* We need a thread of control because we're reading/writing pages. */
	session = NULL;
	WT_ERR(__wt_session_api_set(conn,
	    "CacheReconciliation", NULL, &session));

	/*
	 * Multiple pages are marked for eviction by the eviction server, which
	 * means nobody can read them -- but, this thread of control has to
	 * update higher pages in the tree when it writes this page, which
	 * requires reading other pages, which might themselves be marked for
	 * eviction.   Set a flag to allow this thread of control to see pages
	 * marked for eviction -- we know it's safe, because only this thread
	 * is writing pages.
	 *
	 * Reconciliation is probably running because the cache is full, which
	 * means reads are locked out -- reconciliation can read, regardless.
	 */
	F_SET(session, WT_READ_EVICT | WT_READ_PRIORITY);

	/*
	 * Allocate memory for a copy of the hazard references -- it's a fixed
	 * size so doesn't need run-time adjustments.
	 */
	cache->hazard_elem = conn->session_size * conn->hazard_size;
	WT_ERR(__wt_calloc_def(session, cache->hazard_elem, &cache->hazard));
	cache->hazard_len = cache->hazard_elem * WT_SIZEOF32(WT_PAGE *);

	for (;;) {
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server sleeping"));
		cache->evict_sleeping = 1;
		__wt_lock(session, cache->mtx_evict);
		WT_VERBOSE(conn,
		    WT_VERB_EVICT, (session, "eviction server waking"));

		/*
		 * Check for environment exit; do it here, instead of the top of
		 * the loop because doing it here keeps us from doing a bunch of
		 * worked when simply awakened to quit.
		 */
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;

		for (;;) {
			/* Single-thread reconciliation. */
			__wt_lock(session, cache->mtx_reconcile);
			ret = __wt_evict(session);
			__wt_unlock(session, cache->mtx_reconcile);
			if (ret != 0)
				goto err;

			/*
			 * If we've locked out reads, keep evicting until we
			 * get to at least 5% under the maximum cache.  Else,
			 * quit evicting as soon as we get under the maximum
			 * cache.
			 */
			bytes_inuse = __wt_cache_bytes_inuse(cache);
			bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
			if (cache->read_lockout) {
				if (bytes_inuse <= bytes_max - (bytes_max / 20))
					break;
			} else if (bytes_inuse < bytes_max)
				break;
		}
	}

err:	if (cache->evict != NULL)
		__wt_free(session, cache->evict, cache->evict_len);
	if (cache->hazard != NULL)
		__wt_free(session, cache->hazard, cache->hazard_len);
	if (session != NULL)
		WT_TRET(session->close(session, 0));

	if (ret != 0)
		__wt_err(session, ret, "cache eviction server error");

	WT_VERBOSE(
	    conn, WT_VERB_EVICT, (session, "cache eviction server exiting"));

	return (NULL);
}

/*
 * __wt_evict --
 *	Evict pages from the cache.
 */
static int
__wt_evict(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	u_int elem, i, j;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__wt_evict_walk(session));

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
	    (size_t)elem, sizeof(WT_EVICT_LIST), __wt_evict_compare_page);
	for (i = 0; i < elem; i = j)
		for (j = i + 1; j < elem; ++j) {
			/*
			 * If the leading pointer hits a NULL, we're done, the
			 * NULLs all sorted to the top of the array.
			 */
			if (evict[j].ref == NULL)
				goto done_duplicates;

			/* Delete the second and any subsequent duplicates. */
			if (evict[i].ref == evict[j].ref)
				WT_EVICT_CLR(&evict[j]);
			else
				break;
		}
done_duplicates:

	/* Second, sort the array by LRU. */
	qsort(evict,
	    (size_t)elem, sizeof(WT_EVICT_LIST), __wt_evict_compare_lru);

	/*
	 * Discarding pages is done in 5 steps:
	 *	Set the WT_REF_EVICT state
	 *	Check for any hazard references
	 *	Discard clean pages
	 *	Reconcile dirty pages (making them clean)
	 *	Discard clean pages
	 *
	 * The reason we release clean pages first, then release dirty pages,
	 * is because reconciling a dirty page is a slow operation, and it
	 * releases space sooner.   (Arguably, we are going to discard all of
	 * the pages anyway, so what does it matter if we make clean pages
	 * wait for the dirty page writes?   On the other hand, it's a small
	 * change and benefits any thread waiting to read a clean page picked
	 * for discarding, unlikely though that may be.)
	 */
	__wt_evict_set(session);
	__wt_evict_hazard_check(session);
	__wt_evict_state_check(session);
	__wt_evict_clean(session);
	WT_RET(__wt_evict_dirty(session));

	return (0);
}

/*
 * __wt_evict_walk --
 *	Fill in the array by walk the next set of pages.
 */
static int
__wt_evict_walk(SESSION *session)
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
			WT_ERR(__wt_evict_walk_single(session, btree, i));
			i += WT_EVICT_WALK_PER_TABLE;
		}
	}
err:	__wt_unlock(session, conn->mtx);
	return (ret);
}

/*
 * __wt_evict_walk_single --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__wt_evict_walk_single(SESSION *session, BTREE *btree, u_int slot)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	int i, restarted_once;

	cache = S2C(session)->cache;

	/*
	 * Tricky little loop that restarts the walk as necessary, without
	 * resetting the count of pages retrieved.
	 */
	i = restarted_once = 0;

	/* If we haven't yet opened a tree-walk structure, do so. */
	if (btree->evict_walk.tree == NULL)
restart:	WT_RET(__wt_walk_begin(session,
		    &btree->root_page, &btree->evict_walk));

	/* Get the next WT_EVICT_WALK_PER_TABLE entries. */
	do {
		evict = &cache->evict[slot];
		WT_RET(__wt_walk_next(session,
		    &btree->evict_walk, WT_WALK_CACHE, &evict->ref));

		/*
		 * Restart the walk as necessary,  but only once (after one
		 * restart we've already acquired all of the pages, and we
		 * could loop infinitely on a tree with a single, pinned, page).
		 */
		if (evict->ref == NULL) {
			if (restarted_once++)
				break;
			goto restart;
		}

		evict->btree = btree;
		++slot;
	} while (++i < WT_EVICT_WALK_PER_TABLE);

	return (0);
}

/*
 * __wt_evict_db_clear --
 *	Remove any entries for a file from the eviction list.
 */
void
__wt_evict_db_clear(SESSION *session)
{
	BTREE *btree;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	u_int i;

	btree = session->btree;
	cache = S2C(session)->cache;

	/*
	 * Discard any entries in the eviction list to a file we're closing
	 * (the caller better have locked out the eviction thread).
	 */
	if (cache->evict == NULL)
		return;
	WT_EVICT_FOREACH(cache, evict, i)
		if (evict->ref != NULL && evict->btree == btree)
			WT_EVICT_CLR(evict);
}

/*
 * __wt_evict_set --
 *	Set the WT_REF_EVICT flag on a set of pages.
 */
static void
__wt_evict_set(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_REF *ref;
	u_int i;

	cache = S2C(session)->cache;

	/*
	 * Set the entry state so readers don't try and use the pages.   Once
	 * that's done, any thread searching for a page will either see our
	 * state value, or will have already set a hazard reference to the page.
	 * We don't evict a page with a hazard reference set, so we can't race.
	 *
	 * No memory flush needed, the state field is declared volatile.
	 */
	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		ref->state = WT_REF_EVICT;
	}
}

/*
 * __wt_evict_hazard_check --
 *	Compare the list of hazard references to the list of pages to be
 *	discarded.
 */
static void
__wt_evict_hazard_check(SESSION *session)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE **hazard, **end_hazard, *page;
	WT_REF *ref;
	WT_STATS *stats;
	u_int i;

	conn = S2C(session);
	cache = conn->cache;
	stats = cache->stats;

	/* Sort the eviction candidates by WT_PAGE address. */
	qsort(cache->evict, (size_t)WT_EVICT_GROUP,
	    sizeof(WT_EVICT_LIST), __wt_evict_compare_page);

	/* Copy the hazard reference array and sort it by WT_PAGE address. */
	hazard = cache->hazard;
	end_hazard = hazard + cache->hazard_elem;
	memcpy(hazard, conn->hazard, cache->hazard_elem * sizeof(WT_PAGE *));
	qsort(hazard, (size_t)cache->hazard_elem,
	    sizeof(WT_PAGE *), __wt_evict_hazard_compare);

	/* Walk the lists in parallel and look for matches. */
	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;

		/*
		 * Look for the page in the hazard list until we reach the end
		 * of the list or find a hazard pointer larger than the page.
		 */
		for (page = ref->page;
		    hazard < end_hazard && *hazard < page; ++hazard)
			;
		if (hazard == end_hazard)
			break;

		/*
		 * If we find a matching hazard reference, the page is in use:
		 * remove it from the eviction list.
		 *
		 * No memory flush needed, the state field is declared volatile.
		 */
		if (*hazard == page) {
			WT_VERBOSE(conn, WT_VERB_EVICT, (session,
			    "eviction skipped page addr %lu (hazard reference)",
			    page->addr));
			WT_STAT_INCR(stats, CACHE_EVICT_HAZARD);

			/*
			 * A page with a low LRU and a hazard reference?
			 *
			 * Set the page's LRU so we don't select it again.
			 * Return the page to service.
			 * Discard our reference.
			 */
			page->read_gen = ++cache->read_gen;
			ref->state = WT_REF_CACHE;
			WT_EVICT_CLR(evict);
		}
	}
}

/*
 * __wt_evict_state_check --
 *	Confirm these are pages we want to evict.
 */
static void
__wt_evict_state_check(SESSION *session)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE *page;
	WT_REF *ref;
	u_int i;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * We "own" the pages (we've flagged them for eviction, and there were
	 * no hazard references).   Now do checks to see if these are pages we
	 * can evict -- we have to wait until after we own the page because the
	 * page might be updated and race with us.
	 */
	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		page = ref->page;

		/* Ignore pinned pages. */
		if (WT_PAGE_IS_PINNED(page)) {
			WT_VERBOSE(conn, WT_VERB_EVICT, (session,
			    "eviction skipped page addr %lu (pinned)",
			    page->addr));
			goto skip;
		}

		/* Ignore pages with in-memory subtrees. */
		switch (page->dsk->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			if (__wt_evict_subtrees(page)) {
				WT_VERBOSE(conn, WT_VERB_EVICT, (session,
				    "eviction skipped page addr %lu (subtrees)",
				    page->addr));
				goto skip;
			}
			break;
		}

		continue;

skip:		/*
		 * Set the page's LRU so we don't select it again.
		 * Return the page to service.
		 * Discard our reference.
		 */
		page->read_gen = ++cache->read_gen;
		ref->state = WT_REF_CACHE;
		WT_EVICT_CLR(evict);
	}
}

/*
 * __wt_evict_clean --
 *	Discard clean cache pages.
 */
static void
__wt_evict_clean(SESSION *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE *page;
	WT_REF *ref;
	WT_STATS *stats;
	u_int i;

	cache = S2C(session)->cache;
	stats = cache->stats;

	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		page = ref->page;
		if (WT_PAGE_IS_MODIFIED(page))
			continue;

#ifdef HAVE_DIAGNOSTIC
		__wt_evict_hazard_validate(S2C(session), page);
#endif
		WT_STAT_INCR(stats, CACHE_EVICT_UNMODIFIED);
		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "cache evicting clean page addr %lu", page->addr));

		/*
		 * Clear the cache entry -- no memory flush needed, the state
		 * field is declared volatile.
		 */
		ref->page = NULL;
		ref->state = WT_REF_DISK;

		/* Remove the entry from the eviction list. */
		WT_EVICT_CLR(evict);

		/* We've got more space. */
		WT_CACHE_PAGE_OUT(cache, page->size);

		/* The page can no longer be found, free the memory. */
		__wt_page_discard(session, page);
	}
}

/*
 * __wt_evict_dirty --
 *	Discard dirty cache pages.
 */
static int
__wt_evict_dirty(SESSION *session)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_PAGE *page;
	WT_REF *ref;
	WT_STATS *stats;
	u_int i;
	int ret, update_gen;

	conn = S2C(session);
	cache = conn->cache;
	stats = cache->stats;
	update_gen = 0;

	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		page = ref->page;

		/*
		 * We are going to write a page, which means any running
		 * verification may be incorrect -- see the comment in
		 * the WT_CACHE structure declaration for a explanation.
		 */
		if (!update_gen) {
			update_gen = 1;
			++cache->evict_rec_gen;
		}

#ifdef HAVE_DIAGNOSTIC
		__wt_evict_hazard_validate(conn, page);
#endif
		WT_STAT_INCR(stats, CACHE_EVICT_MODIFIED);
		WT_VERBOSE(conn, WT_VERB_EVICT, (session,
		    "cache evicting dirty page addr %lu", page->addr));

		/*
		 * We're using our session handle, it needs to reference the
		 * correct btree handle.
		 *
		 * XXX
		 * This is pretty sleazy, but I'm hesitant to try and drive
		 * a separate btree handle down through the reconciliation
		 * code.
		 */
		session->btree = evict->btree;
		WT_ERR(__wt_page_reconcile(session, page));

		/*
		 * One special case -- of the page was deleted, the state has
		 * been reset to WT_REF_DELETED.  If still in "evict" mode,
		 * clear the cache entry -- no memory flush needed, the state
		 * field is declared volatile.
		 */
		ref->page = NULL;
		if (ref->state == WT_REF_EVICT)
			ref->state = WT_REF_DISK;

		/* Remove the entry from the eviction list. */
		WT_EVICT_CLR(evict);

		/* We've got more space. */
		WT_CACHE_PAGE_OUT(cache, page->size);

		/* The page can no longer be found, free the memory. */
		__wt_page_discard(session, page);
	}
	return (0);

err:	/*
	 * Writes to the file are likely failing -- quit trying to reconcile
	 * pages, and clear any remaining eviction flags.
	 */
	WT_EVICT_FOREACH(cache, evict, i) {
		if ((ref = evict->ref) == NULL)
			continue;
		if (ref->state == WT_REF_EVICT)
			ref->state = WT_REF_DISK;
	}
	return (ret);
}

/*
 * __wt_evict_subtrees --
 *	Return if a page has an in-memory subtree.
 */
static int
__wt_evict_subtrees(WT_PAGE *page)
{
	WT_ROW_REF *rref;
	WT_COL_REF *cref;
	uint32_t i;

	/*
	 * Return if a page has an in-memory subtree -- this array search could
	 * be replaced by a reference count in the page, but (1) the eviction
	 * thread isn't where I expect performance problems, (2) I hate to lose
	 * more bytes on every page, (3) how often will an internal page be
	 * evicted anyway?
	 *
	 * The state check can't be for (state == WT_REF_CACHE) because we may
	 * be evicting pages from more than a single level of the tree, and in
	 * that case, the parent's page will have a WT_REF with state equal to
	 * WT_REF_EVICT, and we still can't touch the parent until the child
	 * is flushed.
	 */
	switch (page->dsk->type) {
	case WT_PAGE_COL_INT:
		WT_COL_REF_FOREACH(page, cref, i)
			if (WT_COL_REF_STATE(cref) != WT_REF_DISK &&
			    WT_COL_REF_STATE(cref) != WT_REF_DELETED)
				return (1);
		break;
	case WT_PAGE_ROW_INT:
		WT_ROW_REF_FOREACH(page, rref, i)
			if (WT_ROW_REF_STATE(rref) != WT_REF_DISK &&
			    WT_ROW_REF_STATE(rref) != WT_REF_DELETED)
				return (1);
		break;
	}

	return (0);
}

/*
 * __wt_evict_compare_page --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's address.
 */
static int
__wt_evict_compare_page(const void *a, const void *b)
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
 * __wt_evict_compare_lru --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's read
 *	generation.
 */
static int
__wt_evict_compare_lru(const void *a, const void *b)
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

/*
 * __wt_evict_hazard_compare --
 *	Qsort function: sort hazard list based on the page's address.
 */
static int
__wt_evict_hazard_compare(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	a_page = *(WT_PAGE **)a;
	b_page = *(WT_PAGE **)b;

	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_evict_hazard_validate --
 *	Return if a page is or isn't on the hazard list.
 */
static void
__wt_evict_hazard_validate(CONNECTION *conn, WT_PAGE *page)
{
	WT_PAGE **hp;
	SESSION **tp, *session;

	for (tp = conn->sessions; (session = *tp) != NULL; ++tp)
		for (hp = session->hazard;
		    hp < session->hazard + S2C(session)->hazard_size; ++hp)
			if (*hp == page) {
				__wt_err(session, 0,
				    "hazard eviction check for page %lu "
				    "failed",
				    (u_long)page->addr);
				__wt_abort(session);
			}
}

/*
 * __wt_evict_dump --
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

/*
 * __wt_evict_dump_cache
 *	Dump the in-memory cache.
 */
int
__wt_evict_cache_dump(SESSION *session)
{
	BTREE *btree;
	CONNECTION *conn;

	conn = S2C(session);

	TAILQ_FOREACH(btree, &conn->dbqh, q)
		WT_RET(__wt_evict_tree_dump(session, btree));
	return (0);
}

/*
 * __wt_evict_tree_dump
 *	Dump an in-memory tree.
 */
int
__wt_evict_tree_dump(SESSION *session, BTREE *btree)
{
	CONNECTION *conn;
	WT_CACHE *cache;
	WT_REF *ref;
	WT_WALK walk;
	WT_MBUF mb;
	int sep;

	conn = S2C(session);
	cache = conn->cache;

	WT_VERBOSE(conn, WT_VERB_EVICT, (session,
	    "%s: pages inuse %llu, bytes inuse (%llu), max (%llu)",
	    btree->name,
	    __wt_cache_pages_inuse(cache),
	    __wt_cache_bytes_inuse(cache),
	    WT_STAT(cache->stats, CACHE_BYTES_MAX)));

	__wt_mb_init(session, &mb);
	__wt_mb_add(&mb, "in-memory page list");

	WT_CLEAR(walk);
	WT_RET(__wt_walk_begin(session, &btree->root_page, &walk));
	for (sep = ':';;) {
		WT_RET(__wt_walk_next(session, &walk, WT_WALK_CACHE, &ref));
		if (ref == NULL)
			break;
		__wt_mb_add(&mb, "%c %lu", sep, (u_long)ref->page->addr);
		sep = ',';
	}
	__wt_walk_end(session, &walk);
	__wt_mb_discard(&mb);

	return (0);
}

/*
 * __wt_evict_cache_count
 *	Return the count of nodes in the cache.
 */
int
__wt_evict_cache_count(SESSION *session, uint64_t *nodesp)
{
	BTREE *btree;
	CONNECTION *conn;
	uint64_t nodes;

	conn = S2C(session);

	*nodesp = 0;
	TAILQ_FOREACH(btree, &conn->dbqh, q) {
		WT_RET(__wt_evict_tree_count(session, btree, &nodes));
		*nodesp += nodes;
	}
	return (0);
}

/*
 * __wt_evict_tree_count
 *	Return a count of nodes in the tree.
 */
int
__wt_evict_tree_count(SESSION *session, BTREE *btree, uint64_t *nodesp)
{
	WT_REF *ref;
	WT_WALK walk;
	uint64_t nodes;

	WT_CLEAR(walk);
	WT_RET(__wt_walk_begin(session, &btree->root_page, &walk));
	for (nodes = 0;;) {
		WT_RET(__wt_walk_next(session, &walk, WT_WALK_CACHE, &ref));
		if (ref == NULL)
			break;
		++nodes;
	}
	*nodesp = nodes;
	__wt_walk_end(session, &walk);

	return (0);
}
#endif
