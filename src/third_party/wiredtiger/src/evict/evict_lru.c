/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_clear_all_walks(WT_SESSION_IMPL *);
static int  __evict_clear_walks(WT_SESSION_IMPL *);
static int  __evict_has_work(WT_SESSION_IMPL *, uint32_t *);
static int  WT_CDECL __evict_lru_cmp(const void *, const void *);
static int  __evict_lru_pages(WT_SESSION_IMPL *, int);
static int  __evict_lru_walk(WT_SESSION_IMPL *, uint32_t);
static int  __evict_page(WT_SESSION_IMPL *, int);
static int  __evict_pass(WT_SESSION_IMPL *);
static int  __evict_walk(WT_SESSION_IMPL *, uint32_t);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *, uint32_t);
static WT_THREAD_RET __evict_worker(void *);
static int  __evict_server_work(WT_SESSION_IMPL *);

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

	/* Any empty page (leaf or internal), is a good choice. */
	if (__wt_page_is_empty(page))
		return (WT_READGEN_OLDEST);

	/*
	 * Skew the read generation for internal pages, we prefer to evict leaf
	 * pages.
	 */
	read_gen = page->read_gen + entry->btree->evict_priority;
	if (WT_PAGE_IS_INTERNAL(page))
		read_gen += WT_EVICT_INT_SKEW;

	return (read_gen);
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort the eviction array.
 */
static int WT_CDECL
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
static WT_THREAD_RET
__evict_server(void *arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int spins;

	session = arg;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN)) {
		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_pass(session));

		if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
			break;

		/*
		 * Clear the walks so we don't pin pages while asleep,
		 * otherwise we can block applications evicting large pages.
		 */
		if (!F_ISSET(cache, WT_CACHE_STUCK)) {
			for (spins = 0; (ret = __wt_spin_trylock(
			    session, &conn->dhandle_lock)) == EBUSY &&
			    !F_ISSET(cache, WT_CACHE_CLEAR_WALKS);
			    spins++) {
				if (spins < 1000)
					__wt_yield();
				else
					__wt_sleep(0, 1000);
			}
			/*
			 * If we gave up acquiring the lock, that indicates a
			 * session is waiting for us to clear walks.  Do that
			 * as part of a normal pass (without the handle list
			 * lock) to avoid deadlock.
			 */
			if (ret == EBUSY)
				continue;
			WT_ERR(ret);
			ret = __evict_clear_all_walks(session);
			__wt_spin_unlock(session, &conn->dhandle_lock);
			WT_ERR(ret);

			/* Next time we wake up, reverse the sweep direction. */
			cache->flags ^= WT_CACHE_WALK_REVERSE;
		}

		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "sleeping"));
		/* Don't rely on signals: check periodically. */
		WT_ERR(__wt_cond_wait(session, cache->evict_cond, 100000));
		WT_ERR(__wt_verbose(session, WT_VERB_EVICTSERVER, "waking"));
	}

	/*
	 * The eviction server is shutting down: in case any trees are still
	 * open, clear all walks now so that they can be closed.
	 */
	WT_ERR(__evict_clear_all_walks(session));

	WT_ERR(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "cache eviction server exiting"));

	if (0) {
err:		WT_PANIC_MSG(session, ret, "cache eviction server error");
	}
	return (WT_THREAD_RET_VALUE);
}

/*
 * __evict_workers_resize --
 *	Resize the array of eviction workers (as needed after a reconfigure).
 *	We don't do this during the reconfigure because the eviction server
 *	thread owns these structures.
 */
static int
__evict_workers_resize(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *workers;
	size_t alloc;
	uint32_t i;

	conn = S2C(session);

	alloc = conn->evict_workers_alloc * sizeof(*workers);
	WT_RET(__wt_realloc(session, &alloc,
	    conn->evict_workers_max * sizeof(*workers), &conn->evict_workctx));
	workers = conn->evict_workctx;

	for (i = conn->evict_workers_alloc; i < conn->evict_workers_max; i++) {
		WT_ERR(__wt_open_internal_session(conn,
		    "eviction-worker", 0, 0, &workers[i].session));
		workers[i].id = i;
		F_SET(workers[i].session, WT_SESSION_CAN_WAIT);

		if (i < conn->evict_workers_min) {
			++conn->evict_workers;
			F_SET(&workers[i], WT_EVICT_WORKER_RUN);
			WT_ERR(__wt_thread_create(workers[i].session,
			    &workers[i].tid, __evict_worker, &workers[i]));
		}
	}

err:	conn->evict_workers_alloc = conn->evict_workers_max;
	return (ret);
}

