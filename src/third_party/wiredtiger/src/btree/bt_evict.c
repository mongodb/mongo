/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int   __evict_clear_walks(WT_SESSION_IMPL *);
static int   __evict_has_work(WT_SESSION_IMPL *, uint32_t *);
static int   __evict_lru_cmp(const void *, const void *);
static int   __evict_lru_pages(WT_SESSION_IMPL *, int);
static int   __evict_lru_walk(WT_SESSION_IMPL *, uint32_t);
static int   __evict_pass(WT_SESSION_IMPL *);
static int   __evict_walk(WT_SESSION_IMPL *, uint32_t *, uint32_t);
static int   __evict_walk_file(WT_SESSION_IMPL *, u_int *, uint32_t);
static void *__evict_worker(void *);
static int __evict_server_work(WT_SESSION_IMPL *);

/*
 * __evict_read_gen --
 *	Get the adjusted read generation for an eviction entry.
 */
static inline uint64_t
__evict_read_gen(const WT_EVICT_ENTRY *entry)
{
	WT_PAGE *page;
	uint64_t read_gen;

	/* Never prioritize empty slots. */
	if (entry->ref == NULL)
		return (UINT64_MAX);

	page = entry->ref->page;
	read_gen = page->read_gen + entry->btree->evict_priority;

	/*
	 * Skew the read generation for internal pages, we prefer to evict leaf
	 * pages.
	 */
	if (page->type == WT_PAGE_ROW_INT || page->type == WT_PAGE_COL_INT)
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
	if (e->ref != NULL) {
		WT_ASSERT(session,
		    F_ISSET_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU);
	}
	e->ref = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __wt_evict_list_clear_page --
 *	Make sure a page is not in the LRU eviction list.  This called from the
 *	page eviction code to make sure there is no attempt to evict a child
 *	page multiple times.
 */
void
__wt_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t i, elem;

	WT_ASSERT(session, 
	    __wt_ref_is_root(ref) || ref->state == WT_REF_LOCKED);

	/* Fast path: if the page isn't on the queue, don't bother searching. */
	if (!F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, &cache->evict_lock);

	elem = cache->evict_max;
	for (i = 0, evict = cache->evict; i < elem; i++, evict++)
		if (evict->ref == ref) {
			__evict_list_clear(session, evict);
			break;
		}

	WT_ASSERT(session, !F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU));

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

	if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTSERVER)) {
		uint64_t bytes_inuse, bytes_max;

		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
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
 * __evict_server --
 *	Thread to evict pages from the cache.
 */
static void *
__evict_server(void *arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *worker;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)) {
		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_pass(session));

		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;

		/*
		 * If we have caught up and there are more than the minimum
		 * number of eviction workers running, shut one down.
		 */
		if (conn->evict_workers > conn->evict_workers_min) {
			WT_TRET(__wt_verbose(session, WT_VERB_EVICTSERVER,
			    "Stopping evict worker: %"PRIu32"\n",
			    conn->evict_workers));
			worker = &conn->evict_workctx[--conn->evict_workers];
			F_CLR(worker, WT_EVICT_WORKER_RUN);
			WT_TRET(__wt_cond_signal(
			    session, cache->evict_waiter_cond));
			WT_TRET(__wt_thread_join(session, worker->tid));
			/*
			 * Flag errors here with a message, but don't shut down
			 * the eviction server - that's fatal.
			 */
			WT_ASSERT(session, ret == 0);
			if (ret != 0) {
				(void)__wt_msg(session,
				    "Error stopping eviction worker: %d", ret);
				ret = 0;
			}
		}
		F_CLR(cache, WT_EVICT_ACTIVE);
		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "sleeping"));
		/* Don't rely on signals: check periodically. */
		WT_ERR(__wt_cond_wait(session, cache->evict_cond, 100000));
		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "waking"));
	}

	WT_ERR(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "cache eviction server exiting"));

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

	if (0) {
err:		WT_PANIC_MSG(session, ret, "cache eviction server error");
	}
	return (NULL);
}

