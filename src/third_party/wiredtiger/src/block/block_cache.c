/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __blkcache_aggregate_metadata --
 *     Sum the metadata for all buckets and write it into the cache wide variables.
 */
static void
__blkcache_aggregate_metadata(WT_BLKCACHE *blkcache)
{
    uint64_t bytes_used, num_data_blocks;
    u_int i;

    bytes_used = num_data_blocks = 0;

    for (i = 0; i < blkcache->hash_size; i++) {
        bytes_used += blkcache->bucket_metadata[i].bucket_bytes_used;
        num_data_blocks += blkcache->bucket_metadata[i].bucket_num_data_blocks;
    }

    blkcache->bytes_used = bytes_used;
    blkcache->num_data_blocks = num_data_blocks;
}

/*
 * __blkcache_alloc --
 *     Allocate a block of memory in the cache.
 */
static int
__blkcache_alloc(WT_SESSION_IMPL *session, size_t size, void **retp)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;

    if (blkcache->type == BLKCACHE_DRAM)
        return (__wt_malloc(session, size, retp));
    else if (blkcache->type == BLKCACHE_NVRAM) {
#ifdef HAVE_LIBMEMKIND
        *retp = memkind_malloc(blkcache->pmem_kind, size);
        if (*retp == NULL)
            return (WT_BLKCACHE_FULL);
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

    blkcache = &S2C(session)->blkcache;

    if ((double)(blkcache->inserts + blkcache->removals) / (double)(blkcache->lookups) >
      (double)blkcache->overhead_pct)
        return (true);

    return (false);
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
     * Don't evict if there is high overhead due to blocks being inserted/removed. Churn kills
     * performance and evicting when churn is high will exacerbate the overhead.
     */
    if (__blkcache_high_overhead(session)) {
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
    else {
        *reason = BLKCACHE_NOT_EVICTION_CANDIDATE;
        return (false);
    }
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
            return (0);

        /* Aggregate per bucket metadata into cache wide variables. */
        __blkcache_aggregate_metadata(blkcache);

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
                    blkcache->bucket_metadata[i].bucket_num_data_blocks--;
                    blkcache->bucket_metadata[i].bucket_bytes_used -= blkcache_item->id.size;

                    /*
                     * Update the number of removals because it is used to estimate the overhead,
                     * and we want the overhead contributed by eviction to be part of that
                     * calculation.
                     */
                    blkcache->removals++;

                    WT_STAT_CONN_INCR(session, block_cache_blocks_evicted);
                    WT_STAT_CONN_DECRV(session, block_cache_bytes, blkcache_item->id.size);
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
                return (0);
        }
        if (no_eviction_candidates) {
            blkcache->min_num_references += BLKCACHE_MINREF_INCREMENT;
        }

        WT_STAT_CONN_INCR(session, block_cache_eviction_passes);
    }
    return (0);
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
 * __wt_blkcache_get_or_check --
 *     Get a block from the cache or check if one exists.
 */
int
__wt_blkcache_get_or_check(
  WT_SESSION_IMPL *session, wt_off_t offset, size_t size, uint32_t checksum, void *data_ptr)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ID id;
    WT_BLKCACHE_ITEM *blkcache_item;
    uint64_t bucket, hash;

    blkcache = &S2C(session)->blkcache;

    if (blkcache->type == BLKCACHE_UNCONFIGURED)
        return (-1);

    WT_STAT_CONN_INCR(session, block_cache_data_refs);

    /*
     * We race to avoid using synchronization. We only care about an approximate value, so we accept
     * inaccuracy for the sake of avoiding synchronization on the critical path.
     */
    blkcache->lookups++;

    /*
     * If more than the configured fraction of all file objects is likely to fit in the OS buffer
     * cache, don't use this cache.
     */
    if (blkcache->system_ram >=
      __blkcache_estimate_filesize(session) * blkcache->fraction_in_os_cache) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_get);
        return (WT_BLKCACHE_BYPASS);
    }

    /*
     * We will use the item id structure as a byte array for (1) computing a hash, and (2) for
     * comparing with the items in the hash table.
     */
    id.checksum = checksum;
    id.offset = offset;
    id.size = (uint32_t)size;
    id.fid = S2BT(session)->id;
    hash = __wt_hash_city64(&id, sizeof(id));

    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);
    TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq) {
        if (blkcache_item->id.checksum == id.checksum && blkcache_item->id.offset == id.offset &&
          blkcache_item->id.size == id.size && blkcache_item->id.fid == id.fid) {
            if (data_ptr != NULL)
                memcpy(data_ptr, blkcache_item->data, size);

            blkcache_item->num_references++;
            if (blkcache_item->freq_rec_counter < 0)
                blkcache_item->freq_rec_counter = 0;
            blkcache_item->freq_rec_counter++;

            __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
            WT_STAT_CONN_INCR(session, block_cache_hits);
            __wt_verbose(session, WT_VERB_BLKCACHE,
              "block found in cache: offset=%" PRIuMAX ", size=%" WT_SIZET_FMT ", checksum=%" PRIu32
              ", hash=%" PRIu64,
              (uintmax_t)offset, size, checksum, hash);
            return (0);
        }
    }

    /* Block not found */
    __wt_verbose(session, WT_VERB_BLKCACHE,
      "block not found in cache: offset=%" PRIuMAX ", size=%" WT_SIZET_FMT ", checksum=%" PRIu32
      ", hash=%" PRIu64,
      (uintmax_t)offset, size, checksum, hash);

    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
    WT_STAT_CONN_INCR(session, block_cache_misses);
    return (-1);
}

