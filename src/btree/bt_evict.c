/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __evict_clear_walks(WT_SESSION_IMPL *);
static void __evict_init_candidate(
    WT_SESSION_IMPL *, WT_EVICT_ENTRY *, WT_PAGE *);
static int  __evict_lru(WT_SESSION_IMPL *, uint32_t);
static int  __evict_lru_cmp(const void *, const void *);
static int __evict_lru_pages(WT_SESSION_IMPL *, int);
static int  __evict_pass(WT_SESSION_IMPL *);
static int  __evict_walk(WT_SESSION_IMPL *, uint32_t *, uint32_t);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *, uint32_t);
static void *__evict_worker(void *);

typedef struct {
	WT_CONNECTION_IMPL *conn;
	int id;

	pthread_t tid;
} WT_EVICTION_WORKER;

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
 * __evict_list_clear --
 *	Clear an entry in the LRU eviction list.
 */
static inline void
__evict_list_clear(WT_SESSION_IMPL *session, WT_EVICT_ENTRY *e)
{
	if (e->page != NULL) {
		WT_ASSERT(session, F_ISSET_ATOMIC(e->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->page, WT_PAGE_EVICT_LRU);
	}
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __wt_evict_list_clear_page --
 *	Make sure a page is not in the LRU eviction list.  This called from the
 *	page eviction code to make sure there is no attempt to evict a child
 *	page multiple times.
 */
void
__wt_evict_list_clear_page(WT_SESSION_IMPL *session, WT_PAGE *page)
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

	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++)
		if (evict->page == page) {
			__evict_list_clear(session, evict);
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

	conn = S2C(session);
	cache = conn->cache;

	if (WT_VERBOSE_ISSET(session, evictserver)) {
		uint64_t bytes_inuse, bytes_max;

		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
		WT_RET(__wt_verbose(session,
		    "waking, bytes inuse %s max (%" PRIu64
		    "MB %s %" PRIu64 "MB)",
		    bytes_inuse <= bytes_max ? "<=" : ">",
		    bytes_inuse / WT_MEGABYTE,
		    bytes_inuse <= bytes_max ? "<=" : ">",
		    bytes_max / WT_MEGABYTE));
	}

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
	WT_EVICTION_WORKER *workers;
	WT_SESSION_IMPL *session;
	u_int i;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;
	workers = NULL;

	WT_ERR(__wt_calloc_def(session, cache->eviction_workers, &workers));
	for (i = 0; i < cache->eviction_workers; i++) {
		workers[i].conn = conn;
		workers[i].id = i;
		WT_ERR(__wt_thread_create(session,
		    &workers[i].tid, __evict_worker, &workers[i]));
	}

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)) {
		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_pass(session));

		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;

		F_CLR(cache, WT_EVICT_ACTIVE);
		WT_VERBOSE_ERR(session, evictserver, "sleeping");
		/* Don't rely on signals: check periodically. */
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, cache->evict_cond, 100000));
		WT_VERBOSE_ERR(session, evictserver, "waking");
	}

	WT_VERBOSE_ERR(session, evictserver, "exiting");

err:	WT_VERBOSE_TRET(session, evictserver, "waiting for helper threads");
	for (i = 0; i < cache->eviction_workers; i++) {
		__wt_cond_signal(session, cache->evict_waiter_cond);
		WT_TRET(__wt_thread_join(session, workers[i].tid));
	}
	__wt_free(session, workers);

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
		WT_PANIC_ERR(session, ret, "eviction server error");

	/* Close the eviction session. */
	(void)session->iface.close(&session->iface, NULL);

	return (NULL);
}

/*
 * __evict_worker --
 *	Thread to help evict pages from the cache.
 */
