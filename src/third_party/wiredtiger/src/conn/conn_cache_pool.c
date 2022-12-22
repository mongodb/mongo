/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Tuning constants.
 */
/*
 * Threshold when a connection is allocated more cache, as a percentage of the amount of pressure
 * the busiest participant has.
 */
#define WT_CACHE_POOL_BUMP_THRESHOLD 60
/*
 * Threshold when a connection is allocated less cache, as a percentage of the amount of pressure
 * the busiest participant has.
 */
#define WT_CACHE_POOL_REDUCE_THRESHOLD 20
/* Balancing passes after a bump before a connection is a candidate. */
#define WT_CACHE_POOL_BUMP_SKIPS 5
/* Balancing passes after a reduction before a connection is a candidate. */
#define WT_CACHE_POOL_REDUCE_SKIPS 10

/*
 * Constants that control how much influence different metrics have on the pressure calculation.
 */
#define WT_CACHE_POOL_APP_EVICT_MULTIPLIER 3
#define WT_CACHE_POOL_APP_WAIT_MULTIPLIER 6
#define WT_CACHE_POOL_READ_MULTIPLIER 1

static void __cache_pool_adjust(WT_SESSION_IMPL *, uint64_t, uint64_t, bool, bool *);
static void __cache_pool_assess(WT_SESSION_IMPL *, uint64_t *);
static void __cache_pool_balance(WT_SESSION_IMPL *, bool);

/*
 * __wt_cache_pool_config --
 *     Parse and setup the cache pool options.
 */
