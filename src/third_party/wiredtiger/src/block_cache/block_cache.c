/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __blkcache_verbose --
 *     Block cache verbose logging.
 */
static inline void
__blkcache_verbose(
  WT_SESSION_IMPL *session, const char *tag, uint64_t hash, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_ITEM(tmp);
    const char *addr_string;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_BLKCACHE))
        return;

    /*
     * Complicate the error handling so we don't have to return a value from this function, it
     * simplifies error handling in our callers.
     */
    addr_string = __wt_scr_alloc(session, 0, &tmp) == 0 ?
      __wt_addr_string(session, addr, addr_size, tmp) :
      "[unable to format addr]";
    __wt_verbose(session, WT_VERB_BLKCACHE, "%s: %s, hash=%" PRIu64, tag, addr_string, hash);
    __wt_scr_free(session, &tmp);
}

/*
 * __blkcache_alloc --
 *     Allocate a block of memory in the cache.
 */
static int
__blkcache_alloc(WT_SESSION_IMPL *session, size_t size, void **retp)
{
    WT_BLKCACHE *blkcache;

    *(void **)retp = NULL;

    blkcache = &S2C(session)->blkcache;

    if (blkcache->type == BLKCACHE_DRAM)
        return (__wt_malloc(session, size, retp));
    else if (blkcache->type == BLKCACHE_NVRAM) {
#ifdef HAVE_LIBMEMKIND
        *retp = memkind_malloc(blkcache->pmem_kind, size);
#else
        WT_RET_MSG(session, EINVAL, "NVRAM block cache type requires libmemkind");
#endif
    }
    return (0);
}

/*
 * __blkcache_free --
 *     Free a chunk of memory.
 */
static void
__blkcache_free(WT_SESSION_IMPL *session, void *ptr)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;

    if (blkcache->type == BLKCACHE_DRAM)
        __wt_free(session, ptr);
    else if (blkcache->type == BLKCACHE_NVRAM) {
#ifdef HAVE_LIBMEMKIND
        memkind_free(blkcache->pmem_kind, ptr);
#else
        __wt_err(session, EINVAL, "NVRAM block cache type requires libmemkind");
#endif
    }
}

/*
 * __blkcache_update_ref_histogram --
 *     Update the histogram of block accesses when the block is freed or on exit.
 */
static void
__blkcache_update_ref_histogram(WT_SESSION_IMPL *session, WT_BLKCACHE_ITEM *blkcache_item, int type)
{
    WT_BLKCACHE *blkcache;
    u_int bucket;

    blkcache = &S2C(session)->blkcache;

    bucket = blkcache_item->num_references / BLKCACHE_HIST_BOUNDARY;
    if (bucket > BLKCACHE_HIST_BUCKETS - 1)
        bucket = BLKCACHE_HIST_BUCKETS - 1;

    blkcache->cache_references[bucket]++;

    if (type == BLKCACHE_RM_FREE)
        blkcache->cache_references_removed_blocks[bucket]++;
    else if (type == BLKCACHE_RM_EVICTION)
        blkcache->cache_references_evicted_blocks[bucket]++;
}

/*
 * __blkcache_print_reference_hist --
 *     Print a histogram showing how a type of block given in the header is reused.
 */
static void
__blkcache_print_reference_hist(WT_SESSION_IMPL *session, const char *header, uint32_t *hist)
{
    int j;

    __wt_verbose(session, WT_VERB_BLKCACHE, "%s:", header);
    __wt_verbose(session, WT_VERB_BLKCACHE, "%s", "Reuses \t Number of blocks");
    __wt_verbose(session, WT_VERB_BLKCACHE, "%s", "-----------------------------");
    for (j = 0; j < BLKCACHE_HIST_BUCKETS; j++) {
        __wt_verbose(session, WT_VERB_BLKCACHE, "[%d - %d] \t %u", j * BLKCACHE_HIST_BOUNDARY,
          (j + 1) * BLKCACHE_HIST_BOUNDARY, hist[j]);
    }
}