static void *
__evict_worker(void *arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICTION_WORKER *worker;
	WT_SESSION_IMPL *session;

	worker = arg;
	conn = worker->conn;
	cache = conn->cache;
	ret = 0;

	/*
	 * We need a session handle because we're reading/writing pages.
	 * Start with the default session to keep error handling simple.
	 */
	session = conn->default_session;
	WT_ERR(__wt_open_session(conn, 1, NULL, NULL, &session));

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)) {
		WT_VERBOSE_ERR(session, evictserver, "worker sleeping");
		__wt_cond_wait(session, cache->evict_waiter_cond, 100000);
		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;
		WT_VERBOSE_ERR(session, evictserver, "worker waking");

		WT_ERR(__evict_lru_pages(session, 1));
	}

	if (0) {
err:		__wt_err(session, ret, "cache eviction helper error");
	}

	WT_VERBOSE_TRET(session, evictserver, "helper exiting");

	if (session != conn->default_session)
		(void)session->iface.close(&session->iface, NULL);

	return (NULL);
}

/*
 * __evict_pass --
 *	Evict pages from memory.
 */
static int
__evict_pass(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	int loop;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;
	uint32_t flags;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/*
		 * If there is a request to clear eviction walks, do that now,
		 * before checking if the cache is full.
		 */
		if (F_ISSET(cache, WT_EVICT_CLEAR_WALKS)) {
			F_CLR(cache, WT_EVICT_CLEAR_WALKS);
			WT_RET(__evict_clear_walks(session));
			WT_RET(__wt_cond_signal(
			    session, cache->evict_waiter_cond));
		}

		/*
		 * Keep evicting until we hit the target cache usage and the
		 * target dirty percentage.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		dirty_inuse = cache->bytes_dirty;
		bytes_max = conn->cache_size;

		/* Check to see if the eviction server should run. */
		if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
			flags = (F_ISSET(cache, WT_EVICT_STUCK) || loop > 10) ?
			    WT_EVICT_PASS_AGGRESSIVE : WT_EVICT_PASS_ALL;
		else if (dirty_inuse >
		    (cache->eviction_dirty_target * bytes_max) / 100)
			/* Ignore clean pages unless the cache is too large */
			flags = WT_EVICT_PASS_DIRTY;
		else if (F_ISSET(cache, WT_EVICT_INTERNAL))
			/* Only consider merging internal pages. */
			flags = WT_EVICT_PASS_INTERNAL;
		else
			break;
		F_CLR(cache, WT_EVICT_INTERNAL);

		F_SET(cache, WT_EVICT_ACTIVE);
		WT_VERBOSE_RET(session, evictserver,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64 " Merge: %s",
		    bytes_max, bytes_inuse, dirty_inuse,
		    LF_ISSET(WT_EVICT_PASS_INTERNAL) ? "yes" : "no");

		/*
		 * When the cache is full, track whether pages are being
		 * evicted.  This will be cleared by the next thread to
		 * successfully evict a page.
		 */
		if (bytes_inuse > bytes_max)
			F_SET(cache, WT_EVICT_NO_PROGRESS);
		else
			F_CLR(cache, WT_EVICT_NO_PROGRESS);

		WT_RET(__evict_lru(session, flags));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, mark the cache "stuck" and go back to
		 * sleep, it's not something we can fix.
		 */
		if (F_ISSET(cache, WT_EVICT_NO_PROGRESS)) {
			if (F_ISSET(cache, WT_EVICT_STUCK))
				break;
			if (loop == 100) {
				F_SET(cache, WT_EVICT_STUCK);
				WT_STAT_FAST_CONN_INCR(
				    session, cache_eviction_slow);
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
 * __evict_clear_walks --
 *	Clear the eviction walk points for all files.
 */
static int
__evict_clear_walks(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_PAGE *page;

	conn = S2C(session);
	cache = conn->cache;
	cache->evict_file_next = NULL;

	/*
	 * Lock the dhandle list so sweeping cannot change the pointers out
	 * from under us.
	 *
	 * NOTE: we don't hold the schema lock, so we have to take care
	 * that the handles we see are open and valid.
	 */
	__wt_spin_lock(session, &conn->dhandle_lock);

	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		/* Ignore non-file handles, or handles that aren't open. */
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		btree = dhandle->handle;
		session->dhandle = dhandle;
		if ((page = btree->evict_page) != NULL) {
			/*
			 * Clear evict_page first, in case releasing it forces
			 * eviction (we check that we never try to evict the
			 * current eviction walk point.
			 */
			btree->evict_page = NULL;
			WT_TRET(__wt_page_release(session, page));
		}
		session->dhandle = NULL;
	}

	__wt_spin_unlock(session, &conn->dhandle_lock);

	return (ret);
}

/*
 * __evict_tree_walk_clear --
 *	Clear the tree's current eviction point, acquiring the eviction lock.
 */
static int
__evict_tree_walk_clear(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	while (btree->evict_page != NULL) {
		F_SET(cache, WT_EVICT_CLEAR_WALKS);
		WT_RET(__wt_cond_wait(
		    session, cache->evict_waiter_cond, 100000));
	}

	return (ret);
}

/*
 * __wt_evict_page --
 *	Evict a given page.
 */
int
__wt_evict_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_ISOLATION saved_iso;

	/*
	 * We have to take care when evicting pages not to write a change that:
	 *  (a) is not yet committed; or
	 *  (b) is committed more recently than an in-progress checkpoint.
	 *
	 * We handle both of these cases by setting up the transaction context
	 * before evicting, using a special "eviction" isolation level, where
	 * only globally visible updates can be evicted.
	 */
	__wt_txn_update_oldest(session);
	txn = &session->txn;
	saved_iso = txn->isolation;
	txn->isolation = TXN_ISO_EVICTION;

	/*
	 * Sanity check: if a transaction is running, its updates should not
	 * be visible to eviction.
	 */
	WT_ASSERT(session, !F_ISSET(txn, TXN_RUNNING) ||
	    !__wt_txn_visible(session, txn->id));

	ret = __wt_rec_evict(session, page, 0);
	txn->isolation = saved_iso;
	return (ret);
}

/*
 * __wt_evict_file_exclusive_on --
 *	Get exclusive eviction access to a file and discard any of the file's
 *	blocks queued for eviction.
 */
int
__wt_evict_file_exclusive_on(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	u_int i, elem;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*
	 * Hold the walk lock to set the "no eviction" flag: no new pages from
	 * the file will be queued for eviction after this point.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	F_SET(btree, WT_BTREE_NO_EVICTION);
	__wt_spin_unlock(session, &cache->evict_walk_lock);

	/* Clear any existing LRU eviction walk for the file. */
	WT_RET(__evict_tree_walk_clear(session));

	/* Hold the evict lock to remove any queued pages from this file. */
	__wt_spin_lock(session, &cache->evict_lock);

	/*
	 * The eviction candidate list might reference pages from the file,
	 * clear it.
	 */
	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++)
		if (evict->btree == btree)
			__evict_list_clear(session, evict);
	__wt_spin_unlock(session, &cache->evict_lock);

	/*
	 * We have disabled further eviction: wait for concurrent LRU eviction
	 * activity to drain.
	 */
	while (btree->lru_count > 0)
		__wt_yield();

	return (0);
}

