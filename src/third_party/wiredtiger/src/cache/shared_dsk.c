/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __shared_dsk_cache_verbose --
 *     Shared dsk cache verbose logging.
 */
static WT_INLINE void
__shared_dsk_cache_verbose(WT_SESSION_IMPL *session, WT_VERBOSE_LEVEL level, const char *tag,
  uint64_t hash, u_int bucket, u_int lock_idx, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_ITEM(tmp);
    const char *addr_string;

    if (!WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_CROSS_CHECKPOINT_CACHE, level))
        return;

    addr_string = __wt_scr_alloc(session, 0, &tmp) == 0 ?
      __wt_addr_string(session, addr, addr_size, tmp) :
      "[unable to format addr]";
    __wt_verbose_level(session, WT_VERB_CROSS_CHECKPOINT_CACHE, level,
      "%s: %s, hash=%" PRIu64 ", bucket=%u, lock_idx=%u", tag, addr_string, hash, bucket, lock_idx);
    __wt_scr_free(session, &tmp);
}

/*
 * __wt_shared_dsk_cache_get --
 *     Get a disk image from the shared disk cache.
 */
void
__wt_shared_dsk_cache_get(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_SHARED_DSK_ITEM **shared_dsk_retp)
{
    WT_SHARED_DSK_CACHE *shared_dsk_cache;
    WT_SHARED_DSK_ITEM *shared_dsk_item;
    uint64_t hash;
    u_int bucket, lock_idx;

    *shared_dsk_retp = NULL;

    shared_dsk_cache = &S2C(session)->cache->shared_dsk_cache;
    WT_ASSERT(session, shared_dsk_cache->enabled);

    hash = __wt_hash_city64(addr, addr_size);
    bucket = hash % shared_dsk_cache->hash_size;
    lock_idx = bucket % shared_dsk_cache->hash_lock_size;
    __wt_spin_lock(session, &shared_dsk_cache->hash_locks[lock_idx]);
    TAILQ_FOREACH (shared_dsk_item, &shared_dsk_cache->hash[bucket], hashq) {
        if (shared_dsk_item->addr_size == addr_size && shared_dsk_item->fid == S2BT(session)->id &&
          memcmp(shared_dsk_item->addr, addr, addr_size) == 0) {
            ++shared_dsk_item->ref_count;
#ifdef HAVE_DIAGNOSTIC
            if (shared_dsk_cache->max_ref_count < shared_dsk_item->ref_count) {
                shared_dsk_cache->max_ref_count = shared_dsk_item->ref_count;
                __wt_verbose_debug2(session, WT_VERB_CROSS_CHECKPOINT_CACHE,
                  "get: new max_ref_count=%" PRId32, shared_dsk_item->ref_count);
            }
#endif
            break;
        }
    }
    __wt_spin_unlock(session, &shared_dsk_cache->hash_locks[lock_idx]);

    if (shared_dsk_item != NULL) {
        *shared_dsk_retp = shared_dsk_item;
        WT_STAT_CONN_INCR(session, cache_shared_dsk_hit);
        __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
          "get: disk image found in shared dsk cache", hash, bucket, lock_idx, addr, addr_size);
    } else {
        WT_STAT_CONN_INCR(session, cache_shared_dsk_miss);
        __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
          "get: disk image not found in shared dsk cache", hash, bucket, lock_idx, addr, addr_size);
    }
}

/*
 * __wt_shared_dsk_cache_put --
 *     Put a disk image into the shared disk cache. 1. On success, ownership of data transfers to
 *     the cache and insertedp is set to true. 2. On collision, the existing entry is returned with
 *     its reference count incremented and insertedp is set to false; the caller retains ownership
 *     of data. 3. On error, the caller retains ownership of data.
 */