/*
 * __wt_evict_create --
 *	Start the eviction server thread.
 */
int
__wt_evict_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_EVICTION_RUN);

	/* We need a session handle because we're reading/writing pages. */
	WT_RET(__wt_open_internal_session(
	    conn, "eviction-server", 0, 0, &conn->evict_session));
	session = conn->evict_session;

	/*
	 * If eviction workers were configured, allocate sessions for them now.
	 * This is done to reduce the chance that we will open new eviction
	 * sessions after WT_CONNECTION::close is called.
	 *
	 * If there's only a single eviction thread, it may be called upon to
	 * perform slow operations for the block manager.  (The flag is not
	 * reset if reconfigured later, but I doubt that's a problem.)
	 */
	if (conn->evict_workers_max > 0)
		WT_RET(__evict_workers_resize(session));
	else
		F_SET(session, WT_SESSION_CAN_WAIT);

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
 *	Destroy the eviction threads.
 */
int
__wt_evict_destroy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *workers;
	WT_SESSION *wt_session;
	uint32_t i;

	conn = S2C(session);
	cache = conn->cache;
	workers = conn->evict_workctx;

	F_CLR(conn, WT_CONN_EVICTION_RUN);

	/*
	 * Wait for the main eviction thread to exit before waiting on the
	 * helpers.  The eviction server spawns helper threads, so we can't
	 * safely know how many helpers are running until the main thread is
	 * done.
	 */
	WT_TRET(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "waiting for main thread"));
	if (conn->evict_tid_set) {
		WT_TRET(__wt_evict_server_wake(session));
		WT_TRET(__wt_thread_join(session, conn->evict_tid));
		conn->evict_tid_set = 0;
	}

	WT_TRET(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "waiting for helper threads"));
	for (i = 0; i < conn->evict_workers; i++) {
		WT_TRET(__wt_cond_signal(session, cache->evict_waiter_cond));
		WT_TRET(__wt_thread_join(session, workers[i].tid));
	}
	/* Handle shutdown when cleaning up after a failed open. */
	if (conn->evict_workctx != NULL) {
		for (i = 0; i < conn->evict_workers_alloc; i++) {
			wt_session = &conn->evict_workctx[i].session->iface;
			if (wt_session != NULL)
				WT_TRET(wt_session->close(wt_session, NULL));
		}
		__wt_free(session, conn->evict_workctx);
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
static WT_THREAD_RET
__evict_worker(void *arg)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_WORKER *worker;
	WT_SESSION_IMPL *session;

	worker = arg;
	session = worker->session;
	conn = S2C(session);
	cache = conn->cache;

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN) &&
	    F_ISSET(worker, WT_EVICT_WORKER_RUN)) {
		/* Don't spin in a busy loop if there is no work to do */
		if ((ret = __evict_lru_pages(session, 0)) == WT_NOTFOUND)
			WT_ERR(__wt_cond_wait(
			    session, cache->evict_waiter_cond, 10000));
		else
			WT_ERR(ret);
	}
	WT_ERR(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "cache eviction worker exiting"));

	if (0) {
err:		WT_PANIC_MSG(session, ret, "cache eviction worker error");
	}
	return (WT_THREAD_RET_VALUE);
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
	int evict, dirty;

	conn = S2C(session);
	cache = conn->cache;
	*flagsp = flags = 0;

	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return (0);

	/* Check to see if the eviction server should run. */
	__wt_cache_status(session, &evict, &dirty);
	if (evict)
		/* The cache is too small. */
		LF_SET(WT_EVICT_PASS_ALL);
	else if (dirty)
		/* Too many dirty pages, ignore clean pages. */
		LF_SET(WT_EVICT_PASS_DIRTY);
	else if (F_ISSET(cache, WT_CACHE_WOULD_BLOCK)) {
		/*
		 * Evict pages with oldest generation (which would otherwise
		 * block application threads) set regardless of whether we have
		 * reached the eviction trigger.
		 */
		LF_SET(WT_EVICT_PASS_WOULD_BLOCK);
		F_CLR(cache, WT_CACHE_WOULD_BLOCK);
	}

	if (F_ISSET(cache, WT_CACHE_STUCK))
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
	uint64_t pages_evicted;
	uint32_t flags;
	int loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Track whether pages are being evicted and progress is made. */
	pages_evicted = cache->pages_evict;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/*
		 * If there is a request to clear eviction walks, do that now,
		 * before checking if the cache is full.
		 */
		if (F_ISSET(cache, WT_CACHE_CLEAR_WALKS)) {
			F_CLR(cache, WT_CACHE_CLEAR_WALKS);
			WT_RET(__evict_clear_walks(session));
			WT_RET(__wt_cond_signal(
			    session, cache->evict_waiter_cond));
		}

		/*
		 * Increment the shared read generation.  We do this
		 * occasionally even if eviction is not currently required, so
		 * that pages have some relative read generation when the
		 * eviction server does need to do some work.
		 */
		__wt_cache_read_gen_incr(session);

		/*
		 * Update the oldest ID: we use it to decide whether pages are
		 * candidates for eviction.  Without this, if all threads are
		 * blocked after a long-running transaction (such as a
		 * checkpoint) completes, we may never start evicting again.
		 *
		 * Do this every time the eviction server wakes up, regardless
		 * of whether the cache is full, to prevent the oldest ID
		 * falling too far behind.
		 */
		__wt_txn_update_oldest(session, 1);

		WT_RET(__evict_has_work(session, &flags));
		if (flags == 0)
			break;

		if (loop > 10)
			LF_SET(WT_EVICT_PASS_AGGRESSIVE);

		/*
		 * Start a worker if we have capacity and we haven't reached
		 * the eviction targets.
		 */
		if (LF_ISSET(WT_EVICT_PASS_ALL |
		    WT_EVICT_PASS_DIRTY | WT_EVICT_PASS_WOULD_BLOCK) &&
		    conn->evict_workers < conn->evict_workers_max) {
			WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
			    "Starting evict worker: %"PRIu32"\n",
			    conn->evict_workers));
			if (conn->evict_workers >= conn->evict_workers_alloc)
				WT_RET(__evict_workers_resize(session));
			worker = &conn->evict_workctx[conn->evict_workers++];
			F_SET(worker, WT_EVICT_WORKER_RUN);
			WT_RET(__wt_thread_create(session,
			    &worker->tid, __evict_worker, worker));
		}

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64,
		    conn->cache_size, cache->bytes_inmem, cache->bytes_dirty));

		WT_RET(__evict_lru_walk(session, flags));
		WT_RET(__evict_server_work(session));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, mark the cache "stuck" and go back to
		 * sleep, it's not something we can fix.
		 */
		if (pages_evicted == cache->pages_evict) {
			/*
			 * Back off if we aren't making progress: walks hold
			 * the handle list lock, which blocks other operations
			 * that can free space in cache, such as LSM discarding
			 * handles.
			 */
			__wt_sleep(0, 1000 * (uint64_t)loop);
			if (loop == 100) {
				/*
				 * Mark the cache as stuck if we need space
				 * and aren't evicting any pages.
				 */
				if (!LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK)) {
					F_SET(cache, WT_CACHE_STUCK);
					WT_STAT_FAST_CONN_INCR(
					    session, cache_eviction_slow);
					WT_RET(__wt_verbose(
					    session, WT_VERB_EVICTSERVER,
					    "unable to reach eviction goal"));
				}
				break;
			}
		} else {
			loop = 0;
			pages_evicted = cache->pages_evict;
		}
	}
	return (0);
}