/*
 * __wt_evict_file_exclusive_off --
 *	Release exclusive eviction access to a file.
 */
void
__wt_evict_file_exclusive_off(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	WT_ASSERT(session, btree->evict_page == NULL);

	F_CLR(btree, WT_BTREE_NO_EVICTION);
}

/*
 * __wt_evict_file --
 *	Discard pages for a specific file.
 */
int
__wt_evict_file(WT_SESSION_IMPL *session, int syncop)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *next_page, *page;

	btree = S2BT(session);

	/*
	 * We need exclusive access to the file -- disable ordinary eviction
	 * and drain any blocks already queued.
	 */
	WT_RET(__wt_evict_file_exclusive_on(session));

	/* Make sure the oldest transaction ID is up-to-date. */
	__wt_txn_update_oldest(session);

	/* Walk the tree, discarding pages. */
	next_page = NULL;
	WT_RET(__wt_tree_walk(
	    session, &next_page, WT_READ_CACHE | WT_READ_NO_GEN));
	while ((page = next_page) != NULL) {
		/*
		 * Eviction can fail when a page in the evicted page's subtree
		 * switches state.  For example, if we don't evict a page marked
		 * empty, because we expect it to be merged into its parent, it
		 * might no longer be empty after it's reconciled, in which case
		 * eviction of its parent would fail.  We can either walk the
		 * tree multiple times (until it's finally empty), or reconcile
		 * each page to get it to its final state before considering if
		 * it's an eviction target or will be merged into its parent.
		 *
		 * Don't limit this test to any particular page type, that tends
		 * to introduce bugs when the reconciliation of other page types
		 * changes, and there's no advantage to doing so.
		 */
		if (syncop == WT_SYNC_DISCARD && __wt_page_is_modified(page))
			WT_ERR(__wt_rec_write(
			    session, page, NULL, WT_SKIP_UPDATE_ERR));

		/*
		 * We can't evict the page just returned to us (it marks our
		 * place in the tree), so move the walk to one page ahead of
		 * the page being evicted.  Note, we reconcile the returned
		 * page first: if reconciliation of that page were to change
		 * the shape of the tree, and we did the next walk call before
		 * the reconciliation, the next walk call could miss a page in
		 * the tree.
		 */
		WT_ERR(__wt_tree_walk(
		    session, &next_page, WT_READ_CACHE | WT_READ_NO_GEN));

		switch (syncop) {
		case WT_SYNC_DISCARD:
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
				__wt_cache_dirty_decr(session, page);
			__wt_page_out(session, &page);
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	if (0) {
err:		/* On error, clear any left-over tree walk. */
		if (next_page != NULL)
			WT_TRET(__wt_page_release(session, next_page));
	}

	__wt_evict_file_exclusive_off(session);

	return (ret);
}

/*
 * __wt_sync_file --
 *	Flush pages for a specific file.
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
		 * hazard pointers to prevent eviction.
		 */
		flags = WT_READ_CACHE | WT_READ_SKIP_INTL;
		if (syncop == WT_SYNC_WRITE_LEAVES)
			flags |= WT_READ_NO_WAIT;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			/* Write dirty pages if nobody beat us to it. */
			if (__wt_page_is_modified(page)) {
				if (txn->isolation == TXN_ISO_READ_COMMITTED)
					__wt_txn_refresh(
					    session, WT_TXN_NONE, 1);
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
		 * it will have set the ref state to WT_REF_LOCKED, and the
		 * checkpoint will notice and wait for eviction to complete
		 * before proceeding.
		 */
		__wt_spin_lock(session, &cache->evict_lock);
		btree->checkpointing = 1;
		__wt_spin_unlock(session, &cache->evict_lock);

		/*
		 * The second pass walks all cache internal pages, waiting for
		 * concurrent activity to be resolved.
		 */
		flags = WT_READ_CACHE | WT_READ_NO_GEN | WT_READ_SKIP_LEAF;
		WT_ERR(__wt_tree_walk(session, &page, flags));
		while (page != NULL) {
			/* Write dirty pages. */
			if (__wt_page_is_modified(page))
				WT_ERR(__wt_rec_write(session, page, NULL, 0));
			WT_ERR(__wt_tree_walk(session, &page, flags));
		}
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

err:	/* On error, clear any left-over tree walk. */
	if (page != NULL)
		WT_TRET(__wt_page_release(session, page));

	if (btree->checkpointing) {
		/*
		 * Clear the checkpoint flag and push the change; not required,
		 * but publishing the change means stalled eviction gets moving
		 * as soon as possible.
		 */
		btree->checkpointing = 0;
		WT_FULL_BARRIER();

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
 * __evict_lru_pages --
 *	Get pages from the LRU queue to evict.
 */
static int
__evict_lru_pages(WT_SESSION_IMPL *session, int is_app)
{
	WT_DECL_RET;

	/*
	 * Reconcile and discard some pages: EBUSY is returned if a page fails
	 * eviction because it's unavailable, continue in that case.
	 */
	while ((ret = __wt_evict_lru_page(session, is_app)) == 0 ||
	    ret == EBUSY)
		;
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint64_t cutoff;
	uint32_t candidates, entries, i;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	WT_RET(__evict_walk(session, &entries, flags));

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);

	qsort(cache->evict,
	    entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	while (entries > 0 && cache->evict[entries - 1].page == NULL)
		--entries;

	cache->evict_entries = entries;

	if (entries == 0) {
		/*
		 * If there are no entries, there cannot be any candidates.
		 * Make sure application threads don't read past the end of the
		 * candidate list, or they may race with the next walk.
		 */
		cache->evict_candidates = 0;
		cache->evict_current = NULL;
		__wt_spin_unlock(session, &cache->evict_lock);
		return (0);
	}

	WT_ASSERT(session, cache->evict[0].page != NULL);

	/* Find the bottom 25% of read generations. */
	cutoff = (3 * __evict_read_gen(&cache->evict[0]) +
	    __evict_read_gen(&cache->evict[entries - 1])) / 4;

	/*
	 * Don't take less than 10% or more than 50% of entries, regardless.
	 * That said, if there is only one entry, which is normal when
	 * populating an empty file, don't exclude it.
	 */
	for (candidates = 1 + entries / 10;
	    candidates < entries / 2;
	    candidates++)
		if (__evict_read_gen(&cache->evict[candidates]) > cutoff)
			break;
	cache->evict_candidates = candidates;

	/* If we have more than the minimum number of entries, clear them. */
	if (cache->evict_entries > WT_EVICT_WALK_BASE) {
		for (i = WT_EVICT_WALK_BASE, evict = cache->evict + i;
		    i < cache->evict_entries;
		    i++, evict++)
			__evict_list_clear(session, evict);
		cache->evict_entries = WT_EVICT_WALK_BASE;
	}

	cache->evict_current = cache->evict;
	__wt_spin_unlock(session, &cache->evict_lock);

	/*
	 * Signal any application or worker threads waiting for the eviction
	 * queue to have candidates.
	 */
	WT_RET(__wt_cond_signal(session, cache->evict_waiter_cond));

	return (__evict_lru_pages(session, 0));
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
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, old_slot, retries, slot;

	conn = S2C(session);
	cache = S2C(session)->cache;
	retries = 0;

	/* Increment the shared read generation. */
	__wt_cache_read_gen_incr(session);

	/*
	 * Update the oldest ID: we use it to decide whether pages are
	 * candidates for eviction.  Without this, if all threads are blocked
	 * after a long-running transaction (such as a checkpoint) completes,
	 * we may never start evicting again.
	 */
	__wt_txn_update_oldest(session);

	/*
	 * Set the starting slot in the queue and the maximum pages added
	 * per walk.
	 */
	slot = cache->evict_entries;
	max_entries = slot + WT_EVICT_WALK_INCR;

	/*
	 * Lock the dhandle list so sweeping cannot change the pointers out
	 * from under us.
	 *
	 * NOTE: we don't hold the schema lock, so we have to take care
	 * that the handles we see are open and valid.
	 */
	__wt_spin_lock(session, &conn->dhandle_lock);

retry:	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		/* Ignore non-file handles, or handles that aren't open. */
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		/*
		 * Each time we reenter this function, start at the next handle
		 * on the list.
		 */
		if (cache->evict_file_next != NULL &&
		    cache->evict_file_next != dhandle)
			continue;
		cache->evict_file_next = NULL;

		/*
		 * Skip files without a root page, marked as cache-resident and
		 * files potentially involved in a bulk-load.  The problem is
		 * eviction doesn't want to be walking the file as it converts
		 * to a bulk-loaded object, and empty trees aren't worth trying
		 * to evict, anyway.
		 */
		btree = dhandle->handle;
		if (btree->root_page == NULL ||
		    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
		    btree->bulk_load_ok)
			continue;

		/*
		 * Also skip files that are configured to stick in cache until
		 * we get aggressive.
		 */
		if (btree->evict_priority != 0 &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			continue;

		/*
		 * If we are filling the queue, skip files that haven't been
		 * useful in the past.
		 */
		if (btree->evict_walk_period != 0 &&
		    cache->evict_entries >= WT_EVICT_WALK_INCR &&
		    btree->evict_walk_skips++ < btree->evict_walk_period)
			continue;
		btree->evict_walk_skips = 0;
		old_slot = slot;

		__wt_spin_lock(session, &cache->evict_walk_lock);

		/*
		 * Re-check the "no eviction" flag -- it is used to enforce
		 * exclusive access when a handle is being closed.
		 */
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION))
			WT_WITH_BTREE(session, btree,
			    ret = __evict_walk_file(session, &slot, flags));

		__wt_spin_unlock(session, &cache->evict_walk_lock);

		/*
		 * If we didn't find enough candidates in the file, skip it
		 * next time.
		 */
		if (slot >= old_slot + WT_EVICT_WALK_PER_FILE ||
		    slot >= max_entries)
			btree->evict_walk_period = 0;
		else
			btree->evict_walk_period = WT_MIN(
			    WT_MAX(1, 2 * btree->evict_walk_period), 1000);

		if (ret != 0 || slot >= max_entries)
			break;
	}

	/* Walk the list of files a few times if we don't find enough pages. */
	if (ret == 0 && slot < max_entries && ++retries < 10)
		goto retry;

	/* Remember the file we should visit first, next loop. */
	if (dhandle != NULL)
		dhandle = SLIST_NEXT(dhandle, l);
	cache->evict_file_next = dhandle;

	__wt_spin_unlock(session, &conn->dhandle_lock);

	*entriesp = slot;
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
		__evict_list_clear(session, evict);
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
__evict_walk_file(WT_SESSION_IMPL *session, u_int *slotp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	int internal_pages, levels, modified, restarts;
	uint32_t walk_flags;
	uint64_t pages_walked;

	btree = S2BT(session);
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = WT_MIN(start + WT_EVICT_WALK_PER_FILE,
	    cache->evict + cache->evict_slots);

	walk_flags = WT_READ_CACHE | WT_READ_NO_GEN | WT_READ_NO_WAIT;
	if (LF_ISSET(WT_EVICT_PASS_INTERNAL))
		walk_flags |= WT_READ_SKIP_LEAF;

	/*
	 * Get some more eviction candidate pages.
	 */
	for (evict = start, pages_walked = 0, internal_pages = restarts = 0;
	    evict < end && (ret == 0 || ret == WT_NOTFOUND);
	    ret = __wt_tree_walk(session, &btree->evict_page, walk_flags),
	    ++pages_walked) {
		if ((page = btree->evict_page) == NULL) {
			ret = 0;
			/*
			 * Take care with terminating this loop.
			 *
			 * Don't make an extra call to __wt_tree_walk: that
			 * will leave a page pinned, which may prevent any work
			 * from being done.
			 */
			if (++restarts == 2)
				break;
			continue;
		}

		/* Ignore root pages entirely. */
		if (WT_PAGE_IS_ROOT(page))
			continue;

		/*
		 * Look for a split-merge (grand)parent page to merge.
		 *
		 * Only look for a parent at exactly the right height above: if
		 * the stack is deep enough, we'll find it eventually, and we
		 * don't want to do too much work on every level.
		 */
		levels = 0;
		if (__wt_btree_mergeable(page))
			for (levels = 1;
			    levels < WT_MERGE_STACK_MIN &&
			    __wt_btree_mergeable(page->parent) &&
			    page->parent->ref->state == WT_REF_MEM;
			    page = page->parent, levels++)
				;
		else if (page->modify != NULL &&
		    F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
			continue;

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		/*
		 * !!!
		 * In normal operation, don't restrict ourselves to only the
		 * top-most page (that is, don't require that page->parent is
		 * not mergeable).  If there is a big, busy enough split-merge
		 * tree, the top-level merge will only happen if we can lock
		 * the whole subtree exclusively.  Consider smaller merges in
		 * case locking the whole tree fails.
		 */
		if (levels != 0) {
			if (levels < WT_MERGE_STACK_MIN)
				continue;

			/*
			 * Concentrate near the top of a stack -- with forced
			 * eviction, stacks of split-merge pages can get very
			 * deep, and merging near the bottom isn't helpful.
			 */
			if (LF_ISSET(WT_EVICT_PASS_INTERNAL) &&
			    __wt_btree_mergeable(page->parent) &&
			    __wt_btree_mergeable(page->parent->parent))
				continue;

			/* The remaining checks don't apply to merges. */
			goto add;
		} else if (LF_ISSET(WT_EVICT_PASS_INTERNAL))
			continue;

		/* Limit internal pages to 50% unless we get aggressive. */
		if ((page->type == WT_PAGE_COL_INT ||
		    page->type == WT_PAGE_ROW_INT) &&
		    ++internal_pages > WT_EVICT_WALK_PER_FILE / 2 &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			break;

		/*
		 * If this page has never been considered for eviction,
		 * set its read generation to a little bit in the
		 * future and move on, give readers a chance to start
		 * updating the read generation.
		 */
		if (page->read_gen == WT_READGEN_NOTSET) {
			page->read_gen =
			    __wt_cache_read_gen_set(session);
			continue;
		}

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
		if (!modified && LF_ISSET(WT_EVICT_PASS_DIRTY))
			continue;

		/*
		 * If the page is clean but has modifications that appear too
		 * new to evict, skip it.
		 */
		if (!modified && page->modify != NULL &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE) &&
		    !__wt_txn_visible_all(session, page->modify->rec_max_txn))
			continue;

		/*
		 * If the oldest transaction hasn't changed since the
		 * last time this page was written, it's unlikely that
		 * we can make progress.  Similarly, if the most recent
		 * update on the page is not yet globally visible,
		 * eviction will fail.  These heuristics attempt to
		 * avoid repeated attempts to evict the same page.
		 *
		 * That said, if eviction is stuck, or the file is
		 * being checkpointed, try anyway: maybe a transaction
		 * that were running last time we wrote the page has
		 * since rolled back, or we can help get the checkpoint
		 * completed sooner.
		 */
		if (modified && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE) &&
		    (page->modify->disk_snap_min ==
		    S2C(session)->txn_global.oldest_id ||
		    !__wt_txn_visible_all(session,
		    page->modify->update_txn)))
			continue;

add:		WT_ASSERT(session, evict->page == NULL);
		__evict_init_candidate(session, evict, page);
		++evict;

		WT_VERBOSE_RET(session, evictserver,
		    "select: %p, size %" PRIu64, page, page->memory_footprint);
	}

	*slotp += (u_int)(evict - start);
	WT_STAT_FAST_CONN_INCRV(session, cache_eviction_walk, pages_walked);
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
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	cache = S2C(session)->cache;
	*btreep = NULL;
	*pagep = NULL;

	/*
	 * A pathological case: if we're the oldest transaction in the system
	 * and the eviction server is stuck trying to find space, abort the
	 * transaction to give up all hazard pointers before trying again.
	 */
	if (is_app && F_ISSET(cache, WT_EVICT_STUCK) &&
	    __wt_txn_am_oldest(session)) {
		F_CLR(cache, WT_EVICT_STUCK);
		WT_STAT_FAST_CONN_INCR(session, txn_fail_cache);
		return (WT_DEADLOCK);
	}

	/*
	 * Avoid the LRU lock if no pages are available.  If there are pages
	 * available, spin until we get the lock.  If this function returns
	 * without getting a page to evict, application threads assume there
	 * are no more pages available and will attempt to wake the eviction
	 * server.
	 */
	for (;;) {
		if (cache->evict_current == NULL)
			return (WT_NOTFOUND);
		if (__wt_spin_trylock(session, &cache->evict_lock, &id) == 0)
			break;
		__wt_yield();
	}

	/*
	 * The eviction server only tries to evict half of the pages before
	 * looking for more.
	 */
	candidates = cache->evict_candidates;
	if (!is_app && candidates > 1)
		candidates /= 2;

	/* Get the next page queued for eviction. */
	while ((evict = cache->evict_current) != NULL &&
	    evict < cache->evict + candidates &&
	    evict->page != NULL) {
		WT_ASSERT(session, evict->btree != NULL);

		/* Move to the next item. */
		++cache->evict_current;

		/*
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, this operation will fail and we will move on.
		 */
		ref = evict->page->ref;
		WT_ASSERT(session, evict->page == ref->page);

		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
			__evict_list_clear(session, evict);
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
		__evict_list_clear(session, evict);
		break;
	}

	/* Clear the current pointer if there are no more candidates. */
	if (evict >= cache->evict + cache->evict_candidates)
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

	/*
	 * In case something goes wrong, don't pick the same set of pages every
	 * time.
	 *
	 * We used to bump the page's read generation only if eviction failed,
	 * but that isn't safe: at that point, eviction has already unlocked
	 * the page and some other thread may have evicted it by the time we
	 * look at it.
	 */
	if (page->read_gen != WT_READGEN_OLDEST)
		page->read_gen = __wt_cache_read_gen_set(session);

	WT_WITH_BTREE(session, btree, ret = __wt_evict_page(session, page));

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
	uint64_t file_intl_pages, file_leaf_pages;
	uint64_t file_bytes, file_dirty, total_bytes;

	conn = S2C(session);
	total_bytes = 0;

	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		btree = dhandle->handle;
		if (btree->root_page == NULL ||
		    F_ISSET(btree, WT_BTREE_NO_EVICTION) ||
		    btree->bulk_load_ok)
			continue;

		file_bytes = file_dirty = file_intl_pages = file_leaf_pages = 0;
		page = NULL;
		session->dhandle = dhandle;
		while (__wt_tree_walk(
		    session, &page, WT_READ_CACHE | WT_READ_NO_WAIT) == 0 &&
		    page != NULL) {
			if (page->type == WT_PAGE_COL_INT ||
			    page->type == WT_PAGE_ROW_INT)
				++file_intl_pages;
			else
				++file_leaf_pages;
			file_bytes += page->memory_footprint;
			if (__wt_page_is_modified(page))
				file_dirty += page->memory_footprint;
		}
		session->dhandle = NULL;

		printf("cache dump: %s [%s]:"
		    " %" PRIu64 " intl pages, %" PRIu64 " leaf pages,"
		    " %" PRIu64 "MB, %" PRIu64 "MB dirty\n",
		    dhandle->name, dhandle->checkpoint,
		    file_intl_pages, file_leaf_pages,
		    file_bytes >> 20, file_dirty >> 20);

		total_bytes += file_bytes;
	}
	printf("cache dump: total found = %" PRIu64 "MB"
	    " vs tracked inuse %" PRIu64 "MB\n",
	    total_bytes >> 20, __wt_cache_bytes_inuse(conn->cache) >> 20);
	fflush(stdout);
}
#endif