/*
 * __blkcache_high_overhead --
 *     Estimate the overhead of using the cache. The overhead comes from block insertions and
 *     removals, which produce writes. Writes disproportionally slow down the reads on Optane NVRAM.
 */
static inline bool
__blkcache_high_overhead(WT_SESSION_IMPL *session)
{
    WT_BLKCACHE *blkcache;
    uint64_t ops;

    blkcache = &S2C(session)->blkcache;

    ops = blkcache->inserts + blkcache->removals;
    return (blkcache->lookups == 0 || ((ops * 100) / blkcache->lookups) > blkcache->overhead_pct);
}

/*
 * __blkcache_should_evict --
 *     Decide if the block should be evicted.
 */
static bool
__blkcache_should_evict(WT_SESSION_IMPL *session, WT_BLKCACHE_ITEM *blkcache_item, int *reason)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;
    *reason = BLKCACHE_EVICT_OTHER;

    /* Blocks in use cannot be evicted. */
    if (blkcache_item->ref_count != 0)
        return (false);

    /*
     * Keep track of the smallest number of references for blocks that have not been recently
     * accessed.
     */
    if ((blkcache_item->freq_rec_counter < blkcache->evict_aggressive) &&
      (blkcache_item->num_references < blkcache->min_num_references))
        blkcache->min_num_references = blkcache_item->num_references;

    /* Don't evict if there is plenty of free space */
    if (blkcache->bytes_used < blkcache->full_target)
        return (false);

    /*
     * In an NVRAM cache, don't evict if there is high overhead due to blocks being
     * inserted/removed. Churn kills performance and evicting when churn is high will exacerbate the
     * overhead.
     */
    if (blkcache->type == BLKCACHE_NVRAM && __blkcache_high_overhead(session)) {
        WT_STAT_CONN_INCR(session, block_cache_not_evicted_overhead);
        return (false);
    }

    /*
     * Evict if the block has not been accessed for the amount of time corresponding to the evict
     * aggressive setting. Within the category of blocks that fit this criterion, choose those with
     * the lowest number of accesses first.
     */
    if (blkcache_item->freq_rec_counter < blkcache->evict_aggressive &&
      blkcache_item->num_references < (blkcache->min_num_references + BLKCACHE_MINREF_INCREMENT))
        return (true);

    *reason = BLKCACHE_NOT_EVICTION_CANDIDATE;
    return (false);
}

/*
 * __blkcache_eviction_thread --
 *     Periodically sweep the cache and evict unused blocks.
 */