/*
 * __wt_evict_create --
 *	Start the eviction server thread.
 */
int
__wt_evict_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_EVICT_WORKER *workers;
	u_int i;

	conn = S2C(session);

	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_EVICTION_RUN);

	/* We need a session handle because we're reading/writing pages. */
	WT_RET(__wt_open_internal_session(
	    conn, "eviction-server", 0, 0, &conn->evict_session));
	session = conn->evict_session;

	/*
	 * If there's only a single eviction thread, it may be called upon to
	 * perform slow operations for the block manager.  (The flag is not
	 * reset if reconfigured later, but I doubt that's a problem.)
	 */
	if (conn->evict_workers_max == 0)
		F_SET(session, WT_SESSION_CAN_WAIT);

	if (conn->evict_workers_max > 0) {
		WT_RET(__wt_calloc_def(
		    session, conn->evict_workers_max, &workers));
		conn->evict_workctx = workers;

		for (i = 0; i < conn->evict_workers_max; i++) {
			WT_RET(__wt_open_internal_session(conn,
			    "eviction-worker", 0, 0, &workers[i].session));
			workers[i].id = i;
			F_SET(workers[i].session, WT_SESSION_CAN_WAIT);

			if (i < conn->evict_workers_min) {
				++conn->evict_workers;
				F_SET(&workers[i], WT_EVICT_WORKER_RUN);
				WT_RET(__wt_thread_create(
				    workers[i].session, &workers[i].tid,
				    __evict_worker, &workers[i]));
			}
		}
	}

	/*
	 * Start the primary eviction server thread after the worker threads
	 * have started to avoid it starting additional worker threads before
	 * the worker's sessions are created.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->evict_tid, __evict_server, session));
	conn->evict_tid_set = 1;

	return (0);
}

/*
 * __wt_evict_destroy --
 *	Destroy the eviction server thread.
 */
int
__wt_evict_destroy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *workers;
	WT_SESSION *wt_session;
	u_int i;

	conn = S2C(session);
	cache = conn->cache;
	workers = conn->evict_workctx;

	F_CLR(conn, WT_CONN_EVICTION_RUN);

	WT_TRET(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "waiting for helper threads"));
	for (i = 0; i < conn->evict_workers; i++) {
		WT_TRET(__wt_cond_signal(session, cache->evict_waiter_cond));
		WT_TRET(__wt_thread_join(session, workers[i].tid));
	}
	/* Handle shutdown when cleaning up after a failed open */
	if (conn->evict_workctx != NULL) {
		for (i = 0; i < conn->evict_workers_max; i++) {
			wt_session = &conn->evict_workctx[i].session->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
		}
		__wt_free(session, conn->evict_workctx);
	}

	if (conn->evict_tid_set) {
		WT_TRET(__wt_evict_server_wake(session));
		WT_TRET(__wt_thread_join(session, conn->evict_tid));
		conn->evict_tid_set = 0;
	}

	if (conn->evict_session != NULL) {
		wt_session = &conn->evict_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		conn->evict_session = NULL;
	}

	return (ret);
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
	WT_EVICT_WORKER *worker;
	WT_SESSION_IMPL *session;
	uint32_t flags;

	worker = arg;
	session = worker->session;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN) &&
	    F_ISSET(worker, WT_EVICT_WORKER_RUN)) {
		/* Don't spin in a busy loop if there is no work to do */
		WT_ERR(__evict_has_work(session, &flags));
		if (flags == 0)
			WT_ERR(__wt_cond_wait(
			    session, cache->evict_waiter_cond, 10000));
		else
			WT_ERR(__evict_lru_pages(session, 1));
	}
	WT_ERR(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "cache eviction worker exiting"));

	if (0) {
err:		WT_PANIC_MSG(session, ret, "cache eviction worker error");
	}
	return (NULL);
}

/*
 * __evict_has_work --
 *	Find out if there is eviction work to be done.
 */