/*
 * __evict_clear_walk --
 *	Clear a single walk point.
 */
static int
__evict_clear_walk(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_REF *ref;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	if (session->dhandle == cache->evict_file_next)
		cache->evict_file_next = NULL;

	if ((ref = btree->evict_ref) == NULL)
		return (0);

	/*
	 * Clear evict_ref first, in case releasing it forces eviction (we
	 * assert we never try to evict the current eviction walk point).
	 */
	btree->evict_ref = NULL;
	return (__wt_page_release(session, ref, WT_READ_NO_EVICT));
}

/*
 * __evict_clear_walks --
 *	Clear the eviction walk points for any file a session is waiting on.
 */
static int
__evict_clear_walks(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *s;
	u_int i, session_cnt;

	conn = S2C(session);

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
		if (!s->active || !F_ISSET(s, WT_SESSION_CLEAR_EVICT_WALK))
			continue;
		WT_WITH_DHANDLE(
		    session, s->dhandle, WT_TRET(__evict_clear_walk(session)));
	}
	return (ret);
}

/*
 * __evict_request_walk_clear --
 *	Request that the eviction server clear the tree's current eviction
 *	point.
 */
static int
__evict_request_walk_clear(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	F_SET(session, WT_SESSION_CLEAR_EVICT_WALK);

	while (ret == 0 && (btree->evict_ref != NULL ||
	    cache->evict_file_next == session->dhandle)) {
		F_SET(cache, WT_CACHE_CLEAR_WALKS);
		ret = __wt_cond_wait(
		    session, cache->evict_waiter_cond, 100000);
	}

	F_CLR(session, WT_SESSION_CLEAR_EVICT_WALK);

	return (ret);
}

