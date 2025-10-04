/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cache_config --
 *     Configure or reconfigure the current cache and shared cache.
 */
int
__wt_cache_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_CACHE *cache;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    bool now_shared, was_shared;

    conn = S2C(session);
    cache = conn->cache;

    WT_ASSERT(session, cache != NULL);

    WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
    now_shared = cval.len != 0;
    was_shared = F_ISSET_ATOMIC_32(conn, WT_CONN_CACHE_POOL);

    /* Cleanup if reconfiguring */
    if (reconfig && was_shared && !now_shared)
        /* Remove ourselves from the pool if necessary */
        WT_RET(__wt_cache_pool_destroy(session));
    else if (reconfig && !was_shared && now_shared)
        /*
         * Cache size will now be managed by the cache pool - the start size always needs to be zero
         * to allow the pool to manage how much memory is in-use.
         */
        conn->cache_size = 0;

    /*
     * If not using a shared cache configure the cache size, otherwise check for a reserved size.
     * All other settings are independent of whether we are using a shared cache or not.
     */
    if (!now_shared) {
        WT_RET(__wt_config_gets(session, cfg, "cache_size", &cval));
        conn->cache_size = (uint64_t)cval.val;
    }
    /* Set config values as percentages. */
    WT_RET(__wt_config_gets(session, cfg, "cache_overhead", &cval));
    cache->overhead_pct = (u_int)cval.val;

    return (0);
}

/*
 * __wt_cache_create --
 *     Create the underlying cache.
 */
int
__wt_cache_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_ASSERT(session, S2C(session)->cache == NULL);
    WT_RET(__wt_calloc_one(session, &S2C(session)->cache));

    /* Use a common routine for run-time configuration options. */
    WT_RET(__wt_cache_config(session, cfg, false));

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
    uint64_t avg_internal_chain, avg_leaf_chain, intl, inuse, leaf;

    conn = S2C(session);
    cache = conn->cache;
    stats = conn->stats;

    inuse = __wt_cache_bytes_inuse(cache);
    intl = __wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_internal));
    /*
     * There are races updating the different cache tracking values so be paranoid calculating the
     * leaf byte usage.
     */
    leaf = inuse > intl ? inuse - intl : 0;

    WT_STATP_CONN_SET(session, stats, cache_bytes_max, conn->cache_size);
    WT_STATP_CONN_SET(session, stats, cache_bytes_inuse, inuse);
    WT_STATP_CONN_SET(session, stats, cache_overhead, cache->overhead_pct);

    WT_STATP_CONN_SET(
      session, stats, cache_bytes_delta_updates, __wt_cache_bytes_delta_updates(cache));
    WT_STATP_CONN_SET(session, stats, cache_bytes_dirty, __wt_cache_dirty_inuse(cache));
    WT_STATP_CONN_SET(session, stats, cache_bytes_dirty_leaf, __wt_cache_dirty_leaf_inuse(cache));
    WT_STATP_CONN_SET(
      session, stats, cache_bytes_dirty_internal, __wt_cache_dirty_intl_inuse(cache));
    WT_STATP_CONN_SET(session, stats, cache_bytes_dirty_total,
      __wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_dirty_total)));
    WT_STATP_CONN_SET(session, stats, cache_bytes_hs,
      __wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_hs)));
    WT_STATP_CONN_SET(session, stats, cache_bytes_hs_dirty,
      __wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_hs_dirty)));
    WT_STATP_CONN_SET(session, stats, cache_bytes_hs_updates,
      __wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_hs_updates)));
    WT_STATP_CONN_SET(session, stats, cache_bytes_image, __wt_cache_bytes_image(cache));
    WT_STATP_CONN_SET(session, stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
    WT_STATP_CONN_SET(session, stats, cache_bytes_internal, intl);
    WT_STATP_CONN_SET(session, stats, cache_bytes_leaf, leaf);
    WT_STATP_CONN_SET(session, stats, cache_bytes_other, __wt_cache_bytes_other(cache));
    WT_STATP_CONN_SET(session, stats, cache_bytes_updates, __wt_cache_bytes_updates(cache));

    WT_STATP_CONN_SET(
      session, stats, cache_pages_dirty, cache->pages_dirty_intl + cache->pages_dirty_leaf);

    WT_STATP_CONN_SET(
      session, stats, rec_maximum_hs_wrapup_milliseconds, conn->rec_maximum_hs_wrapup_milliseconds);
    WT_STATP_CONN_SET(session, stats, rec_maximum_image_build_milliseconds,
      conn->rec_maximum_image_build_milliseconds);
    WT_STATP_CONN_SET(session, stats, rec_maximum_milliseconds, conn->rec_maximum_milliseconds);

    avg_internal_chain = (uint64_t)WT_STAT_CONN_READ(stats, rec_pages_with_internal_deltas) == 0 ?
      0 :
      (uint64_t)WT_STAT_CONN_READ(stats, rec_page_delta_internal) /
        (uint64_t)WT_STAT_CONN_READ(stats, rec_pages_with_internal_deltas);
    avg_leaf_chain = (uint64_t)WT_STAT_CONN_READ(stats, rec_pages_with_leaf_deltas) == 0 ?
      0 :
      (uint64_t)WT_STAT_CONN_READ(stats, rec_page_delta_leaf) /
        (uint64_t)WT_STAT_CONN_READ(stats, rec_pages_with_leaf_deltas);
    WT_STATP_CONN_SET(
      session, stats, rec_average_internal_page_delta_chain_length, avg_internal_chain);
    WT_STATP_CONN_SET(session, stats, rec_average_leaf_page_delta_chain_length, avg_leaf_chain);

    WT_STATP_CONN_SET(
      session, stats, rec_max_internal_page_deltas, conn->page_delta.max_internal_delta_count);
    WT_STATP_CONN_SET(
      session, stats, rec_max_leaf_page_deltas, conn->page_delta.max_leaf_delta_count);
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

    conn = S2C(session);
    cache = conn->cache;

    if (cache == NULL)
        return (0);

    /* The cache should be empty at this point.  Complain if not. */
    if (cache->pages_inmem != cache->pages_evicted)
        __wt_errx(session,
          "cache server: exiting with %" PRIu64 " pages in memory and %" PRIu64 " pages evicted",
          cache->pages_inmem, cache->pages_evicted);
    if ((__wt_atomic_load64(&cache->bytes_image_intl) +
          __wt_atomic_load64(&cache->bytes_image_leaf)) != 0)
        __wt_errx(session, "cache server: exiting with %" PRIu64 " image bytes in memory",
          __wt_atomic_load64(&cache->bytes_image_intl) +
            __wt_atomic_load64(&cache->bytes_image_leaf));
    if (__wt_atomic_load64(&cache->bytes_inmem) != 0)
        __wt_errx(session, "cache server: exiting with %" PRIu64 " bytes in memory",
          __wt_atomic_load64(&cache->bytes_inmem));
    if ((__wt_atomic_load64(&cache->bytes_dirty_intl) +
          __wt_atomic_load64(&cache->bytes_dirty_leaf)) != 0 ||
      cache->pages_dirty_intl + cache->pages_dirty_leaf != 0)
        __wt_errx(session,
          "cache server: exiting with %" PRIu64 " bytes dirty and %" PRIu64 " pages dirty",
          __wt_atomic_load64(&cache->bytes_dirty_intl) +
            __wt_atomic_load64(&cache->bytes_dirty_leaf),
          cache->pages_dirty_intl + cache->pages_dirty_leaf);

    __wt_free(session, conn->cache);
    return (0);
}
