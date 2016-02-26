/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_clear_all_walks(WT_SESSION_IMPL *);
static int  __evict_clear_walks(WT_SESSION_IMPL *);
static int  WT_CDECL __evict_lru_cmp(const void *, const void *);
static int  __evict_lru_pages(WT_SESSION_IMPL *, bool);
static int  __evict_lru_walk(WT_SESSION_IMPL *);
static int  __evict_page(WT_SESSION_IMPL *, bool);
static int  __evict_pass(WT_SESSION_IMPL *);
static int  __evict_walk(WT_SESSION_IMPL *);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *);
static WT_THREAD_RET __evict_worker(void *);
static int  __evict_server_work(WT_SESSION_IMPL *);

/*
 * __evict_read_gen --
 *	Get the adjusted read generation for an eviction entry.
 */
static inline uint64_t
__evict_read_gen(const WT_EVICT_ENTRY *entry)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	uint64_t read_gen;

	btree = entry->btree;

	/* Never prioritize empty slots. */
	if (entry->ref == NULL)
		return (UINT64_MAX);

	page = entry->ref->page;

	/* Any page set to the oldest generation should be discarded. */
	if (page->read_gen == WT_READGEN_OLDEST)
		return (WT_READGEN_OLDEST);

	/*
	 * Any leaf page from a dead tree is a great choice (not internal pages,
	 * they may have children and are not yet evictable).
	 */
	if (!WT_PAGE_IS_INTERNAL(page) &&
	    F_ISSET(btree->dhandle, WT_DHANDLE_DEAD))
		return (WT_READGEN_OLDEST);

	/* Any empty page (leaf or internal), is a good choice. */
	if (__wt_page_is_empty(page))
		return (WT_READGEN_OLDEST);

	/*
	 * The base read-generation is skewed by the eviction priority.
	 * Internal pages are also adjusted, we prefer to evict leaf pages.
	 */
	read_gen = page->read_gen + btree->evict_priority;
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
	for (i = 0, evict = cache->evict_queue; i < elem; i++, evict++)
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
#ifdef HAVE_DIAGNOSTIC
	struct timespec now, stuck_ts;
	uint64_t pages_evicted = 0;