static int
__evict_has_work(WT_SESSION_IMPL *session, uint32_t *flagsp)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint32_t flags;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;
	flags = 0;
	*flagsp = 0;

	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return (0);

	/*
	 * Figure out whether the cache usage exceeds either the eviction
	 * target or the dirty target.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	dirty_inuse = cache->bytes_dirty;
	bytes_max = conn->cache_size;

	/* Check to see if the eviction server should run. */
	if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
		LF_SET(WT_EVICT_PASS_ALL);
	else if (dirty_inuse >
	    (cache->eviction_dirty_target * bytes_max) / 100)
		/* Ignore clean pages unless the cache is too large */
		LF_SET(WT_EVICT_PASS_DIRTY);

	if (F_ISSET(cache, WT_EVICT_STUCK))
		LF_SET(WT_EVICT_PASS_AGGRESSIVE);

	*flagsp = flags;
	return (0);
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
	WT_EVICT_WORKER *worker;
	int loop;
	uint32_t flags;
	uint64_t bytes_inuse;

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

		WT_RET(__evict_has_work(session, &flags));
		if (flags == 0)
			break;

		if (loop > 10)
			LF_SET(WT_EVICT_PASS_AGGRESSIVE);

		bytes_inuse = __wt_cache_bytes_inuse(cache);
		/*
		 * When the cache is full, track whether pages are being
		 * evicted.  This will be cleared by the next thread to
		 * successfully evict a page.
		 */
		if (bytes_inuse > conn->cache_size) {
			F_SET(cache, WT_EVICT_NO_PROGRESS);
		} else
			F_CLR(cache, WT_EVICT_NO_PROGRESS);

		/* Start a worker if we have capacity and the cache is full. */
		if (bytes_inuse > conn->cache_size &&
		    conn->evict_workers < conn->evict_workers_max) {
			WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
			    "Starting evict worker: %"PRIu32"\n",
			    conn->evict_workers));
			worker = &conn->evict_workctx[conn->evict_workers++];
			F_SET(worker, WT_EVICT_WORKER_RUN);
			WT_RET(__wt_thread_create(session,
			    &worker->tid, __evict_worker, worker));
		}

		F_SET(cache, WT_EVICT_ACTIVE);
		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64,
		    conn->cache_size, bytes_inuse, cache->bytes_dirty));

		WT_RET(__evict_lru_walk(session, flags));
		WT_RET(__evict_server_work(session));

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
				WT_RET(__wt_verbose(
				    session, WT_VERB_EVICTSERVER,
				    "unable to reach eviction goal"));
				break;
			}
		} else
			loop = 0;
	}
	return (0);
}

/*
 * __evict_clear_walks --
 *	Clear the eviction walk points for any file a session is waiting on.
 */
static int
__evict_clear_walks(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_REF *ref;
	WT_SESSION_IMPL *s;
	u_int i, session_cnt;

	conn = S2C(session);
	cache = conn->cache;

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
		if (!s->active || !F_ISSET(s, WT_SESSION_CLEAR_EVICT_WALK))
			continue;
		if (s->dhandle == cache->evict_file_next)
			cache->evict_file_next = NULL;

		session->dhandle = s->dhandle;
		btree = s->dhandle->handle;
		if ((ref = btree->evict_ref) != NULL) {
			/*
			 * Clear evict_ref first, in case releasing it forces
			 * eviction (we assert that we never try to evict the
			 * current eviction walk point).
			 */
			btree->evict_ref = NULL;
			WT_TRET(__wt_page_release(session, ref, 0));
		}
		session->dhandle = NULL;
	}

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

	F_SET(session, WT_SESSION_CLEAR_EVICT_WALK);

	while (btree->evict_ref != NULL && ret == 0) {
		F_SET(cache, WT_EVICT_CLEAR_WALKS);
		ret = __wt_cond_wait(
		    session, cache->evict_waiter_cond, 100000);
	}

	F_CLR(session, WT_SESSION_CLEAR_EVICT_WALK);

	return (ret);
}

/*
 * __wt_evict_page --
 *	Evict a given page.
 */
