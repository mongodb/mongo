/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_cache_pool_config --
 *	Parse and setup and cache pool options.
 */
int
__wt_conn_cache_pool_config(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CACHE_POOL *cp;
	WT_CACHE_POOL_ENTRY *entry;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	char *pool_name;
	int created, create_server;

	conn = S2C(session);
	created = 0;

	WT_ERR(__wt_config_gets(session, cfg, "cache_pool", &cval));
	if (cval.len <= 0)
		return (0);

	WT_ERR(__wt_strndup(session, cval.str, cval.len, &pool_name));
	WT_ERR(__wt_config_gets(session, cfg, "cache_pool_size", &cval));
	if (cval.len > 0) {
		/*
		 * Check to ensure we can create the pool. If someone
		 * beat us to creating it, return an error.
		 */
		WT_ERR(__wt_calloc(
		    conn->default_session, sizeof(WT_CACHE_POOL), 1, &cp));
		cp->name = pool_name;
		TAILQ_INIT(&cp->cache_pool_qh);
		__wt_spin_init(conn->default_session, &cp->cache_pool_lock);
		WT_ERR(__wt_cond_alloc(conn->default_session,
		    "cache pool server", 1, &cp->cache_pool_cond));
		created = 1;

		cp->size = cval.val;
		WT_ERR(__wt_config_gets(
		    session, cfg, "cache_pool_chunk", &cval));
		if (cval.len > 0)
			cp->chunk = cval.val;
		else
			cp->chunk = WT_MAX(
			    50 * WT_MEGABYTE, cp->size / 20);
		WT_ERR(__wt_config_gets(
		    session, cfg, "cache_pool_quota", &cval));
		if (cval.len > 0)
			cp->quota = cval.val;
		else
			cp->quota = cp->size / 2;

		/* Setup the cache pool in the WT_PROCESS handle. */
		__wt_spin_lock(conn->default_session, &__wt_process.spinlock);
		if (__wt_process.cache_pool != NULL) {
			__wt_spin_unlock(
			    conn->default_session, &__wt_process.spinlock);
			WT_ERR_MSG(session, WT_ERROR,
			    "Attempting to create cache pool when one "
			    "already exists");
		}
		__wt_process.cache_pool = cp;
		pool_name = NULL; /* It belongs to the cache pool now. */
		/* Trade down to the pool lock. */
		__wt_spin_lock(conn->default_session, &cp->cache_pool_lock);
		__wt_spin_unlock(conn->default_session, &__wt_process.spinlock);
	} else {
		/* Validate the existing pool */
		__wt_spin_lock(conn->default_session, &__wt_process.spinlock);
		if (__wt_process.cache_pool == NULL ||
		    !WT_STRING_MATCH(__wt_process.cache_pool->name,
		    pool_name, strlen(pool_name))) {
			__wt_spin_unlock(
			    conn->default_session, &__wt_process.spinlock);
			WT_ERR_MSG(session, WT_ERROR,
			    "Attempting to join a cache pool that does not "
			    "exist: %s", pool_name);
		}
		/* Trade down to the pool lock. */
		cp = __wt_process.cache_pool;
		__wt_spin_lock(conn->default_session, &cp->cache_pool_lock);
		__wt_spin_unlock(conn->default_session, &__wt_process.spinlock);
	}
	/* Add this connection into the cache pool connection queue. */
	WT_ERR(__wt_calloc(
	    conn->default_session, sizeof(WT_CACHE_POOL_ENTRY), 1, &entry));
	entry->conn = conn;
	entry->active = 1;
	/*
	 * Figure this out while holding the lock, but don't start the thread
	 * until we have released the lock.
	 */
	create_server = TAILQ_EMPTY(&cp->cache_pool_qh);
	TAILQ_INSERT_TAIL(&cp->cache_pool_qh, entry, q);
	F_SET(conn, WT_CONN_CACHE_POOL);
	__wt_spin_unlock(conn->default_session, &cp->cache_pool_lock);

	/* Start the cache pool server if required. */
	if (create_server) {
		F_SET(cp, WT_CACHE_POOL_RUN);
		WT_ERR(__wt_thread_create(
		    &cp->cache_pool_tid, __wt_cache_pool_server, NULL));
	}
	/* Wake up the cache pool server to get our initial chunk. */
	__wt_cond_signal(conn->default_session, cp->cache_pool_cond);

err:	__wt_free(conn->default_session, pool_name);
	if (ret != 0 && created)
		__wt_free(conn->default_session, cp);
	return (0);
}

/*
 * __wt_conn_cache_pool_destroy --
 *	Remove our resources from the shared cache pool
 */