static WT_THREAD_RET
__blkcache_eviction_thread(void *arg)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item, *blkcache_item_tmp;
    WT_SESSION_IMPL *session;
    int i, reason;
    bool no_eviction_candidates;

    session = (WT_SESSION_IMPL *)arg;
    blkcache = &S2C(session)->blkcache;

    __wt_verbose(session, WT_VERB_BLKCACHE,
      "Block cache eviction thread starting... "
      "Aggressive target = %d, full target = %" PRIu64 ":",
      blkcache->evict_aggressive, blkcache->full_target);

    while (!blkcache->blkcache_exiting) {
        /*
         * Sweep the cache every second to ensure time-based decay of frequency/recency counters of
         * resident blocks.
         */
        __wt_sleep(1, 0);

        /* Check if the cache is being destroyed */
        if (blkcache->blkcache_exiting)
            return (WT_THREAD_RET_VALUE);

        /*
         * Walk the cache, gathering statistics and evicting blocks that are within our target. We
         * sweep the cache every second, decrementing the frequency/recency counter of each block.
         * Blocks whose counter goes below the threshold will get evicted. The threshold is set
         * according to how soon we expect the blocks to become irrelevant. For example, if the
         * threshold is set to 1800 seconds (=30 minutes), blocks that were used once but then
         * weren't referenced for 30 minutes will be evicted. Blocks that were referenced a lot in
         * the past but weren't referenced in the past 30 minutes will stay in the cache a bit
         * longer, until their frequency/recency counter drops below the threshold.
         *
         * As we cycle through the blocks, we keep track of the minimum number of references
         * observed for blocks whose frequency/recency counter has gone below the threshold. We will
         * evict blocks with the smallest counter before evicting those with a larger one.
         */
        no_eviction_candidates = true;
        for (i = 0; i < (int)blkcache->hash_size; i++) {
            __wt_spin_lock(session, &blkcache->hash_locks[i]);
            TAILQ_FOREACH_SAFE(blkcache_item, &blkcache->hash[i], hashq, blkcache_item_tmp)
            {
                if (__blkcache_should_evict(session, blkcache_item, &reason)) {
                    TAILQ_REMOVE(&blkcache->hash[i], blkcache_item, hashq);
                    __blkcache_free(session, blkcache_item->data);
                    __blkcache_update_ref_histogram(session, blkcache_item, BLKCACHE_RM_EVICTION);
                    (void)__wt_atomic_sub64(&blkcache->bytes_used, blkcache_item->data_size);

                    /*
                     * Update the number of removals because it is used to estimate the overhead,
                     * and we want the overhead contributed by eviction to be part of that
                     * calculation.
                     */
                    blkcache->removals++;

                    WT_STAT_CONN_INCR(session, block_cache_blocks_evicted);
                    WT_STAT_CONN_DECRV(session, block_cache_bytes, blkcache_item->data_size);
                    WT_STAT_CONN_DECR(session, block_cache_blocks);
                    __wt_free(session, blkcache_item);
                } else {
                    blkcache_item->freq_rec_counter--;
                    if (reason != BLKCACHE_NOT_EVICTION_CANDIDATE)
                        no_eviction_candidates = false;
                }
            }
            __wt_spin_unlock(session, &blkcache->hash_locks[i]);
            if (blkcache->blkcache_exiting)
                return (WT_THREAD_RET_VALUE);
        }
        if (no_eviction_candidates)
            blkcache->min_num_references += BLKCACHE_MINREF_INCREMENT;

        WT_STAT_CONN_INCR(session, block_cache_eviction_passes);
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __blkcache_estimate_filesize --
 *     Estimate the size of files used by this workload.
 */
static size_t
__blkcache_estimate_filesize(WT_SESSION_IMPL *session)
{
    WT_BLKCACHE *blkcache;
    WT_BLOCK *block;
    WT_CONNECTION_IMPL *conn;
    size_t size;
    uint64_t bucket;

    conn = S2C(session);
    blkcache = &conn->blkcache;

    /* This is a deliberate race condition */
    if (blkcache->refs_since_filesize_estimated++ < BLKCACHE_FILESIZE_EST_FREQ)
        return (blkcache->estimated_file_size);

    blkcache->refs_since_filesize_estimated = 0;

    size = 0;
    __wt_spin_lock(session, &conn->block_lock);
    for (bucket = 0; bucket < conn->hash_size; bucket++) {
        TAILQ_FOREACH (block, &conn->blockhash[bucket], hashq) {
            size += (size_t)block->size;
        }
    }
    blkcache->estimated_file_size = size;
    __wt_spin_unlock(session, &conn->block_lock);

    WT_STAT_CONN_SET(session, block_cache_bypass_filesize, blkcache->estimated_file_size);

    return (blkcache->estimated_file_size);
}

/*
 * __wt_blkcache_get --
 *     Get a block from the cache.
 */
void
__wt_blkcache_get(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_BLKCACHE_ITEM **blkcache_retp, bool *foundp, bool *skip_cache_putp)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item;
    uint64_t bucket, hash;

    *foundp = *skip_cache_putp = false;
    *blkcache_retp = NULL;

    blkcache = &S2C(session)->blkcache;

    WT_STAT_CONN_INCR(session, block_cache_lookups);

    /*
     * In an NVRAM cache, we track lookups to calculate the overhead of using the cache. We race to
     * avoid using synchronization. We only care about an approximate value, so we accept inaccuracy
     * for the sake of avoiding synchronization on the critical path.
     */
    if (blkcache->type == BLKCACHE_NVRAM)
        blkcache->lookups++;

    /*
     * An NVRAM cache is slower than retrieving the block from the OS buffer cache, a DRAM cache is
     * faster than the OS buffer cache. In the case of NVRAM, if more than the configured fraction
     * of all file objects is likely to fit in the OS buffer cache, don't use the block cache.
     */
    if (blkcache->type == BLKCACHE_NVRAM &&
      (__blkcache_estimate_filesize(session) * blkcache->percent_file_in_os_cache) / 100 <
        blkcache->system_ram) {
        *skip_cache_putp = true;
        WT_STAT_CONN_INCR(session, block_cache_bypass_get);
        return;
    }

    hash = __wt_hash_city64(addr, addr_size);
    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);
    TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq) {
        if (blkcache_item->addr_size == addr_size && blkcache_item->fid == S2BT(session)->id &&
          memcmp(blkcache_item->addr, addr, addr_size) == 0) {
            blkcache_item->num_references++;
            if (blkcache_item->freq_rec_counter < 0)
                blkcache_item->freq_rec_counter = 0;
            blkcache_item->freq_rec_counter++;
            (void)__wt_atomic_addv32(&blkcache_item->ref_count, 1);
            break;
        }
    }
    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);

    if (blkcache_item != NULL) {
        *blkcache_retp = blkcache_item;
        *foundp = *skip_cache_putp = true;
        WT_STAT_CONN_INCR(session, block_cache_hits);
        __blkcache_verbose(session, "block found in cache", hash, addr, addr_size);
    } else {
        WT_STAT_CONN_INCR(session, block_cache_misses);
        __blkcache_verbose(session, "block not found in cache", hash, addr, addr_size);
    }
}