/*
 * __evict_clear_all_walks --
 *	Clear the eviction walk points for all files a session is waiting on.
 */
static int
__evict_clear_all_walks(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);

	TAILQ_FOREACH(dhandle, &conn->dhqh, q)
		if (WT_PREFIX_MATCH(dhandle->name, "file:"))
			WT_WITH_DHANDLE(session,
			    dhandle, WT_TRET(__evict_clear_walk(session)));
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
	__wt_txn_update_oldest(session, 1);
	txn = &session->txn;
	saved_iso = txn->isolation;
	txn->isolation = WT_ISO_EVICTION;

	/*
	 * Sanity check: if a transaction has updates, its updates should not
	 * be visible to eviction.
	 */
	WT_ASSERT(session, !F_ISSET(txn, WT_TXN_HAS_ID) ||
	    !__wt_txn_visible(session, txn->id));

	ret = __wt_evict(session, ref, 0);
	txn->isolation = saved_iso;

	return (ret);
}

/*
 * __wt_evict_file_exclusive_on --
 *	Get exclusive eviction access to a file and discard any of the file's
 *	blocks queued for eviction.
 */
int
__wt_evict_file_exclusive_on(WT_SESSION_IMPL *session, int *evict_resetp)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	u_int i, elem;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*
	 * If the file isn't evictable, there's no work to do.
	 */
	if (F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
		*evict_resetp = 0;
		return (0);
	}
	*evict_resetp = 1;

	/*
	 * Hold the walk lock to set the "no eviction" flag: no new pages from
	 * the file will be queued for eviction after this point.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	F_SET(btree, WT_BTREE_NO_EVICTION);
	__wt_spin_unlock(session, &cache->evict_walk_lock);

	/* Clear any existing LRU eviction walk for the file. */
	WT_RET(__evict_request_walk_clear(session));

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
__evict_lru_pages(WT_SESSION_IMPL *session, int is_server)
{
	WT_DECL_RET;

	/*
	 * Reconcile and discard some pages: EBUSY is returned if a page fails
	 * eviction because it's unavailable, continue in that case.
	 */
	while ((ret = __evict_page(session, is_server)) == 0 || ret == EBUSY)
		;
	return (ret);
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
	if ((ret = __evict_walk(session, flags)) != 0)
		return (ret == EBUSY ? 0 : ret);

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);

	entries = cache->evict_entries;
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

	/* Track the oldest read generation we have in the queue. */
	cache->read_gen_oldest = cache->evict[0].ref->page->read_gen;

	if (LF_ISSET(WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK))
		/*
		 * Take all candidates if we only gathered pages with an oldest
		 * read generation set.
		 */
		cache->evict_candidates = entries;
	else {
		/* Find the bottom 25% of read generations. */
		cutoff = (3 * __evict_read_gen(&cache->evict[0]) +
		    __evict_read_gen(&cache->evict[entries - 1])) / 4;
		/*
		 * Don't take less than 10% or more than 50% of entries,
		 * regardless.  That said, if there is only one entry, which is
		 * normal when populating an empty file, don't exclude it.
		 */
		for (candidates = 1 + entries / 10;
		    candidates < entries / 2;
		    candidates++)
			if (__evict_read_gen(
			    &cache->evict[candidates]) > cutoff)
				break;
		cache->evict_candidates = candidates;
	}

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
	} else
		WT_RET_NOTFOUND_OK(__evict_lru_pages(session, 1));

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, prev_slot, retries, slot, start_slot, spins;
	int incr, dhandle_locked;

	conn = S2C(session);
	cache = S2C(session)->cache;
	dhandle = NULL;
	incr = dhandle_locked = 0;
	retries = 0;

	if (cache->evict_current == NULL)
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_empty);
	else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_queue_not_empty);

	/*
	 * Set the starting slot in the queue and the maximum pages added
	 * per walk.
	 */
	start_slot = slot = cache->evict_entries;
	max_entries = slot + WT_EVICT_WALK_INCR;