int
__wt_cache_pool_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CACHE_POOL *cp;
    WT_CONFIG_ITEM cval, cval_cache_size;
    WT_CONNECTION_IMPL *conn, *entry;
    WT_DECL_RET;
    uint64_t chunk, quota, reserve, size, used_cache;
    char *pool_name;
    bool cp_locked, created, updating;

    conn = S2C(session);
    cp_locked = created = updating = false;
    pool_name = NULL;
    cp = NULL;

    if (F_ISSET(conn, WT_CONN_CACHE_POOL))
        updating = true;
    else {
        WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
        if (cval.len == 0) {
            /*
             * Tell the user if they configured a cache pool size but didn't enable it by naming the
             * pool.
             */
            if (__wt_config_gets(session, &cfg[1], "shared_cache.size", &cval) != WT_NOTFOUND)
                WT_RET_MSG(session, EINVAL, "Shared cache configuration requires a pool name");
            return (0);
        }

        if (__wt_config_gets(session, &cfg[1], "cache_size", &cval_cache_size) != WT_NOTFOUND)
            WT_RET_MSG(session, EINVAL,
              "Only one of cache_size and shared_cache can be in the configuration");

        /*
         * NOTE: The allocations made when configuring and opening a cache pool don't really belong
         * to the connection that allocates them. If a memory allocator becomes connection specific
         * in the future we will need a way to allocate memory outside of the connection here.
         */
        WT_RET(__wt_strndup(session, cval.str, cval.len, &pool_name));
    }

    __wt_spin_lock(session, &__wt_process.spinlock);
    if (__wt_process.cache_pool == NULL) {
        WT_ASSERT(session, !updating);
        /* Create a cache pool. */
        WT_ERR(__wt_calloc_one(session, &cp));
        created = true;
        cp->name = pool_name;
        pool_name = NULL; /* Belongs to the cache pool now. */
        TAILQ_INIT(&cp->cache_pool_qh);
        WT_ERR(__wt_spin_init(session, &cp->cache_pool_lock, "cache shared pool"));
        WT_ERR(__wt_cond_alloc(session, "cache pool server", &cp->cache_pool_cond));

        __wt_process.cache_pool = cp;
        __wt_verbose(session, WT_VERB_SHARED_CACHE, "Created cache pool %s", cp->name);
    } else if (!updating && strcmp(__wt_process.cache_pool->name, pool_name) != 0)
        /* Only a single cache pool is supported. */
        WT_ERR_MSG(
          session, WT_ERROR, "Attempting to join a cache pool that does not exist: %s", pool_name);

    /*
     * At this point we have a cache pool to use. We need to take its lock. We need to drop the
     * process lock first to avoid deadlock and acquire in the proper order.
     */
    __wt_spin_unlock(session, &__wt_process.spinlock);
    cp = __wt_process.cache_pool;
    __wt_spin_lock(session, &cp->cache_pool_lock);
    cp_locked = true;
    __wt_spin_lock(session, &__wt_process.spinlock);

    /*
     * The cache pool requires a reference count to avoid a race between configuration/open and
     * destroy.
     */
    if (!updating)
        ++cp->refs;

    /*
     * Cache pool configurations are optional when not creating. If values aren't being changed,
     * retrieve the current value so that validation of settings works.
     */
    if (!created) {
        if (__wt_config_gets(session, &cfg[1], "shared_cache.size", &cval) == 0 && cval.val != 0)
            size = (uint64_t)cval.val;
        else
            size = cp->size;
        if (__wt_config_gets(session, &cfg[1], "shared_cache.chunk", &cval) == 0 && cval.val != 0)
            chunk = (uint64_t)cval.val;
        else
            chunk = cp->chunk;
        if (__wt_config_gets(session, &cfg[1], "shared_cache.quota", &cval) == 0 && cval.val != 0)
            quota = (uint64_t)cval.val;
        else
            quota = cp->quota;
    } else {
        /*
         * The only time shared cache configuration uses default values is when we are creating the
         * pool.
         */
        WT_ERR(__wt_config_gets(session, cfg, "shared_cache.size", &cval));
        WT_ASSERT(session, cval.val != 0);
        size = (uint64_t)cval.val;
        WT_ERR(__wt_config_gets(session, cfg, "shared_cache.chunk", &cval));
        WT_ASSERT(session, cval.val != 0);
        chunk = (uint64_t)cval.val;
        WT_ERR(__wt_config_gets(session, cfg, "shared_cache.quota", &cval));
        quota = (uint64_t)cval.val;
    }

    /*
     * Retrieve the reserve size here for validation of configuration.
     * Don't save it yet since the connections cache is not created if
     * we are opening. Cache configuration is responsible for saving the
     * setting.
     * The different conditions when reserved size are set are:
     *  - It's part of the users configuration - use that value.
     *  - We are reconfiguring - keep the previous value.
     *  - We are joining a cache pool for the first time (including
     *  creating the pool) - use the chunk size; that's the default.
     */
    if (__wt_config_gets(session, &cfg[1], "shared_cache.reserve", &cval) == 0 && cval.val != 0)
        reserve = (uint64_t)cval.val;
    else if (updating)
        reserve = conn->cache->cp_reserved;
    else
        reserve = chunk;

    /*
     * Validate that size and reserve values don't cause the cache pool to be over subscribed.
     */
    used_cache = 0;
    if (!created) {
        TAILQ_FOREACH (entry, &cp->cache_pool_qh, cpq)
            used_cache += entry->cache->cp_reserved;
    }
    /* Ignore our old allocation if reconfiguring */
    if (updating)
        used_cache -= conn->cache->cp_reserved;
    if (used_cache + reserve > size)
        WT_ERR_MSG(session, EINVAL,
          "Shared cache unable to accommodate this configuration. Shared cache size: %" PRIu64
          ", requested min: %" PRIu64,
          size, used_cache + reserve);

    /* The configuration is verified - it's safe to update the pool. */
    cp->size = size;
    cp->chunk = chunk;
    cp->quota = quota;

    conn->cache->cp_reserved = reserve;
    conn->cache->cp_quota = quota;
    __wt_spin_unlock(session, &cp->cache_pool_lock);
    cp_locked = false;

    /* Wake up the cache pool server so any changes are noticed. */
    if (updating)
        __wt_cond_signal(session, __wt_process.cache_pool->cache_pool_cond);

    __wt_verbose(session, WT_VERB_SHARED_CACHE,
      "Configured cache pool %s. Size: %" PRIu64 ", chunk size: %" PRIu64, cp->name, cp->size,
      cp->chunk);

    F_SET(conn, WT_CONN_CACHE_POOL);