/*
 * __wt_blkcache_put --
 *     Put a block into the cache.
 */
int
__wt_blkcache_put(
  WT_SESSION_IMPL *session, WT_ITEM *data, const uint8_t *addr, size_t addr_size, bool write)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item, *blkcache_store;
    WT_DECL_RET;
    uint64_t bucket, hash;
    void *data_ptr;

    blkcache = &S2C(session)->blkcache;
    blkcache_store = NULL;

    /* Are we within cache size limits? */
    if (blkcache->bytes_used > blkcache->max_bytes)
        return (0);

    /*
     * An NVRAM cache is slower than retrieving the block from the OS buffer cache, a DRAM cache is
     * faster than the OS buffer cache. In the case of NVRAM, if more than the configured fraction
     * of all file objects is likely to fit in the OS buffer cache, don't use the block cache.
     */
    if (blkcache->type == BLKCACHE_NVRAM &&
      (__blkcache_estimate_filesize(session) * blkcache->percent_file_in_os_cache) / 100 <
        blkcache->system_ram) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_put);
        return (0);
    }

    /* Bypass on high overhead */
    if (blkcache->type == BLKCACHE_NVRAM && __blkcache_high_overhead(session)) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_overhead_put);
        return (0);
    }

    /*
     * Allocate and initialize space in the cache outside of the critical section. In the unlikely
     * event that we fail an allocation, free the space. NVRAM allocations can fail if there's no
     * available memory, treat it as a cache-full failure.
     */
    WT_RET(__blkcache_alloc(session, data->size, &data_ptr));
    if (data_ptr == NULL)
        return (0);
    WT_ERR(__wt_calloc(session, 1, sizeof(*blkcache_store) + addr_size, &blkcache_store));
    blkcache_store->data = data_ptr;
    blkcache_store->data_size = WT_STORE_SIZE(data->size);
    memcpy(blkcache_store->data, data->data, data->size);
    blkcache_store->fid = S2BT(session)->id;
    blkcache_store->addr_size = (uint8_t)addr_size;
    memcpy(blkcache_store->addr, addr, addr_size);

    hash = __wt_hash_city64(addr, addr_size);
    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);

    /*
     * In the case of a read, check if the block is already in the cache: it's possible because two
     * readers can attempt to cache the same overflow block because overflow blocks aren't cached at
     * the btree level. Collisions are relatively unlikely because other page types are cached at
     * higher levels and reads of those tree pages are single-threaded so the page can be converted
     * to its in-memory form before reader access. In summary, because collisions are unlikely, the
     * allocation and copying remains outside of the bucket lock and collision check. Writing a
     * block is single-threaded at a higher level, and as there should never be a collision, only
     * check in diagnostic mode.
     */
