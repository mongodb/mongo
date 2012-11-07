/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Tuning constants.
 */
/* Threshold when a connection is allocated more cache */
#define	WT_CACHE_POOL_BUMP_THRESHOLD	6
/* Threshold when a connection is allocated less cache */
#define	WT_CACHE_POOL_REDUCE_THRESHOLD	2
/* Balancing passes after a bump before a connection is a candidate. */
#define	WT_CACHE_POOL_BUMP_SKIPS	10
/* Balancing passes after a reduction before a connection is a candidate. */
#define	WT_CACHE_POOL_REDUCE_SKIPS	5

static int  __cache_pool_balance(void);

/*
 * __wt_conn_cache_pool_config --
 *	Parse and setup and cache pool options.
 */
int
__wt_conn_cache_pool_config(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CACHE_POOL *cp;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	char *pool_name;
	int created, create_server, pool_locked, process_locked;

	conn = S2C(session);
	created = pool_locked = process_locked = 0;
	pool_name = NULL;
	cp = NULL;

	WT_RET(__wt_config_gets(session, cfg, "cache.pool", &cval));
	if (cval.len <= 0)
		return (0);

	WT_RET(__wt_strndup(session, cval.str, cval.len, &pool_name));
	__wt_spin_lock(conn->default_session, &__wt_process.spinlock);
	process_locked = 1;
	if (__wt_process.cache_pool == NULL) {
		/* Create a cache pool. */
		WT_ERR(__wt_config_gets(
		    session, cfg, "cache.pool_size", &cval));
		if (cval.len <= 0) {
			WT_ERR_MSG(session, WT_ERROR,
			    "Attempting to join a cache pool that does not "
			    "exist: %s. Must specify a pool size if creating.",
			    pool_name);
		}
		WT_ERR(__wt_calloc_def(conn->default_session, 1, &cp));
		created = 1;
		cp->size = cval.val;
		cp->name = pool_name;
		pool_name = NULL; /* Belongs to the cache pool now. */
		TAILQ_INIT(&cp->cache_pool_qh);
		__wt_spin_init(conn->default_session, &cp->cache_pool_lock);
		WT_ERR(__wt_cond_alloc(conn->default_session,
		    "cache pool server", 0, &cp->cache_pool_cond));

		WT_ERR(__wt_config_gets(
		    session, cfg, "cache.pool_chunk", &cval));
		if (cval.len > 0)
			cp->chunk = cval.val;
		else
			cp->chunk = WT_MAX(
			    50 * WT_MEGABYTE, cp->size / 20);
		WT_ERR(__wt_config_gets(
		    session, cfg, "cache.pool_quota", &cval));
		if (cval.len > 0)
			cp->quota = cval.val;
		else
			cp->quota = cp->size / 2;

		__wt_process.cache_pool = cp;
		WT_VERBOSE_VOID(session, cache_pool,
		    "Created cache pool %s. Size: %" PRIu64
		    ", chunk size: %" PRIu64 ", quota: %" PRIu64,
		    cp->name, cp->size, cp->chunk, cp->quota);
	} else if (!WT_STRING_MATCH(
	    __wt_process.cache_pool->name, pool_name, strlen(pool_name)))
		WT_ERR_MSG(session, WT_ERROR,
		    "Attempting to join a cache pool that does not exist: %s",
		    pool_name);
	else
		cp = __wt_process.cache_pool;

	/* Trade down to the pool lock. */
	__wt_spin_lock(conn->default_session, &cp->cache_pool_lock);
	pool_locked = 1;
	__wt_spin_unlock(conn->default_session, &__wt_process.spinlock);
	process_locked = 0;
	/* Add this connection into the cache pool connection queue. */
	WT_ERR(__wt_calloc_def(conn->default_session, 1, &conn->cache));

	/*
	 * Figure out if a manager thread is needed while holding the lock.
	 * Don't start the thread until we have released the lock.
	 */
	create_server = TAILQ_EMPTY(&cp->cache_pool_qh);
	TAILQ_INSERT_TAIL(&cp->cache_pool_qh, conn, cpq);
	F_SET(conn, WT_CONN_CACHE_POOL);
	__wt_spin_unlock(conn->default_session, &cp->cache_pool_lock);
	pool_locked = 0;
	WT_VERBOSE_VOID(session, cache_pool,
	    "Added %s to cache pool %s.", conn->home, cp->name);

	/* Start the cache pool server if required. */
	if (create_server) {
		F_SET(cp, WT_CACHE_POOL_RUN);
		WT_ERR(__wt_thread_create(
		    &cp->cache_pool_tid, __wt_cache_pool_server, NULL));
	}
	/* Wake up the cache pool server to get our initial chunk. */
	__wt_cond_signal(conn->default_session, cp->cache_pool_cond);

err:	if (process_locked)
		__wt_spin_unlock(
		    conn->default_session, &__wt_process.spinlock);
	if (pool_locked)
		__wt_spin_unlock(conn->default_session, &cp->cache_pool_lock);
	__wt_free(conn->default_session, pool_name);
	if (ret != 0 && created)
		__wt_free(conn->default_session, cp);
	return (ret);
}

