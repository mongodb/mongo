/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cache_config_abs_to_pct --
 *     Cache configuration values can be either a percentage or an absolute size, this function
 *     converts an absolute size to a percentage.
 */
static inline int
__cache_config_abs_to_pct(
  WT_SESSION_IMPL *session, double *param, const char *param_name, bool shared)
{
    WT_CONNECTION_IMPL *conn;
    double input;

    conn = S2C(session);

    WT_ASSERT(session, param != NULL);
    input = *param;

    /*
     * Anything above 100 is an absolute value; convert it to percentage.
     */
    if (input > 100.0) {
        /*
         * In a shared cache configuration the cache size changes regularly. Therefore, we require a
         * percentage setting and do not allow an absolute size setting.
         */
        if (shared)
            WT_RET_MSG(session, EINVAL,
              "Shared cache configuration requires a percentage value for %s", param_name);
        /* An absolute value can't exceed the cache size. */
        if (input > conn->cache_size)
            WT_RET_MSG(session, EINVAL, "%s should not exceed cache size", param_name);

        *param = (input * 100.0) / (conn->cache_size);
    }

    return (0);
}

/*
 * __cache_config_local --
 *     Configure the underlying cache.
 */
static int
__cache_config_local(WT_SESSION_IMPL *session, bool shared, const char *cfg[])
{
    WT_CACHE *cache;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint32_t evict_threads_max, evict_threads_min;

    conn = S2C(session);
    cache = conn->cache;

    /*
     * If not using a shared cache configure the cache size, otherwise check for a reserved size.
     * All other settings are independent of whether we are using a shared cache or not.
     */
    if (!shared) {
        WT_RET(__wt_config_gets(session, cfg, "cache_size", &cval));
        conn->cache_size = (uint64_t)cval.val;
    }

    WT_RET(__wt_config_gets(session, cfg, "cache_overhead", &cval));
    cache->overhead_pct = (u_int)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "eviction_target", &cval));
    cache->eviction_target = (double)cval.val;
    WT_RET(
      __cache_config_abs_to_pct(session, &(cache->eviction_target), "eviction target", shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
    cache->eviction_trigger = (double)cval.val;
    WT_RET(
      __cache_config_abs_to_pct(session, &(cache->eviction_trigger), "eviction trigger", shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_checkpoint_target", &cval));
    cache->eviction_checkpoint_target = (double)cval.val;
    WT_RET(__cache_config_abs_to_pct(
      session, &(cache->eviction_checkpoint_target), "eviction checkpoint target", shared));

    WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_target", &cval));
    cache->eviction_dirty_target = (double)cval.val;
    WT_RET(__cache_config_abs_to_pct(
      session, &(cache->eviction_dirty_target), "eviction dirty target", shared));

    /*
     * Don't allow the dirty target to be larger than the overall target.
     */
    if (cache->eviction_dirty_target > cache->eviction_target)
        cache->eviction_dirty_target = cache->eviction_target;

    /*
     * Sanity check the checkpoint target: don't allow a value lower than the dirty target.
     */
    if (cache->eviction_checkpoint_target > 0 &&
      cache->eviction_checkpoint_target < cache->eviction_dirty_target)
        cache->eviction_checkpoint_target = cache->eviction_dirty_target;

    WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_trigger", &cval));
    cache->eviction_dirty_trigger = (double)cval.val;
    WT_RET(__cache_config_abs_to_pct(
      session, &(cache->eviction_dirty_trigger), "eviction dirty trigger", shared));

    /*
     * Don't allow the dirty trigger to be larger than the overall trigger or we can get stuck with
     * a cache full of dirty data.
     */
    if (cache->eviction_dirty_trigger > cache->eviction_trigger)
        cache->eviction_dirty_trigger = cache->eviction_trigger;

    /* Configure updates target / trigger */
    WT_RET(__wt_config_gets(session, cfg, "eviction_updates_target", &cval));
    cache->eviction_updates_target = (double)cval.val;
    WT_RET(__cache_config_abs_to_pct(
      session, &(cache->eviction_updates_target), "eviction updates target", shared));
    if (cache->eviction_updates_target < DBL_EPSILON)
        cache->eviction_updates_target = cache->eviction_dirty_target / 2;

    WT_RET(__wt_config_gets(session, cfg, "eviction_updates_trigger", &cval));
    cache->eviction_updates_trigger = (double)cval.val;
    WT_RET(__cache_config_abs_to_pct(
      session, &(cache->eviction_updates_trigger), "eviction updates trigger", shared));
    if (cache->eviction_updates_trigger < DBL_EPSILON)
        cache->eviction_updates_trigger = cache->eviction_dirty_trigger / 2;

    /* Don't allow the trigger to be larger than the overall trigger. */
    if (cache->eviction_updates_trigger > cache->eviction_trigger)
        cache->eviction_updates_trigger = cache->eviction_trigger;

    WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
    WT_ASSERT(session, cval.val > 0);
    evict_threads_max = (uint32_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "eviction.threads_min", &cval));
    WT_ASSERT(session, cval.val > 0);
    evict_threads_min = (uint32_t)cval.val;

    if (evict_threads_min > evict_threads_max)
        WT_RET_MSG(
          session, EINVAL, "eviction=(threads_min) cannot be greater than eviction=(threads_max)");
    conn->evict_threads_max = evict_threads_max;
    conn->evict_threads_min = evict_threads_min;

    /* Retrieve the wait time and convert from milliseconds */
    WT_RET(__wt_config_gets(session, cfg, "cache_max_wait_ms", &cval));
    cache->cache_max_wait_us = (uint64_t)(cval.val * WT_THOUSAND);

    return (0);
}

/*
 * __wt_cache_config --
 *     Configure or reconfigure the current cache and shared cache.
 */
int
__wt_cache_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    bool now_shared, was_shared;

    conn = S2C(session);

    WT_ASSERT(session, conn->cache != NULL);

    WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
    now_shared = cval.len != 0;
    was_shared = F_ISSET(conn, WT_CONN_CACHE_POOL);

    /* Cleanup if reconfiguring */
    if (reconfig && was_shared && !now_shared)
        /* Remove ourselves from the pool if necessary */
        WT_RET(__wt_conn_cache_pool_destroy(session));
    else if (reconfig && !was_shared && now_shared)
        /*
         * Cache size will now be managed by the cache pool - the start size always needs to be zero
         * to allow the pool to manage how much memory is in-use.
         */
        conn->cache_size = 0;

    /*
     * Always setup the local cache - it's used even if we are participating in a shared cache.
     */
    WT_RET(__cache_config_local(session, now_shared, cfg));
    if (now_shared) {
        WT_RET(__wt_cache_pool_config(session, cfg));
        WT_ASSERT(session, F_ISSET(conn, WT_CONN_CACHE_POOL));
        if (!was_shared)
            WT_RET(__wt_conn_cache_pool_open(session));
    }

    /*
     * Resize the thread group if reconfiguring, otherwise the thread group will be initialized as
     * part of creating the cache.
     */
    if (reconfig)
        WT_RET(__wt_thread_group_resize(session, &conn->evict_threads, conn->evict_threads_min,
          conn->evict_threads_max, WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL));

    return (0);
}

/*
 * __wt_cache_create --
 *     Create the underlying cache.
 */
int
__wt_cache_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    int i;

    conn = S2C(session);

    WT_ASSERT(session, conn->cache == NULL);

    WT_RET(__wt_calloc_one(session, &conn->cache));

    cache = conn->cache;

    /* Use a common routine for run-time configuration options. */
    WT_RET(__wt_cache_config(session, cfg, false));

    /*
     * The lowest possible page read-generation has a special meaning, it marks a page for forcible
     * eviction; don't let it happen by accident.
     */
    cache->read_gen = cache->read_gen_oldest = WT_READGEN_START_VALUE;

    /*
     * The target size must be lower than the trigger size or we will never get any work done.
     */
    if (cache->eviction_target >= cache->eviction_trigger)
        WT_RET_MSG(session, EINVAL, "eviction target must be lower than the eviction trigger");
    if (cache->eviction_dirty_target >= cache->eviction_dirty_trigger)
        WT_RET_MSG(
          session, EINVAL, "eviction dirty target must be lower than the eviction dirty trigger");
    if (cache->eviction_updates_target >= cache->eviction_updates_trigger)
        WT_RET_MSG(session, EINVAL,
          "eviction updates target must be lower than the eviction updates trigger");

    WT_RET(__wt_cond_auto_alloc(
      session, "cache eviction server", 10000, WT_MILLION, &cache->evict_cond));
    WT_RET(__wt_spin_init(session, &cache->evict_pass_lock, "evict pass"));
    WT_RET(__wt_spin_init(session, &cache->evict_queue_lock, "cache eviction queue"));
    WT_RET(__wt_spin_init(session, &cache->evict_walk_lock, "cache walk"));
    if ((ret = __wt_open_internal_session(
           conn, "evict pass", false, WT_SESSION_NO_DATA_HANDLES, 0, &cache->walk_session)) != 0)
        WT_RET_MSG(NULL, ret, "Failed to create session for eviction walks");

    /* Allocate the LRU eviction queue. */
    cache->evict_slots = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
    for (i = 0; i < WT_EVICT_QUEUE_MAX; ++i) {
        WT_RET(__wt_calloc_def(session, cache->evict_slots, &cache->evict_queues[i].evict_queue));
        WT_RET(__wt_spin_init(session, &cache->evict_queues[i].evict_lock, "cache eviction"));
    }

    /* Ensure there are always non-NULL queues. */
    cache->evict_current_queue = cache->evict_fill_queue = &cache->evict_queues[0];
    cache->evict_other_queue = &cache->evict_queues[1];
    cache->evict_urgent_queue = &cache->evict_queues[WT_EVICT_URGENT_QUEUE];

    /*
     * We get/set some values in the cache statistics (rather than have two copies), configure them.
     */
    __wt_cache_stats_update(session);
    return (0);
}