#if !defined(HAVE_DIAGNOSTIC)
    if (!write)
#endif
        TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq)
            if (blkcache_item->addr_size == addr_size && blkcache_item->fid == S2BT(session)->id &&
              memcmp(blkcache_item->addr, addr, addr_size) == 0) {
                __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
                WT_ASSERT(session, !write);

                WT_STAT_CONN_INCRV(session, block_cache_bytes_update, data->size);
                WT_STAT_CONN_INCR(session, block_cache_blocks_update);
                __blkcache_verbose(session, "block already in cache", hash, addr, addr_size);
                goto err;
            }

    /*
     * Set the recency timestamp on newly inserted blocks to the maximum value to reduce the chance
     * of them being evicted before they are reused.
     */
    blkcache_store->freq_rec_counter = 1;

    TAILQ_INSERT_HEAD(&blkcache->hash[bucket], blkcache_store, hashq);

    (void)__wt_atomic_add64(&blkcache->bytes_used, data->size);
    blkcache->inserts++;

    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);

    WT_STAT_CONN_INCRV(session, block_cache_bytes, data->size);
    WT_STAT_CONN_INCR(session, block_cache_blocks);
    if (write) {
        WT_STAT_CONN_INCRV(session, block_cache_bytes_insert_write, data->size);
        WT_STAT_CONN_INCR(session, block_cache_blocks_insert_write);
    } else {
        WT_STAT_CONN_INCRV(session, block_cache_bytes_insert_read, data->size);
        WT_STAT_CONN_INCR(session, block_cache_blocks_insert_read);
    }

    __blkcache_verbose(session, "block inserted in cache", hash, addr, addr_size);
    return (0);

err:
    __blkcache_free(session, data_ptr);
    __blkcache_free(session, blkcache_store);
    return (ret);
}

/*
 * __wt_blkcache_remove --
 *     Remove a block from the cache.
 */
void
__wt_blkcache_remove(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item;
    uint64_t bucket, hash;

    blkcache = &S2C(session)->blkcache;

    hash = __wt_hash_city64(addr, addr_size);
    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);
    TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq) {
        if (blkcache_item->addr_size == addr_size && blkcache_item->fid == S2BT(session)->id &&
          memcmp(blkcache_item->addr, addr, addr_size) == 0) {
            TAILQ_REMOVE(&blkcache->hash[bucket], blkcache_item, hashq);
            __blkcache_update_ref_histogram(session, blkcache_item, BLKCACHE_RM_FREE);
            __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
            WT_STAT_CONN_DECRV(session, block_cache_bytes, blkcache_item->data_size);
            WT_STAT_CONN_DECR(session, block_cache_blocks);
            WT_STAT_CONN_INCR(session, block_cache_blocks_removed);
            (void)__wt_atomic_sub64(&blkcache->bytes_used, blkcache_item->data_size);
            blkcache->removals++;
            WT_ASSERT(session, blkcache_item->ref_count == 0);
            __blkcache_free(session, blkcache_item->data);
            __wt_overwrite_and_free(session, blkcache_item);
            __blkcache_verbose(session, "block removed from cache", hash, addr, addr_size);
            return;
        }
    }
    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
}

/*
 * __blkcache_init --
 *     Initialize the block cache.
 */
