/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WiredTiger's block cache. It is used to cache blocks identical to those that live on disk in a
 * faster storage medium, such as NVRAM.
 */

#ifdef HAVE_LIBMEMKIND
#include <memkind.h>
#endif

/*
 * Determines now often we compute the total size of the files open in the block manager.
 */
#define BLKCACHE_FILESIZE_EST_FREQ 5000

#define BLKCACHE_HASHSIZE_DEFAULT 32768
#define BLKCACHE_HASHSIZE_MIN 512
#define BLKCACHE_HASHSIZE_MAX WT_GIGABYTE

#define WT_BLKCACHE_FULL -2
#define WT_BLKCACHE_BYPASS -3

#define BLKCACHE_MINREF_INCREMENT 20
#define BLKCACHE_EVICT_OTHER 0
#define BLKCACHE_NOT_EVICTION_CANDIDATE 1

/*
 * WT_BLKCACHE_ID --
 *    Checksum, offset and size uniquely identify a block.
 *    These are the same items used to compute the cookie.
 */
struct __wt_blkcache_id {
    uint32_t checksum;
    wt_off_t offset;
    uint32_t size;
};

/*
 * WT_BLKCACHE_ITEM --
 *     Block cache item. It links with other items in the same hash bucket.
 */
struct __wt_blkcache_item {
    struct __wt_blkcache_id id;
    TAILQ_ENTRY(__wt_blkcache_item) hashq;
    void *data;
    uint32_t num_references;

    /*
     * This counter is incremented every time a block is referenced and decremented every time the
     * eviction thread sweeps through the cache. This counter will be low for blocks that have not
     * been reused or for blocks that were reused in the past but lost their appeal. In this sense,
     * this counter is a metric combining frequency and recency, and hence its name.
     */
    int32_t freq_rec_counter;
};

/*
 * WT_BLKCACHE_BUCKET_METADATA --
 *     The metadata indicating the number of bytes in cache is accumulated per
 *     bucket, because we do locking per bucket. Then the eviction thread accumulates
 *     per-bucket data into a global metadata value that is stored in the block
 *     cache structure.
 */

struct __wt_blkcache_bucket_metadata {
    WT_CACHE_LINE_PAD_BEGIN
    volatile uint64_t bucket_num_data_blocks; /* Number of blocks in the bucket */
    volatile uint64_t bucket_bytes_used;      /* Bytes in the bucket */
    WT_CACHE_LINE_PAD_END
};

/*
 * WT_BLKCACHE --
 *     Block cache metadata includes the hashtable of cached items, number of cached data blocks
 * and the total amount of space they occupy.
 */
struct __wt_blkcache {
    /* Locked: Block manager cache. Locks are per-bucket. */
    TAILQ_HEAD(__wt_blkcache_hash, __wt_blkcache_item) * hash;
    WT_SPINLOCK *hash_locks;
    WT_BLKCACHE_BUCKET_METADATA *bucket_metadata;

    wt_thread_t evict_thread_tid;
    volatile bool blkcache_exiting; /* If destroying the cache */
    int32_t evict_aggressive;       /* Seconds an unused block stays in the cache */

    bool cache_on_checkpoint; /* Don't cache blocks written by checkpoints */
    bool cache_on_writes;     /* Cache blocks on writes */

#ifdef HAVE_LIBMEMKIND
    struct memkind *pmem_kind; /* NVRAM connection */
#endif
    char *nvram_device_path; /* The absolute path of the file system on NVRAM device */

    uint64_t full_target; /* Number of bytes in the block cache that triggers eviction */
    float overhead_pct;   /* Overhead percentage that suppresses population and eviction */

    size_t estimated_file_size;        /* Estimated size of all files used by the workload. */
    int refs_since_filesize_estimated; /* Counter for recalculating the aggregate file size */

    /*
     * This fraction tells us the good enough ratio of file data cached in the DRAM
     * resident OS buffer cache, which makes the use of this block cache unnecessary.
     * Suppose we set that fraction to 50%. Then if half of our file data fits into
     * system DRAM, we consider this block cache unhelpful.
     *
     * E.g., if the fraction is set to 50%, our aggregate file size is
     * 500GB, and we have 300GB of RAM, then we will not use this block cache,
     * because we know that half of our files (250GB) must be cached by the OS in DRAM.
     */
    float fraction_in_os_cache;

    u_int hash_size;                   /* Number of block cache hash buckets */
    u_int type;                        /* Type of block cache (NVRAM or DRAM) */
    volatile uint64_t bytes_used;      /* Bytes in the block cache */
    volatile uint64_t num_data_blocks; /* Number of blocks in the block cache */
    uint64_t max_bytes;                /* Block cache size */
    uint64_t system_ram;               /* Configured size of system RAM */

    uint32_t min_num_references; /* The per-block number of references triggering eviction. */

    /*
     * Various metrics helping us measure the overhead and decide if to bypass the cache. We access
     * some of them without synchronization despite races. These serve as heuristics, and we don't
     * need precise values for them to be useful. If, because of races, we lose updates of these
     * values, assuming that we lose them at the same rate for all variables, the ratio should
     * remain roughly accurate. We care about the ratio.
     */
    uint64_t lookups;
    uint64_t inserts;
    uint64_t removals;

    /* Histograms keeping track of number of references to each block */
#define BLKCACHE_HIST_BUCKETS 11
#define BLKCACHE_HIST_BOUNDARY 10
    uint32_t cache_references[BLKCACHE_HIST_BUCKETS];
    uint32_t cache_references_removed_blocks[BLKCACHE_HIST_BUCKETS];
    uint32_t cache_references_evicted_blocks[BLKCACHE_HIST_BUCKETS];
};

#define BLKCACHE_UNCONFIGURED 0
#define BLKCACHE_DRAM 1
#define BLKCACHE_NVRAM 2

#define BLKCACHE_RM_EXIT 1
#define BLKCACHE_RM_FREE 2
#define BLKCACHE_RM_EVICTION 3
