/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Supported btree formats: the "current" version is the maximum supported major/minor versions.
 */
#define WT_BTREE_VERSION_MIN ((WT_BTREE_VERSION){1, 1, 0}) /* Oldest version supported */

/* Increase the version number for standalone build. */
#ifdef WT_STANDALONE_BUILD
#define WT_BTREE_VERSION_MAX ((WT_BTREE_VERSION){2, 1, 0}) /* Newest version supported */
#else
#define WT_BTREE_VERSION_MAX ((WT_BTREE_VERSION){1, 1, 0}) /* Newest version supported */
#endif

#define WT_BTREE_MIN_ALLOC_SIZE 512

/*
 * The maximum btree leaf and internal page size is 512MB (2^29). The limit is enforced in software,
 * it could be larger, specifically, the underlying default block manager can support 4GB (2^32).
 * Currently, the maximum page size must accommodate our dependence on the maximum page size fitting
 * into a number of bits less than 32; see the row-store page key-lookup functions for the magic.
 */
#define WT_BTREE_PAGE_SIZE_MAX (512 * WT_MEGABYTE)

/*
 * The length of variable-length column-store values and row-store keys/values are stored in a 4B
 * type, so the largest theoretical key/value item is 4GB. However, in the WT_UPDATE structure we
 * use the UINT32_MAX size as a "deleted" flag, and second, the size of an overflow object is
 * constrained by what an underlying block manager can actually write. (For example, in the default
 * block manager, writing an overflow item includes the underlying block's page header and block
 * manager specific structure, aligned to an allocation-sized unit). The btree engine limits the
 * size of a single object to (4GB - 1KB); that gives us additional bytes if we ever want to store a
 * structure length plus the object size in 4B, or if we need additional flag values. Attempts to
 * store large key/value items in the tree trigger an immediate check to the block manager, to make
 * sure it can write the item. Storing 4GB objects in a btree borders on clinical insanity, anyway.
 *
 * Record numbers are stored in 64-bit unsigned integers, meaning the largest record number is
 * "really, really big".
 */
#define WT_BTREE_MAX_OBJECT_SIZE ((uint32_t)(UINT32_MAX - 1024))

/* Evict pages if we see this many consecutive deleted records. */
#define WT_BTREE_DELETE_THRESHOLD WT_THOUSAND

/*
 * Minimum size of the chunks (in percentage of the page size) a page gets split into during
 * reconciliation.
 */
#define WT_BTREE_MIN_SPLIT_PCT 50

/*
 * Normalized position constants for "start" when calculating the page's position.
 */
#define WT_NPOS_MID 0.5           /* Middle of the current page */
#define WT_NPOS_LEFT -1e-8        /* Leftmost position in the current page or previous page */
#define WT_NPOS_RIGHT (1. + 1e-8) /* Rightmost position in the current page or next page */
/*
 * Invalid position. This is used to indicate that there is no stored position. The constant -1
 * employs the fact that __wt_page_npos returns a number in range 0...1, therefore storing anything
 * outside of this range can be used as an invalid position.
 */
#define WT_NPOS_INVALID -1.0 /* Store this as an invalid position */
#define WT_NPOS_IS_INVALID(pos) ((pos) < 0.0)

typedef enum __wt_btree_type {
    BTREE_COL_FIX = 1, /* Fixed-length column store */
    BTREE_COL_VAR = 2, /* Variable-length column store */
    BTREE_ROW = 3      /* Row-store */
} WT_BTREE_TYPE;

typedef enum __wt_btree_sync {
    WT_BTREE_SYNC_OFF,
    WT_BTREE_SYNC_WAIT,
    WT_BTREE_SYNC_RUNNING
} WT_BTREE_SYNC;

typedef enum {
    CKSUM_ON = 1,           /* On */
    CKSUM_OFF = 2,          /* Off */
    CKSUM_UNCOMPRESSED = 3, /* Uncompressed blocks only */
    CKSUM_UNENCRYPTED = 4   /* Unencrypted blocks only */
} WT_BTREE_CHECKSUM;

typedef enum { /* Start position for eviction walk */
    WT_EVICT_WALK_PREV,
    WT_EVICT_WALK_NEXT,
    WT_EVICT_WALK_RAND_NEXT,
    WT_EVICT_WALK_RAND_PREV
} WT_EVICT_WALK_TYPE;