int
__wt_conn_cache_pool_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_CACHE_POOL *cp;
	WT_CACHE_POOL_ENTRY *entry;
	WT_SESSION_IMPL *session;
	int found;

	found = 0;
	session = conn->default_session;

	if (!F_ISSET(conn, WT_CONN_CACHE_POOL))
		return (0);

	cp = __wt_process.cache_pool;
	__wt_spin_lock(session, &cp->cache_pool_lock);
	TAILQ_FOREACH(entry, &cp->cache_pool_qh, q)
		if (entry->conn == conn) {
			found = 1;
			break;
		}
	if (found) {
		TAILQ_REMOVE(&cp->cache_pool_qh, entry, q);
		/* Give the connections resources back to the pool. */
		WT_ASSERT(session, cp->currently_used >= conn->cache_size);
		cp->currently_used -= conn->cache_size;
		__wt_free(session, entry);
	}
	/*
	 * TODO: Should the last connection free the cache pool? If so it
	 * will need to get the process lock first.
	 */

	if (TAILQ_EMPTY(&cp->cache_pool_qh))
		F_CLR(cp, WT_CACHE_POOL_RUN);

	__wt_spin_unlock(session, &cp->cache_pool_lock);
	if (!found)
		WT_RET_MSG(session, WT_ERROR,
		    "Failed to find connection in shared cache pool.");

	return (0);
}

/*
 * __wt_cache_pool_server --
 *	Thread to manage cache pool amongst connections.
 */
void *
__wt_cache_pool_server(void *arg)
{
	WT_CACHE_POOL *cp;
	WT_CACHE_POOL_ENTRY *entry;
	WT_SESSION_IMPL *session;
	uint64_t added, highest, new, read_pressure;
	int entries;

	cp = __wt_process.cache_pool;

	/*
	 * TODO: Use a dummy session handle for calls to Wired Tiger functions
	 * so that error handling works as expected.
	 */
	while (F_SET(cp, WT_CACHE_POOL_RUN)) {
		__wt_cond_wait(NULL, cp->cache_pool_cond, 1000000);
		__wt_spin_lock(NULL, &cp->cache_pool_lock);
		entry = NULL;
		if (!TAILQ_EMPTY(&cp->cache_pool_qh))
			entry = TAILQ_FIRST(&cp->cache_pool_qh);
		if (entry == NULL) {
			__wt_spin_unlock(NULL, &cp->cache_pool_lock);
			continue;
		}
		/* HACK: Use the default session from the first entry. */
		session = entry->conn->default_session;

		/* Generate read pressure information. */
		entries = 0;
		highest = 0;
		TAILQ_FOREACH(entry, &cp->cache_pool_qh, q) {
			if (!entry->active ||
			    entry->cache_size == 0 ||
			    entry->conn->cache == NULL)
				continue;
			++entries;
			new = entry->conn->cache->bytes_evict;
			/* Handle wrapping of eviction requests. */
			if (new >= entry->saved_evict)
				entry->current_evict = new - entry->saved_evict;
			else
				entry->current_evict = new;
			entry->saved_evict = new;
			if (entry->current_evict > highest)
				highest = entry->current_evict;
		}
		WT_VERBOSE_VOID(session, cache_pool,
		    "Highest eviction count: %d, entries: %d",
		    (int)highest, entries);
		/* Normalize eviction information across connections. */
		highest = highest / 10;
		++highest; /* Avoid divide by zero. */

		TAILQ_FOREACH(entry, &cp->cache_pool_qh, q) {
			/* Allow to stabilize after changes. */
			if (!entry->active || --entry->skip_count > 0)
				continue;

			read_pressure = entry->current_evict / highest;
			/*
			 * TODO: Use __wt_cache_bytes_inuse instead of
			 * eviction_target - it doesn't do the right thing at
			 * the moment.
			 */
			if (entry->cache_size == 0) {
				entry->cache_size = cp->chunk;
				cp->currently_used += cp->chunk;
				entry->skip_count = 10;
			} else if (highest > 1 &&
			    entry->cache_size < cp->quota &&
			     entry->conn->cache->bytes_inmem >=
			     (entry->cache_size *
			      entry->conn->cache->eviction_target) / 100 &&
			     cp->currently_used < cp->size &&
			     read_pressure > 6) {
				added = WT_MIN(cp->chunk,
				    cp->size - cp->currently_used);
				entry->cache_size += added;
				cp->currently_used += added;
				entry->skip_count = 10;
			} else if (read_pressure < 2 &&
			    highest > 1 &&
			    entry->cache_size > cp->chunk &&
			    cp->currently_used >= cp->size) {
				/*
				 * If a connection isn't actively using
				 * it's assigned cache and is assigned
				 * a reasonable amount - reduce it.
				 */
				entry->cache_size -= cp->chunk;
				cp->currently_used -= cp->chunk;
				entry->skip_count = 5;
			}
			if (entry->cache_size != entry->conn->cache_size) {
				WT_VERBOSE_VOID(session, cache_pool,
				    "Allocated %d to %s",
				    (int)(entry->cache_size -
				    entry->conn->cache_size),
				    entry->conn->home);
				entry->conn->cache_size = entry->cache_size;
				/*
				 * TODO: Add a loop waiting for connection to
				 * give up cache.
				 */
			}
		}
		__wt_spin_unlock(NULL, &cp->cache_pool_lock);
	}
	return (arg);
}