retry:	while (slot < max_entries && ret == 0) {
		/*
		 * If another thread is waiting on the eviction server to clear
		 * the walk point in a tree, give up.
		 */
		if (F_ISSET(cache, WT_CACHE_CLEAR_WALKS))
			break;

		/*
		 * Lock the dhandle list to find the next handle and bump its
		 * reference count to keep it alive while we sweep.
		 */
		if (!dhandle_locked) {
			for (spins = 0; (ret = __wt_spin_trylock(
			    session, &conn->dhandle_lock)) == EBUSY &&
			    !F_ISSET(cache, WT_CACHE_CLEAR_WALKS);
			    spins++) {
				if (spins < 1000)
					__wt_yield();
				else
					__wt_sleep(0, 1000);
			}
			if (ret != 0)
				break;
			dhandle_locked = 1;
		}

		if (dhandle == NULL) {
			/*
			 * On entry, continue from wherever we got to in the
			 * scan last time through.  If we don't have a saved
			 * handle, start from the beginning of the list.
			 */
			if ((dhandle = cache->evict_file_next) != NULL)
				cache->evict_file_next = NULL;
			else
				dhandle = TAILQ_FIRST(&conn->dhqh);
		} else {
			if (incr) {
				WT_ASSERT(session, dhandle->session_inuse > 0);
				(void)__wt_atomic_subi32(
				    &dhandle->session_inuse, 1);
				incr = 0;
			}
			dhandle = TAILQ_NEXT(dhandle, q);
		}

		/* If we reach the end of the list, we're done. */
		if (dhandle == NULL)
			break;

		/* Ignore non-file handles, or handles that aren't open. */
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		/* Skip files that don't allow eviction. */
		btree = dhandle->handle;
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
			continue;

		/*
		 * Also skip files that are checkpointing or configured to
		 * stick in cache until we get aggressive.
		 */
		if ((btree->checkpointing || btree->evict_priority != 0) &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			continue;

		/* Skip files if we have used all available hazard pointers. */
		if (btree->evict_ref == NULL && session->nhazard >=
		    conn->hazard_max - WT_MIN(conn->hazard_max / 2, 10))
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
		prev_slot = slot;

		(void)__wt_atomic_addi32(&dhandle->session_inuse, 1);
		incr = 1;
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = 0;

		__wt_spin_lock(session, &cache->evict_walk_lock);

		/*
		 * Re-check the "no eviction" flag -- it is used to enforce
		 * exclusive access when a handle is being closed.
		 */
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
			WT_WITH_DHANDLE(session, dhandle,
			    ret = __evict_walk_file(session, &slot, flags));
			WT_ASSERT(session, session->split_gen == 0);
		}

		__wt_spin_unlock(session, &cache->evict_walk_lock);

		/*
		 * If we didn't find any candidates in the file, skip it next
		 * time.
		 */
		if (slot == prev_slot)
			btree->evict_walk_period = WT_MIN(
			    WT_MAX(1, 2 * btree->evict_walk_period), 100);
		else
			btree->evict_walk_period = 0;
	}

	if (incr) {
		/* Remember the file we should visit first, next loop. */
		cache->evict_file_next = dhandle;

		WT_ASSERT(session, dhandle->session_inuse > 0);
		(void)__wt_atomic_subi32(&dhandle->session_inuse, 1);
		incr = 0;
	}

	if (dhandle_locked) {
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = 0;
	}

	/*
	 * Walk the list of files a few times if we don't find enough pages.
	 * Try two passes through all the files, give up when we have some
	 * candidates and we aren't finding more.
	 */
	if (!F_ISSET(cache, WT_CACHE_CLEAR_WALKS) && ret == 0 &&
	    slot < max_entries && (retries < 2 ||
	    (!LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK) && retries < 10 &&
	    (slot == cache->evict_entries || slot > start_slot)))) {
		start_slot = slot;
		++retries;
		goto retry;
	}

	cache->evict_entries = slot;
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
	WT_REF *ref;
	uint64_t pages_walked;
	uint32_t walk_flags;
	int enough, internal_pages, modified, restarts;

	btree = S2BT(session);
	cache = S2C(session)->cache;
	start = cache->evict + *slotp;
	end = WT_MIN(start + WT_EVICT_WALK_PER_FILE,
	    cache->evict + cache->evict_slots);
	enough = internal_pages = restarts = 0;

	walk_flags = WT_READ_CACHE | WT_READ_NO_EVICT |
	    WT_READ_NO_GEN | WT_READ_NO_WAIT;

	if (F_ISSET(cache, WT_CACHE_WALK_REVERSE))
		walk_flags |= WT_READ_PREV;

	/*
	 * Get some more eviction candidate pages.
	 *
	 * !!! Take care terminating this loop.
	 *
	 * Don't make an extra call to __wt_tree_walk after we hit the end of a
	 * tree: that will leave a page pinned, which may prevent any work from
	 * being done.
	 *
	 * Once we hit the page limit, do one more step through the walk in
	 * case we are appending and only the last page in the file is live.
	 */
	for (evict = start, pages_walked = 0;
	    evict < end && !enough && (ret == 0 || ret == WT_NOTFOUND);
	    ret = __wt_tree_walk(
	    session, &btree->evict_ref, &pages_walked, walk_flags)) {
		enough = (pages_walked > WT_EVICT_MAX_PER_FILE);
		if ((ref = btree->evict_ref) == NULL) {
			if (++restarts == 2 || enough)
				break;
			continue;
		}

		/* Ignore root pages entirely. */
		if (__wt_ref_is_root(ref))
			continue;

		page = ref->page;
		modified = __wt_page_is_modified(page);

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		/* Pages we no longer need (clean or dirty), are found money. */
		if (__wt_page_is_empty(page))
			goto fast;

		/* Optionally ignore clean pages. */
		if (!modified && LF_ISSET(WT_EVICT_PASS_DIRTY))
			continue;

		/*
		 * If we are only trickling out pages marked for definite
		 * eviction, skip anything that isn't marked.
		 */
		if (LF_ISSET(WT_EVICT_PASS_WOULD_BLOCK) &&
		    page->read_gen != WT_READGEN_OLDEST)
			continue;

		/* Limit internal pages to 50% unless we get aggressive. */
		if (WT_PAGE_IS_INTERNAL(page) &&
		    ++internal_pages > WT_EVICT_WALK_PER_FILE / 2 &&
		    !LF_ISSET(WT_EVICT_PASS_AGGRESSIVE))
			continue;

		/*
		 * If this page has never been considered for eviction, set its
		 * read generation to somewhere in the middle of the LRU list.
		 */
		if (page->read_gen == WT_READGEN_NOTSET)
			page->read_gen = __wt_cache_read_gen_new(session);

fast:		/* If the page can't be evicted, give up. */
		if (!__wt_page_can_evict(session, page, 1, NULL))
			continue;

		/*
		 * If the page is clean but has modifications that appear too
		 * new to evict, skip it.
		 *
		 * Note: take care with ordering: if we detected that the page
		 * is modified above, we expect mod != NULL.
		 */
		mod = page->modify;
		if (!modified && mod != NULL && !LF_ISSET(
		    WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK) &&
		    !__wt_txn_visible_all(session, mod->rec_max_txn))
			continue;

		/*
		 * If the oldest transaction hasn't changed since the last time
		 * this page was written, it's unlikely that we can make
		 * progress.  Similarly, if the most recent update on the page
		 * is not yet globally visible, eviction will fail.  These
		 * heuristics attempt to avoid repeated attempts to evict the
		 * same page.
		 *
		 * That said, if eviction is stuck, or we are helping with
		 * forced eviction, try anyway: maybe a transaction that was
		 * running last time we wrote the page has since rolled back,
		 * or we can help get the checkpoint completed sooner.
		 */
		if (modified && !LF_ISSET(
		    WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK) &&
		    (mod->disk_snap_min == S2C(session)->txn_global.oldest_id ||
		    !__wt_txn_visible_all(session, mod->update_txn)))
			continue;

		WT_ASSERT(session, evict->ref == NULL);
		__evict_init_candidate(session, evict, ref);
		++evict;

		WT_RET(__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "select: %p, size %" PRIu64, page, page->memory_footprint));
	}
	WT_RET_NOTFOUND_OK(ret);

	*slotp += (u_int)(evict - start);

	/*
	 * If we happen to end up on the root page, clear it.  We have to track
	 * hazard pointers, and the root page complicates that calculation.
	 *
	 * If we land on a page requiring forced eviction, move on to the next
	 * page: we want this page evicted as quickly as possible.
	 */
	if ((ref = btree->evict_ref) != NULL) {
		if (__wt_ref_is_root(ref))
			WT_RET(__evict_clear_walk(session));
		else if (ref->page->read_gen == WT_READGEN_OLDEST)
			WT_RET_NOTFOUND_OK(__wt_tree_walk(session,
			    &btree->evict_ref, &pages_walked, walk_flags));
	}

	WT_STAT_FAST_CONN_INCRV(session, cache_eviction_walk, pages_walked);

	return (0);
}