/* An invalid btree file ID value. ID 0 is reserved for the metadata file. */
#define WT_BTREE_ID_INVALID UINT32_MAX

#define WT_BTREE_ID_NAMESPACE_BITS 3
#define WT_BTREE_ID_NAMESPACE_SHARED 1

#define WT_BTREE_ID_NAMESPACED(x) ((x) << WT_BTREE_ID_NAMESPACE_BITS)
#define WT_BTREE_ID_UNNAMESPACED(x) ((x) >> WT_BTREE_ID_NAMESPACE_BITS)
#define WT_BTREE_ID_NAMESPACE_ID(x) ((x) & ((1 << WT_BTREE_ID_NAMESPACE_BITS) - 1))
#define WT_BTREE_ID_SHARED(x) (WT_BTREE_ID_NAMESPACE_ID(x) == WT_BTREE_ID_NAMESPACE_SHARED)

/*
 * WT_BTREE --
 *	A btree handle.
 */
struct __wt_btree {
    WT_DATA_HANDLE *dhandle;

    WT_CKPT *ckpt;               /* Checkpoint information */
    size_t ckpt_bytes_allocated; /* Checkpoint information array allocation size */

    WT_BTREE_TYPE type; /* Type */

    const char *key_format;   /* Key format */
    const char *value_format; /* Value format */
    uint8_t bitcnt;           /* Fixed-length field size in bits */

    WT_COLLATOR *collator; /* Row-store comparator */
    int collator_owned;    /* The collator needs to be freed */

    uint32_t id; /* File ID, for logging */

    uint32_t allocsize;             /* Allocation size */
    wt_shared uint32_t maxintlpage; /* Internal page max size */
    uint32_t maxleafpage;           /* Leaf page max size */
    uint32_t maxleafkey;            /* Leaf page max key size */
    uint32_t maxleafvalue;          /* Leaf page max value size */
    uint64_t maxmempage;            /* In-memory page max size */
    uint32_t maxmempage_image;      /* In-memory page image max size */
    uint64_t splitmempage;          /* In-memory split trigger size */

    WT_BTREE_CHECKSUM checksum; /* Checksum configuration */

    /*
     * Reconciliation...
     */
    u_int dictionary;                         /* Dictionary slots */
    bool internal_key_truncate;               /* Internal key truncate */
    bool prefix_compression;                  /* Prefix compression */
    u_int prefix_compression_min;             /* Prefix compression min */
    wt_shared wt_timestamp_t prune_timestamp; /* Garbage collection timestamp for the ingest
                                                 component of layered tables */

#define WT_SPLIT_DEEPEN_MIN_CHILD_DEF (10 * WT_THOUSAND)
    u_int split_deepen_min_child; /* Minimum entries to deepen tree */
#define WT_SPLIT_DEEPEN_PER_CHILD_DEF 100
    u_int split_deepen_per_child; /* Entries per child when deepened */
    int split_pct;                /* Split page percent */

    WT_COMPRESSOR *compressor;    /* Page compressor */
                                  /*
                                   * When doing compression, the pre-compression in-memory byte size
                                   * is optionally adjusted based on previous compression results.
                                   * It's an 8B value because it's updated without a lock.
                                   */
    bool leafpage_compadjust;     /* Run-time compression adjustment */
    uint64_t maxleafpage_precomp; /* Leaf page pre-compression size */
    bool intlpage_compadjust;     /* Run-time compression adjustment */
    uint64_t maxintlpage_precomp; /* Internal page pre-compression size */

    WT_BUCKET_STORAGE *bstorage;    /* Bucket storage source */
    WT_KEYED_ENCRYPTOR *kencryptor; /* Page encryptor */

    WT_PAGE_LOG *page_log; /* Page and log service for disaggregated storage */

    WT_RWLOCK ovfl_lock; /* Overflow lock */

    int maximum_depth;        /* Maximum tree depth during search */
    u_int rec_multiblock_max; /* Maximum blocks written for a page */

    uint64_t last_recno; /* Column-store last record number */

    WT_REF root;                /* Root page reference */
    wt_shared bool modified;    /* If the tree ever modified */
    wt_shared uint8_t original; /* Newly created: bulk-load possible
                         (want a bool but needs atomic cas) */

    bool hs_entries; /* Has entries in the history store table */