/*
 * __wt_conn_cache_pool_destroy --
 *	Remove our resources from the shared cache pool. Remove the cache pool
 *	if we were the last connection.
 */
int
__wt_conn_cache_pool_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_CACHE_POOL *cp;
	WT_CONNECTION_IMPL *entry;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int found;

	found = 0;
	session = conn->default_session;

	if (!F_ISSET(conn, WT_CONN_CACHE_POOL))
		return (0);

	__wt_spin_lock(session, &__wt_process.cache_pool->cache_pool_lock);
	cp = __wt_process.cache_pool;
	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq)
		if (entry == conn) {
			found = 1;
			break;
		}

	if (!found) {
		__wt_spin_unlock(session, &cp->cache_pool_lock);
		WT_RET_MSG(session, WT_ERROR,
		    "Failed to find connection in shared cache pool.");
	}

	/* Ignore any errors - it's possible the session isn't valid. */
	WT_VERBOSE_VOID(session, cache_pool,
	    "Removing %s from cache pool.", entry->home);
	TAILQ_REMOVE(&cp->cache_pool_qh, entry, cpq);

	/* Give the connections resources back to the pool. */
	WT_ASSERT(session, cp->currently_used >= conn->cache_size);
	cp->currently_used -= conn->cache_size;
	if (TAILQ_EMPTY(&cp->cache_pool_qh))
		F_CLR(cp, WT_CACHE_POOL_RUN);

	__wt_spin_unlock(session, &cp->cache_pool_lock);
	if (!F_ISSET(cp, WT_CACHE_POOL_RUN)) {
		__wt_spin_lock(session, &__wt_process.spinlock);
		cp = __wt_process.cache_pool;
		if (!TAILQ_EMPTY(&cp->cache_pool_qh)) {
			/* Someone came in after the pool lock was released. */
			__wt_spin_unlock(session, &__wt_process.spinlock);
			return (0);
		}
		__wt_process.cache_pool = NULL;
		__wt_spin_unlock(session, &__wt_process.spinlock);
		WT_VERBOSE_VOID(session, cache_pool, "Destroying cache pool.");
		/* Shut down the cache pool worker. */
		__wt_cond_signal(
		    conn->default_session, cp->cache_pool_cond);
		WT_TRET(__wt_thread_join(cp->cache_pool_tid));

		/*
		 * Get the pool lock out of paranoia - there should not be
		 * any connections accessing the contents.
		 */
		__wt_spin_lock(session, &cp->cache_pool_lock);
		__wt_spin_unlock(session, &cp->cache_pool_lock);

		/* Now free the pool. */
		__wt_free(session, cp->name);
		__wt_spin_destroy(session, &cp->cache_pool_lock);
		WT_TRET(__wt_cond_destroy(session, cp->cache_pool_cond));
		__wt_free(session, cp);
	}

	return (ret);
}

/*
 * __cache_pool_balance --
 *	Do a pass over the cache pool members and ensure the pool is being
 *	effectively used.
 */