static int
__blkcache_init(WT_SESSION_IMPL *session, size_t cache_size, u_int hash_size, u_int type,
  char *nvram_device_path, size_t system_ram, u_int percent_file_in_os_cache, bool cache_on_writes,
  u_int overhead_pct, u_int evict_aggressive, uint64_t full_target, bool cache_on_checkpoint)
{
    WT_BLKCACHE *blkcache;
    WT_DECL_RET;
    uint64_t i;

    blkcache = &S2C(session)->blkcache;
    blkcache->cache_on_checkpoint = cache_on_checkpoint;
    blkcache->cache_on_writes = cache_on_writes;
    blkcache->hash_size = hash_size;
    blkcache->percent_file_in_os_cache = percent_file_in_os_cache;
    blkcache->full_target = full_target;
    blkcache->max_bytes = cache_size;
    blkcache->overhead_pct = overhead_pct;
    blkcache->system_ram = system_ram;

    if (type == BLKCACHE_NVRAM) {
#ifdef HAVE_LIBMEMKIND
        if ((ret = memkind_create_pmem(nvram_device_path, 0, &blkcache->pmem_kind)) != 0)
            WT_RET_MSG(session, ret, "block cache failed to initialize: memkind_create_pmem");

        WT_RET(__wt_strndup(
          session, nvram_device_path, strlen(nvram_device_path), &blkcache->nvram_device_path));
        __wt_free(session, nvram_device_path);
#else
        (void)nvram_device_path;
        WT_RET_MSG(session, EINVAL, "NVRAM block cache type requires libmemkind");
#endif
    }

    WT_RET(__wt_calloc_def(session, blkcache->hash_size, &blkcache->hash));
    WT_RET(__wt_calloc_def(session, blkcache->hash_size, &blkcache->hash_locks));

    for (i = 0; i < blkcache->hash_size; i++) {
        TAILQ_INIT(&blkcache->hash[i]); /* Block cache hash lists */
        WT_RET(__wt_spin_init(session, &blkcache->hash_locks[i], "block cache bucket locks"));
    }

    /* Create the eviction thread */
    WT_RET(__wt_thread_create(
      session, &blkcache->evict_thread_tid, __blkcache_eviction_thread, (void *)session));
    blkcache->evict_aggressive = -((int)evict_aggressive);
    blkcache->min_num_references = 1000; /* initialize to a large value */

    blkcache->type = type;

    __wt_verbose(session, WT_VERB_BLKCACHE,
      "block cache initialized: type=%s, size=%" WT_SIZET_FMT " path=%s",
      (type == BLKCACHE_NVRAM) ? "nvram" : (type == BLKCACHE_DRAM) ? "dram" : "unconfigured",
      cache_size, (blkcache->nvram_device_path == NULL) ? "--" : blkcache->nvram_device_path);

    return (ret);
}

/*
 * __wt_blkcache_destroy --
 *     Destroy the block cache and free all memory.
 */
void
__wt_blkcache_destroy(WT_SESSION_IMPL *session)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ITEM *blkcache_item;
    WT_DECL_RET;
    uint64_t i;

    blkcache = &S2C(session)->blkcache;

    __wt_verbose(session, WT_VERB_BLKCACHE,
      "block cache with %" PRIu64 " bytes used to be destroyed", blkcache->bytes_used);

    if (blkcache->type == BLKCACHE_UNCONFIGURED)
        return;

    blkcache->blkcache_exiting = true;
    WT_TRET(__wt_thread_join(session, &blkcache->evict_thread_tid));
    __wt_verbose(session, WT_VERB_BLKCACHE, "%s", "block cache eviction thread exited");

    for (i = 0; i < blkcache->hash_size; i++) {
        __wt_spin_lock(session, &blkcache->hash_locks[i]);
        while (!TAILQ_EMPTY(&blkcache->hash[i])) {
            blkcache_item = TAILQ_FIRST(&blkcache->hash[i]);
            TAILQ_REMOVE(&blkcache->hash[i], blkcache_item, hashq);

            /* Assert we never left a block pinned. */
            if (blkcache_item->ref_count != 0)
                __wt_err(session, EINVAL,
                  "block cache reference count of %" PRIu32 " not zero on destroy",
                  blkcache_item->ref_count);

            /*
             * Some workloads crash on freeing NVRAM arenas. If that occurs the call to free can be
             * removed and the library/OS will clean up for us once the process exits.
             */
            __blkcache_free(session, blkcache_item->data);
            __blkcache_update_ref_histogram(session, blkcache_item, BLKCACHE_RM_EXIT);
            (void)__wt_atomic_sub64(&blkcache->bytes_used, blkcache_item->data_size);
            __wt_free(session, blkcache_item);
        }
        __wt_spin_unlock(session, &blkcache->hash_locks[i]);
    }
    WT_ASSERT(session, blkcache->bytes_used == 0);

    /* Print reference histograms */
    __blkcache_print_reference_hist(session, "All blocks", blkcache->cache_references);
    __blkcache_print_reference_hist(
      session, "Removed blocks", blkcache->cache_references_removed_blocks);
    __blkcache_print_reference_hist(
      session, "Evicted blocks", blkcache->cache_references_evicted_blocks);