/*
 * __evict_get_ref --
 *	Get a page for eviction.
 */
static int
__evict_get_ref(
    WT_SESSION_IMPL *session, int is_server, WT_BTREE **btreep, WT_REF **refp)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t candidates;

	cache = S2C(session)->cache;
	*btreep = NULL;
	*refp = NULL;

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
		if (__wt_spin_trylock(session, &cache->evict_lock) == 0)
			break;
		__wt_yield();
	}

	/*
	 * The eviction server only tries to evict half of the pages before
	 * looking for more.
	 */
	candidates = cache->evict_candidates;
	if (is_server && candidates > 1)
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
		if (!__wt_atomic_casv32(
		    &evict->ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
			__evict_list_clear(session, evict);
			continue;
		}

		/*
		 * Increment the busy count in the btree handle to prevent it
		 * from being closed under us.
		 */
		(void)__wt_atomic_addv32(&evict->btree->evict_busy, 1);

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
 * __evict_page --
 *	Called by both eviction and application threads to evict a page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, int is_server)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;

	WT_RET(__evict_get_ref(session, is_server, &btree, &ref));
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	/*
	 * An internal session flags either the server itself or an eviction
	 * worker thread.
	 */
	if (F_ISSET(session, WT_SESSION_INTERNAL)) {
		if (is_server)
			WT_STAT_FAST_CONN_INCR(
			    session, cache_eviction_server_evicting);
		else
			WT_STAT_FAST_CONN_INCR(
			    session, cache_eviction_worker_evicting);
	} else
		WT_STAT_FAST_CONN_INCR(session, cache_eviction_app);

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
		page->read_gen = __wt_cache_read_gen_bump(session);

	/*
	 * If we are evicting in a dead tree, don't write dirty pages.
	 *
	 * Force pages clean to keep statistics correct and to let the
	 * page-discard function assert that no dirty pages are ever
	 * discarded.
	 */
	if (F_ISSET(btree->dhandle, WT_DHANDLE_DEAD) &&
	    __wt_page_is_modified(page)) {
		page->modify->write_gen = 0;
		__wt_cache_dirty_decr(session, page);
	}

	WT_WITH_BTREE(session, btree, ret = __wt_evict_page(session, ref));

	(void)__wt_atomic_subv32(&btree->evict_busy, 1);

	WT_RET(ret);

	cache = S2C(session)->cache;
	if (F_ISSET(cache, WT_CACHE_STUCK))
		F_CLR(cache, WT_CACHE_STUCK);

	return (ret);
}