    WT_BM *bm;          /* Block manager reference */
    u_int block_header; /* WT_PAGE_HEADER_BYTE_SIZE */

    uint64_t write_gen;      /* Write generation */
    uint64_t base_write_gen; /* Write generation on startup. */
    uint64_t run_write_gen;  /* Runtime write generation. */
    uint64_t rec_max_txn;    /* Maximum transaction seen by reconciliation (clean trees). */
    wt_timestamp_t rec_max_timestamp; /* Maximum timestamp seen by reconciliation (clean trees). */

    wt_shared uint64_t checkpoint_gen;       /* Checkpoint generation */
    wt_shared WT_SESSION_IMPL *sync_session; /* Syncing session */
    wt_shared WT_BTREE_SYNC syncing;         /* Sync status */

/*
 * Helper macros: WT_BTREE_SYNCING indicates if a sync is active (either waiting to start or already
 * running), so no new operations should start that would conflict with the sync.
 * WT_SESSION_BTREE_SYNC indicates if the session is performing a sync on its current tree.
 * WT_SESSION_BTREE_SYNC_SAFE checks whether it is safe to perform an operation that would conflict
 * with a sync.
 */
#define WT_BTREE_SYNCING(btree) (__wt_atomic_load_enum(&(btree)->syncing) != WT_BTREE_SYNC_OFF)
#define WT_SESSION_BTREE_SYNC(session) \
    (__wt_atomic_load_pointer(&S2BT(session)->sync_session) == (session))
#define WT_SESSION_BTREE_SYNC_SAFE(session, btree)                        \
    (__wt_atomic_load_enum(&(btree)->syncing) != WT_BTREE_SYNC_RUNNING || \
      __wt_atomic_load_pointer(&(btree)->sync_session) == (session))

    wt_shared uint64_t bytes_dirty_intl;    /* Bytes in dirty internal pages. */
    wt_shared uint64_t bytes_dirty_leaf;    /* Bytes in dirty leaf pages. */
    wt_shared uint64_t bytes_dirty_total;   /* Bytes ever dirtied in cache. */
    wt_shared uint64_t bytes_inmem;         /* Cache bytes in memory. */
    wt_shared uint64_t bytes_internal;      /* Bytes in internal pages. */
    wt_shared uint64_t bytes_updates;       /* Bytes in updates. */
    wt_shared uint64_t bytes_delta_updates; /* Bytes of updates reconstructed from deltas */

    uint64_t max_upd_txn; /* Transaction ID for the latest update on the btree. */

    /*
     * The maximum bytes allowed to be used for the table on disk. This is currently only used for
     * the history store table.
     */
    uint64_t file_max;

/*
 * We maintain a timer for a clean file to avoid excessive checking of checkpoint information that
 * incurs a large processing penalty. We avoid that but will periodically incur the cost to clean up
 * checkpoints that can be deleted.
 */
#define WT_BTREE_CLEAN_CKPT(session, btree, val)                          \
    do {                                                                  \
        (btree)->clean_ckpt_timer = (val);                                \
        WT_STAT_DSRC_SET((session), btree_clean_checkpoint_timer, (val)); \
    } while (0)
/* Statistics don't like UINT64_MAX, use INT64_MAX. It's still forever. */
#define WT_BTREE_CLEAN_CKPT_FOREVER INT64_MAX
#define WT_BTREE_CLEAN_MINUTES 10
    uint64_t clean_ckpt_timer;

    /*
     * Track the number of obsolete time window pages that are changed into dirty page
     * reconciliation by the checkpoint cleanup.
     */
    wt_shared uint32_t checkpoint_cleanup_obsolete_tw_pages;

    /*
     * Track the number of obsolete time window pages that are changed into dirty page
     * reconciliation by the eviction.
     */
    wt_shared uint32_t eviction_obsolete_tw_pages;

    /*
     * We flush pages from the tree (in order to make checkpoint faster), without a high-level lock.
     * To avoid multiple threads flushing at the same time, lock the tree.
     */
    WT_SPINLOCK flush_lock;          /* Lock to flush the tree's pages */
    uint64_t flush_most_recent_secs; /* Wall clock time for the most recent flush */
    uint64_t flush_most_recent_ts;   /* Timestamp of the most recent flush */

/*
 * All of the following fields live at the end of the structure so it's easier to clear everything
 * but the fields that persist.
 */
#define WT_BTREE_CLEAR_SIZE (offsetof(WT_BTREE, evict_ref))