/*
 * __wt_blkcache_put --
 *     Put a block into the cache.
 */
int
__wt_blkcache_put(WT_SESSION_IMPL *session, wt_off_t offset, size_t size, uint32_t checksum,
  void *data, bool checkpoint_io, bool write)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ID id;
    WT_BLKCACHE_ITEM *blkcache_item;
    WT_DECL_RET;
    uint64_t bucket, hash;
    void *data_ptr;

    blkcache = &S2C(session)->blkcache;
    data_ptr = NULL;

    if (blkcache->type == BLKCACHE_UNCONFIGURED)
        return (-1);

    /* Bypass on write if the no-write-allocate setting is on */
    if (write && blkcache->cache_on_writes == false) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_writealloc);
        return (-1);
    }

    /* Are we within cache size limits? */
    if (blkcache->bytes_used >= blkcache->max_bytes)
        return (WT_BLKCACHE_FULL);

    /*
     * If more than the configured fraction of the file is likely to fit into the OS buffer cache,
     * don't use this cache.
     */
    if (blkcache->system_ram >=
      __blkcache_estimate_filesize(session) * blkcache->fraction_in_os_cache) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_put);
        return (WT_BLKCACHE_BYPASS);
    }

    /*
     * Do not write allocate if this block is written as part of checkpoint. Hot blocks get written
     * and over-written a lot as part of checkpoint, so we don't want to cache them, because (a)
     * they are in the DRAM cache anyway, and (b) they are likely to be overwritten anyway.
     *
     * Writes that are not part of checkpoint I/O are done in the service of eviction. Those are the
     * blocks that the DRAM cache would like to keep but can't, and we definitely want to keep them.
     */
    if ((blkcache->cache_on_checkpoint == false) && checkpoint_io) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_chkpt);
        return (WT_BLKCACHE_BYPASS);
    }

    /* Bypass on high overhead */
    if (__blkcache_high_overhead(session) == true) {
        WT_STAT_CONN_INCR(session, block_cache_bypass_overhead_put);
        return (WT_BLKCACHE_BYPASS);
    }
    /*
     * Allocate space in the cache outside of the critical section. In the unlikely event that we
     * fail to allocate metadata, or if the item exists and the caller did not check for that prior
     * to calling this function, we will free the space.
     */
    WT_RET(__blkcache_alloc(session, size, &data_ptr));

    /*
     * We will use the item id structure as a byte array for (1) computing a hash, and (2) for
     * comparing with the items in the hash table.
     */
    id.checksum = checksum;
    id.offset = offset;
    id.size = (uint32_t)size;
    id.fid = S2BT(session)->id;
    hash = __wt_hash_city64(&id, sizeof(id));

    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);
    TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq) {
        if (blkcache_item->id.checksum == id.checksum && blkcache_item->id.offset == id.offset &&
          blkcache_item->id.size == id.size && blkcache_item->id.fid == id.fid)
            goto item_exists;
    }

    WT_ERR(__wt_calloc_one(session, &blkcache_item));
    blkcache_item->id = id;
    blkcache_item->data = data_ptr;

    /*
     * Set the recency timestamp on newly inserted blocks to the maximum value to reduce the chance
     * of them being evicted before they are reused.
     */
    blkcache_item->freq_rec_counter = 1;

    if (data != NULL && blkcache_item->data != NULL) /* This makes static analyzers happier. */
        memcpy(blkcache_item->data, data, size);
    TAILQ_INSERT_HEAD(&blkcache->hash[bucket], blkcache_item, hashq);

    blkcache->bucket_metadata[bucket].bucket_num_data_blocks++;
    blkcache->bucket_metadata[bucket].bucket_bytes_used += size;
    blkcache->inserts++;

    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);

    WT_STAT_CONN_INCRV(session, block_cache_bytes, size);
    WT_STAT_CONN_INCR(session, block_cache_blocks);
    if (write == true) {
        WT_STAT_CONN_INCRV(session, block_cache_bytes_insert_write, size);
        WT_STAT_CONN_INCR(session, block_cache_blocks_insert_write);
    } else {
        WT_STAT_CONN_INCRV(session, block_cache_bytes_insert_read, size);
        WT_STAT_CONN_INCR(session, block_cache_blocks_insert_read);
    }

    __wt_verbose(session, WT_VERB_BLKCACHE,
      "block inserted in cache: offset=%" PRIuMAX ", size=%" WT_SIZET_FMT ", checksum=%" PRIu32
      ", hash=%" PRIu64,
      (uintmax_t)offset, size, checksum, hash);
    return (0);