err:
    __wt_spin_unlock(session, &__wt_process.spinlock);
    if (cp_locked)
        __wt_spin_unlock(session, &cp->cache_pool_lock);
    __wt_free(session, pool_name);
    if (ret != 0 && created) {
        __wt_free(session, cp->name);
        __wt_cond_destroy(session, &cp->cache_pool_cond);
        __wt_free(session, cp);
    }
    return (ret);
}

/*
 * __wt_conn_cache_pool_open --
 *     Add a connection to the cache pool.
 */
int
__wt_conn_cache_pool_open(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CACHE_POOL *cp;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t session_flags;

    conn = S2C(session);
    cache = conn->cache;
    cp = __wt_process.cache_pool;

    /*
     * Create a session that can be used by the cache pool thread, do it in the main thread to avoid
     * shutdown races
     */
    session_flags = WT_SESSION_NO_DATA_HANDLES;
    if ((ret = __wt_open_internal_session(
           conn, "cache-pool", false, session_flags, 0, &cache->cp_session)) != 0)
        WT_RET_MSG(NULL, ret, "Failed to create session for cache pool");

    /*
     * Add this connection into the cache pool connection queue. Figure out if a manager thread is
     * needed while holding the lock. Don't start the thread until we have released the lock.
     */
    __wt_spin_lock(session, &cp->cache_pool_lock);
    TAILQ_INSERT_TAIL(&cp->cache_pool_qh, conn, cpq);
    __wt_spin_unlock(session, &cp->cache_pool_lock);

    __wt_verbose(session, WT_VERB_SHARED_CACHE, "Added %s to cache pool %s", conn->home, cp->name);

    /*
     * Each connection participating in the cache pool starts a manager thread. Only one manager is
     * active at a time, but having a thread in each connection saves having a complex election
     * process when the active connection shuts down.
     */
    F_SET(cp, WT_CACHE_POOL_ACTIVE);
    FLD_SET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_RUN);
    WT_RET(__wt_thread_create(session, &cache->cp_tid, __wt_cache_pool_server, cache->cp_session));

    /* Wake up the cache pool server to get our initial chunk. */
    __wt_cond_signal(session, cp->cache_pool_cond);

    return (0);
}

/*
 * __wt_conn_cache_pool_destroy --
 *     Remove our resources from the shared cache pool. Remove the cache pool if we were the last
 *     connection.
 */
