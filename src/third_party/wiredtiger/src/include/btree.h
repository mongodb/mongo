/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Supported btree formats: the "current" version is the maximum supported major/minor versions.
 */
#define WT_BTREE_MAJOR_VERSION_MIN 1 /* Oldest version supported */
#define WT_BTREE_MINOR_VERSION_MIN 1

/* Increase the version number for standalone build. */
#ifdef WT_STANDALONE_BUILD
#define WT_BTREE_MAJOR_VERSION_MAX 2 /* Newest version supported */
#else
#define WT_BTREE_MAJOR_VERSION_MAX 1 /* Newest version supported */
#endif

#define WT_BTREE_MINOR_VERSION_MAX 1

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

/*
 * A location in a file is a variable-length cookie, but it has a maximum size so it's easy to
 * create temporary space in which to store them. (Locations can't be much larger than this anyway,
 * they must fit onto the minimum size page because a reference to an overflow page is itself a
 * location.)
 */
#define WT_BTREE_MAX_ADDR_COOKIE 255 /* Maximum address cookie */

/* Evict pages if we see this many consecutive deleted records. */
#define WT_BTREE_DELETE_THRESHOLD 1000

/*
 * Minimum size of the chunks (in percentage of the page size) a page gets split into during
 * reconciliation.
 */
#define WT_BTREE_MIN_SPLIT_PCT 50

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
    WT_EVICT_WALK_NEXT,
    WT_EVICT_WALK_PREV,
    WT_EVICT_WALK_RAND_NEXT,
    WT_EVICT_WALK_RAND_PREV
} WT_EVICT_WALK_TYPE;

/*
 * An invalid btree file ID value. ID 0 is reserved for the metadata file.
 */
#define WT_BTREE_ID_INVALID UINT32_MAX

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

    uint32_t allocsize;        /* Allocation size */
    uint32_t maxintlpage;      /* Internal page max size */
    uint32_t maxleafpage;      /* Leaf page max size */
    uint32_t maxleafkey;       /* Leaf page max key size */
    uint32_t maxleafvalue;     /* Leaf page max value size */
    uint64_t maxmempage;       /* In-memory page max size */
    uint32_t maxmempage_image; /* In-memory page image max size */
    uint64_t splitmempage;     /* In-memory split trigger size */

    void *huffman_value; /* Value huffman encoding */

    WT_BTREE_CHECKSUM checksum; /* Checksum configuration */

    /*
     * Reconciliation...
     */
    u_int dictionary;             /* Dictionary slots */
    bool internal_key_truncate;   /* Internal key truncate */
    bool prefix_compression;      /* Prefix compression */
    u_int prefix_compression_min; /* Prefix compression min */

#define WT_SPLIT_DEEPEN_MIN_CHILD_DEF 10000
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

    WT_BUCKET_STORAGE *bstorage;    /* Tiered storage source */
    WT_KEYED_ENCRYPTOR *kencryptor; /* Page encryptor */

    WT_RWLOCK ovfl_lock; /* Overflow lock */

    int maximum_depth;        /* Maximum tree depth during search */
    u_int rec_multiblock_max; /* Maximum blocks written for a page */

    uint64_t last_recno; /* Column-store last record number */

    WT_REF root;      /* Root page reference */
    bool modified;    /* If the tree ever modified */
    uint8_t original; /* Newly created: bulk-load possible
                         (want a bool but needs atomic cas) */

    bool hs_entries;  /* Has entries in the history store table */
    bool lsm_primary; /* Handle is/was the LSM primary */

    WT_BM *bm;          /* Block manager reference */
    u_int block_header; /* WT_PAGE_HEADER_BYTE_SIZE */

    uint64_t write_gen;      /* Write generation */
    uint64_t base_write_gen; /* Write generation on startup. */
    uint64_t run_write_gen;  /* Runtime write generation. */
    uint64_t rec_max_txn;    /* Maximum txn seen (clean trees) */
    wt_timestamp_t rec_max_timestamp;

    uint64_t checkpoint_gen;       /* Checkpoint generation */
    WT_SESSION_IMPL *sync_session; /* Syncing session */
    WT_BTREE_SYNC syncing;         /* Sync status */

