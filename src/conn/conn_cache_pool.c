/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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

static int __cache_pool_adjust(uint64_t, uint64_t);
static int __cache_pool_assess(uint64_t *);
static int  __cache_pool_balance(void);

/*
 * __wt_conn_cache_pool_config --
 *	Parse and setup the cache pool options.
 */
int
__wt_conn_cache_pool_config(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CACHE_POOL *cp;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn, *entry;
	WT_DECL_RET;
	char *pool_name;
	int created, reconfiguring;
	uint64_t chunk, reserve, size, used_cache;

	conn = S2C(session);
	created = reconfiguring = 0;
	pool_name = NULL;
	cp = NULL;
	reserve = size = 0;

	if (F_ISSET(conn, WT_CONN_CACHE_POOL))
		reconfiguring = 1;
	else {
		/* Only setup if a shared cache was explicitly configured. */
		WT_RET(__wt_config_gets(
		    session, cfg, "shared_cache.enable", &cval));
		if (!cval.val)
			return (0);
		WT_RET_NOTFOUND_OK(
		    __wt_config_gets(session, cfg, "shared_cache.name", &cval));
		/*
		 * NOTE: The allocations made when configuring and opening a
		 * cache pool don't really belong to the connection that
		 * allocates them. If a memory allocator becomes connection
		 * specific in the future we will need a way to allocate memory
		 * outside of the connection here.
		 */
		WT_RET(__wt_strndup(session, cval.str, cval.len, &pool_name));
	}

	__wt_spin_lock(session, &__wt_process.spinlock);
	if (__wt_process.cache_pool == NULL) {
		WT_ASSERT(session, !reconfiguring);
		/* Create a cache pool. */
		WT_ERR(__wt_calloc_def(session, 1, &cp));
		created = 1;
		cp->name = pool_name;
		pool_name = NULL; /* Belongs to the cache pool now. */
		TAILQ_INIT(&cp->cache_pool_qh);
		WT_ERR(__wt_spin_init(
		    session, &cp->cache_pool_lock, "cache shared pool"));
		WT_ERR(__wt_cond_alloc(session,
		    "cache pool server", 0, &cp->cache_pool_cond));

		__wt_process.cache_pool = cp;
		WT_VERBOSE_ERR(session, shared_cache,
		    "Created cache pool %s.", cp->name);
	} else if (!reconfiguring && !WT_STRING_MATCH(
	    __wt_process.cache_pool->name, pool_name, strlen(pool_name)))
		/* Only a single cache pool is supported. */
		WT_ERR_MSG(session, WT_ERROR,
		    "Attempting to join a cache pool that does not exist: %s",
		    pool_name);

	cp = __wt_process.cache_pool;
	/*
	 * The cache pool requires a reference count to avoid a race between
	 * configuration/open and destroy.
	 */
	if (!reconfiguring)
		++cp->refs;

	/*
	 * Retrieve the pool configuration options. The values are optional if
	 * we are re-configuring.
	 */
	ret = __wt_config_gets(session, cfg, "shared_cache.size", &cval);
	if (reconfiguring && ret == WT_NOTFOUND)
		/* Not being changed; use the old value. */
		size = cp->size;
	else {
		WT_ERR(ret);
		size = (uint64_t)cval.val;
	}
	ret = __wt_config_gets(session, cfg, "shared_cache.chunk", &cval);
	if (reconfiguring && ret == WT_NOTFOUND)
		/* Not being changed; use the old value. */
		chunk = cp->chunk;
	else {
		WT_ERR(ret);
		chunk = (uint64_t)cval.val;
	}
	/*
	 * Retrieve the reserve size here for validation of configuration.
	 * Don't save it yet since the connections cache is not created if
	 * we are opening. Cache configuration is responsible for saving the
	 * setting.
	 */
	ret = __wt_config_gets(session, cfg, "shared_cache.reserve", &cval);
	if (reconfiguring && ret == WT_NOTFOUND)
		/* It is safe to access the cache during reconfigure. */
		reserve = conn->cache->cp_reserved;
	else {
		WT_ERR(ret);
		reserve = (uint64_t)cval.val;
	}

	/*
	 * Validate that size and reserve values don't cause the cache
	 * pool to be over subscribed.
	 */
	used_cache = 0;
	if (!created) {
		TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq)
			used_cache += entry->cache->cp_reserved;
	}
	if (used_cache + reserve > size)
		WT_ERR_MSG(session, EINVAL,
		    "Shared cache unable to accommodate this configuration. "
		    "Shared cache size: %" PRIu64 ", reserved: %" PRIu64,
		    size, used_cache + reserve);

	/* The configuration is verified - it's safe to update the pool. */
	cp->size = size;
	cp->chunk = chunk;

	WT_VERBOSE_ERR(session, shared_cache,
	    "Configured cache pool %s. Size: %" PRIu64
	    ", chunk size: %" PRIu64,
	    cp->name, cp->size, cp->chunk);

	F_SET(conn, WT_CONN_CACHE_POOL);