int
__wt_conn_cache_pool_destroy(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CACHE_POOL *cp;
    WT_CONNECTION_IMPL *conn, *entry;
    WT_DECL_RET;
    bool cp_locked, found;

    conn = S2C(session);
    cache = conn->cache;
    WT_NOT_READ(cp_locked, false);
    found = false;
    cp = __wt_process.cache_pool;

    if (!F_ISSET(conn, WT_CONN_CACHE_POOL))
        return (0);
    F_CLR(conn, WT_CONN_CACHE_POOL);

    __wt_spin_lock(session, &cp->cache_pool_lock);
    cp_locked = true;
    TAILQ_FOREACH (entry, &cp->cache_pool_qh, cpq)
        if (entry == conn) {
            found = true;
            break;
        }

    /*
     * If there was an error during open, we may not have made it onto the queue. We did increment
     * the reference count, so proceed regardless.
     */
    if (found) {
        __wt_verbose(session, WT_VERB_SHARED_CACHE, "Removing %s from cache pool", entry->home);
        TAILQ_REMOVE(&cp->cache_pool_qh, entry, cpq);

        /* Give the connection's resources back to the pool. */
        WT_ASSERT(session, cp->currently_used >= conn->cache_size);
        cp->currently_used -= conn->cache_size;

        /*
         * Stop our manager thread - release the cache pool lock while joining the thread to allow
         * it to complete any balance operation.
         */
        __wt_spin_unlock(session, &cp->cache_pool_lock);
        WT_NOT_READ(cp_locked, false);

        FLD_CLR_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_RUN);
        __wt_cond_signal(session, cp->cache_pool_cond);
        WT_TRET(__wt_thread_join(session, &cache->cp_tid));

        WT_TRET(__wt_session_close_internal(cache->cp_session));

        /*
         * Grab the lock again now to stop other threads joining the pool while we are figuring out
         * whether we were the last participant.
         */
        __wt_spin_lock(session, &cp->cache_pool_lock);
        cp_locked = true;
    }

    /*
     * If there are no references, we are cleaning up after a failed wiredtiger_open, there is
     * nothing further to do.
     */
    if (cp->refs < 1) {
        if (cp_locked)
            __wt_spin_unlock(session, &cp->cache_pool_lock);
        return (0);
    }

    if (--cp->refs == 0) {
        WT_ASSERT(session, TAILQ_EMPTY(&cp->cache_pool_qh));
        F_CLR(cp, WT_CACHE_POOL_ACTIVE);
    }

    if (!F_ISSET(cp, WT_CACHE_POOL_ACTIVE)) {
        __wt_verbose(session, WT_VERB_SHARED_CACHE, "%s", "Destroying cache pool");
        __wt_spin_lock(session, &__wt_process.spinlock);
        /*
         * We have been holding the pool lock - no connections could have been added.
         */
        WT_ASSERT(session, cp == __wt_process.cache_pool && TAILQ_EMPTY(&cp->cache_pool_qh));
        __wt_process.cache_pool = NULL;
        __wt_spin_unlock(session, &__wt_process.spinlock);
        __wt_spin_unlock(session, &cp->cache_pool_lock);
        cp_locked = false;

        /* Now free the pool. */
        __wt_free(session, cp->name);

        __wt_spin_destroy(session, &cp->cache_pool_lock);
        __wt_cond_destroy(session, &cp->cache_pool_cond);
        __wt_free(session, cp);
    }

    if (cp_locked) {
        __wt_spin_unlock(session, &cp->cache_pool_lock);

        /* Notify other participants if we were managing */
        if (FLD_ISSET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_MANAGER)) {
            cp->pool_managed = 0;
            __wt_verbose(
              session, WT_VERB_SHARED_CACHE, "%s", "Shutting down shared cache manager connection");
        }
    }

    return (ret);
}

/*
 * __cache_pool_balance --
 *     Do a pass over the cache pool members and ensure the pool is being effectively used.
 */
static void
__cache_pool_balance(WT_SESSION_IMPL *session, bool forward)
{
    WT_CACHE_POOL *cp;
    uint64_t bump_threshold, highest;
    int i;
    bool adjusted;

    cp = __wt_process.cache_pool;
    adjusted = false;
    highest = 0;

    __wt_spin_lock(NULL, &cp->cache_pool_lock);

    /* If the queue is empty there is nothing to do. */
    if (TAILQ_FIRST(&cp->cache_pool_qh) == NULL) {
        __wt_spin_unlock(NULL, &cp->cache_pool_lock);
        return;
    }

    __cache_pool_assess(session, &highest);
    bump_threshold = WT_CACHE_POOL_BUMP_THRESHOLD;

    /*
     * Actively attempt to:
     * - Reduce the amount allocated, if we are over the budget.
     * - Increase the amount used if there is capacity and any pressure.
     * Don't keep trying indefinitely, if we aren't succeeding in reducing
     * the cache in use re-assessing the participants' states is necessary.
     * We are also holding a lock across this process, which can slow
     * participant shutdown if we spend a long time balancing.
     */
    for (i = 0; i < 2 * WT_CACHE_POOL_BUMP_THRESHOLD && F_ISSET(cp, WT_CACHE_POOL_ACTIVE) &&
         FLD_ISSET_ATOMIC_16(S2C(session)->cache->pool_flags_atomic, WT_CACHE_POOL_RUN);
         i++) {
        __cache_pool_adjust(session, highest, bump_threshold, forward, &adjusted);
        /*
         * Stop if the amount of cache being used is stable, and we aren't over capacity.
         */
        if (cp->currently_used <= cp->size && !adjusted)
            break;
        if (bump_threshold > 0)
            --bump_threshold;
    }

    __wt_spin_unlock(NULL, &cp->cache_pool_lock);
}