/*
 * Helper macros: WT_BTREE_SYNCING indicates if a sync is active (either waiting to start or already
 * running), so no new operations should start that would conflict with the sync.
 * WT_SESSION_BTREE_SYNC indicates if the session is performing a sync on its current tree.
 * WT_SESSION_BTREE_SYNC_SAFE checks whether it is safe to perform an operation that would conflict
 * with a sync.
 */
#define WT_BTREE_SYNCING(btree) ((btree)->syncing != WT_BTREE_SYNC_OFF)
#define WT_SESSION_BTREE_SYNC(session) (S2BT(session)->sync_session == (session))
#define WT_SESSION_BTREE_SYNC_SAFE(session, btree) \
    ((btree)->syncing != WT_BTREE_SYNC_RUNNING || (btree)->sync_session == (session))

    uint64_t bytes_dirty_intl;  /* Bytes in dirty internal pages. */
    uint64_t bytes_dirty_leaf;  /* Bytes in dirty leaf pages. */
    uint64_t bytes_dirty_total; /* Bytes ever dirtied in cache. */
    uint64_t bytes_inmem;       /* Cache bytes in memory. */
    uint64_t bytes_internal;    /* Bytes in internal pages. */
    uint64_t bytes_updates;     /* Bytes in updates. */

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
        WT_STAT_DATA_SET((session), btree_clean_checkpoint_timer, (val)); \
    } while (0)
/* Statistics don't like UINT64_MAX, use INT64_MAX. It's still forever. */
#define WT_BTREE_CLEAN_CKPT_FOREVER INT64_MAX
#define WT_BTREE_CLEAN_MINUTES 10
    uint64_t clean_ckpt_timer;

    /*
     * We flush pages from the tree (in order to make checkpoint faster), without a high-level lock.
     * To avoid multiple threads flushing at the same time, lock the tree.
     */
    WT_SPINLOCK flush_lock; /* Lock to flush the tree's pages */

/*
 * All of the following fields live at the end of the structure so it's easier to clear everything
 * but the fields that persist.
 */
#define WT_BTREE_CLEAR_SIZE (offsetof(WT_BTREE, evict_ref))

    /*
     * Eviction information is maintained in the btree handle, but owned by eviction, not the btree
     * code.
     */
    WT_REF *evict_ref;            /* Eviction thread's location */
    uint64_t evict_priority;      /* Relative priority of cached pages */
    uint32_t evict_walk_progress; /* Eviction walk progress */
    uint32_t evict_walk_target;   /* Eviction walk target */
    u_int evict_walk_period;      /* Skip this many LRU walks */
    u_int evict_walk_saved;       /* Saved walk skips for checkpoints */
    u_int evict_walk_skips;       /* Number of walks skipped */
    int32_t evict_disabled;       /* Eviction disabled count */
    bool evict_disabled_open;     /* Eviction disabled on open */
    volatile uint32_t evict_busy; /* Count of threads in eviction */
    WT_EVICT_WALK_TYPE evict_start_type;

/*
 * Flag values up to 0xfff are reserved for WT_DHANDLE_XXX. See comment with dhandle flags for an
 * explanation.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 12 */
#define WT_BTREE_ALTER 0x0001000u          /* Handle is for alter */
#define WT_BTREE_BULK 0x0002000u           /* Bulk-load handle */
#define WT_BTREE_CLOSED 0x0004000u         /* Handle closed */
#define WT_BTREE_IGNORE_CACHE 0x0008000u   /* Cache-resident object */
#define WT_BTREE_IN_MEMORY 0x0010000u      /* Cache-resident object */
#define WT_BTREE_NO_CHECKPOINT 0x0020000u  /* Disable checkpoints */
#define WT_BTREE_NO_LOGGING 0x0040000u     /* Disable logging */
#define WT_BTREE_OBSOLETE_PAGES 0x0080000u /* Handle has obsolete pages */
#define WT_BTREE_READONLY 0x0100000u       /* Handle is readonly */
#define WT_BTREE_SALVAGE 0x0200000u        /* Handle is for salvage */
#define WT_BTREE_SKIP_CKPT 0x0400000u      /* Handle skipped checkpoint */
#define WT_BTREE_UPGRADE 0x0800000u        /* Handle is for upgrade */
#define WT_BTREE_VERIFY 0x1000000u         /* Handle is for verify */
                                           /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/* Flags that make a btree handle special (not for normal use). */
#define WT_BTREE_SPECIAL_FLAGS \
    (WT_BTREE_ALTER | WT_BTREE_BULK | WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)

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