err:	__wt_spin_unlock(session, &__wt_process.spinlock);
	if (!reconfiguring)
		__wt_free(session, pool_name);
	if (ret != 0 && created) {
		__wt_free(session, cp->name);
		WT_TRET(__wt_cond_destroy(session, &cp->cache_pool_cond));
		__wt_free(session, cp);
	}
	return (ret);
}

/*
 * __wt_conn_cache_pool_open --
 *	Add a connection to the cache pool.
 */
int
__wt_conn_cache_pool_open(WT_SESSION_IMPL *session)
{
	WT_CACHE_POOL *cp;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int create_server, locked;

	conn = S2C(session);
	locked = 0;
	cp = __wt_process.cache_pool;

	__wt_spin_lock(session, &cp->cache_pool_lock);
	locked = 1;

	/*
	 * Add this connection into the cache pool connection queue. Figure
	 * out if a manager thread is needed while holding the lock. Don't
	 * start the thread until we have released the lock.
	 */
	create_server = TAILQ_EMPTY(&cp->cache_pool_qh);

	TAILQ_INSERT_TAIL(&cp->cache_pool_qh, conn, cpq);
	__wt_spin_unlock(session, &cp->cache_pool_lock);
	locked = 0;
	WT_VERBOSE_ERR(session, shared_cache,
	    "Added %s to cache pool %s.", conn->home, cp->name);

	/* Start the cache pool server if required. */
	if (create_server) {
		F_SET(cp, WT_CACHE_POOL_RUN);
		WT_ERR(__wt_thread_create(session,
		    &cp->cache_pool_tid, __wt_cache_pool_server, NULL));
	}
	/* Wake up the cache pool server to get our initial chunk. */
	WT_ERR(__wt_cond_signal(session, cp->cache_pool_cond));

err:	if (locked)
		__wt_spin_unlock(session, &cp->cache_pool_lock);
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
	WT_SESSION *wt_session;
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

	/*
	 * If there was an error during open, we may not have made it onto the
	 * queue.  We did increment the reference count, so proceed regardless.
	 */
	if (found) {
		WT_VERBOSE_TRET(session, shared_cache,
		    "Removing %s from cache pool.", entry->home);
		TAILQ_REMOVE(&cp->cache_pool_qh, entry, cpq);

		/* Give the connection's resources back to the pool. */
		WT_ASSERT(session, cp->currently_used >= conn->cache_size);
		cp->currently_used -= conn->cache_size;
	}

	/*
	 * If there are no references, we are cleaning up after a failed
	 * wiredtiger_open, there is nothing further to do.
	 */
	if (cp->refs < 1) {
		__wt_spin_unlock(session, &cp->cache_pool_lock);
		return (0);
	}

	if (--cp->refs == 0) {
		WT_ASSERT(session, TAILQ_EMPTY(&cp->cache_pool_qh));
		F_CLR(cp, WT_CACHE_POOL_RUN);
	}

	/*
	 * Free the connection pool session if it was created by this
	 * connection. A new one will be created by the next balance pass.
	 */
	if (cp->session != NULL && conn == S2C(cp->session)) {
		WT_VERBOSE_TRET(cp->session, shared_cache,
		    "Freeing a cache pool session due to connection close.");
		wt_session = &cp->session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		/*
		 * This is safe after the close because session handles are
		 * not freed, but are managed by the connection.
		 */
		__wt_free(NULL, cp->session->hazard);
		cp->session = NULL;
	}

	if (F_ISSET(cp, WT_CACHE_POOL_RUN))
		__wt_spin_unlock(session, &cp->cache_pool_lock);
	else {
		WT_VERBOSE_TRET(
		    session, shared_cache, "Destroying cache pool.");
		__wt_spin_lock(session, &__wt_process.spinlock);
		cp = __wt_process.cache_pool;
		/*
		 * We have been holding the pool lock - no connections could
		 * have been added.
		 */
		WT_ASSERT(session, TAILQ_EMPTY(&cp->cache_pool_qh));
		__wt_process.cache_pool = NULL;
		__wt_spin_unlock(session, &__wt_process.spinlock);
		__wt_spin_unlock(session, &cp->cache_pool_lock);

		if (found) {
			/* Shut down the cache pool worker. */
			WT_TRET(__wt_cond_signal(session, cp->cache_pool_cond));
			WT_TRET(__wt_thread_join(session, cp->cache_pool_tid));
		}

		/* Now free the pool. */
		__wt_free(session, cp->name);
		__wt_spin_destroy(session, &cp->cache_pool_lock);
		WT_TRET(__wt_cond_destroy(session, &cp->cache_pool_cond));
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
	WT_CONNECTION_IMPL *entry;
	WT_DECL_RET;
	uint64_t bump_threshold, highest, last_used;

	cp = __wt_process.cache_pool;
	highest = 0;

	__wt_spin_lock(NULL, &cp->cache_pool_lock);
	/* If the queue is empty there is nothing to do. */
	if ((entry = TAILQ_FIRST(&cp->cache_pool_qh)) == NULL)
		goto err;

	/*
	 * The cache pool session handle may be NULL if this is the first
	 * balance pass, or the connection that created the session handle
	 * has closed. If it's NULL create a new one.
	 */
	if (cp->session == NULL &&
	    (ret = __wt_open_session(entry, 1, NULL, NULL, &cp->session)) != 0)
		WT_ERR_MSG(NULL, ret,
		    "Failed to create session for cache pool");

	WT_ERR(__cache_pool_assess(&highest));
	last_used = cp->currently_used;
	bump_threshold = WT_CACHE_POOL_BUMP_THRESHOLD;
	/*
	 * Actively attempt to:
	 * - Reduce the amount allocated, if we are over the budget
	 * - Increase the amount used if there is capacity and any pressure.
	 */
	do {
		WT_ERR(__cache_pool_adjust(highest, --bump_threshold));
	} while (cp->currently_used > cp->size ||
	    (cp->currently_used == last_used && bump_threshold > 0 &&
	    cp->currently_used < cp->size));

err:	__wt_spin_unlock(NULL, &cp->cache_pool_lock);
	return (ret);
}

static int
__cache_pool_assess(uint64_t *phighest)
{
	WT_CACHE_POOL *cp;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *entry;
	WT_SESSION_IMPL *session;
	uint64_t entries, highest, new;

	cp = __wt_process.cache_pool;
	session = cp->session;
	entries = highest = 0;

	/* Generate read pressure information. */
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
	WT_VERBOSE_RET(session, shared_cache,
	    "Highest eviction count: %" PRIu64 ", entries: %" PRIu64,
	    highest, entries);
	/* Normalize eviction information across connections. */
	highest = highest / (entries + 1);
	++highest; /* Avoid divide by zero. */

	*phighest = highest;
	return (0);
}

/*
 * Adjust the allocation of cache to each connection. If force is set ignore
 * cache load information, and reduce the allocation for every connection
 * allocated more than their reserved size.
 */
static int
__cache_pool_adjust(uint64_t highest, uint64_t bump_threshold)
{
	WT_CACHE_POOL *cp;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *entry;
	WT_SESSION_IMPL *session;
	uint64_t adjusted, reserved, read_pressure;
	int force, grew;

	cp = __wt_process.cache_pool;
	session = cp->session;
	reserved = read_pressure = 0;
	grew = 0;
	force = (cp->currently_used > cp->size);
	if (WT_VERBOSE_ISSET(session, shared_cache)) {
		WT_VERBOSE_RET(session, shared_cache,
		    "Cache pool distribution: ");
		WT_VERBOSE_RET(session, shared_cache,
		    "\t" "cache_size, read_pressure, skips: ");
	}

	TAILQ_FOREACH(entry, &cp->cache_pool_qh, cpq) {
		cache = entry->cache;
		reserved = cache->cp_reserved;
		adjusted = 0;

		read_pressure = cache->cp_current_evict / highest;
		WT_VERBOSE_RET(session, shared_cache,
		    "\t%"PRIu64", %"PRIu64", %d",
		    entry->cache_size, read_pressure, cache->cp_skip_count);

		/* Allow to stabilize after changes. */
		if (cache->cp_skip_count > 0 && --cache->cp_skip_count > 0)
			continue;
		/*
		 * TODO: Use __wt_cache_bytes_inuse instead of eviction_target
		 * which doesn't do the right thing at the moment.
		 */
		if (entry->cache_size < reserved) {
			grew = 1;
			adjusted = reserved - entry->cache_size;
		} else if ((force && entry->cache_size > reserved) ||
		    (read_pressure < WT_CACHE_POOL_REDUCE_THRESHOLD &&
		     highest > 1 && entry->cache_size > reserved &&
		     cp->currently_used >= cp->size)) {
			/*
			 * If a connection isn't actively using it's assigned
			 * cache and is assigned a reasonable amount - reduce
			 * it.
			 */
			grew = 0;
			adjusted = (cp->chunk > entry->cache_size - reserved) ?
			    cp->chunk : (entry->cache_size - reserved);
		} else if (highest > 1 &&
		    entry->cache_size < cp->size &&
		     cache->bytes_inmem >=
		     (entry->cache_size * cache->eviction_target) / 100 &&
		     cp->currently_used < cp->size &&
		     read_pressure > bump_threshold) {
			grew = 1;
			adjusted = WT_MIN(cp->chunk,
			    cp->size - cp->currently_used);
		}
		if (adjusted > 0) {
			if (grew > 0) {
				cache->cp_skip_count = WT_CACHE_POOL_BUMP_SKIPS;
				entry->cache_size += adjusted;
				cp->currently_used += adjusted;
			} else {
				cache->cp_skip_count =
				    WT_CACHE_POOL_REDUCE_SKIPS;
				entry->cache_size -= adjusted;
				cp->currently_used -= adjusted;
			}
			WT_VERBOSE_RET(session, shared_cache,
			    "Allocated %s%" PRId64 " to %s",
			    grew ? "" : "-", adjusted, entry->home);
			/*
			 * TODO: Add a loop waiting for connection to give up
			 * cache.
			 */
		}
	}
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
	WT_SESSION_IMPL *session;

	WT_UNUSED(arg);

	cp = __wt_process.cache_pool;
	session = cp->session;

	while (F_ISSET(cp, WT_CACHE_POOL_RUN)) {
		if (cp->currently_used <= cp->size)
			WT_ERR(__wt_cond_wait(
			    session, cp->cache_pool_cond, 1000000));
		/*
		 * Re-check pool run flag - since we want to avoid getting the
		 * lock on shutdown.
		 */
		if (!F_ISSET(cp, WT_CACHE_POOL_RUN))
			break;
		/*
		 * Continue even if there was an error. Details of errors are
		 * reported in the balance function.
		 */
		(void)__cache_pool_balance();
	}

	if (0) {
err:		__wt_err(session, ret, "cache pool manager server error");
	}
	return (NULL);
}
