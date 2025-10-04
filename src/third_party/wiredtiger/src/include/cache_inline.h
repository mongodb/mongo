/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_cache_pages_inuse --
 *     Return the number of pages in use.
 */
static WT_INLINE uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
    return (cache->pages_inmem - cache->pages_evicted);
}

/*
 * __wt_cache_bytes_plus_overhead --
 *     Apply the cache overhead to a size in bytes.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_plus_overhead(WT_CACHE *cache, uint64_t sz)
{
    if (cache->overhead_pct != 0)
        sz += (sz * (uint64_t)cache->overhead_pct) / 100;

    return (sz);
}

/*
 * __wt_cache_bytes_inuse --
 *     Return the number of bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_inmem)));
}

/*
 * __wt_cache_dirty_inuse --
 *     Return the number of dirty bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_inuse(WT_CACHE *cache)
{
    uint64_t dirty_inuse;
    dirty_inuse =
      __wt_atomic_load64(&cache->bytes_dirty_intl) + __wt_atomic_load64(&cache->bytes_dirty_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&dirty_inuse)));
}

/*
 * __wt_cache_dirty_intl_inuse --
 *     Return the number of dirty bytes in use by internal pages.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_intl_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_dirty_intl)));
}

/*
 * __wt_cache_dirty_leaf_inuse --
 *     Return the number of dirty bytes in use by leaf pages.
 */
static WT_INLINE uint64_t
__wt_cache_dirty_leaf_inuse(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_dirty_leaf)));
}

/*
 * __wt_cache_bytes_updates --
 *     Return the number of bytes in use for updates.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_updates(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_updates)));
}

/*
 * __wt_cache_bytes_delta_updates --
 *     Return the number of bytes in use for delta updates.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_delta_updates(WT_CACHE *cache)
{
    return (__wt_cache_bytes_plus_overhead(cache, __wt_atomic_load64(&cache->bytes_delta_updates)));
}

/*
 * __wt_cache_bytes_image --
 *     Return the number of page image bytes in use.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_image(WT_CACHE *cache)
{
    uint64_t bytes_image;
    bytes_image =
      __wt_atomic_load64(&cache->bytes_image_intl) + __wt_atomic_load64(&cache->bytes_image_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, bytes_image));
}

/*
 * __wt_cache_bytes_other --
 *     Return the number of bytes in use not for page images.
 */
static WT_INLINE uint64_t
__wt_cache_bytes_other(WT_CACHE *cache)
{
    uint64_t bytes_other, bytes_inmem, bytes_image_intl, bytes_image_leaf;

    bytes_inmem = __wt_atomic_load64(&cache->bytes_inmem);
    bytes_image_intl = __wt_atomic_load64(&cache->bytes_image_intl);
    bytes_image_leaf = __wt_atomic_load64(&cache->bytes_image_leaf);
    /*
     * Reads can race with changes to the values, so check that the calculation doesn't go negative.
     */
    bytes_other = __wt_safe_sub(bytes_inmem, bytes_image_intl + bytes_image_leaf);
    return (__wt_cache_bytes_plus_overhead(cache, bytes_other));
}

/*
 * __wt_session_can_wait --
 *     Return if a session available for a potentially slow operation.
 */
static WT_INLINE bool
__wt_session_can_wait(WT_SESSION_IMPL *session)
{
    /*
     * Return if a session available for a potentially slow operation; for example, used by the
     * block manager in the case of flushing the system cache.
     */
    if (!F_ISSET(session, WT_SESSION_CAN_WAIT))
        return (false);

    /*
     * Don't block to perform slow operations for sessions that set the "ignore cache size" flag, or
     * when holding the schema lock.
     */
    return (!(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE) ||
      FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA)));
}

/*
 * __wt_cache_full --
 *     Return if the cache is at (or over) capacity.
 */
static WT_INLINE bool
__wt_cache_full(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    cache = conn->cache;

    return (__wt_cache_bytes_inuse(cache) >= conn->cache_size);
}