int
__wt_shared_dsk_cache_put(WT_SESSION_IMPL *session, void *data, size_t data_size,
  const uint8_t *addr, size_t addr_size, WT_PAGE_BLOCK_META *block_meta,
  WT_SHARED_DSK_ITEM **shared_dsk_retp, bool *insertedp)
{
    WT_DECL_RET;
    WT_SHARED_DSK_CACHE *shared_dsk_cache;
    WT_SHARED_DSK_ITEM *shared_dsk_item, *shared_dsk_store;
    uint64_t hash;
    u_int bucket, lock_idx;
    bool cache_inserted;
#ifdef HAVE_DIAGNOSTIC
    uint32_t bucket_walk = 0;
#endif

    shared_dsk_cache = &S2C(session)->cache->shared_dsk_cache;
    WT_ASSERT(session, shared_dsk_cache->enabled);
    cache_inserted = false;
    shared_dsk_store = NULL;
    *shared_dsk_retp = NULL;

    WT_ASSERT(session, data != NULL);
    WT_ASSERT(session, data_size != 0);
    WT_ASSERT(session, block_meta != NULL);

    WT_ERR(__wt_calloc(session, 1, sizeof(*shared_dsk_store) + addr_size, &shared_dsk_store));
    shared_dsk_store->data = data;
    shared_dsk_store->data_size = WT_STORE_SIZE(data_size);
    shared_dsk_store->block_meta = *block_meta;
    shared_dsk_store->fid = S2BT(session)->id;
    shared_dsk_store->addr_size = (uint8_t)addr_size;
    shared_dsk_store->ref_count = 1;
    memcpy(shared_dsk_store->addr, addr, addr_size);

    hash = __wt_hash_city64(addr, addr_size);
    bucket = hash % shared_dsk_cache->hash_size;
    lock_idx = bucket % shared_dsk_cache->hash_lock_size;

    /*
     * Because collisions are unlikely, the allocation and copying remains outside of the bucket
     * lock and collision check.
     */
    __wt_spin_lock(session, &shared_dsk_cache->hash_locks[lock_idx]);
    TAILQ_FOREACH (shared_dsk_item, &shared_dsk_cache->hash[bucket], hashq) {
#ifdef HAVE_DIAGNOSTIC
        ++bucket_walk;
#endif
        if (shared_dsk_item->addr_size == addr_size && shared_dsk_item->fid == S2BT(session)->id &&
          memcmp(shared_dsk_item->addr, addr, addr_size) == 0) {
            ++shared_dsk_item->ref_count;
            __wt_spin_unlock(session, &shared_dsk_cache->hash_locks[lock_idx]);

            *shared_dsk_retp = shared_dsk_item;
            __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
              "put: disk image already in shared dsk cache", hash, bucket, lock_idx, addr,
              addr_size);
            goto done;
        }
    }
#ifdef HAVE_DIAGNOSTIC
    __wt_verbose_debug2(session, WT_VERB_CROSS_CHECKPOINT_CACHE,
      "put: bucket=%u, walked %" PRIu32 " entries", bucket, bucket_walk);
    if (shared_dsk_cache->max_bucket_walk < bucket_walk) {
        shared_dsk_cache->max_bucket_walk = bucket_walk;
        __wt_verbose_debug2(session, WT_VERB_CROSS_CHECKPOINT_CACHE,
          "put: new max_bucket_walk=%" PRIu32, bucket_walk);
    }
#endif

    TAILQ_INSERT_HEAD(&shared_dsk_cache->hash[bucket], shared_dsk_store, hashq);

    __wt_spin_unlock(session, &shared_dsk_cache->hash_locks[lock_idx]);

    *shared_dsk_retp = shared_dsk_store;
    cache_inserted = true;

    /* Update disk image statistics.*/
    __wt_cache_shared_dsk_inmem_incr(session, ((WT_PAGE_HEADER *)data)->type, data_size);
    __wt_cache_image_incr(session, ((WT_PAGE_HEADER *)data)->type, WT_STORE_SIZE(data_size));

    __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
      "put: disk image inserted in shared dsk cache", hash, bucket, lock_idx, addr, addr_size);
done:
err:
    /*
     * On error or collision, the store wrapper was not inserted. The caller retains ownership of
     * data in both cases. Only free the unused store wrapper.
     */
    if (!cache_inserted)
        __wt_free(session, shared_dsk_store);
    *insertedp = cache_inserted;
    return (ret);
}

/*
 * __wt_shared_dsk_cache_release --
 *     Release a disk image from the shared disk cache, removing it when the reference count reaches
 *     zero.
 */