/*
 * __cache_pool_assess --
 *     Assess the usage of the cache pool.
 */
static void
__cache_pool_assess(WT_SESSION_IMPL *session, uint64_t *phighest)
{
    WT_CACHE *cache;
    WT_CACHE_POOL *cp;
    WT_CONNECTION_IMPL *entry;
    uint64_t app_evicts, app_waits, reads;
    uint64_t balanced_size, entries, highest, tmp;

    cp = __wt_process.cache_pool;
    balanced_size = entries = 0;
    highest = 1; /* Avoid divide by zero */

    TAILQ_FOREACH (entry, &cp->cache_pool_qh, cpq) {
        if (entry->cache_size == 0 || entry->cache == NULL)
            continue;
        ++entries;
    }

    if (entries > 0)
        balanced_size = cp->currently_used / entries;

    /* Generate read pressure information. */
    TAILQ_FOREACH (entry, &cp->cache_pool_qh, cpq) {
        if (entry->cache_size == 0 || entry->cache == NULL)
            continue;
        cache = entry->cache;

        /*
         * Figure out a delta since the last time we did an assessment for each metric we are
         * tracking. Watch out for wrapping of values.
         *
         * Count pages read, assuming pages are 4KB.
         */
        tmp = cache->bytes_read >> 12;
        if (tmp >= cache->cp_saved_read)
            reads = tmp - cache->cp_saved_read;
        else
            reads = tmp;
        cache->cp_saved_read = tmp;

        /* Update the application eviction count information */
        tmp = cache->app_evicts;
        if (tmp >= cache->cp_saved_app_evicts)
            app_evicts = tmp - cache->cp_saved_app_evicts;
        else
            app_evicts = (UINT64_MAX - cache->cp_saved_app_evicts) + tmp;
        cache->cp_saved_app_evicts = tmp;

        /* Update the eviction wait information */
        tmp = cache->app_waits;
        if (tmp >= cache->cp_saved_app_waits)
            app_waits = tmp - cache->cp_saved_app_waits;
        else
            app_waits = (UINT64_MAX - cache->cp_saved_app_waits) + tmp;
        cache->cp_saved_app_waits = tmp;

        /* Calculate the weighted pressure for this member. */
        tmp = (app_evicts * WT_CACHE_POOL_APP_EVICT_MULTIPLIER) +
          (app_waits * WT_CACHE_POOL_APP_WAIT_MULTIPLIER) + (reads * WT_CACHE_POOL_READ_MULTIPLIER);

        /* Weight smaller caches higher. */
        tmp = (uint64_t)(tmp * ((double)balanced_size / entry->cache_size));

        /* Smooth over history. */
        cache->cp_pass_pressure = (9 * cache->cp_pass_pressure + tmp) / 10;

        if (cache->cp_pass_pressure > highest)
            highest = cache->cp_pass_pressure;

        __wt_verbose_debug2(session, WT_VERB_SHARED_CACHE,
          "Assess entry. reads: %" PRIu64 ", app evicts: %" PRIu64 ", app waits: %" PRIu64
          ", pressure: %" PRIu64,
          reads, app_evicts, app_waits, cache->cp_pass_pressure);
    }
    __wt_verbose(session, WT_VERB_SHARED_CACHE,
      "Highest eviction count: %" PRIu64 ", entries: %" PRIu64, highest, entries);

    *phighest = highest;
}

/*
 * __cache_pool_adjust --
 *     Adjust the allocation of cache to each connection. If full is set ignore cache load
 *     information, and reduce the allocation for every connection allocated more than their
 *     reserved size.
 */