#endif
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
				if (spins < WT_THOUSAND)
					__wt_yield();
				else
					__wt_sleep(0, WT_THOUSAND);
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
#ifdef HAVE_DIAGNOSTIC
			pages_evicted = 0;
		} else if (pages_evicted != cache->pages_evict) {
			WT_ERR(__wt_epoch(session, &stuck_ts));
			pages_evicted = cache->pages_evict;
		} else {
			/* After being stuck for 5 minutes, give up. */
			WT_ERR(__wt_epoch(session, &now));
			if (WT_TIMEDIFF_SEC(now, stuck_ts) > 300) {
				__wt_err(session, ETIMEDOUT,
				    "Cache stuck for too long, giving up");
				(void)__wt_cache_dump(session, NULL);
				WT_ERR(ETIMEDOUT);
			}
#endif
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
	uint32_t i, session_flags;

	conn = S2C(session);

	if (conn->evict_workers_alloc < conn->evict_workers_max) {
		alloc = conn->evict_workers_alloc * sizeof(*workers);
		WT_RET(__wt_realloc(session, &alloc,
		    conn->evict_workers_max * sizeof(*workers),
		    &conn->evict_workctx));
		workers = conn->evict_workctx;
	}

	for (i = conn->evict_workers_alloc; i < conn->evict_workers_max; i++) {
		/*
		 * Eviction worker threads get their own session.
		 * Eviction worker threads may be called upon to perform slow
		 * operations for the block manager.
		 *
		 * Eviction worker threads get their own lookaside table cursor
		 * if the lookaside table is open.  Note that eviction is also
		 * started during recovery, before the lookaside table is
		 * created.
		 */
		session_flags = WT_SESSION_CAN_WAIT;
		if (F_ISSET(conn, WT_CONN_LAS_OPEN))
			FLD_SET(session_flags, WT_SESSION_LOOKASIDE_CURSOR);
		WT_ERR(__wt_open_internal_session(conn, "eviction-worker",
		    false, session_flags, &workers[i].session));
		workers[i].id = i;

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
	uint32_t session_flags;

	conn = S2C(session);

	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_EVICTION_RUN);

	/*
	 * We need a session handle because we're reading/writing pages.
	 *
	 * The eviction server gets its own lookaside table cursor.
	 *
	 * If there's only a single eviction thread, it may be called upon to
	 * perform slow operations for the block manager.  (The flag is not
	 * reset if reconfigured later, but I doubt that's a problem.)
	 */
	session_flags = F_ISSET(conn, WT_CONN_LAS_OPEN) ?
	    WT_SESSION_LOOKASIDE_CURSOR : 0;
	if (conn->evict_workers_max == 0)
		FLD_SET(session_flags, WT_SESSION_CAN_WAIT);
	WT_RET(__wt_open_internal_session(conn,
	    "eviction-server", false, session_flags, &conn->evict_session));
	session = conn->evict_session;

	/*
	 * If eviction workers were configured, allocate sessions for them now.
	 * This is done to reduce the chance that we will open new eviction
	 * sessions after WT_CONNECTION::close is called.
	 */
	if (conn->evict_workers_max > 0)
		WT_RET(__evict_workers_resize(session));

	/*
	 * Start the primary eviction server thread after the worker threads
	 * have started to avoid it starting additional worker threads before
	 * the worker's sessions are created.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->evict_tid, __evict_server, session));
	conn->evict_tid_set = true;

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
		conn->evict_tid_set = false;
	}

	WT_TRET(__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "waiting for helper threads"));
	for (i = 0; i < conn->evict_workers; i++) {
		WT_TRET(__wt_cond_signal(session, cache->evict_waiter_cond));
		WT_TRET(__wt_thread_join(session, workers[i].tid));
	}
	conn->evict_workers = 0;

	/* Handle shutdown when cleaning up after a failed open. */
	if (conn->evict_workctx != NULL) {
		for (i = 0; i < conn->evict_workers_alloc; i++) {
			wt_session = &conn->evict_workctx[i].session->iface;
			if (wt_session != NULL)
				WT_TRET(wt_session->close(wt_session, NULL));
		}
		__wt_free(session, conn->evict_workctx);
	}
	conn->evict_workers_alloc = 0;

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
		if ((ret = __evict_lru_pages(session, false)) == WT_NOTFOUND)
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
 * __evict_update_work --
 *	Configure eviction work state.
 */
static bool
__evict_update_work(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	WT_STAT_FAST_CONN_SET(session, cache_eviction_aggressive_set, 0);
	/* Clear previous state. */
	cache->state = 0;

	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return (false);

	/*
	 * Setup the number of refs to consider in each handle, depending
	 * on how many handles are open. We want to consider less candidates
	 * from each file as more files are open. Handle the case where there
	 * are no files open by adding 1.
	 */
	cache->evict_max_refs_per_file =
	    WT_MAX(100, WT_MILLION / (conn->open_file_count + 1));

	/*
	 * Page eviction overrides the dirty target and other types of eviction,
	 * that is, we don't care where we are with respect to the dirty target
	 * if page eviction is configured.
	 *
	 * Avoid division by zero if the cache size has not yet been set in a
	 * shared cache.
	 */
	bytes_max = conn->cache_size + 1;
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	if (bytes_inuse > (cache->eviction_target * bytes_max) / 100) {
		FLD_SET(cache->state, WT_EVICT_PASS_ALL);
		goto done;
	}

	/*
	 * If the cache has been stuck and is now under control, clear the
	 * stuck flag.
	 */
	if (bytes_inuse < bytes_max)
		F_CLR(cache, WT_CACHE_STUCK);

	dirty_inuse = __wt_cache_dirty_inuse(cache);
	if (dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100) {
		FLD_SET(cache->state, WT_EVICT_PASS_DIRTY);
		goto done;
	}

	/*
	 * Evict pages with oldest generation (which would otherwise block
	 * application threads), set regardless of whether we have reached
	 * the eviction trigger.
	 */
	if (F_ISSET(cache, WT_CACHE_WOULD_BLOCK)) {
		FLD_SET(cache->state, WT_EVICT_PASS_WOULD_BLOCK);

		F_CLR(cache, WT_CACHE_WOULD_BLOCK);
		goto done;
	}

	return (false);

done:	if (F_ISSET(cache, WT_CACHE_STUCK)) {
		WT_STAT_FAST_CONN_SET(session,
		    cache_eviction_aggressive_set, 1);
		FLD_SET(cache->state, WT_EVICT_PASS_AGGRESSIVE);
	}
	return (true);
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
		 * Increment the shared read generation. Do this occasionally
		 * even if eviction is not currently required, so that pages
		 * have some relative read generation when the eviction server
		 * does need to do some work.
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
		__wt_txn_update_oldest(session, true);

		if (!__evict_update_work(session))
			break;

		if (loop > 10) {
			WT_STAT_FAST_CONN_SET(session,
			    cache_eviction_aggressive_set, 1);
			FLD_SET(cache->state, WT_EVICT_PASS_AGGRESSIVE);
		}

		/*
		 * Start a worker if we have capacity and we haven't reached
		 * the eviction targets.
		 */
		if (FLD_ISSET(cache->state, WT_EVICT_PASS_ALL |
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

		WT_RET(__evict_lru_walk(session));
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
			__wt_sleep(0, WT_THOUSAND * (uint64_t)loop);
			if (loop == 100) {
				/*
				 * Mark the cache as stuck if we need space
				 * and aren't evicting any pages.
				 */
				if (!FLD_ISSET(cache->state,
				    WT_EVICT_PASS_WOULD_BLOCK)) {
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

	/* An error is unexpected - flag the failure. */
	if (ret != 0)
		__wt_err(session, ret, "Failed to clear eviction walk point");

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
 * __wt_evict_file_exclusive_on --
 *	Get exclusive eviction access to a file and discard any of the file's
 *	blocks queued for eviction.
 */
int
__wt_evict_file_exclusive_on(WT_SESSION_IMPL *session, bool *evict_resetp)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	u_int i, elem;

	*evict_resetp = false;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/* If the file wasn't evictable, there's no work to do. */
	if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return (0);

	/*
	 * Hold the walk lock to set the "no eviction" flag: no new pages from
	 * the file will be queued for eviction after this point.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	F_SET(btree, WT_BTREE_NO_EVICTION);
	__wt_spin_unlock(session, &cache->evict_walk_lock);

	/* Clear any existing LRU eviction walk for the file. */
	WT_ERR(__evict_request_walk_clear(session));

	/* Hold the evict lock to remove any queued pages from this file. */
	__wt_spin_lock(session, &cache->evict_lock);

	/*
	 * The eviction candidate list might reference pages from the file,
	 * clear it.
	 */
	elem = cache->evict_max;
	for (i = 0, evict = cache->evict_queue; i < elem; i++, evict++)
		if (evict->btree == btree)
			__evict_list_clear(session, evict);
	__wt_spin_unlock(session, &cache->evict_lock);

	/*
	 * We have disabled further eviction: wait for concurrent LRU eviction
	 * activity to drain.
	 */
	while (btree->evict_busy > 0)
		__wt_yield();

	*evict_resetp = true;
	return (0);

err:	F_CLR(btree, WT_BTREE_NO_EVICTION);
	return (ret);
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
__evict_lru_pages(WT_SESSION_IMPL *session, bool is_server)
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
__evict_lru_walk(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	uint64_t cutoff;
	uint32_t candidates, entries;

	cache = S2C(session)->cache;

	/* Get some more pages to consider for eviction. */
	if ((ret = __evict_walk(session)) != 0)
		return (ret == EBUSY ? 0 : ret);

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &cache->evict_lock);

	entries = cache->evict_entries;
	qsort(cache->evict_queue,
	    entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	while (entries > 0 && cache->evict_queue[entries - 1].ref == NULL)
		--entries;

	/*
	 * If we have more entries than the maximum tracked between walks,
	 * clear them.  Do this before figuring out how many of the entries are
	 * candidates so we never end up with more candidates than entries.
	 */
	while (entries > WT_EVICT_WALK_BASE)
		__evict_list_clear(session, &cache->evict_queue[--entries]);

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

	WT_ASSERT(session, cache->evict_queue[0].ref != NULL);

	/* Track the oldest read generation we have in the queue. */
	cache->read_gen_oldest = cache->evict_queue[0].ref->page->read_gen;

	if (FLD_ISSET(cache->state,
	    WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK))
		/*
		 * Take all candidates if we only gathered pages with an oldest
		 * read generation set.
		 */
		cache->evict_candidates = entries;
	else {
		/* Find the bottom 25% of read generations. */
		cutoff = (3 * __evict_read_gen(&cache->evict_queue[0]) +
		    __evict_read_gen(&cache->evict_queue[entries - 1])) / 4;
		/*
		 * Don't take less than 10% or more than 50% of entries,
		 * regardless.  That said, if there is only one entry, which is
		 * normal when populating an empty file, don't exclude it.
		 */
		for (candidates = 1 + entries / 10;
		    candidates < entries / 2;
		    candidates++)
			if (__evict_read_gen(
			    &cache->evict_queue[candidates]) > cutoff)
				break;
		cache->evict_candidates = candidates;
	}

	cache->evict_current = cache->evict_queue;
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
		WT_RET_NOTFOUND_OK(__evict_lru_pages(session, true));

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
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, prev_slot, retries, slot, start_slot, spins;
	bool dhandle_locked, incr;

	conn = S2C(session);
	cache = S2C(session)->cache;
	btree = NULL;
	dhandle = NULL;
	dhandle_locked = incr = false;
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
				if (spins < WT_THOUSAND)
					__wt_yield();
				else
					__wt_sleep(0, WT_THOUSAND);
			}
			if (ret != 0)
				break;
			dhandle_locked = true;
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
				incr = false;
				cache->evict_file_next = NULL;
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
		if ((btree->checkpointing != WT_CKPT_OFF ||
		    btree->evict_priority != 0) &&
		    !FLD_ISSET(cache->state, WT_EVICT_PASS_AGGRESSIVE))
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
		incr = true;
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = false;

		__wt_spin_lock(session, &cache->evict_walk_lock);

		/*
		 * Re-check the "no eviction" flag -- it is used to enforce
		 * exclusive access when a handle is being closed.
		 */
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
			/* Remember the file to visit first, next loop. */
			cache->evict_file_next = dhandle;

			WT_WITH_DHANDLE(session, dhandle,
			    ret = __evict_walk_file(session, &slot));
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
		WT_ASSERT(session, dhandle->session_inuse > 0);
		(void)__wt_atomic_subi32(&dhandle->session_inuse, 1);
		incr = false;
	}

	if (dhandle_locked) {
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = false;
	}

	/*
	 * Walk the list of files a few times if we don't find enough pages.
	 * Try two passes through all the files, give up when we have some
	 * candidates and we aren't finding more.
	 */
	if (!F_ISSET(cache, WT_CACHE_CLEAR_WALKS) && ret == 0 &&
	    slot < max_entries && (retries < 2 ||
	    (retries < 10 &&
	    !FLD_ISSET(cache->state, WT_EVICT_PASS_WOULD_BLOCK) &&
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
	slot = (u_int)(evict - cache->evict_queue);
	if (slot >= cache->evict_max)
		cache->evict_max = slot + 1;

	if (evict->ref != NULL)
		__evict_list_clear(session, evict);
	evict->ref = ref;
	evict->btree = S2BT(session);

	/* Mark the page on the list; set last to flush the other updates. */
	F_SET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU);
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
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	uint64_t pages_walked;
	uint32_t walk_flags;
	int internal_pages, restarts;
	bool enough, modified;

	conn = S2C(session);
	btree = S2BT(session);
	cache = conn->cache;
	internal_pages = restarts = 0;
	enough = false;

	start = cache->evict_queue + *slotp;
	end = start + WT_EVICT_WALK_PER_FILE;
	if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
	    end > cache->evict_queue + cache->evict_slots)
		end = cache->evict_queue + cache->evict_slots;

	walk_flags =
	    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT;
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
	    ret = __wt_tree_walk_count(
	    session, &btree->evict_ref, &pages_walked, walk_flags)) {
		enough = pages_walked > cache->evict_max_refs_per_file;
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
		if (__wt_page_is_empty(page) ||
		    F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
		    page->read_gen == WT_READGEN_OLDEST)
			goto fast;

		/* Skip clean pages if appropriate. */
		if (!modified && (F_ISSET(conn, WT_CONN_IN_MEMORY) ||
		    FLD_ISSET(cache->state, WT_EVICT_PASS_DIRTY)))
			continue;

		/*
		 * If we are only trickling out pages marked for definite
		 * eviction, skip anything that isn't marked.
		 */
		if (FLD_ISSET(cache->state, WT_EVICT_PASS_WOULD_BLOCK) &&
		    page->memory_footprint < btree->splitmempage)
			continue;

		/* Limit internal pages to 50% unless we get aggressive. */
		if (WT_PAGE_IS_INTERNAL(page) &&
		    !FLD_ISSET(cache->state, WT_EVICT_PASS_AGGRESSIVE) &&
		    internal_pages >= (int)(evict - start) / 2)
			continue;

		/*
		 * If this page has never been considered for eviction, set its
		 * read generation to somewhere in the middle of the LRU list.
		 */
		if (page->read_gen == WT_READGEN_NOTSET)
			page->read_gen = __wt_cache_read_gen_new(session);

fast:		/* If the page can't be evicted, give up. */
		if (!__wt_page_can_evict(session, ref, NULL))
			continue;

		/*
		 * Note: take care with ordering: if we detected that
		 * the page is modified above, we expect mod != NULL.
		 */
		mod = page->modify;

		/*
		 * Additional tests if eviction is likely to succeed.
		 *
		 * If eviction is stuck or we are helping with forced eviction,
		 * try anyway: maybe a transaction that was running last time
		 * we wrote the page has since rolled back, or we can help the
		 * checkpoint complete sooner. Additionally, being stuck will
		 * configure lookaside table writes in reconciliation, allowing
		 * us to evict pages we can't usually evict.
		 */
		if (!FLD_ISSET(cache->state,
		    WT_EVICT_PASS_AGGRESSIVE | WT_EVICT_PASS_WOULD_BLOCK)) {
			/*
			 * If the page is clean but has modifications that
			 * appear too new to evict, skip it.
			 */
			if (!modified && mod != NULL &&
			    !__wt_txn_visible_all(session, mod->rec_max_txn))
				continue;
		}

		WT_ASSERT(session, evict->ref == NULL);
		__evict_init_candidate(session, evict, ref);
		++evict;

		if (WT_PAGE_IS_INTERNAL(page))
		    ++internal_pages;

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
			WT_RET_NOTFOUND_OK(__wt_tree_walk_count(
			    session, &btree->evict_ref,
			    &pages_walked, walk_flags));
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
    WT_SESSION_IMPL *session, bool is_server, WT_BTREE **btreep, WT_REF **refp)
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
	 * Only evict half of the pages before looking for more. The remainder
	 * are left to eviction workers (if configured), or application threads
	 * if necessary.
	 */
	candidates = cache->evict_candidates;
	if (is_server && candidates > 1)
		candidates /= 2;

	/* Get the next page queued for eviction. */
	while ((evict = cache->evict_current) != NULL &&
	    evict < cache->evict_queue + candidates && evict->ref != NULL) {
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
	if (evict >= cache->evict_queue + cache->evict_candidates)
		cache->evict_current = NULL;
	__wt_spin_unlock(session, &cache->evict_lock);

	return ((*refp == NULL) ? WT_NOTFOUND : 0);
}

/*
 * __evict_page --
 *	Called by both eviction and application threads to evict a page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, bool is_server)
{
	WT_BTREE *btree;
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

	WT_WITH_BTREE(session, btree, ret = __wt_evict(session, ref, false));

	(void)__wt_atomic_subv32(&btree->evict_busy, 1);

	return (ret);
}

/*
 * __wt_cache_eviction_worker --
 *	Worker function for __wt_cache_eviction_check: evict pages if the cache
 * crosses its boundaries.
 */
int
__wt_cache_eviction_worker(WT_SESSION_IMPL *session, bool busy, u_int pct_full)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	uint64_t init_evict_count, max_pages_evicted;
	bool txn_busy;

	conn = S2C(session);
	cache = conn->cache;

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

	if (txn_busy && pct_full < 100)
		return (0);

	if (busy)
		txn_busy = true;

	/* Wake the eviction server if we need to do work. */
	WT_RET(__wt_evict_server_wake(session));

	/*
	 * If we're busy, either because of the transaction check we just did,
	 * or because our caller is waiting on a longer-than-usual event (such
	 * as a page read), limit the work to a single eviction and return. If
	 * that's not the case, we can do more.
	 */
	init_evict_count = cache->pages_evict;

	for (;;) {
		max_pages_evicted = txn_busy ? 5 : 20;

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

		/* See if eviction is still needed. */
		if (!__wt_eviction_needed(session, NULL) ||
		    cache->pages_evict > init_evict_count + max_pages_evicted)
			return (0);

		/* Evict a page. */
		switch (ret = __evict_page(session, false)) {
		case 0:
			cache->app_evicts++;
			if (txn_busy)
				return (0);
			/* FALLTHROUGH */
		case EBUSY:
			break;
		case WT_NOTFOUND:
			/* Allow the queue to re-populate before retrying. */
			WT_RET(__wt_cond_wait(
			    session, cache->evict_waiter_cond, 100000));
			cache->app_waits++;
			break;
		default:
			return (ret);
		}

		/* Check if we have become busy. */
		if (!txn_busy && txn_state->snap_min != WT_TXN_NONE &&
		    txn_global->current != txn_global->oldest_id)
			txn_busy = true;
	}
	/* NOTREACHED */
}

/*
 * __wt_evict_priority_set --
 *	Set a tree's eviction priority.
 */
void
__wt_evict_priority_set(WT_SESSION_IMPL *session, uint64_t v)
{
	S2BT(session)->evict_priority = v;
}

/*
 * __wt_evict_priority_clear --
 *	Clear a tree's eviction priority.
 */
void
__wt_evict_priority_clear(WT_SESSION_IMPL *session)
{
	S2BT(session)->evict_priority = 0;
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump debugging information to a file (default stderr) about the size of
 *	the files in the cache.
 */
int
__wt_cache_dump(WT_SESSION_IMPL *session, const char *ofile)
{
	FILE *fp;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_PAGE *page;
	WT_REF *next_walk;
	uint64_t dirty_bytes, dirty_pages, intl_bytes, intl_pages;
	uint64_t leaf_bytes, leaf_pages;
	uint64_t max_dirty_bytes, max_intl_bytes, max_leaf_bytes, total_bytes;
	size_t size;

	conn = S2C(session);
	total_bytes = 0;

	if (ofile == NULL)
		fp = stderr;
	else
		WT_RET(__wt_fopen(session, ofile, WT_FHANDLE_WRITE, 0, &fp));

	/* Note: odd string concatenation avoids spelling errors. */
	(void)__wt_fprintf(fp, "==========\n" "cache dump\n");

	saved_dhandle = session->dhandle;
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		dirty_bytes = dirty_pages = intl_bytes = intl_pages = 0;
		leaf_bytes = leaf_pages = 0;
		max_dirty_bytes = max_intl_bytes = max_leaf_bytes = 0;

		next_walk = NULL;
		session->dhandle = dhandle;
		while (__wt_tree_walk(session, &next_walk,
		    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_WAIT) == 0 &&
		    next_walk != NULL) {
			page = next_walk->page;
			size = page->memory_footprint;

			if (WT_PAGE_IS_INTERNAL(page)) {
				++intl_pages;
				intl_bytes += size;
				max_intl_bytes = WT_MAX(max_intl_bytes, size);
			} else {
				++leaf_pages;
				leaf_bytes += size;
				max_leaf_bytes = WT_MAX(max_leaf_bytes, size);
			}
			if (__wt_page_is_modified(page)) {
				++dirty_pages;
				dirty_bytes += size;
				max_dirty_bytes =
				    WT_MAX(max_dirty_bytes, size);
			}
		}
		session->dhandle = NULL;

		if (dhandle->checkpoint == NULL)
			(void)__wt_fprintf(fp, "%s(<live>): \n", dhandle->name);
		else
			(void)__wt_fprintf(fp, "%s(checkpoint=%s): \n",
			    dhandle->name, dhandle->checkpoint);
		if (intl_pages != 0)
			(void)__wt_fprintf(fp, "\t" "internal pages: "
			    "%" PRIu64 " pages, %" PRIu64
			    " max, %" PRIu64 "MB total\n",
			    intl_pages, max_intl_bytes, intl_bytes >> 20);
		if (leaf_pages != 0)
			(void)__wt_fprintf(fp, "\t" "leaf pages: "
			    "%" PRIu64 " pages, %" PRIu64
			    " max, %" PRIu64 "MB total\n",
			    leaf_pages, max_leaf_bytes, leaf_bytes >> 20);
		if (dirty_pages != 0)
			(void)__wt_fprintf(fp, "\t" "dirty pages: "
			    "%" PRIu64 " pages, %" PRIu64
			    " max, %" PRIu64 "MB total\n",
			    dirty_pages, max_dirty_bytes, dirty_bytes >> 20);

		total_bytes += intl_bytes + leaf_bytes;
	}
	session->dhandle = saved_dhandle;

	/*
	 * Apply the overhead percentage so our total bytes are comparable with
	 * the tracked value.
	 */
	if (conn->cache->overhead_pct != 0)
		total_bytes +=
		    (total_bytes * (uint64_t)conn->cache->overhead_pct) / 100;
	(void)__wt_fprintf(fp, "cache dump: total found = %" PRIu64 "MB"
	    " vs tracked inuse %" PRIu64 "MB\n",
	    total_bytes >> 20, __wt_cache_bytes_inuse(conn->cache) >> 20);
	(void)__wt_fprintf(fp, "==========\n");
	if (fp != stderr)
		WT_RET(__wt_fclose(&fp, WT_FHANDLE_WRITE));
	return (0);
}
#endif