/*
 * __wt_cache_eviction_worker --
 *	Worker function for __wt_cache_eviction_check: evict pages if the cache
 * crosses its boundaries.
 */
int
__wt_cache_eviction_worker(WT_SESSION_IMPL *session, int busy, int pct_full)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	int count, q_found, txn_busy;

	conn = S2C(session);
	cache = conn->cache;

	/* First, wake the eviction server. */
	WT_RET(__wt_evict_server_wake(session));

	/*
	 * If the current transaction is keeping the oldest ID pinned, it is in
	 * the middle of an operation.	This may prevent the oldest ID from
	 * moving forward, leading to deadlock, so only evict what we can.
	 * Otherwise, we are at a transaction boundary and we can work harder
	 * to make sure there is free space in the cache.
	 */
	txn_global = &conn->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);
	txn_busy = txn_state->id != WT_TXN_NONE ||
	    session->nhazard > 0 ||
	    (txn_state->snap_min != WT_TXN_NONE &&
	    txn_global->current != txn_global->oldest_id);
	if (txn_busy) {
		if (pct_full < 100)
			return (0);
		busy = 1;
	}

	/*
	 * If we're busy, either because of the transaction check we just did,
	 * or because our caller is waiting on a longer-than-usual event (such
	 * as a page read), limit the work to a single eviction and return. If
	 * that's not the case, we can do more.
	 */
	count = busy ? 1 : 10;

	for (;;) {
		/*
		 * A pathological case: if we're the oldest transaction in the
		 * system and the eviction server is stuck trying to find space,
		 * abort the transaction to give up all hazard pointers before
		 * trying again.
		 */
		if (F_ISSET(cache, WT_CACHE_STUCK) &&
		    __wt_txn_am_oldest(session)) {
			F_CLR(cache, WT_CACHE_STUCK);
			WT_STAT_FAST_CONN_INCR(session, txn_fail_cache);
			return (WT_ROLLBACK);
		}

		/* Evict a page. */
		q_found = 0;
		switch (ret = __evict_page(session, 0)) {
		case 0:
			cache->app_evicts++;
			if (--count == 0)
				return (0);

			q_found = 1;
			break;
		case EBUSY:
			continue;
		case WT_NOTFOUND:
			break;
		default:
			return (ret);
		}

		/* See if eviction is still needed. */
		if (!__wt_eviction_needed(session, NULL))
			return (0);

		/* If we found pages in the eviction queue, continue there. */
		if (q_found)
			continue;

		/*
		 * The cache is still full and no pages were found in the queue
		 * to evict.  If this transaction is the one holding back the
		 * oldest ID, we can't wait forever.  We'll block next time we
		 * are not busy.
		 */
		if (busy) {
			__wt_txn_update_oldest(session, 0);
			if (txn_state->id == txn_global->oldest_id ||
			    txn_state->snap_min == txn_global->oldest_id)
				return (0);
		}

		/* Wait for the queue to re-populate before trying again. */
		WT_RET(
		    __wt_cond_wait(session, cache->evict_waiter_cond, 100000));

		cache->app_waits++;
		/* Check if things have changed so that we are busy. */
		if (!busy && txn_state->snap_min != WT_TXN_NONE &&
		    txn_global->current != txn_global->oldest_id)
			busy = count = 1;
	}
	/* NOTREACHED */
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

	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
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
		    &next_walk, NULL, WT_READ_CACHE | WT_READ_NO_WAIT) == 0 &&
		    next_walk != NULL) {
			page = next_walk->page;
			if (WT_PAGE_IS_INTERNAL(page))
				++file_intl_pages;
			else
				++file_leaf_pages;
			file_bytes += page->memory_footprint;
			if (__wt_page_is_modified(page))
				file_dirty += page->memory_footprint;
		}
		session->dhandle = NULL;

		printf("cache dump: %s%s%s%s:"
		    " %" PRIu64 " intl pages, %" PRIu64 " leaf pages,"
		    " %" PRIu64 "MB, %" PRIu64 "MB dirty\n",
		    dhandle->name,
		    dhandle->checkpoint == NULL ? "" : " [",
		    dhandle->checkpoint == NULL ? "" : dhandle->checkpoint,
		    dhandle->checkpoint == NULL ? "" : "]",
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