static void
__cache_pool_adjust(WT_SESSION_IMPL *session, uint64_t highest, uint64_t bump_threshold,
  bool forward, bool *adjustedp)
{
    WT_CACHE *cache;
    WT_CACHE_POOL *cp;
    WT_CONNECTION_IMPL *entry;
    double pct_full;
    uint64_t adjustment, highest_percentile, pressure, reserved, smallest;
    bool busy, decrease_ok, grow, pool_full;

    *adjustedp = false;

    cp = __wt_process.cache_pool;
    grow = false;
    pool_full = cp->currently_used >= cp->size;
    pct_full = 0.0;
    /* Highest as a percentage, avoid 0 */
    highest_percentile = (highest / 100) + 1;

    if (WT_VERBOSE_ISSET(session, WT_VERB_SHARED_CACHE)) {
        __wt_verbose(session, WT_VERB_SHARED_CACHE, "%s", "Cache pool distribution: ");
        __wt_verbose(session, WT_VERB_SHARED_CACHE, "%s",
          "\t"
          "cache (MB), pressure, skips, busy, %% full:");
    }

    for (entry = forward ? TAILQ_FIRST(&cp->cache_pool_qh) :
                           TAILQ_LAST(&cp->cache_pool_qh, __wt_cache_pool_qh);
         entry != NULL;
         entry = forward ? TAILQ_NEXT(entry, cpq) : TAILQ_PREV(entry, __wt_cache_pool_qh, cpq)) {
        cache = entry->cache;
        reserved = cache->cp_reserved;
        adjustment = 0;

        /*
         * The read pressure is calculated as a percentage of how much read pressure there is on
         * this participant compared to the participant with the most activity. The closer we are to
         * the most active the more cache we should get assigned.
         */
        pressure = cache->cp_pass_pressure / highest_percentile;
        busy = __wt_eviction_needed(entry->default_session, false, true, &pct_full);

        __wt_verbose_debug2(session, WT_VERB_SHARED_CACHE,
          "\t%5" PRIu64 ", %3" PRIu64 ", %2" PRIu32 ", %d, %2.3f", entry->cache_size >> 20,
          pressure, cache->cp_skip_count, busy, pct_full);

        /* Allow to stabilize after changes. */
        if (cache->cp_skip_count > 0 && --cache->cp_skip_count > 0)
            continue;

        /*
         * The bump threshold decreases as we try longer to balance the pool. Adjust how
         * aggressively we free space from participants depending on how long we have been trying.
         */
        decrease_ok = false;
        /*
         * Any participant is a candidate if we have been trying for long enough.
         */
        if (bump_threshold == 0)
            decrease_ok = true;
        /*
         * Participants that aren't doing application eviction and are showing a reasonable amount
         * of usage are excluded even if we have been trying for a while.
         */
        else if (bump_threshold < WT_CACHE_POOL_BUMP_THRESHOLD / 3 && (!busy && highest > 1))
            decrease_ok = true;
        /*
         * Any participant that is proportionally less busy is a candidate from the first attempt.
         */
        else if (highest > 1 && pressure < WT_CACHE_POOL_REDUCE_THRESHOLD)
            decrease_ok = true;

        /*
         * If the entry is currently allocated less than the reserved
         * size, increase its allocation. This should only happen if:
         *  - it's the first time we've seen this member, or
         *  - the reserved size has been adjusted
         */
        if (entry->cache_size < reserved) {
            grow = true;
            adjustment = reserved - entry->cache_size;
            /*
             * Conditions for reducing the amount of resources for an
             * entry:
             *  - the pool is full,
             *  - this entry has more than the minimum amount of space in
             *    use,
             *  - it was determined that this slot is a good candidate
             */
        } else if (pool_full && entry->cache_size > reserved && decrease_ok) {
            grow = false;
            /*
             * Don't drop the size down too much - or it can trigger aggressive eviction in the
             * connection, which is likely to lead to lower throughput and potentially a negative
             * feedback loop in the balance algorithm.
             */
            smallest = (uint64_t)((100 * __wt_cache_bytes_inuse(cache)) / cache->eviction_trigger);
            if (entry->cache_size > smallest)
                adjustment = WT_MIN(cp->chunk, (entry->cache_size - smallest) / 2);
            adjustment = WT_MIN(adjustment, entry->cache_size - reserved);
            /*
             * Conditions for increasing the amount of resources for an
             * entry:
             *  - there is space available in the pool
             *  - the connection isn't over quota
             *  - the connection is using enough cache to require eviction
             *  - there was some activity across the pool
             *  - this entry is using less than the entire cache pool
             *  - additional cache would benefit the connection OR
             *  - the pool is less than half distributed
             */
        } else if (!pool_full && (cache->cp_quota == 0 || entry->cache_size < cache->cp_quota) &&
          __wt_cache_bytes_inuse(cache) >= (entry->cache_size * cache->eviction_target) / 100 &&
          (pressure > bump_threshold || cp->currently_used < cp->size * 0.5)) {
            grow = true;
            adjustment = WT_MIN(WT_MIN(cp->chunk, cp->size - cp->currently_used),
              cache->cp_quota - entry->cache_size);
        }
        /*
         * Bounds checking: don't go over the pool size or under the reserved size for this cache.
         *
         * Shrink by a chunk size if that doesn't drop us below the reserved size.
         *
         * Limit the reduction to half of the free space in the connection's cache. This should
         * reduce cache sizes gradually without stalling application threads.
         */
        if (adjustment > 0) {
            *adjustedp = true;
            if (grow) {
                cache->cp_skip_count = WT_CACHE_POOL_BUMP_SKIPS;
                entry->cache_size += adjustment;
                cp->currently_used += adjustment;
            } else {
                cache->cp_skip_count = WT_CACHE_POOL_REDUCE_SKIPS;
                WT_ASSERT(
                  session, entry->cache_size >= adjustment && cp->currently_used >= adjustment);
                entry->cache_size -= adjustment;
                cp->currently_used -= adjustment;
            }
            __wt_verbose_debug2(session, WT_VERB_SHARED_CACHE, "Allocated %s%" PRIu64 " to %s",
              grow ? "" : "-", adjustment, entry->home);

            /*
             * TODO: Add a loop waiting for connection to give up cache.
             */
        }
    }
}