#ifdef HAVE_LIBMEMKIND
    if (blkcache->type == BLKCACHE_NVRAM) {
        memkind_destroy_kind(blkcache->pmem_kind);
        __wt_free(session, blkcache->nvram_device_path);
    }
#endif
    __wt_free(session, blkcache->hash);
    __wt_free(session, blkcache->hash_locks);
    /*
     * Zeroing the structure has the effect of setting the block cache type to unconfigured.
     */
    memset((void *)blkcache, 0, sizeof(WT_BLKCACHE));
}

/*
 * __blkcache_reconfig --
 *     We currently disallow reconfiguration. If and when we do, this function will destroy the
 *     block cache, making it ready for clean initialization.
 */
static int
__blkcache_reconfig(WT_SESSION_IMPL *session, bool reconfig, size_t cache_size, size_t hash_size,
  u_int type, char *nvram_device_path, size_t system_ram, u_int percent_file_in_os_cache,
  bool cache_on_writes, u_int overhead_pct, u_int evict_aggressive, uint64_t full_target,
  bool cache_on_checkpoint)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;

    if (!reconfig || blkcache->type == BLKCACHE_UNCONFIGURED)
        return (0);

    if (blkcache->cache_on_checkpoint != cache_on_checkpoint ||
      blkcache->cache_on_writes != cache_on_writes || blkcache->hash_size != hash_size ||
      blkcache->percent_file_in_os_cache != percent_file_in_os_cache ||
      blkcache->full_target != full_target || blkcache->max_bytes != cache_size ||
      blkcache->overhead_pct != overhead_pct || blkcache->system_ram != system_ram ||
      blkcache->evict_aggressive != -((int)evict_aggressive) || blkcache->type != type ||
      (nvram_device_path != NULL && blkcache->nvram_device_path == NULL) ||
      (nvram_device_path == NULL && blkcache->nvram_device_path != NULL) ||
      (nvram_device_path != NULL && blkcache->nvram_device_path != NULL &&
        (strlen(nvram_device_path) != strlen(blkcache->nvram_device_path))) ||
      (nvram_device_path != NULL &&
        strncmp(nvram_device_path, blkcache->nvram_device_path, strlen(nvram_device_path)) != 0)) {
        __wt_err(session, EINVAL, "block cache reconfiguration not supported");
        return (WT_ERROR);
    }
    return (0);
}

/*
 * __wt_blkcache_setup --
 *     Set up the block cache.
 */
