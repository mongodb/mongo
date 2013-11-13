/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_eviction_check --
 *	Wake the eviction server if necessary.
 */
static inline int
__wt_eviction_check(WT_SESSION_IMPL *session, int *fullp, int wake)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If we're over the maximum cache, shut out reads (which include page
	 * allocations) until we evict to back under the maximum cache.
	 * Eviction will keep pushing out pages so we don't run on the edge all
	 * the time.  Avoid division by zero if the cache size has not yet been
	 * in a shared cache.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	dirty_inuse = cache->bytes_dirty;
	bytes_max = conn->cache_size + 1;

	/* Calculate the cache full percentage. */
	*fullp = (int)((100 * bytes_inuse) / bytes_max);

	/* Wake eviction when we're over the trigger cache size. */
	if (wake &&
	    (bytes_inuse > (cache->eviction_trigger * bytes_max) / 100 ||
	    dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100))
		WT_RET(__wt_evict_server_wake(session));
	return (0);
}

/*
 * __wt_cache_full_check --
 *	Wait for there to be space in the cache before a read or update.
 */
static inline int
__wt_cache_full_check(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	int busy, full;

	/*
	 * If the current transaction is keeping the oldest ID pinned, it is in
	 * the middle of an operation.	This may prevent the oldest ID from
	 * moving forward, leading to deadlock, so only evict what we can.
	 * Otherwise, we are at a transaction boundary and we can work harder
	 * to make sure there is free space in the cache.
	 */
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];
	busy = F_ISSET(session, WT_SESSION_CACHE_BUSY) ||
	    (txn_state->snap_min != WT_TXN_NONE &&
	    txn_global->current != txn_global->oldest_id);

	/*
	 * Only wake the eviction server the first time through here (if the
	 * cache is too full).
	 */
	WT_RET(__wt_eviction_check(session, &full, 1));

	/*
	 * If this is an ordinary page read and the cache isn't full, we're
	 * done.  If we are at the API boundary and the cache is more than 95%
	 * full, try to evict a page before we check again.  This helps with
	 * some eviction-dominated workloads.
	 */
	if (full < (busy ? 100 : 95) || F_ISSET(session,
	    WT_SESSION_NO_CACHE_CHECK | WT_SESSION_SCHEMA_LOCKED))
		return (0);

	if ((btree = S2BT_SAFE(session)) != NULL &&
	    F_ISSET(btree, WT_BTREE_BULK | WT_BTREE_NO_EVICTION))
		return (0);

	for (;;) {
		if ((ret = __wt_evict_lru_page(session, 1)) == 0) {
			if (busy)
				return (0);
		} else if (ret != EBUSY && ret != WT_NOTFOUND)
			return (ret);

		WT_RET(__wt_eviction_check(session, &full, 0));
		if (full < 100)
			return (0);
		if (ret == EBUSY)
			continue;

		/*
		 * No pages were found in the queue to evict.  If this
		 * transaction is the one holding back the oldest ID, we can't
		 * wait forever.  We'll block next time we are not busy.
		 */
		__wt_txn_update_oldest(session);
		if (busy &&
		    txn_state->snap_min == S2C(session)->txn_global.oldest_id)
			return (0);

		/* Wait for the queue to re-populate before trying again. */
		WT_RET(__wt_cond_wait(session,
		    S2C(session)->cache->evict_waiter_cond, 10000));
	}
}