/*
 * __wt_cache_stats_update --
 *     Update the cache statistics for return to the application.
 */
void
__wt_cache_stats_update(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_STATS **stats;
    uint64_t inuse, intl, leaf;

    conn = S2C(session);
    cache = conn->cache;
    stats = conn->stats;

    inuse = __wt_cache_bytes_inuse(cache);
    intl = __wt_cache_bytes_plus_overhead(cache, cache->bytes_internal);
    /*
     * There are races updating the different cache tracking values so be paranoid calculating the
     * leaf byte usage.
     */
    leaf = inuse > intl ? inuse - intl : 0;

    WT_STAT_SET(session, stats, cache_bytes_max, conn->cache_size);
    WT_STAT_SET(session, stats, cache_bytes_inuse, inuse);
    WT_STAT_SET(session, stats, cache_overhead, cache->overhead_pct);

    WT_STAT_SET(session, stats, cache_bytes_dirty, __wt_cache_dirty_inuse(cache));
    WT_STAT_SET(session, stats, cache_bytes_dirty_total,
      __wt_cache_bytes_plus_overhead(cache, cache->bytes_dirty_total));
    WT_STAT_SET(
      session, stats, cache_bytes_hs, __wt_cache_bytes_plus_overhead(cache, cache->bytes_hs));
    WT_STAT_SET(session, stats, cache_bytes_image, __wt_cache_bytes_image(cache));
    WT_STAT_SET(session, stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
    WT_STAT_SET(session, stats, cache_bytes_internal, intl);
    WT_STAT_SET(session, stats, cache_bytes_leaf, leaf);
    WT_STAT_SET(session, stats, cache_bytes_other, __wt_cache_bytes_other(cache));
    WT_STAT_SET(session, stats, cache_bytes_updates, __wt_cache_bytes_updates(cache));

    WT_STAT_SET(session, stats, cache_eviction_maximum_page_size, cache->evict_max_page_size);
    WT_STAT_SET(
      session, stats, cache_pages_dirty, cache->pages_dirty_intl + cache->pages_dirty_leaf);

    WT_STAT_SET(session, stats, cache_eviction_state, cache->flags);
    WT_STAT_SET(session, stats, cache_eviction_aggressive_set, cache->evict_aggressive_score);
    WT_STAT_SET(session, stats, cache_eviction_empty_score, cache->evict_empty_score);
    WT_STAT_SET(session, stats, cache_hs_score, __wt_cache_hs_score(cache));

    WT_STAT_SET(session, stats, cache_eviction_active_workers, conn->evict_threads.current_threads);
    WT_STAT_SET(
      session, stats, cache_eviction_stable_state_workers, cache->evict_tune_workers_best);

    /*
     * The number of files with active walks ~= number of hazard pointers in the walk session. Note:
     * reading without locking.
     */
    if (conn->evict_server_running)
        WT_STAT_SET(session, stats, cache_eviction_walks_active, cache->walk_session->nhazard);

    WT_STAT_SET(session, stats, rec_maximum_seconds, conn->rec_maximum_seconds);

    /* TODO: WT-5585 Remove lookaside score statistic after MongoDB switches to an alternative. */
    WT_STAT_SET(session, stats, cache_lookaside_score, 0);
}

/*
 * __wt_cache_destroy --
 *     Discard the underlying cache.
 */
int
__wt_cache_destroy(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    int i;

    conn = S2C(session);
    cache = conn->cache;

    if (cache == NULL)
        return (0);

    /* The cache should be empty at this point.  Complain if not. */
    if (cache->pages_inmem != cache->pages_evicted)
        __wt_errx(session,
          "cache server: exiting with %" PRIu64 " pages in memory and %" PRIu64 " pages evicted",
          cache->pages_inmem, cache->pages_evicted);
    if (cache->bytes_image_intl + cache->bytes_image_leaf != 0)
        __wt_errx(session, "cache server: exiting with %" PRIu64 " image bytes in memory",
          cache->bytes_image_intl + cache->bytes_image_leaf);
    if (cache->bytes_inmem != 0)
        __wt_errx(
          session, "cache server: exiting with %" PRIu64 " bytes in memory", cache->bytes_inmem);
    if (cache->bytes_dirty_intl + cache->bytes_dirty_leaf != 0 ||
      cache->pages_dirty_intl + cache->pages_dirty_leaf != 0)
        __wt_errx(session,
          "cache server: exiting with %" PRIu64 " bytes dirty and %" PRIu64 " pages dirty",
          cache->bytes_dirty_intl + cache->bytes_dirty_leaf,
          cache->pages_dirty_intl + cache->pages_dirty_leaf);

    __wt_cond_destroy(session, &cache->evict_cond);
    __wt_spin_destroy(session, &cache->evict_pass_lock);
    __wt_spin_destroy(session, &cache->evict_queue_lock);
    __wt_spin_destroy(session, &cache->evict_walk_lock);
    if (cache->walk_session != NULL)
        WT_TRET(__wt_session_close_internal(cache->walk_session));

    for (i = 0; i < WT_EVICT_QUEUE_MAX; ++i) {
        __wt_spin_destroy(session, &cache->evict_queues[i].evict_lock);
        __wt_free(session, cache->evict_queues[i].evict_queue);
    }

    __wt_free(session, conn->cache);
    return (ret);
}
