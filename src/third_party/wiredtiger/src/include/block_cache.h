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

#ifdef ENABLE_MEMKIND
#include <memkind.h>
#endif

/* Cache types. */
#define WT_BLKCACHE_UNCONFIGURED 0
#define WT_BLKCACHE_DRAM 1
#define WT_BLKCACHE_NVRAM 2

/* Hash bucket array size. */
#define WT_BLKCACHE_HASHSIZE_DEFAULT 32768
#define WT_BLKCACHE_HASHSIZE_MIN 512
#define WT_BLKCACHE_HASHSIZE_MAX WT_GIGABYTE

/* How often we compute the total size of the files open in the block manager. */
#define WT_BLKCACHE_FILESIZE_EST_FREQ (5 * WT_THOUSAND)

#define WT_BLKCACHE_MINREF_INCREMENT 20      /* Eviction references window */
#define WT_BLKCACHE_EVICT_OTHER 0            /* Not evicting for various reasons */
#define WT_BLKCACHE_NOT_EVICTION_CANDIDATE 1 /* Not evicting because of frequency counter */

/* Block access operations. */
#define WT_BLKCACHE_RM_EXIT 1
#define WT_BLKCACHE_RM_FREE 2
#define WT_BLKCACHE_RM_EVICTION 3

/*
 * WT_BLKCACHE_ITEM --
 *     Block cache item. It links with other items in the same hash bucket.
 */
struct __wt_blkcache_item {
    TAILQ_ENTRY(__wt_blkcache_item) hashq;

    void *data;
    uint32_t data_size;
    uint32_t num_references;

    /*
     * This counter is incremented every time a block is referenced and decremented every time the
     * eviction thread sweeps through the cache. This counter will be low for blocks that have not
     * been reused or for blocks that were reused in the past but lost their appeal. In this sense,
     * this counter is a metric combining frequency and recency, and hence its name.
     */
    int32_t freq_rec_counter;

    uint32_t ref_count; /* References */

    uint32_t fid;      /* File ID */
    uint8_t addr_size; /* Address cookie */
    uint8_t addr[];
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

    wt_thread_t evict_thread_tid;
    volatile bool blkcache_exiting; /* If destroying the cache */
    int32_t evict_aggressive;       /* Seconds an unused block stays in the cache */

    bool cache_on_checkpoint; /* Don't cache blocks written by checkpoints */
    bool cache_on_writes;     /* Cache blocks on writes */

#ifdef ENABLE_MEMKIND
    struct memkind *pmem_kind; /* NVRAM connection */
#endif
    char *nvram_device_path; /* The absolute path of the file system on NVRAM device */

    uint64_t full_target; /* Number of bytes in the block cache that triggers eviction */
    u_int overhead_pct;   /* Overhead percentage that suppresses population and eviction */

    size_t estimated_file_size;        /* Estimated size of all files used by the workload. */
    int refs_since_filesize_estimated; /* Counter for recalculating the aggregate file size */

    /*
     * This fraction tells us the ratio of total file data to the application-declared size of the
     * OS filesystem buffer cache, which makes the use of this block cache unnecessary. Suppose we
     * set that fraction to 50%. Then if half of our total file data fits into whatever value the
     * user gives us for the filesystem buffer cache, we consider this block cache unhelpful.
     *
     * E.g., if the fraction is set to 50%, our aggregate file size is 500GB, and the application
     * declares there to be 300GB of OS filesystem buffer cache, then we will not use this block
     * cache, because half of our total file size (250GB) would fit into such a buffer cache.
     */
    u_int percent_file_in_os_cache;

    u_int hash_size;     /* Number of block cache hash buckets */
    u_int type;          /* Type of block cache (NVRAM or DRAM) */
    uint64_t bytes_used; /* Bytes in the block cache */
    uint64_t max_bytes;  /* Block cache size */
    uint64_t system_ram; /* Configured size of system RAM */

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
#define WT_BLKCACHE_HIST_BUCKETS 11
#define WT_BLKCACHE_HIST_BOUNDARY 10
    uint32_t cache_references[WT_BLKCACHE_HIST_BUCKETS];
    uint32_t cache_references_removed_blocks[WT_BLKCACHE_HIST_BUCKETS];
    uint32_t cache_references_evicted_blocks[WT_BLKCACHE_HIST_BUCKETS];
};