item_exists:
    if (write) {
        memcpy(blkcache_item->data, data, size);
        WT_STAT_CONN_INCRV(session, block_cache_bytes_update, size);
        WT_STAT_CONN_INCR(session, block_cache_blocks_update);
    }

    __wt_verbose(session, WT_VERB_BLKCACHE,
      "block exists during put: offset=%" PRIuMAX ", size=%" WT_SIZET_FMT
      ", "
      "checksum=%" PRIu32 ", hash=%" PRIu64,
      (uintmax_t)offset, size, checksum, hash);
err:
    __blkcache_free(session, data_ptr);
    __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
    return (ret);
}

/*
 * __wt_blkcache_remove --
 *     Remove a block from the cache.
 */
void
__wt_blkcache_remove(WT_SESSION_IMPL *session, wt_off_t offset, size_t size, uint32_t checksum)
{
    WT_BLKCACHE *blkcache;
    WT_BLKCACHE_ID id;
    WT_BLKCACHE_ITEM *blkcache_item;
    uint64_t bucket, hash;

    blkcache = &S2C(session)->blkcache;

    if (blkcache->type == BLKCACHE_UNCONFIGURED)
        return;

    /*
     * We will use the item id structure as a byte array for (1) computing a hash, and (2) for
     * comparing with the items in the hash table.
     */
    id.checksum = checksum;
    id.offset = offset;
    id.size = (uint32_t)size;
    id.fid = S2BT(session)->id;
    hash = __wt_hash_city64(&id, sizeof(id));

    bucket = hash % blkcache->hash_size;
    __wt_spin_lock(session, &blkcache->hash_locks[bucket]);
    TAILQ_FOREACH (blkcache_item, &blkcache->hash[bucket], hashq) {
        if (blkcache_item->id.checksum == id.checksum && blkcache_item->id.offset == id.offset &&
          blkcache_item->id.size == id.size && blkcache_item->id.fid == id.fid) {
            TAILQ_REMOVE(&blkcache->hash[bucket], blkcache_item, hashq);
            blkcache->bucket_metadata[bucket].bucket_num_data_blocks--;
            blkcache->bucket_metadata[bucket].bucket_bytes_used -= size;
            __blkcache_update_ref_histogram(session, blkcache_item, BLKCACHE_RM_FREE);
            __wt_spin_unlock(session, &blkcache->hash_locks[bucket]);
            __blkcache_free(session, blkcache_item->data);
            __wt_overwrite_and_free(session, blkcache_item);
            WT_STAT_CONN_DECRV(session, block_cache_bytes, size);
            WT_STAT_CONN_DECR(session, block_cache_blocks);
            WT_STAT_CONN_INCR(session, block_cache_blocks_removed);
            blkcache->removals++;
            __wt_verbose(session, WT_VERB_BLKCACHE,
              "block removed from cache: offset=%" PRIuMAX ", size=%" WT_SIZET_FMT
              ", checksum=%" PRIu32 ", hash=%" PRIu64,
              (uintmax_t)offset, size, checksum, hash);
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
  float overhead_pct, u_int evict_aggressive, uint64_t full_target, bool cache_on_checkpoint)
{
    WT_BLKCACHE *blkcache;
    WT_DECL_RET;
    uint64_t i;

    blkcache = &S2C(session)->blkcache;
    blkcache->cache_on_checkpoint = cache_on_checkpoint;
    blkcache->cache_on_writes = cache_on_writes;
    blkcache->hash_size = hash_size;
    blkcache->fraction_in_os_cache = (float)percent_file_in_os_cache / 100;
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
    WT_RET(__wt_calloc_def(session, blkcache->hash_size, &blkcache->bucket_metadata));

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
 * __wt_block_cache_destroy --
 *     Destroy the block cache and free all memory.
 */
void
__wt_block_cache_destroy(WT_SESSION_IMPL *session)
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

    __blkcache_aggregate_metadata(blkcache);

    if (blkcache->bytes_used == 0)
        goto done;

    for (i = 0; i < blkcache->hash_size; i++) {
        __wt_spin_lock(session, &blkcache->hash_locks[i]);
        while (!TAILQ_EMPTY(&blkcache->hash[i])) {
            blkcache_item = TAILQ_FIRST(&blkcache->hash[i]);
            TAILQ_REMOVE(&blkcache->hash[i], blkcache_item, hashq);
            /*
             * Some workloads crash on freeing NVRAM arenas. If that occurs the call to free can be
             * removed and the library/OS will clean up for us once the process exits.
             */
            __blkcache_free(session, blkcache_item->data);
            __blkcache_update_ref_histogram(session, blkcache_item, BLKCACHE_RM_EXIT);
            blkcache->bucket_metadata[i].bucket_num_data_blocks--;
            blkcache->bucket_metadata[i].bucket_bytes_used -= blkcache_item->id.size;
            __wt_free(session, blkcache_item);
        }
        __wt_spin_unlock(session, &blkcache->hash_locks[i]);
    }
    __blkcache_aggregate_metadata(blkcache);
    WT_ASSERT(session, blkcache->bytes_used == 0 && blkcache->num_data_blocks == 0);

done:
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
    __wt_free(session, blkcache->bucket_metadata);
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
  bool cache_on_writes, float overhead_pct, u_int evict_aggressive, uint64_t full_target,
  bool cache_on_checkpoint)
{
    WT_BLKCACHE *blkcache;

    blkcache = &S2C(session)->blkcache;

    if (!reconfig || blkcache->type == BLKCACHE_UNCONFIGURED)
        return (0);

    if (blkcache->cache_on_checkpoint != cache_on_checkpoint ||
      blkcache->cache_on_writes != cache_on_writes || blkcache->hash_size != hash_size ||
      __wt_floatcmp(blkcache->fraction_in_os_cache, (float)percent_file_in_os_cache / 100) != 0 ||
      blkcache->full_target != full_target || blkcache->max_bytes != cache_size ||
      __wt_floatcmp(blkcache->overhead_pct, overhead_pct) != 0 ||
      blkcache->system_ram != system_ram ||
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
 * __wt_block_cache_setup --
 *     Set up the block cache.
 */
int
__wt_block_cache_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
{
    WT_BLKCACHE *blkcache;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    float overhead_pct;
    uint64_t cache_size, full_target, system_ram;
    u_int cache_type, evict_aggressive, hash_size, percent_file_in_os_cache;
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
        WT_ASSERT(session, __wt_absolute_path(nvram_device_path));
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
    overhead_pct = (float)cval.val / (float)100;

    WT_RET(__blkcache_reconfig(session, reconfig, cache_size, hash_size, cache_type,
      nvram_device_path, system_ram, percent_file_in_os_cache, cache_on_writes, overhead_pct,
      evict_aggressive, full_target, cache_on_checkpoint));

    return (__blkcache_init(session, cache_size, hash_size, cache_type, nvram_device_path,
      system_ram, percent_file_in_os_cache, cache_on_writes, overhead_pct, evict_aggressive,
      full_target, cache_on_checkpoint));
}