int
__wt_evict_page(WT_SESSION_IMPL *session, WT_REF *ref)
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
	 * Sanity check: if a transaction has updates, its updates should not
	 * be visible to eviction.
	 */
	WT_ASSERT(session,
	    !F_ISSET(txn, TXN_HAS_ID) || !__wt_txn_visible(session, txn->id));

	ret = __wt_rec_evict(session, ref, 0);
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
	while (btree->evict_busy > 0)
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

	WT_ASSERT(session, btree->evict_ref == NULL);

	F_CLR(btree, WT_BTREE_NO_EVICTION);
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
 * __evict_lru_walk --
 *	Add pages to the LRU queue to be evicted from cache.
 */
static int
__evict_lru_walk(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	uint64_t cutoff;
	uint32_t candidates, entries, i;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	if ((ret = __evict_walk(session, &entries, flags)) != 0)
		return (ret == EBUSY ? 0 : ret);

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);

	qsort(cache->evict,
	    entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	while (entries > 0 && cache->evict[entries - 1].ref == NULL)
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

	WT_ASSERT(session, cache->evict[0].ref != NULL);

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
	 * The eviction server thread doesn't do any actual eviction if there
	 * are multiple eviction workers running.
	 */
	WT_RET(__wt_cond_signal(session, cache->evict_waiter_cond));

	return (0);
}

/*
 * __evict_server_work --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_server_work(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	if (S2C(session)->evict_workers > 1) {
		WT_STAT_FAST_CONN_INCR(
		    session, cache_eviction_server_not_evicting);

		/*
		 * If there are candidates queued, give other threads a chance
		 * to access them before gathering more.
		 */
		if (cache->evict_candidates > 10 &&
		    cache->evict_current != NULL)
			__wt_yield();
	} else {
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_server_evicting);
		WT_RET(__evict_lru_pages(session, 0));
	}

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
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, old_slot, retries, slot;
	WT_DECL_SPINLOCK_ID(id);

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
	if (cache->evict_current == NULL)
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_empty);
	else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_not_empty);

	/*
	 * Lock the dhandle list so sweeping cannot change the pointers out
	 * from under us.  If the lock is not available, give up: there may be
	 * other work for us to do without a new walk.
	 */
	WT_RET(__wt_spin_trylock(session, &conn->dhandle_lock, &id));

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

		/* Skip files that don't allow eviction. */
		btree = dhandle->handle;
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
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
    WT_SESSION_IMPL *session, WT_EVICT_ENTRY *evict, WT_REF *ref)
{
	WT_CACHE *cache;
	u_int slot;

	cache = S2C(session)->cache;

	/* Keep track of the maximum slot we are using. */
	slot = (u_int)(evict - cache->evict);
	if (slot >= cache->evict_max)
		cache->evict_max = slot + 1;

	if (evict->ref != NULL)
		__evict_list_clear(session, evict);
	evict->ref = ref;
	evict->btree = S2BT(session);

	/* Mark the page on the list */
	F_SET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU);
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
	WT_PAGE_MODIFY *mod;
	uint64_t pages_walked;
	uint32_t walk_flags;
	int internal_pages, modified, restarts;

	btree = S2BT(session);
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = WT_MIN(start + WT_EVICT_WALK_PER_FILE,
	    cache->evict + cache->evict_slots);

	walk_flags =
	    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT;

	/*
	 * Get some more eviction candidate pages.
	 */
	for (evict = start, pages_walked = 0, internal_pages = restarts = 0;
	    evict < end && (ret == 0 || ret == WT_NOTFOUND);
	    ret = __wt_tree_walk(session, &btree->evict_ref, walk_flags),
	    ++pages_walked) {
		if (btree->evict_ref == NULL) {
			/*
			 * Take care with terminating this loop.
			 *
			 * Don't make an extra call to __wt_tree_walk: that will
			 * leave a page pinned, which may prevent any work from
			 * being done.
			 */
			if (++restarts == 2)
				break;
			continue;
		}

		/* Ignore root pages entirely. */
		if (__wt_ref_is_root(btree->evict_ref))
			continue;
		page = btree->evict_ref->page;

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
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
			page->read_gen = __wt_cache_read_gen_set(session);
			continue;
		}

		/*
		 * If the file is being checkpointed, there's a period of time
		 * where we can't discard dirty pages because of possible races
		 * with the checkpointing thread.
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
		mod = page->modify;
		if (!modified && mod != NULL &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE) &&
		    !__wt_txn_visible_all(session, mod->rec_max_txn))
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
		 * that was running last time we wrote the page has
		 * since rolled back, or we can help get the checkpoint
		 * completed sooner.
		 */
		if (modified && !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE) &&
		    !btree->checkpointing &&
		    (mod->disk_snap_min == S2C(session)->txn_global.oldest_id ||
		    !__wt_txn_visible_all(session, mod->update_txn)))
			continue;

		WT_ASSERT(session, evict->ref == NULL);
		__evict_init_candidate(session, evict, btree->evict_ref);
		++evict;

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "select: %p, size %" PRIu64, page, page->memory_footprint));
	}

	/* If the walk was interrupted by a locked page, that's okay. */
	if (ret == WT_NOTFOUND)
		ret = 0;

	*slotp += (u_int)(evict - start);
	WT_STAT_FAST_CONN_INCRV(session, cache_eviction_walk, pages_walked);
	return (ret);
}