/*
 * __wt_cache_pool_server --
 *     Thread to manage cache pool among connections.
 */
WT_THREAD_RET
__wt_cache_pool_server(void *arg)
{
    WT_CACHE *cache;
    WT_CACHE_POOL *cp;
    WT_SESSION_IMPL *session;
    bool forward;

    session = (WT_SESSION_IMPL *)arg;

    cp = __wt_process.cache_pool;
    cache = S2C(session)->cache;
    forward = true;

    while (F_ISSET(cp, WT_CACHE_POOL_ACTIVE) &&
      FLD_ISSET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_RUN)) {
        if (cp->currently_used <= cp->size)
            __wt_cond_wait(session, cp->cache_pool_cond, WT_MILLION, NULL);

        /*
         * Re-check pool run flag - since we want to avoid getting the lock on shutdown.
         */
        if (!F_ISSET(cp, WT_CACHE_POOL_ACTIVE) &&
          FLD_ISSET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_RUN))
            break;

        /* Try to become the managing thread */
        if (__wt_atomic_cas8(&cp->pool_managed, 0, 1)) {
            FLD_SET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_MANAGER);
            __wt_verbose(session, WT_VERB_SHARED_CACHE, "%s", "Cache pool switched manager thread");
        }

        /*
         * Continue even if there was an error. Details of errors are reported in the balance
         * function.
         */
        if (FLD_ISSET_ATOMIC_16(cache->pool_flags_atomic, WT_CACHE_POOL_MANAGER)) {
            __cache_pool_balance(session, forward);
            forward = !forward;
        }
    }

    return (WT_THREAD_RET_VALUE);
}