void
__wt_shared_dsk_cache_release(WT_SESSION_IMPL *session, WT_SHARED_DSK_ITEM *shared_dsk_item)
{
    WT_SHARED_DSK_CACHE *shared_dsk_cache;
    uint64_t hash;
    uint32_t data_size;
    uint8_t dsk_type;
    u_int bucket, lock_idx;

    WT_ASSERT(session, shared_dsk_item != NULL);

    shared_dsk_cache = &S2C(session)->cache->shared_dsk_cache;
    WT_ASSERT(session, shared_dsk_cache->enabled);
    hash = __wt_hash_city64(shared_dsk_item->addr, shared_dsk_item->addr_size);
    bucket = hash % shared_dsk_cache->hash_size;
    lock_idx = bucket % shared_dsk_cache->hash_lock_size;

    __wt_spin_lock(session, &shared_dsk_cache->hash_locks[lock_idx]);
    WT_ASSERT(session, shared_dsk_item->ref_count > 0);
    /* Remove the shared dsk item when ref count is reduced to 0. */
    if (--shared_dsk_item->ref_count == 0) {
        TAILQ_REMOVE(&shared_dsk_cache->hash[bucket], shared_dsk_item, hashq);
        data_size = shared_dsk_item->data_size;
        dsk_type = ((WT_PAGE_HEADER *)shared_dsk_item->data)->type;
        __wt_spin_unlock(session, &shared_dsk_cache->hash_locks[lock_idx]);

        /* Symmetrically drain the cache on last release. */
        __wt_evict_shared_dsk_cache_bytes_decr(session, dsk_type, data_size);
        __wt_cache_image_decr(session, dsk_type, data_size);

        __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
          "release: disk image removed from shared dsk cache", hash, bucket, lock_idx,
          shared_dsk_item->addr, shared_dsk_item->addr_size);
        __wt_overwrite_and_free_len(session, shared_dsk_item->data, shared_dsk_item->data_size);
        __wt_free(session, shared_dsk_item);
    } else {
        __wt_spin_unlock(session, &shared_dsk_cache->hash_locks[lock_idx]);
        __shared_dsk_cache_verbose(session, WT_VERBOSE_DEBUG_2,
          "release: disk image ref decremented in shared dsk cache", hash, bucket, lock_idx,
          shared_dsk_item->addr, shared_dsk_item->addr_size);
    }
}

/*
 * __wti_shared_dsk_cache_init --
 *     Initialize the shared disk cache.
 */
int
__wti_shared_dsk_cache_init(WT_SESSION_IMPL *session, u_int hash_size)
{
    WT_DECL_RET;
    WT_SHARED_DSK_CACHE *shared_dsk_cache;
    u_int i;

    shared_dsk_cache = &S2C(session)->cache->shared_dsk_cache;
    shared_dsk_cache->hash_size = hash_size;
    /* FIXME-WT-17066: We should pick a WT_SHARED_DSK_CACHE_MAX_LOCKS wisely. */
    shared_dsk_cache->hash_lock_size = WT_MIN(hash_size, WT_SHARED_DSK_CACHE_MAX_LOCKS);
#ifdef HAVE_DIAGNOSTIC
    shared_dsk_cache->max_bucket_walk = 0;
    shared_dsk_cache->max_ref_count = 0;
#endif

    WT_ERR(__wt_calloc_def(session, shared_dsk_cache->hash_size, &shared_dsk_cache->hash));
    WT_ERR(
      __wt_calloc_def(session, shared_dsk_cache->hash_lock_size, &shared_dsk_cache->hash_locks));

    for (i = 0; i < shared_dsk_cache->hash_size; i++)
        TAILQ_INIT(&shared_dsk_cache->hash[i]);
    for (i = 0; i < shared_dsk_cache->hash_lock_size; i++)
        WT_ERR(__wt_spin_init(
          session, &shared_dsk_cache->hash_locks[i], "shared disk cache bucket locks"));

    return (0);

err:
    __wti_shared_dsk_cache_destroy(session);
    return (ret);
}

/*
 * __wti_shared_dsk_cache_destroy --
 *     Destroy the shared disk cache and free all memory.
 */
void
__wti_shared_dsk_cache_destroy(WT_SESSION_IMPL *session)
{
    WT_SHARED_DSK_CACHE *shared_dsk_cache;
    WT_SHARED_DSK_ITEM *shared_dsk_item;
    u_int i;

    shared_dsk_cache = &S2C(session)->cache->shared_dsk_cache;

    if (shared_dsk_cache->hash == NULL || shared_dsk_cache->hash_locks == NULL)
        goto done;

    for (i = 0; i < shared_dsk_cache->hash_size; i++) {
        while (!TAILQ_EMPTY(&shared_dsk_cache->hash[i])) {
            shared_dsk_item = TAILQ_FIRST(&shared_dsk_cache->hash[i]);
            TAILQ_REMOVE(&shared_dsk_cache->hash[i], shared_dsk_item, hashq);
            __wt_overwrite_and_free_len(session, shared_dsk_item->data, shared_dsk_item->data_size);
            __wt_free(session, shared_dsk_item);
        }
    }
    for (i = 0; i < shared_dsk_cache->hash_lock_size; i++)
        __wt_spin_destroy(session, &shared_dsk_cache->hash_locks[i]);

done:
    __wt_free(session, shared_dsk_cache->hash);
    __wt_free(session, shared_dsk_cache->hash_locks);
}