/*
 * __evict_get_ref --
 *	Get a page for eviction.
 */
static int
__evict_get_ref(
    WT_SESSION_IMPL *session, int is_app, WT_BTREE **btreep, WT_REF **refp)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t candidates;
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	cache = S2C(session)->cache;
	*btreep = NULL;
	*refp = NULL;

	/*
	 * A pathological case: if we're the oldest transaction in the system
	 * and the eviction server is stuck trying to find space, abort the
	 * transaction to give up all hazard pointers before trying again.
	 */
	if (is_app && F_ISSET(cache, WT_EVICT_STUCK) &&
	    __wt_txn_am_oldest(session)) {
		F_CLR(cache, WT_EVICT_STUCK);
		WT_STAT_FAST_CONN_INCR(session, txn_fail_cache);
		return (WT_ROLLBACK);
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
	    evict < cache->evict + candidates && evict->ref != NULL) {
		WT_ASSERT(session, evict->btree != NULL);

		/* Move to the next item. */
		++cache->evict_current;

		/*
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, this operation will fail and we will move on.
		 */
		if (!WT_ATOMIC_CAS4(
		    evict->ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
			__evict_list_clear(session, evict);
			continue;
		}

		/*
		 * Increment the busy count in the btree handle to prevent it
		 * from being closed under us.
		 */
		(void)WT_ATOMIC_ADD4(evict->btree->evict_busy, 1);

		*btreep = evict->btree;
		*refp = evict->ref;

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

	return ((*refp == NULL) ? WT_NOTFOUND : 0);
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
	WT_REF *ref;

	WT_RET(__evict_get_ref(session, is_app, &btree, &ref));
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	/*
	 * In case something goes wrong, don't pick the same set of pages every
	 * time.
	 *
	 * We used to bump the page's read generation only if eviction failed,
	 * but that isn't safe: at that point, eviction has already unlocked
	 * the page and some other thread may have evicted it by the time we
	 * look at it.
	 */
	page = ref->page;
	if (page->read_gen != WT_READGEN_OLDEST)
		page->read_gen = __wt_cache_read_gen_set(session);

	WT_WITH_BTREE(session, btree, ret = __wt_evict_page(session, ref));

	(void)WT_ATOMIC_SUB4(btree->evict_busy, 1);

	WT_RET(ret);

	cache = S2C(session)->cache;
	if (F_ISSET(cache, WT_EVICT_NO_PROGRESS | WT_EVICT_STUCK))
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
	WT_REF *next_walk;
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
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
			continue;

		file_bytes = file_dirty = file_intl_pages = file_leaf_pages = 0;
		next_walk = NULL;
		session->dhandle = dhandle;
		while (__wt_tree_walk(session,
		    &next_walk, WT_READ_CACHE | WT_READ_NO_WAIT) == 0 &&
		    next_walk != NULL) {
			page = next_walk->page;
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
