/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* Cache operations. */
typedef enum __wt_cache_op {
    WT_SYNC_CHECKPOINT,
    WT_SYNC_CLOSE,
    WT_SYNC_DISCARD,
    WT_SYNC_WRITE_LEAVES
} WT_CACHE_OP;

#define WT_HS_FILE_MIN (100 * WT_MEGABYTE)

/*
 * WT_CACHE_EVICTION_CONTROLS --
 *  Cache eviction controls configuration.
 *  WT_CACHE_EVICT_INCREMENTAL_APP: Only a part of application threads will participate in cache
 * management when a cache threshold reaches its trigger limit. WT_CACHE_EVICT_SCRUB_UNDER_TARGET:
 * Change the eviction strategy to scrub eviction when the cache usage is under the target limit.
 */
struct __wt_cache_eviction_controls {
/* cache eviction controls bit positions */
#define WT_CACHE_EVICT_INCREMENTAL_APP 0x1u
#define WT_CACHE_EVICT_SCRUB_UNDER_TARGET 0x2u
    wt_shared uint32_t flags_atomic;
};

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
    /*
     * Different threads read/write pages to/from the cache and create pages in the cache, so we
     * cannot know precisely how much memory is in use at any specific time. However, even though
     * the values don't have to be exact, they can't be garbage, we track what comes in and what
     * goes out and calculate the difference as needed.
     */

    wt_shared uint64_t bytes_dirty_intl; /* Bytes/pages currently dirty */
    wt_shared uint64_t bytes_dirty_leaf;
    wt_shared uint64_t bytes_dirty_total;
    wt_shared uint64_t bytes_evict;         /* Bytes/pages discarded by eviction */
    wt_shared uint64_t bytes_image_intl;    /* Bytes of disk images (internal) */
    wt_shared uint64_t bytes_image_leaf;    /* Bytes of disk images (leaf) */
    wt_shared uint64_t bytes_inmem;         /* Bytes/pages in memory */
    wt_shared uint64_t bytes_internal;      /* Bytes of internal pages */
    wt_shared uint64_t bytes_read;          /* Bytes read into memory */
    wt_shared uint64_t bytes_updates;       /* Bytes of updates to pages */
    wt_shared uint64_t bytes_delta_updates; /* Bytes of updates reconstructed from deltas */
    wt_shared uint64_t bytes_written;

    WT_CACHE_EVICTION_CONTROLS cache_eviction_controls;

    /*
     * History store cache usage. TODO: The values for these variables are cached and potentially
     * outdated.
     */
    wt_shared uint64_t bytes_hs;         /* History store bytes inmem */
    wt_shared uint64_t bytes_hs_dirty;   /* History store bytes inmem dirty */
    wt_shared uint64_t bytes_hs_updates; /* History store bytes inmem updates */

    wt_shared uint64_t pages_dirty_intl;
    wt_shared uint64_t pages_dirty_leaf;
    wt_shared uint64_t pages_evicted;
    wt_shared uint64_t pages_inmem;

    u_int overhead_pct; /* Cache percent adjustment */

    uint32_t hs_fileid; /* History store table file ID */

    /*
     * The "history_activity" verbose messages are throttled to once per checkpoint. To accomplish
     * this we track the checkpoint generation for the most recent read and write verbose messages.
     */
    uint64_t hs_verb_gen_read;
    wt_shared uint64_t hs_verb_gen_write;

    /*
     * Cache pool information.
     */
    uint64_t cp_pass_pressure;   /* Calculated pressure from this pass */
    uint64_t cp_quota;           /* Maximum size for this cache */
    uint64_t cp_reserved;        /* Base size for this cache */
    WT_SESSION_IMPL *cp_session; /* May be used for cache management */
    uint32_t cp_skip_count;      /* Post change stabilization */
    wt_thread_t cp_tid;          /* Thread ID for cache pool manager */
    /* State seen at the last pass of the shared cache manager */
    uint64_t cp_saved_app_evicts; /* User eviction count at last review */
    uint64_t cp_saved_app_waits;  /* User wait count at last review */
    uint64_t cp_saved_read;       /* Read count at last review */

/*
 * Flags.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CACHE_POOL_MANAGER 0x1u        /* The active cache pool manager */
#define WT_CACHE_POOL_RUN 0x2u            /* Cache pool thread running */
                                          /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint16_t pool_flags_atomic; /* Cache pool flags */
};

/*
 * WT_CACHE_POOL --
 *	A structure that represents a shared cache.
 */
struct __wt_cache_pool {
    WT_SPINLOCK cache_pool_lock;
    WT_CONDVAR *cache_pool_cond;
    const char *name;
    uint64_t size;
    uint64_t chunk;
    uint64_t quota;
    uint64_t currently_used;
    uint32_t refs; /* Reference count for structure. */
    /* Locked: List of connections participating in the cache pool. */
    TAILQ_HEAD(__wt_cache_pool_qh, __wt_connection_impl) cache_pool_qh;

    wt_shared uint8_t pool_managed; /* Cache pool has a manager thread */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CACHE_POOL_ACTIVE 0x1u /* Cache pool is active */
                                  /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/*
 * Optimize comparisons against the history store URI, flag handles that reference the history store
 * file.
 */
#define WT_IS_HS(dh) F_ISSET(dh, WT_DHANDLE_HS)

/* Optimize comparisons against the shared metadata store for disaggregated storage. */
#define WT_IS_DISAGG_META(dh) F_ISSET(dh, WT_DHANDLE_DISAGG_META)