int
__cache_pool_balance(void)
{
	WT_CACHE_POOL *cp;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *entry;
	WT_SESSION_IMPL *session;
	uint64_t highest, new, read_pressure;
	int64_t added;
	int entries;

	cp = __wt_process.cache_pool;

	__wt_spin_lock(NULL, &cp->cache_pool_lock);
	if ((entry = TAILQ_FIRST(&cp->cache_pool_qh)) == NULL) {
		__wt_spin_unlock(NULL, &cp->cache_pool_lock);
		return (0);
	}

	/* HACK: Use the default session from the first entry. */
	session = entry->default_session;

	/* Generate read pressure information. */
	entries = 0;
	highest = 0;
	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq) {
		if (entry->cache_size == 0 ||
		    entry->cache == NULL)
			continue;
		cache = entry->cache;
		++entries;
		new = cache->bytes_evict;
		/* Handle wrapping of eviction requests. */
		if (new >= cache->cp_saved_evict)
			cache->cp_current_evict = new - cache->cp_saved_evict;
		else
			cache->cp_current_evict = new;
		cache->cp_saved_evict = new;
		if (cache->cp_current_evict > highest)
			highest = cache->cp_current_evict;
	}
	WT_VERBOSE_VOID(session, cache_pool,
	    "Highest eviction count: %d, entries: %d",
	    (int)highest, entries);
	/* Normalize eviction information across connections. */
	highest = highest / 10;
	++highest; /* Avoid divide by zero. */

	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq) {
		cache = entry->cache;
		added = 0;
		/* Allow to stabilize after changes. */
		if (cache->cp_skip_count > 0 && --cache->cp_skip_count > 0)
			continue;

		read_pressure = cache->cp_current_evict / highest;
		/*
		 * TODO: Use __wt_cache_bytes_inuse instead of eviction_target
		 * which doesn't do the right thing at the moment.
		 */
		if (entry->cache_size == 0) {
			added = cp->chunk;
			entry->cache_size = cp->chunk;
			cp->currently_used += cp->chunk;
			cache->cp_skip_count = WT_CACHE_POOL_BUMP_SKIPS;
		} else if (highest > 1 &&
		    entry->cache_size < cp->quota &&
		     cache->bytes_inmem >=
		     (entry->cache_size * cache->eviction_target) / 100 &&
		     cp->currently_used < cp->size &&
		     read_pressure > WT_CACHE_POOL_BUMP_THRESHOLD) {
			added = WT_MIN(cp->chunk,
			    cp->size - cp->currently_used);
			entry->cache_size += added;
			cp->currently_used += added;
			cache->cp_skip_count = WT_CACHE_POOL_BUMP_SKIPS;
		} else if (read_pressure < WT_CACHE_POOL_REDUCE_THRESHOLD &&
		    highest > 1 &&
		    entry->cache_size > cp->chunk &&
		    cp->currently_used >= cp->size) {
			/*
			 * If a connection isn't actively using it's assigned
			 * cache and is assigned a reasonable amount - reduce
			 * it.
			 */
			added = -cp->chunk;
			entry->cache_size -= cp->chunk;
			cp->currently_used -= cp->chunk;
			cache->cp_skip_count = WT_CACHE_POOL_REDUCE_SKIPS;
		}
		if (added != 0) {
			WT_VERBOSE_VOID(session, cache_pool,
			    "Allocated %" PRId64 " to %s",
			    added, entry->home);
			/*
			 * TODO: Add a loop waiting for connection to give up
			 * cache.
			 */
		}
	}
	__wt_spin_unlock(NULL, &cp->cache_pool_lock);
	return (0);
}

/*
 * __wt_cache_pool_server --
 *	Thread to manage cache pool among connections.
 */
void *
__wt_cache_pool_server(void *arg)
{
	WT_CACHE_POOL *cp;
	WT_DECL_RET;

	cp = __wt_process.cache_pool;

	/*
	 * TODO: Figure out what the best session handle to use is, so that
	 * error reporting is reasonably handled.
	 */
	while (F_SET(cp, WT_CACHE_POOL_RUN)) {
		__wt_cond_wait(NULL, cp->cache_pool_cond, 1000000);
		/*
		 * Re-check pool run flag - since we want to avoid getting the
		 * lock on shutdown.
		 */
		if (!F_ISSET(cp, WT_CACHE_POOL_RUN))
			break;
		WT_ERR(__cache_pool_balance());
	}
err:	return (arg);
}