int
__wt_blkcache_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_BLKCACHE *blkcache;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint64_t cache_size, full_target, system_ram;
    u_int cache_type, evict_aggressive, hash_size, overhead_pct, percent_file_in_os_cache;
    char *nvram_device_path;
    bool cache_on_checkpoint, cache_on_writes;

    blkcache = &S2C(session)->blkcache;
    cache_on_checkpoint = cache_on_writes = true;
    nvram_device_path = (char *)"";

    if (blkcache->type != BLKCACHE_UNCONFIGURED && !reconfig)
        WT_RET_MSG(session, EINVAL, "block cache setup requested for a configured cache");

    /* When reconfiguring, check if there are any modifications that we care about. */
    if (blkcache->type != BLKCACHE_UNCONFIGURED && reconfig) {
        if ((ret = __wt_config_gets(session, cfg + 1, "block_cache", &cval)) == WT_NOTFOUND)
            return (0);
        WT_RET(ret);
    }

    WT_RET(__wt_config_gets(session, cfg, "block_cache.enabled", &cval));
    if (cval.val == 0)
        return (0);

    WT_RET(__wt_config_gets(session, cfg, "block_cache.size", &cval));
    if ((cache_size = (uint64_t)cval.val) <= 0)
        WT_RET_MSG(session, EINVAL, "block cache size must be greater than zero");

    WT_RET(__wt_config_gets(session, cfg, "block_cache.hashsize", &cval));
    if ((hash_size = (u_int)cval.val) == 0)
        hash_size = BLKCACHE_HASHSIZE_DEFAULT;
    else if (hash_size < BLKCACHE_HASHSIZE_MIN || hash_size > BLKCACHE_HASHSIZE_MAX)
        WT_RET_MSG(session, EINVAL, "block cache hash size must be between %d and %d entries",
          BLKCACHE_HASHSIZE_MIN, BLKCACHE_HASHSIZE_MAX);

    WT_RET(__wt_config_gets(session, cfg, "block_cache.type", &cval));
    if (WT_STRING_MATCH("dram", cval.str, cval.len) || WT_STRING_MATCH("DRAM", cval.str, cval.len))
        cache_type = BLKCACHE_DRAM;
    else if (WT_STRING_MATCH("nvram", cval.str, cval.len) ||
      WT_STRING_MATCH("NVRAM", cval.str, cval.len)) {
#ifdef HAVE_LIBMEMKIND
        cache_type = BLKCACHE_NVRAM;
        WT_RET(__wt_config_gets(session, cfg, "block_cache.nvram_path", &cval));
        WT_RET(__wt_strndup(session, cval.str, cval.len, &nvram_device_path));
        if (!__wt_absolute_path(nvram_device_path))
            WT_RET_MSG(session, EINVAL, "NVRAM device path must be an absolute path");
#else
        WT_RET_MSG(session, EINVAL, "NVRAM block cache requires libmemkind");
#endif
    } else
        WT_RET_MSG(session, EINVAL, "Invalid block cache type");

    WT_RET(__wt_config_gets(session, cfg, "block_cache.system_ram", &cval));
    system_ram = (uint64_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "block_cache.percent_file_in_dram", &cval));
    percent_file_in_os_cache = (u_int)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "block_cache.cache_on_checkpoint", &cval));
    if (cval.val == 0)
        cache_on_checkpoint = false;

    WT_RET(__wt_config_gets(session, cfg, "block_cache.blkcache_eviction_aggression", &cval));
    evict_aggressive = (u_int)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "block_cache.full_target", &cval));
    full_target = (uint64_t)((float)cache_size * (float)cval.val / (float)100);

    WT_RET(__wt_config_gets(session, cfg, "block_cache.cache_on_writes", &cval));
    if (cval.val == 0)
        cache_on_writes = false;

    WT_RET(__wt_config_gets(session, cfg, "block_cache.max_percent_overhead", &cval));
    overhead_pct = (u_int)cval.val;

    WT_RET(__blkcache_reconfig(session, reconfig, cache_size, hash_size, cache_type,
      nvram_device_path, system_ram, percent_file_in_os_cache, cache_on_writes, overhead_pct,
      evict_aggressive, full_target, cache_on_checkpoint));

    return (__blkcache_init(session, cache_size, hash_size, cache_type, nvram_device_path,
      system_ram, percent_file_in_os_cache, cache_on_writes, overhead_pct, evict_aggressive,
      full_target, cache_on_checkpoint));
}