    /*
     * Eviction information is maintained in the btree handle, but owned by eviction, not the btree
     * code.
     */
    WT_REF *evict_ref;                         /* Eviction thread's location */
    uint64_t evict_saved_ref_check;            /* Eviction saved thread's location as an ID */
    double evict_pos;                          /* Eviction thread's soft location */
    uint32_t linear_walk_restarts;             /* next/prev walk restarts */
    uint64_t evict_priority;                   /* Relative priority of cached pages */
    uint32_t evict_walk_progress;              /* Eviction walk progress */
    uint32_t evict_walk_target;                /* Eviction walk target */
    wt_shared u_int evict_walk_period;         /* Skip this many LRU walks */
    u_int evict_walk_saved;                    /* Saved walk skips for checkpoints */
    u_int evict_walk_skips;                    /* Number of walks skipped */
    wt_shared int32_t evict_disabled;          /* Eviction disabled count */
    bool evict_disabled_open;                  /* Eviction disabled on open */
    wt_shared volatile uint32_t evict_busy;    /* Count of threads in eviction */
    wt_shared volatile uint32_t prefetch_busy; /* Count of threads in prefetch */
    WT_EVICT_WALK_TYPE evict_start_type;
    uint32_t last_evict_walk_flags; /* A copy of the cache flags from the prior walk */

    /* The next page ID available for allocation in disaggregated storage for this tree. */
    wt_shared uint64_t next_page_id;

/*
 * Flag values up to 0xfff are reserved for WT_DHANDLE_XXX. See comment with dhandle flags for an
 * explanation.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 12 */
#define WT_BTREE_BULK 0x0001000u            /* Bulk-load handle */
#define WT_BTREE_CLOSED 0x0002000u          /* Handle closed */
#define WT_BTREE_DISAGGREGATED 0x0004000u   /* In disaggregated storage */
#define WT_BTREE_GARBAGE_COLLECT 0x0008000u /* Content becomes obsolete automatically */
#define WT_BTREE_IGNORE_CACHE 0x0010000u    /* Cache-resident object */
#define WT_BTREE_IN_MEMORY 0x0020000u       /* Cache-resident object */
#define WT_BTREE_LOGGED 0x0040000u          /* Commit-level durability without timestamps */
#define WT_BTREE_NO_CHECKPOINT 0x0080000u   /* Disable checkpoints */
#define WT_BTREE_NO_EVICT 0x0100000u        /* Cache-resident object. Never run eviction on it. */
#define WT_BTREE_READONLY 0x0200000u        /* Handle is readonly */
#define WT_BTREE_SALVAGE 0x0400000u         /* Handle is for salvage */
#define WT_BTREE_SKIP_CKPT 0x0800000u       /* Handle skipped checkpoint */
#define WT_BTREE_VERIFY 0x1000000u          /* Handle is for verify */
                                            /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/* Flags that make a btree handle special (not for normal use). */
#define WT_BTREE_SPECIAL_FLAGS (WT_BTREE_BULK | WT_BTREE_SALVAGE | WT_BTREE_VERIFY)

/*
 * WT_SALVAGE_COOKIE --
 *	Encapsulation of salvage information for reconciliation.
 */
struct __wt_salvage_cookie {
    uint64_t missing; /* Initial items to create */
    uint64_t skip;    /* Initial items to skip */
    uint64_t take;    /* Items to take */

    bool done; /* Ignore the rest */
};

#define WT_DELTA_LEAF_ENABLED(session)                 \
    (F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED) && \
      F_ISSET(&S2C(session)->page_delta, WT_LEAF_PAGE_DELTA))

#define WT_DELTA_INT_ENABLED(btree, conn) \
    (F_ISSET(btree, WT_BTREE_DISAGGREGATED) && F_ISSET(&conn->page_delta, WT_INTERNAL_PAGE_DELTA))

#define WT_DELTA_ENABLED_FOR_PAGE(session, type)                   \
    ((type) == WT_PAGE_ROW_LEAF ? WT_DELTA_LEAF_ENABLED(session) : \
                                  WT_DELTA_INT_ENABLED(S2BT(session), S2C(session)))
