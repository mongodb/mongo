/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_RECNO_OOB 0 /* Illegal record number */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_READ_CACHE 0x0001u
#define WT_READ_IGNORE_CACHE_SIZE 0x0002u
#define WT_READ_INTERNAL_OP 0x0004u /* Internal operations don't bump a page's readgen */
#define WT_READ_NOTFOUND_OK 0x0008u
#define WT_READ_NO_SPLIT 0x0010u
#define WT_READ_NO_WAIT 0x0020u
#define WT_READ_PREFETCH 0x0040u
#define WT_READ_PREV 0x0080u
#define WT_READ_RESTART_OK 0x0100u
#define WT_READ_SEE_DELETED 0x0200u
#define WT_READ_SKIP_DELETED 0x0400u
#define WT_READ_SKIP_INTL 0x0800u
#define WT_READ_TRUNCATE 0x1000u
#define WT_READ_VISIBLE_ALL 0x2000u
#define WT_READ_WONT_NEED 0x4000u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

#define WT_READ_EVICT_WALK_FLAGS \
    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_INTERNAL_OP | WT_READ_NO_WAIT
#define WT_READ_EVICT_READ_FLAGS WT_READ_EVICT_WALK_FLAGS | WT_READ_NOTFOUND_OK | WT_READ_RESTART_OK
#define WT_READ_DATA_FLAGS WT_READ_NO_SPLIT | WT_READ_SKIP_INTL

/*
 * Helper: in order to read a Btree without triggering eviction we have to ignore the cache size and
 * disable splits.
 */
#define WT_READ_NO_EVICT (WT_READ_IGNORE_CACHE_SIZE | WT_READ_NO_SPLIT)

/*
 * WT_PAGE_HEADER --
 *	Blocks have a common header, a WT_PAGE_HEADER structure followed by a
 * block-manager specific structure.
 */
struct __wt_page_header {
    /*
     * The record number of the first record of the page is stored on disk so we can figure out
     * where the column-store leaf page fits into the key space during salvage.
     */
    uint64_t recno; /* 00-07: column-store starting recno */

    /*
     * We maintain page write-generations in the non-transactional case as that's how salvage can
     * determine the most recent page between pages overlapping the same key range.
     */
    uint64_t write_gen; /* 08-15: write generation */

    /*
     * The page's in-memory size isn't rounded or aligned, it's the actual number of bytes the
     * disk-image consumes when instantiated in memory.
     */
    uint32_t mem_size; /* 16-19: in-memory page size */

    union {
        uint32_t entries; /* 20-23: number of cells on page */
        uint32_t datalen; /* 20-23: overflow data length */
    } u;

    uint8_t type; /* 24: page type */

/*
 * No automatic generation: flag values cannot change, they're written to disk.
 */
#define WT_PAGE_COMPRESSED 0x01u   /* Page is compressed on disk */
#define WT_PAGE_EMPTY_V_ALL 0x02u  /* Page has all zero-length values */
#define WT_PAGE_EMPTY_V_NONE 0x04u /* Page has no zero-length values */
#define WT_PAGE_ENCRYPTED 0x08u    /* Page is encrypted on disk */
#define WT_PAGE_UNUSED 0x10u       /* Historic lookaside store page updates, no longer used */
#define WT_PAGE_FT_UPDATE 0x20u    /* Page contains updated fast-truncate information */
    uint8_t flags;                 /* 25: flags */

    /* A byte of padding, positioned to be added to the flags. */
    uint8_t unused; /* 26: unused padding */

#define WT_PAGE_VERSION_ORIG 0 /* Original version */
#define WT_PAGE_VERSION_TS 1   /* Timestamps added */
    uint8_t version;           /* 27: version */
};
/*
 * WT_PAGE_HEADER_SIZE is the number of bytes we allocate for the structure: if the compiler inserts
 * padding it will break the world.
 */
#define WT_PAGE_HEADER_SIZE 28

/*
 * __wt_page_header_byteswap --
 *     Handle big- and little-endian transformation of a page header.
 */
static WT_INLINE void
__wt_page_header_byteswap(WT_PAGE_HEADER *dsk)
{
#ifdef WORDS_BIGENDIAN
    dsk->recno = __wt_bswap64(dsk->recno);
    dsk->write_gen = __wt_bswap64(dsk->write_gen);
    dsk->mem_size = __wt_bswap32(dsk->mem_size);
    dsk->u.entries = __wt_bswap32(dsk->u.entries);
#else
    WT_UNUSED(dsk);
#endif
}

/*
 * The block-manager specific information immediately follows the WT_PAGE_HEADER structure.
 */
#define WT_BLOCK_HEADER_REF(dsk) ((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_SIZE))

/*
 * WT_PAGE_HEADER_BYTE --
 * WT_PAGE_HEADER_BYTE_SIZE --
 *	The first usable data byte on the block (past the combined headers).
 */
#define WT_PAGE_HEADER_BYTE_SIZE(btree) ((u_int)(WT_PAGE_HEADER_SIZE + (btree)->block_header))
#define WT_PAGE_HEADER_BYTE(btree, dsk) \
    ((void *)((uint8_t *)(dsk) + WT_PAGE_HEADER_BYTE_SIZE(btree)))

/*
 * The number of deltas for a base page must be strictly less than or equal to WT_DELTA_LIMIT.
 * Though we have made the number of consecutive deltas adjustable through the max_consecutive_delta
 * config, 32 remains the maximum value we support. Thus WT_DELTA_LIMIT can be used to size arrays
 * that contain the base page plus all associated deltas.
 */
#define WT_DELTA_LIMIT 32

/*
 * WT_ADDR --
 *	An in-memory structure to hold a block's location.
 */
struct __wt_addr {
    WT_TIME_AGGREGATE ta;

    uint8_t *block_cookie;     /* Block-manager's cookie */
    uint8_t block_cookie_size; /* Block-manager's cookie length */

#define WT_ADDR_INT 1     /* Internal page */
#define WT_ADDR_LEAF 2    /* Leaf page */
#define WT_ADDR_LEAF_NO 3 /* Leaf page, no overflow */
    uint8_t type;
};

/*
 * Overflow tracking for reuse: When a page is reconciled, we write new K/V overflow items. If pages
 * are reconciled multiple times, we need to know if we've already written a particular overflow
 * record (so we don't write it again), as well as if we've modified an overflow record previously
 * written (in which case we want to write a new record and discard blocks used by the previously
 * written record). Track overflow records written for the page, storing the values in a skiplist
 * with the record's value as the "key".
 */
struct __wt_ovfl_reuse {
    uint32_t value_offset; /* Overflow value offset */
    uint32_t value_size;   /* Overflow value size */
    uint8_t addr_offset;   /* Overflow addr offset */
    uint8_t addr_size;     /* Overflow addr size */

/*
 * On each page reconciliation, we clear the entry's in-use flag, and reset it as the overflow
 * record is re-used. After reconciliation completes, unused skiplist entries are discarded, along
 * with their underlying blocks.
 *
 * On each page reconciliation, set the just-added flag for each new skiplist entry; if
 * reconciliation fails for any reason, discard the newly added skiplist entries, along with their
 * underlying blocks.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_OVFL_REUSE_INUSE 0x1u
#define WT_OVFL_REUSE_JUST_ADDED 0x2u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;

/*
 * The untyped address immediately follows the WT_OVFL_REUSE structure, the untyped value
 * immediately follows the address.
 */
#define WT_OVFL_REUSE_ADDR(p) ((void *)((uint8_t *)(p) + (p)->addr_offset))
#define WT_OVFL_REUSE_VALUE(p) ((void *)((uint8_t *)(p) + (p)->value_offset))

    WT_OVFL_REUSE *next[0]; /* Forward-linked skip list */
};

/*
 * History store table support: when a page is being reconciled for eviction and has updates that
 * might be required by earlier readers in the system, the updates are written into the history
 * store table, and restored as necessary if the page is read.
 *
 * The first part of the key is comprised of a file ID, record key (byte-string for row-store,
 * record number for column-store) and timestamp. This allows us to search efficiently for a given
 * record key and read timestamp combination. The last part of the key is a monotonically increasing
 * counter to keep the key unique in the case where we have multiple transactions committing at the
 * same timestamp.
 * The value is the WT_UPDATE structure's:
 * 	- stop timestamp
 * 	- durable timestamp
 *	- update type
 *	- value.
 *
 * As the key for the history store table is different for row- and column-store, we store both key
 * types in a WT_ITEM, building/parsing them in the code, because otherwise we'd need two
 * history store files with different key formats. We could make the history store table's key
 * standard by moving the source key into the history store table value, but that doesn't make the
 * coding any simpler, and it makes the history store table's value more likely to overflow the page
 * size when the row-store key is relatively large.
 *
 * Note that we deliberately store the update type as larger than necessary (8 bytes vs 1 byte).
 * We've done this to leave room in case we need to store extra bit flags in this value at a later
 * point. If we need to store more information, we can potentially tack extra information at the end
 * of the "value" buffer and then use bit flags within the update type to determine how to interpret
 * it.
 *
 * We also configure a larger than default internal page size to accommodate for larger history
 * store keys. We do that to reduce the chances of having to create overflow keys on the page.
 */
#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
#define WT_HS_COMPRESSOR "snappy"
#else
#define WT_HS_COMPRESSOR "none"
#endif
#define WT_HS_KEY_FORMAT WT_UNCHECKED_STRING(IuQQ)
#define WT_HS_VALUE_FORMAT WT_UNCHECKED_STRING(QQQu)
/* Disable logging for history store in the metadata. */
#define WT_HS_CONFIG_COMMON                                            \
    "key_format=" WT_HS_KEY_FORMAT ",value_format=" WT_HS_VALUE_FORMAT \
    ",block_compressor=" WT_HS_COMPRESSOR                              \
    ",log=(enabled=false)"                                             \
    ",internal_page_max=16KB"                                          \
    ",leaf_value_max=64MB"                                             \
    ",prefix_compression=false"
#define WT_HS_CONFIG_LOCAL WT_HS_CONFIG_COMMON
#define WT_HS_CONFIG_SHARED WT_HS_CONFIG_COMMON ",block_manager=disagg"

/*
 * WT_SAVE_UPD --
 *	Unresolved updates found during reconciliation.
 */
struct __wt_save_upd {
    WT_INSERT *ins; /* Insert list reference */
    WT_ROW *rip;    /* Original on-page reference */
    WT_UPDATE *onpage_upd;
    WT_UPDATE *onpage_tombstone;
    WT_UPDATE *free_upds; /* Updates to be freed */
    WT_TIME_WINDOW tw;
    bool restore; /* Whether to restore this saved update chain */
};

/*
 * WT_PAGE_BLOCK_META --
 *  Block management metadata associated with a page.
 */
struct __wt_page_block_meta {
    uint64_t page_id;
    uint64_t disagg_lsn;

    uint64_t backlink_lsn;
    uint64_t base_lsn;

    uint32_t checksum;

    size_t image_size; /* The in-memory size of the fully constructed page image. */

    WT_PAGE_LOG_ENCRYPTION encryption;

    uint8_t delta_count;
};

/*
 * WT_PAGE_DISAGG_INFO --
 *  Page information associated with disaggregated storage.
 */
struct __wt_page_disagg_info {
    uint64_t old_rec_lsn_max; /* The LSN associated with the page's before the most recent
                                 reconciliation */
    uint64_t rec_lsn_max;     /* The LSN associated with the page's most recent reconciliation */

    WT_PAGE_BLOCK_META block_meta;
};

/*
 * WT_MULTI --
 *	Replacement block information used during reconciliation.
 */
struct __wt_multi {
    /*
     * Block's key: either a column-store record number or a row-store variable length byte string.
     */
    union {
        uint64_t recno;
        WT_IKEY *ikey;
    } key;

    /*
     * A disk image that may or may not have been written, used to re-instantiate the page in
     * memory.
     */
    void *disk_image;
    WT_PAGE_BLOCK_META *block_meta; /* the metadata for the disk image */

    /*
     * List of unresolved updates. Updates are either a row-store insert or update list, or
     * column-store insert list. When creating history store records, there is an additional value,
     * the committed item's transaction information.
     *
     * If there are unresolved updates, the block wasn't written and there will always be a disk
     * image.
     */
    WT_SAVE_UPD *supd;
    uint32_t supd_entries;
    bool supd_restore; /* Whether to restore saved update chains to this page */

    WT_ADDR addr; /* Disk image written address */
};

/*
 * WT_OVFL_TRACK --
 *  Overflow record tracking for reconciliation. We assume overflow records are relatively rare,
 * so we don't allocate the structures to track them until we actually see them in the data.
 */
struct __wt_ovfl_track {
    /*
     * Overflow key/value address/byte-string pairs we potentially reuse each time we reconcile the
     * page.
     */
    WT_OVFL_REUSE *ovfl_reuse[WT_SKIP_MAXDEPTH];

    /*
     * Overflow key/value addresses to be discarded from the block manager after reconciliation
     * completes successfully.
     */
    WT_CELL **discard;
    size_t discard_entries;
    size_t discard_allocated;
};

/*
 * WT_PAGE_MODIFY --
 *	When a page is modified, there's additional information to maintain.
 */
struct __wt_page_modify {
    /* The first unwritten transaction ID (approximate). */
    uint64_t first_dirty_txn;

    /* The transaction state last time eviction was attempted. */
    uint64_t last_evict_pass_gen;
    uint64_t last_eviction_id;
    wt_timestamp_t last_eviction_timestamp;

#ifdef HAVE_DIAGNOSTIC
    /* Check that transaction time moves forward. */
    uint64_t last_oldest_id;
#endif

    /* Avoid checking for obsolete updates during checkpoints. */
    uint64_t obsolete_check_txn;
    wt_timestamp_t obsolete_check_timestamp;

    /* The largest transaction and timestamp seen on the page by reconciliation. */
    uint64_t rec_max_txn;
    wt_timestamp_t rec_max_timestamp;

    /*
     * Track the timestamp used for the most recent reconciliation. It's useful to avoid duplicating
     * work when precise checkpoints are enabled, so we don't re-reconcile pages when no new content
     * could be written.
     */
    wt_timestamp_t rec_pinned_stable_timestamp;

    /* An approximate timestamp of the newest update */
    wt_shared wt_timestamp_t newest_commit_timestamp;

    /* The largest update transaction ID (approximate). */
    wt_shared uint64_t update_txn;

    /* Dirty bytes added to the cache. */
    wt_shared uint64_t bytes_dirty;
    wt_shared uint64_t bytes_updates;
    wt_shared uint64_t bytes_delta_updates;

    /*
     * When pages are reconciled, the result is one or more replacement blocks. A replacement block
     * can be in one of two states: it was written to disk, and so we have a block address, or it
     * contained unresolved modifications and we have a disk image for it with a list of those
     * unresolved modifications. The former is the common case: we only build lists of unresolved
     * modifications when we're evicting a page, and we only expect to see unresolved modifications
     * on a page being evicted in the case of a hot page that's too large to keep in memory as it
     * is. In other words, checkpoints will skip unresolved modifications, and will write the blocks
     * rather than build lists of unresolved modifications.
     *
     * Ugly union/struct layout to conserve memory, we never have both a replace address and
     * multiple replacement blocks.
     */
    union {
        struct { /* Single, written replacement block */
            WT_ADDR replace;

            /*
             * A disk image that may or may not have been written, used to re-instantiate the page
             * in memory.
             */
            void *disk_image;
        } r;
#undef mod_replace
#define mod_replace u1.r.replace
#undef mod_disk_image
#define mod_disk_image u1.r.disk_image

        struct {
            WT_MULTI *multi;        /* Multiple replacement blocks */
            uint32_t multi_entries; /* Multiple blocks element count */
        } m;
#undef mod_multi
#define mod_multi u1.m.multi
#undef mod_multi_entries
#define mod_multi_entries u1.m.multi_entries
    } u1;

    /*
     * Internal pages need to be able to chain root-page splits and have a special transactional
     * eviction requirement. Column-store leaf pages need update and append lists.
     *
     * Ugly union/struct layout to conserve memory, a page is either a leaf page or an internal
     * page.
     */
    union {
        struct {
            /*
             * When a root page splits, we create a new page and write it; the new page can also
             * split and so on, and we continue this process until we write a single replacement
             * root page. We use the root split field to track the list of created pages so they can
             * be discarded when no longer needed.
             */
            WT_PAGE *root_split; /* Linked list of root split pages */
        } intl;
#undef mod_root_split
#define mod_root_split u2.intl.root_split
        struct {
            /*
             * Appended items to column-stores. Actual appends to the tree only happen on the last
             * page, but gaps created in the namespace by truncate operations can result in the
             * append lists of other pages becoming populated.
             */
            wt_shared WT_INSERT_HEAD **append;

            /*
             * Updated items in column-stores: variable-length RLE entries can expand to multiple
             * entries which requires some kind of list we can expand on demand. Updated items in
             * fixed-length files could be done based on an WT_UPDATE array as in row-stores, but
             * there can be a very large number of bits on a single page, and the cost of the
             * WT_UPDATE array would be huge.
             */
            wt_shared WT_INSERT_HEAD **update;

            /*
             * Split-saved last column-store page record. If a fixed-length column-store page is
             * split, we save the first record number moved so that during reconciliation we know
             * the page's last record and can write any implicitly created deleted records for the
             * page. No longer used by VLCS.
             */
            uint64_t split_recno;
        } column_leaf;
#undef mod_col_append
#define mod_col_append u2.column_leaf.append
#undef mod_col_update
#define mod_col_update u2.column_leaf.update
#undef mod_col_split_recno
#define mod_col_split_recno u2.column_leaf.split_recno
        struct {
            /* Inserted items for row-store. */
            wt_shared WT_INSERT_HEAD **insert;

            /* Updated items for row-stores. */
            wt_shared WT_UPDATE **update;
        } row_leaf;
#undef mod_row_insert
#define mod_row_insert u2.row_leaf.insert
#undef mod_row_update
#define mod_row_update u2.row_leaf.update
    } u2;

    /* Overflow record tracking for reconciliation. */
    WT_OVFL_TRACK *ovfl_track;

    /*
     * Stop aggregated timestamp information when all the keys on the page are removed. This time
     * aggregate information is used to skip these deleted pages as part of the tree walk if the
     * delete operation is visible to the reader.
     */
    wt_shared WT_TIME_AGGREGATE *stop_ta;

    /*
     * Page-delete information for newly instantiated deleted pages. The instantiated flag remains
     * set until the page is reconciled successfully; this indicates that the page_del information
     * in the ref remains valid. The update list remains set (if set at all) until the transaction
     * that deleted the page is resolved. These transitions are independent; that is, the first
     * reconciliation can happen either before or after the delete transaction resolves.
     */
    bool instantiated;        /* True if this is a newly instantiated page. */
    WT_UPDATE **inst_updates; /* Update list for instantiated page with unresolved truncate. */

#define WT_PAGE_LOCK(s, p) __wt_spin_lock_track((s), &(p)->modify->page_lock)
#define WT_PAGE_TRYLOCK(s, p) __wt_spin_trylock_track((s), &(p)->modify->page_lock)
#define WT_PAGE_UNLOCK(s, p) __wt_spin_unlock((s), &(p)->modify->page_lock)
    WT_SPINLOCK page_lock; /* Page's spinlock */

/*
 * The page state is incremented when a page is modified.
 *
 * WT_PAGE_CLEAN --
 *	The page is clean.
 * WT_PAGE_DIRTY_FIRST --
 *	The page is in this state after the first operation that marks a
 *	page dirty, or when reconciliation is checking to see if it has
 *	done enough work to be able to mark the page clean.
 * WT_PAGE_DIRTY --
 *	Two or more updates have been added to the page.
 */
#define WT_PAGE_CLEAN 0
#define WT_PAGE_DIRTY_FIRST 1
#define WT_PAGE_DIRTY 2
    wt_shared uint32_t page_state;

#define WT_PM_REC_EMPTY 1      /* Reconciliation: no replacement */
#define WT_PM_REC_MULTIBLOCK 2 /* Reconciliation: multiple blocks */
#define WT_PM_REC_REPLACE 3    /* Reconciliation: single block */
    uint8_t rec_result;        /* Reconciliation state */

#define WT_PAGE_RS_RESTORED 0x1
    uint8_t restore_state; /* Created by restoring updates */

/* Additional diagnostics fields to catch invalid updates to page_state, even in release builds. */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_PAGE_MODIFY_EXCLUSIVE 0x1u
#define WT_PAGE_MODIFY_RECONCILING 0x2u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/*
 * WT_COL_RLE --
 *	Variable-length column-store pages have an array of page entries with
 *	RLE counts greater than 1 when reading the page, so it's not necessary
 *	to walk the page counting records to find a specific entry. We can do a
 *	binary search in this array, then an offset calculation to find the
 *	cell.
 */
WT_PACKED_STRUCT_BEGIN(__wt_col_rle)
    uint64_t recno; /* Record number of first repeat. */
    uint64_t rle;   /* Repeat count. */
    uint32_t indx;  /* Slot of entry in col_var. */
WT_PACKED_STRUCT_END

/*
 * WT_PAGE_INDEX --
 *	The page index held by each internal page.
 */
struct __wt_page_index {
#define WT_SPLIT_DEEPEN_MIN_CREATE_CHILD_PAGES 10
#define WT_INTERNAL_SPLIT_MIN_KEYS 100
    uint32_t entries;
    wt_shared uint32_t deleted_entries;
    WT_REF **index;
};

/*
 * WT_COL_VAR_REPEAT --
 *  Variable-length column-store pages have an array of page entries with RLE counts
 * greater than 1 when reading the page, so it's not necessary to walk the page counting
 * records to find a specific entry. We can do a binary search in this array, then an
 * offset calculation to find the cell.
 *
 * It's a separate structure to keep the page structure as small as possible.
 */
struct __wt_col_var_repeat {
    uint32_t nrepeats;     /* repeat slots */
    WT_COL_RLE repeats[0]; /* lookup RLE array */
};

/*
 * WT_COL_FIX_TW_ENTRY --
 *     This is a single entry in the WT_COL_FIX_TW array. It stores the offset from the page's
 * starting recno and the offset into the page to find the value cell containing the time window.
 */
struct __wt_col_fix_tw_entry {
    uint32_t recno_offset;
    uint32_t cell_offset;
};

/*
 * WT_COL_FIX_TW --
 *     Fixed-length column-store pages carry an array of page entries that have time windows. This
 * is built when reading the page to avoid the need to walk the page to find a specific entry. We
 * can do a binary search in this array instead.
 */
struct __wt_col_fix_tw {
    uint32_t numtws;            /* number of time window slots */
    WT_COL_FIX_TW_ENTRY tws[0]; /* lookup array */
};

/* WT_COL_FIX_TW_CELL gets the cell pointer from a WT_COL_FIX_TW_ENTRY. */
#define WT_COL_FIX_TW_CELL(page, entry) ((WT_CELL *)((uint8_t *)(page)->dsk + (entry)->cell_offset))

#ifdef HAVE_DIAGNOSTIC
/*
 * WT_SPLIT_HIST --
 *	State information of a split at a single point in time.
 */
struct __wt_split_page_hist {
    const char *name;
    const char *func;
    uint64_t split_gen;
    uint32_t entries;
    uint32_t time_sec;
    uint16_t line;
};
#endif

/*
 * WT_PAGE --
 *	The WT_PAGE structure describes the in-memory page information.
 */
struct __wt_page {
    /* Per page-type information. */
    union {
        /*
         * Internal pages (both column- and row-store).
         *
         * In-memory internal pages have an array of pointers to child
         * structures, maintained in collated order.
         *
         * Multiple threads of control may be searching the in-memory
         * internal page and a child page of the internal page may
         * cause a split at any time.  When a page splits, a new array
         * is allocated and atomically swapped into place.  Threads in
         * the old array continue without interruption (the old array is
         * still valid), but have to avoid racing.  No barrier is needed
         * because the array reference is updated atomically, but code
         * reading the fields multiple times would be a very bad idea.
         * Specifically, do not do this:
         *	WT_REF **refp = page->u.intl__index->index;
         *	uint32_t entries = page->u.intl__index->entries;
         *
         * The field is declared volatile (so the compiler knows not to
         * read it multiple times), and we obscure the field name and
         * use a copy macro in all references to the field (so the code
         * doesn't read it multiple times).
         */
        struct {
            WT_REF *parent_ref; /* Parent reference */
            uint64_t split_gen; /* Generation of last split */

            wt_shared WT_PAGE_INDEX *volatile __index; /* Collated children */
        } intl;
#undef pg_intl_parent_ref
#define pg_intl_parent_ref u.intl.parent_ref
#undef pg_intl_split_gen
#define pg_intl_split_gen u.intl.split_gen

/*
 * Macros to copy/set the index because the name is obscured to ensure the field isn't read multiple
 * times.
 *
 * There are two versions of WT_INTL_INDEX_GET because the session split generation is usually set,
 * but it's not always required: for example, if a page is locked for splitting, or being created or
 * destroyed.
 */
#ifdef TSAN_BUILD
/*
 * TSan doesn't detect the acquire/release barriers used in our normal WT_INTL_INDEX_* functions, so
 * use __atomic intrinsics instead. We can use __atomics here as MSVC doesn't support TSan.
 */
#define WT_INTL_INDEX_GET_SAFE(page, pindex)                                   \
    do {                                                                       \
        (pindex) = __atomic_load_n(&(page)->u.intl.__index, __ATOMIC_ACQUIRE); \
    } while (0)
#define WT_INTL_INDEX_GET(session, page, pindex)                          \
    do {                                                                  \
        WT_ASSERT(session, __wt_session_gen(session, WT_GEN_SPLIT) != 0); \
        WT_INTL_INDEX_GET_SAFE(page, (pindex));                           \
    } while (0)
#define WT_INTL_INDEX_SET(page, v)                                        \
    do {                                                                  \
        __atomic_store_n(&(page)->u.intl.__index, (v), __ATOMIC_RELEASE); \
    } while (0)
#else
/* Use WT_ACQUIRE_READ to enforce acquire semantics rather than relying on address dependencies. */
#define WT_INTL_INDEX_GET_SAFE(page, pindex) WT_ACQUIRE_READ((pindex), (page)->u.intl.__index)
#define WT_INTL_INDEX_GET(session, page, pindex)                          \
    do {                                                                  \
        WT_ASSERT(session, __wt_session_gen(session, WT_GEN_SPLIT) != 0); \
        WT_INTL_INDEX_GET_SAFE(page, (pindex));                           \
    } while (0)
#define WT_INTL_INDEX_SET(page, v)                               \
    do {                                                         \
        WT_RELEASE_BARRIER();                                    \
        __wt_atomic_store_pointer(&(page)->u.intl.__index, (v)); \
    } while (0)
#endif

/*
 * Macro to walk the list of references in an internal page.
 */
#define WT_INTL_FOREACH_BEGIN(session, page, ref)                                    \
    do {                                                                             \
        WT_PAGE_INDEX *__pindex;                                                     \
        WT_REF **__refp;                                                             \
        uint32_t __entries;                                                          \
        WT_INTL_INDEX_GET(session, page, __pindex);                                  \
        for (__refp = __pindex->index, __entries = __pindex->entries; __entries > 0; \
             --__entries) {                                                          \
            (ref) = *__refp++;
#define WT_INTL_FOREACH_REVERSE_BEGIN(session, page, ref)                                 \
    do {                                                                                  \
        WT_PAGE_INDEX *__pindex;                                                          \
        WT_REF **__refp;                                                                  \
        uint32_t __entries;                                                               \
        WT_INTL_INDEX_GET(session, page, __pindex);                                       \
        for (__refp = __pindex->index + __pindex->entries, __entries = __pindex->entries; \
             __entries > 0; --__entries) {                                                \
            (ref) = *--__refp;
#define WT_INTL_FOREACH_END \
    }                       \
    }                       \
    while (0)

        /* Row-store leaf page. */
        WT_ROW *row; /* Key/value pairs */
#undef pg_row
#define pg_row u.row

        /* Fixed-length column-store leaf page. */
        struct {
            uint8_t *fix_bitf;     /* Values */
            WT_COL_FIX_TW *fix_tw; /* Time window index */
#define WT_COL_FIX_TWS_SET(page) ((page)->u.col_fix.fix_tw != NULL)
        } col_fix;
#undef pg_fix_bitf
#define pg_fix_bitf u.col_fix.fix_bitf
#undef pg_fix_numtws
#define pg_fix_numtws u.col_fix.fix_tw->numtws
#undef pg_fix_tws
#define pg_fix_tws u.col_fix.fix_tw->tws

        /* Variable-length column-store leaf page. */
        struct {
            WT_COL *col_var;            /* Values */
            WT_COL_VAR_REPEAT *repeats; /* Repeats array */
#define WT_COL_VAR_REPEAT_SET(page) ((page)->u.col_var.repeats != NULL)
        } col_var;
#undef pg_var
#define pg_var u.col_var.col_var
#undef pg_var_repeats
#define pg_var_repeats u.col_var.repeats->repeats
#undef pg_var_nrepeats
#define pg_var_nrepeats u.col_var.repeats->nrepeats
    } u;

    /*
     * Page entry count, page-wide prefix information, type and flags are positioned at the end of
     * the WT_PAGE union to reduce cache misses when searching row-store pages.
     *
     * The entries field only applies to leaf pages, internal pages use the page-index entries
     * instead.
     */
    uint32_t entries; /* Leaf page entries */

    uint32_t prefix_start; /* Best page prefix starting slot */
    uint32_t prefix_stop;  /* Maximum slot to which the best page prefix applies */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_PAGE_BUILD_KEYS 0x0001u         /* Keys have been built in memory */
#define WT_PAGE_COMPACTION_WRITE 0x0002u   /* Writing the page for compaction */
#define WT_PAGE_DISK_ALLOC 0x0004u         /* Disk image in allocated memory */
#define WT_PAGE_DISK_MAPPED 0x0008u        /* Disk image in mapped memory */
#define WT_PAGE_EVICT_LRU 0x0010u          /* Page is on the LRU queue */
#define WT_PAGE_EVICT_LRU_URGENT 0x0020u   /* Page is in the urgent queue */
#define WT_PAGE_EVICT_NO_PROGRESS 0x0040u  /* Eviction doesn't count as progress */
#define WT_PAGE_INTL_OVERFLOW_KEYS 0x0080u /* Internal page has overflow keys (historic only) */
#define WT_PAGE_INTL_PINDEX_UPDATE 0x0100u /* Page index updated */
#define WT_PAGE_PREFETCH 0x0200u           /* The page is being pre-fetched */
#define WT_PAGE_REC_FAIL 0x0400u           /* The previous reconciliation failed on the page. */
#define WT_PAGE_SPLIT_INSERT 0x0800u       /* A leaf page was split for append */
#define WT_PAGE_UPDATE_IGNORE 0x1000u      /* Ignore updates on page discard */
#define WT_PAGE_WITH_DELTAS 0x2000u        /* Page was built with deltas */
                                           /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    wt_shared uint16_t flags_atomic;       /* Atomic flags, use F_*_ATOMIC_16 */

#define WT_PAGE_IS_INTERNAL(page) \
    ((page)->type == WT_PAGE_COL_INT || (page)->type == WT_PAGE_ROW_INT)
#define WT_PAGE_INVALID 0       /* Invalid page */
#define WT_PAGE_BLOCK_MANAGER 1 /* Block-manager page */
#define WT_PAGE_COL_FIX 2       /* Col-store fixed-len leaf */
#define WT_PAGE_COL_INT 3       /* Col-store internal page */
#define WT_PAGE_COL_VAR 4       /* Col-store var-length leaf page */
#define WT_PAGE_OVFL 5          /* Overflow page */
#define WT_PAGE_ROW_INT 6       /* Row-store internal page */
#define WT_PAGE_ROW_LEAF 7      /* Row-store leaf page */
#define WT_PAGE_TYPE_COUNT 8    /* First value beyond valid for checks */
    uint8_t type;               /* Page type */

    /* 1 byte hole expected. */

    wt_shared size_t memory_footprint; /* Memory attached to the page */

    /* Page's on-disk representation: NULL for pages created in memory. */
    const WT_PAGE_HEADER *dsk;

    /* If/when the page is modified, we need lots more information. */
    wt_shared WT_PAGE_MODIFY *modify;

    /*
     * !!!
     * This is the 64 byte boundary, try to keep hot fields above here.
     */

/*
 * The page's read generation acts as an LRU value for each page in the
 * tree; it is used by the eviction server thread to select pages to be
 * discarded from the in-memory tree.
 *
 * The read generation is a 64-bit value, if incremented frequently, a
 * 32-bit value could overflow.
 *
 * The read generation is a piece of shared memory potentially read
 * by many threads.  We don't want to update page read generations for
 * in-cache workloads and suffer the cache misses, so we don't simply
 * increment the read generation value on every access.  Instead, the
 * read generation is incremented by the eviction server each time it
 * becomes active.  To avoid incrementing a page's read generation too
 * frequently, it is set to a future point.
 *
 * Because low read generation values have special meaning, and there
 * are places where we manipulate the value, use an initial value well
 * outside of the special range.
 */
#define WT_READGEN_NOTSET 0
#define WT_READGEN_EVICT_SOON 1
#define WT_READGEN_WONT_NEED 2
#define WT_READGEN_START_VALUE 100
#define WT_READGEN_STEP 100
    uint64_t read_gen;

    uint64_t cache_create_gen; /* Page create timestamp */
    uint64_t evict_pass_gen;   /* Eviction pass generation */

    WT_PAGE_DISAGG_INFO *disagg_info;

#ifdef HAVE_DIAGNOSTIC
#define WT_SPLIT_SAVE_STATE_MAX 3
    WT_SPLIT_PAGE_HIST split_hist[WT_SPLIT_SAVE_STATE_MAX];
    uint64_t splitoff;

#define WT_SPLIT_PAGE_SAVE_STATE(page, session, e, g)                                \
    do {                                                                             \
        (page)->split_hist[(page)->splitoff].name = (session)->name;                 \
        __wt_seconds32((session), &(page)->split_hist[(page)->splitoff].time_sec);   \
        (page)->split_hist[(page)->splitoff].func = __PRETTY_FUNCTION__;             \
        (page)->split_hist[(page)->splitoff].line = (uint16_t)__LINE__;              \
        (page)->split_hist[(page)->splitoff].split_gen = (uint32_t)(g);              \
        (page)->split_hist[(page)->splitoff].entries = (uint32_t)(e);                \
        (page)->splitoff = ((page)->splitoff + 1) % WT_ELEMENTS((page)->split_hist); \
    } while (0)
#else
#define WT_SPLIT_PAGE_SAVE_STATE(page, session, e, g)
#endif
};

/*
 * WT_PAGE_DISK_OFFSET, WT_PAGE_REF_OFFSET --
 *	Return the offset/pointer of a pointer/offset in a page disk image.
 */
#define WT_PAGE_DISK_OFFSET(page, p) WT_PTRDIFF32(p, (page)->dsk)
#define WT_PAGE_REF_OFFSET(page, o) ((void *)((uint8_t *)((page)->dsk) + (o)))

/*
 * WT_PAGE_WALK_SKIP_STATS --
 *	Statistics to track how many deleted pages are skipped as part of the tree walk.
 */
struct __wt_page_walk_skip_stats {
    size_t total_del_pages_skipped;
    size_t total_inmem_del_pages_skipped;
};

/*
 * Type used by WT_REF::state and valid values.
 *
 * Declared well before __wt_ref struct as the type is used in other structures in this header.
 */
typedef uint8_t WT_REF_STATE;

#define WT_REF_DISK 0    /* Page is on disk */
#define WT_REF_DELETED 1 /* Page is on disk, but deleted */
#define WT_REF_LOCKED 2  /* Page locked for exclusive access */
#define WT_REF_MEM 3     /* Page is in cache and valid */
#define WT_REF_SPLIT 4   /* Parent page split (WT_REF dead) */

/*
 * Prepare states.
 *
 * Prepare states are used by both updates and the fast truncate page_del structure to indicate
 * which phase of the prepared transaction lifecycle they are in.
 *
 * WT_PREPARE_INIT:
 *  The default prepare state, indicating that the transaction which created the update or fast
 *  truncate structure has not yet called prepare transaction. There is no requirement that an
 *  object moves beyond this state.
 *
 *  This state has no impact on the visibility of the associated update or fast truncate structure.
 *
 * WT_PREPARE_INPROGRESS:
 *  When a transaction calls prepare all objects created by it, updates or page_del structures will
 *  transition to this state.
 *
 * WT_PREPARE_LOCKED:
 *  This state is a transitional state between INPROGRESS and RESOLVED. It occurs when a prepared
 *  transaction commits. If a reader sees an object in this state it is required to wait until the
 *  object transitions out of this state, and then read the associated visibility information. This
 *  state exists to provide a clear ordering semantic between transitioning the prepare state and
 *  a reader reading it concurrently. For more details see below.
 *
 * WT_PREPARE_RESOLVED:
 *  When a prepared transaction calls commit all objects created by it will transition to this
 *  state, which is the final state.
 *
 * ---
 * State transitions:
 * Object created (uncommitted) -> transaction prepare -> transaction commit:
 *  INIT --> INPROGRESS --> LOCKED --> RESOLVED
 *  LOCKED will be a momentary phase during timestamp update.
 *
 * Object created -> transaction prepare -> transaction rollback:
 *  INIT --> INPROGRESS
 *  The prepare state will not be modified during rollback.
 *
 * Object created -> transaction commit:
 *  INIT
 *  Preparing a transaction is the uncommon case and most updates and page_del structures will
 *  never leave the INIT state.
 *
 * ---
 * Ordering complexities of prepare state transitions.
 *
 * The prepared transaction system works alongside the timestamp system. When committing a
 * transaction a user can define a timestamp which represents the time at which a key/value pair
 * is visible. This is represented by the start_ts in memory on the update structure. The page_del
 * structure visibility is the same as an update's for the most part, this comment won't refer to it
 * again.
 *
 * Prepared transactions introduce the need for an additional two timestamps, a prepare timestamp.
 * The point at which an update is "prepared" and the durable timestamp which is when update is to
 * be made stable by WiredTiger. The purpose of those timestamps is not something that will be
 * described here.
 *
 * When a transaction calls prepare the start_ts field on the update structure is used to represent
 * the prepare timestamp. The start_ts field is also used to represent the commit timestamp of the
 * transaction. The prepare state tells the reader which of the two timestamps are available.
 * However these two fields can't be written atomically and no lock is taken when transitioning
 * between prepared update states. So the writer must write one field then the other, and a reader
 * can read at any time. This introduces a need for the WT_PREPARE_LOCKED state and memory barriers.
 *
 * Let's construct a simplified example:
 * We can assume the writing thread does the sane thing by writing the timestamp and then the
 * prepare state. We also assume the reader thread reads the state then the timestamp. If we ignore
 * the locked state we can construct a scenario where the reader can't tell which timestamp, commit
 * or prepare, exists on the update. Ignoring CPU instruction reordering for now.
 *
 * The writer performs the following operations:
 *  1: start_ts = 5
 *  2: prepare_state = WT_PREPARE_INPROGRESS
 *  3: start_ts = 10
 *  4: prepare_state = WT_PREPARE_RESOLVED
 *
 * In this scenario it is possible the writer thread context switches between 3 and 4, and therefore
 * the reader may see a timestamp of 10 for start_ts and wrongly attribute it to the prepare
 * timestamp. This creates a need for the WT_PREPARE_LOCKED state. The writer thread now performs:
 *  1: start_ts = 5
 *  2: prepare_state = WT_PREPARE_INPROGRESS
 *  3: prepare_state = WT_PREPARE_LOCKED
 *  3: start_ts = 10
 *  4: prepare_state = WT_PREPARE_RESOLVED
 *
 * The reader thread can, in this scenario, read any prepare state and behave correctly. If it reads
 * WT_PREPARE_INPROGRESS it knows the start_ts is the prepare timestamp. If it reads
 * WT_PREPARE_RESOLVED it knows the start_ts is the commit timestamp. If it reads WT_PREPARE_LOCKED
 * it will wait until it reads WT_PREPARE_RESOLVED.
 *
 * By introducing the WT_PREPARE_LOCKED field we resolve some ambiguity about the start_ts. However
 * as previously mentioned we were ignoring CPU reordering hijinx. CPU reordering will cause issues,
 * to be fully correct here we need memory barriers. The need for a WT_PREPARE_LOCKED state makes
 * the ordering requirements somewhat more complex than the typical message passing scenario.
 *
 * The writer thread has two orderings:
 * Prepare transaction:
 *  - start_ts = X
 *  - WT_RELEASE_BARRIER
 *  - prepare_state = WT_PREPARE_INPROGRESS
 *
 * Commit transaction:
 *  - prepare_state = WT_PREPARE_LOCKED
 *  - WT_RELEASE_BARRIER
 *  - start_ts = Y
 *  - durable_ts = Z
 *  - WT_RELEASE_BARRIER
 *  - prepare_state = WT_PREPARE_RESOLVED
 *
 * The reader does the opposite. The more complex of the two is as follows:
 *  - read prepare_state
 *  - WT_ACQUIRE_BARRIER
 *  - if locked, retry
 *  - read start_ts
 *  - read durable_ts
 *  - WT_ACQUIRE_BARRIER
 *  - read prepare_state
 *  - if prepare state has changed, retry
 */

/* Must be 0, as structures will be default initialized with 0. */
#define WT_PREPARE_INIT (uint8_t)0
#define WT_PREPARE_INPROGRESS (uint8_t)1
#define WT_PREPARE_LOCKED (uint8_t)2
#define WT_PREPARE_RESOLVED (uint8_t)3

/*
 * Page state.
 *
 * Synchronization is based on the WT_REF->state field, which has a number of
 * possible states:
 *
 * WT_REF_DISK:
 *	The initial setting before a page is brought into memory, and set as a
 *	result of page eviction; the page is on disk, and must be read into
 *	memory before use.  WT_REF_DISK has a value of 0 (the default state
 *	after allocating cleared memory).
 *
 * WT_REF_DELETED:
 *	The page is on disk, but has been deleted from the tree; we can delete
 *	row-store and VLCS leaf pages without reading them if they don't
 *	reference overflow items.
 *
 * WT_REF_LOCKED:
 *	Locked for exclusive access.  In eviction, this page or a parent has
 *	been selected for eviction; once hazard pointers are checked, the page
 *	will be evicted.  When reading a page that was previously deleted, it
 *	is locked until the page is in memory and the deletion has been
 *      instantiated with tombstone updates. The thread that set the page to
 *      WT_REF_LOCKED has exclusive access; no other thread may use the WT_REF
 *      until the state is changed.
 *
 * WT_REF_MEM:
 *	Set by a reading thread once the page has been read from disk; the page
 *	is in the cache and the page reference is OK.
 *
 * WT_REF_SPLIT:
 *	Set when the page is split; the WT_REF is dead and can no longer be
 *	used.
 *
 * The life cycle of a typical page goes like this: pages are read into memory
 * from disk and their state set to WT_REF_MEM.  When the page is selected for
 * eviction, the page state is set to WT_REF_LOCKED.  In all cases, evicting
 * threads reset the page's state when finished with the page: if eviction was
 * successful (a clean page was discarded, and a dirty page was written to disk
 * and then discarded), the page state is set to WT_REF_DISK; if eviction failed
 * because the page was busy, page state is reset to WT_REF_MEM.
 *
 * Readers check the state field and if it's WT_REF_MEM, they set a hazard
 * pointer to the page, flush memory and re-confirm the page state.  If the
 * page state is unchanged, the reader has a valid reference and can proceed.
 *
 * When an evicting thread wants to discard a page from the tree, it sets the
 * WT_REF_LOCKED state, flushes memory, then checks hazard pointers.  If a
 * hazard pointer is found, state is reset to WT_REF_MEM, restoring the page
 * to the readers.  If the evicting thread does not find a hazard pointer,
 * the page is evicted.
 */

/*
 * WT_PAGE_DELETED --
 *	Information about how they got deleted for deleted pages. This structure records the
 *      transaction that deleted the page, plus the state the ref was in when the deletion happened.
 *      This structure is akin to an update but applies to a whole page.
 */
struct __wt_page_deleted {
    /*
     * Transaction IDs are set when updates are created (before they become visible) and only change
     * when marked with WT_TXN_ABORTED. Transaction ID readers expect to copy a transaction ID into
     * a local variable and see a stable value. In case a compiler might re-read the transaction ID
     * from memory rather than using the local variable, mark the shared transaction IDs volatile to
     * prevent unexpected repeated/reordered reads.
     */
    wt_shared volatile uint64_t txnid; /* Transaction ID */

    union {
        struct {
            wt_timestamp_t durable_ts; /* timestamps */
            wt_timestamp_t start_ts;
        } commit;

        struct {
            wt_timestamp_t rollback_ts; /* rollback timestamp */
            uint64_t saved_txnid;       /* transaction id before rollback */
        } prepare_rollback;
    } u;

#define pg_del_durable_ts u.commit.durable_ts
#define pg_del_start_ts u.commit.start_ts
#define pg_del_rollback_ts u.prepare_rollback.rollback_ts
#define pg_del_saved_txnid u.prepare_rollback.saved_txnid

    /* Prepared transaction fields */
    uint64_t prepared_id;
    wt_timestamp_t prepare_ts;

    /*
     * The prepare state is used for transaction prepare to manage visibility and propagating the
     * prepare state to the updates generated at instantiation time.
     */
    wt_shared volatile uint8_t prepare_state;

    /*
     * If the fast-truncate transaction has committed. If we're forced to instantiate the page, and
     * the committed flag isn't set, we have to create an update structure list for the transaction
     * to resolve in a subsequent commit. (This is tricky: if the transaction is rolled back, the
     * entire structure is discarded, that is, the flag is set only on commit and not on rollback.)
     */
    bool committed;

    /* Flag to indicate fast-truncate is written to disk. */
    bool selected_for_write;
};

/*
 * A location in a file is a variable-length cookie, but it has a maximum size so it's easy to
 * create temporary space in which to store them. (Locations can't be much larger than this anyway,
 * they must fit onto the minimum size page because a reference to an overflow page is itself a
 * location.)
 */
#define WT_ADDR_MAX_COOKIE 255 /* Maximum address cookie */

/*
 * WT_ADDR_COPY --
 *	We have to lock the WT_REF to look at a WT_ADDR: a structure we can use to quickly get a
 * copy of the WT_REF address information.
 */
struct __wt_addr_copy {
    uint8_t type;

    uint8_t addr[WT_ADDR_MAX_COOKIE];
    uint8_t size;

    WT_TIME_AGGREGATE ta;

    WT_PAGE_DELETED del; /* Fast-truncate page information */
    bool del_set;
};

/*
 * WT_REF_HIST --
 *	State information of a ref at a single point in time.
 */
struct __wt_ref_hist {
    WT_SESSION_IMPL *session;
    const char *name;
    const char *func;
    uint32_t time_sec;
    uint16_t line;
    uint16_t state;
};

/*
 * WT_PREFETCH_QUEUE_ENTRY --
 *	Queue entry for pages queued for pre-fetch.
 */
struct __wt_prefetch_queue_entry {
    WT_REF *ref;
    WT_PAGE *first_home;
    WT_DATA_HANDLE *dhandle;
    TAILQ_ENTRY(__wt_prefetch_queue_entry) q; /* List of pages queued for pre-fetch. */
};

/*
 * WT_REF --
 *	A single in-memory page and state information.
 */
struct __wt_ref {
    wt_shared WT_PAGE *page; /* Page */

    /*
     * When the tree deepens as a result of a split, the home page value changes. Don't cache it, we
     * need to see that change when looking up our slot in the page's index structure.
     */
    wt_shared WT_PAGE *volatile home;        /* Reference page */
    wt_shared volatile uint32_t pindex_hint; /* Reference page index hint */

    /*
     * A counter used to track how many times a ref has changed during internal page reconciliation.
     * The value is compared and swapped to 0 for each internal page reconciliation. If the counter
     * has a value greater than zero, this implies that the ref has been changed concurrently and
     * that the ref remains dirty after internal page reconciliation. It is possible for other
     * operations such as page splits and fast-truncate to concurrently write new values to the ref,
     * but depending on timing or race conditions, it cannot be guaranteed that these new values are
     * included as part of the reconciliation. The page would need to be reconciled again to ensure
     * that these modifications are included.
     */
    wt_shared volatile uint8_t ref_changes;

/*
 * Define both internal- and leaf-page flags for now: we only need one, but it provides an easy way
 * to assert a page-type flag is always set (we allocate WT_REFs in lots of places and it's easy to
 * miss one). If we run out of bits in the flags field, remove the internal flag and rewrite tests
 * depending on it to be "!leaf" instead.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_REF_FLAG_INTERNAL 0x1u     /* Page is an internal page */
#define WT_REF_FLAG_LEAF 0x2u         /* Page is a leaf page */
#define WT_REF_FLAG_REC_MULTIPLE 0x4u /* Page has been split in reconciliation */
                                      /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_REF_FLAG_PREFETCH 0x1u   /* Page is on the pre-fetch queue */
#define WT_REF_FLAG_READING 0x2u    /* Page is being read in */
                                    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    wt_shared uint8_t flags_atomic; /* Atomic flags, use F_*_ATOMIC_8 */

    /*
     * Ref state: Obscure the field name as this field shouldn't be accessed directly. The public
     * interface is made up of five functions:
     *  - WT_REF_GET_STATE
     *  - WT_REF_SET_STATE
     *  - WT_REF_CAS_STATE
     *  - WT_REF_LOCK
     *  - WT_REF_UNLOCK
     *
     * For more details on these functions see ref_inline.h.
     */
    wt_shared volatile WT_REF_STATE __state;

    /*
     * Address: on-page cell if read from backing block, off-page WT_ADDR if instantiated in-memory,
     * or NULL if page created in-memory.
     */
    wt_shared void *addr;

    /*
     * The child page's key.  Do NOT change this union without reviewing
     * __wt_ref_key.
     */
    union {
        uint64_t recno;       /* Column-store: starting recno */
        wt_shared void *ikey; /* Row-store: key */
    } key;
#undef ref_recno
#define ref_recno key.recno
#undef ref_ikey
#define ref_ikey key.ikey

    /*
     * Page deletion information, written-to/read-from disk as necessary in the internal page's
     * address cell. (Deleted-address cells are also referred to as "proxy cells".) When a WT_REF
     * first becomes part of a fast-truncate operation, the page_del field is allocated and
     * initialized; it is similar to an update and holds information about the transaction that
     * performed the truncate. It can be discarded and set to NULL when that transaction reaches
     * global visibility.
     *
     * Operations other than truncate that produce deleted pages (checkpoint cleanup, reconciliation
     * as empty, etc.) leave the page_del field NULL as in these cases the deletion is already
     * globally visible.
     *
     * Once the deletion is globally visible, the original on-disk page is no longer needed and can
     * be discarded; this happens the next time the parent page is reconciled, either by eviction or
     * by a checkpoint. The ref remains, however, and still occupies the same key space in the table
     * that it always did.
     *
     * Deleted refs (and thus chunks of the tree namespace) are only discarded at two points: when
     * the parent page is discarded after being evicted, or in the course of internal page splits
     * and reverse splits. Until this happens, the "same" page can be brought back to life by
     * writing to its portion of the key space.
     *
     * A deleted page needs to be "instantiated" (read in from disk and converted to an in-memory
     * page where every item on the page has been individually deleted) if we need to position a
     * cursor on the page, or if we need to visit it for other reasons. Logic exists to avoid that
     * in various common cases (see: __wt_btcur_skip_page, __wt_delete_page_skip) but in many less
     * common situations we proceed with instantiation anyway to avoid multiplying the number of
     * special cases in the system.
     *
     * Common triggers for instantiation include: another thread reading from the page before a
     * truncate commits; an older reader visiting a page after a truncate commits; a thread reading
     * the page via a checkpoint cursor if the truncation wasn't yet globally visible at checkpoint
     * time; a thread reading the page after shutdown and restart under similar circumstances; RTS
     * needing to roll back a committed but unstable truncation (and possibly also updates that
     * occurred before the truncation); and a thread writing to the truncated portion of the table
     * space after the truncation but before the page is completely discarded.
     *
     * If the page must be instantiated for any reason: (1) for each entry on the page a WT_UPDATE
     * is created; (2) the transaction information from page_del is copied to those WT_UPDATE
     * structures (making them a match for the truncate operation), and (3) the WT_REF state
     * switches to WT_REF_MEM.
     *
     * If the fast-truncate operation has not yet committed, an array of references to the WT_UPDATE
     * structures is placed in modify->inst_updates. This is used to find the updates when the
     * operation subsequently resolves. (The page can split, so there needs to be some way to find
     * all of the update structures.)
     *
     * After instantiation, the page_del structure is kept until the instantiated page is next
     * reconciled. This is because in some cases reconciliation of the parent internal page may need
     * to write out a reference to the pre-instantiated on-disk page, at which point the page_del
     * information is needed to build the correct reference.
     *
     * If the ref is in WT_REF_DELETED state, all actions besides checking whether page_del is NULL
     * require that the WT_REF be locked. There are two reasons for this: first, the page might be
     * instantiated at any time, and it is important to not see a partly-completed instantiation;
     * and second, the page_del structure is discarded opportunistically if its transaction is found
     * to be globally visible, so accessing it without locking the ref is unsafe.
     *
     * If the ref is in WT_REF_MEM state because it has been instantiated, the safety requirements
     * are somewhat looser. Checking for an instantiated page by examining modify->instantiated does
     * not require locking. Checking if modify->inst_updates is non-NULL (which means that the
     * truncation isn't committed) also doesn't require locking. In general the page_del structure
     * should not be used after instantiation; exceptions are (a) it is still updated by transaction
     * prepare, commit, and rollback (so that it remains correct) and (b) it is used by internal
     * page reconciliation if that occurs before the instantiated child is itself reconciled. (The
     * latter can only happen if the child is evicted in a fairly narrow time window during a
     * checkpoint.) This still requires locking the ref.
     *
     * It is vital to consider all the possible cases when touching a deleted or instantiated page.
     *
     * There are two major groups of states:
     *
     * 1. The WT_REF state is WT_REF_DELETED. This means the page is deleted and not in memory.
     *    - If the page has no disk address, the ref is a placeholder in the key space and may in
     *      general be discarded at the next opportunity. (Some restrictions apply in VLCS.)
     *    - If the page has a disk address, page_del may be NULL. In this case, the deletion of the
     *      page is globally visible and the on-disk page can be discarded at the next opportunity.
     *    - If the page has a disk address and page_del is not NULL, page_del contains information
     *      about the transaction that deleted the page. It is necessary to lock the ref to read
     *      page_del; at that point (if the state hasn't changed while getting the lock)
     *      page_del->committed can be used to check if the transaction is committed or not.
     *
     * 2. The WT_REF state is WT_REF_MEM. The page is either an ordinary page or an instantiated
     * deleted page.
     *    - If ref->page->modify is NULL, the page is ordinary.
     *    - If ref->page->modify->instantiated is false and ref->page->modify->inst_updates is NULL,
     *      the page is ordinary.
     *    - If ref->page->modify->instantiated is true, the page is instantiated and has not yet
     *      been reconciled. ref->page_del is either NULL (meaning the deletion is globally visible)
     *      or contains information about the transaction that deleted the page. This information is
     *      only meaningful either (a) in relation to the existing on-disk page rather than the in-
     *      memory page (this can be needed to reconcile the parent internal page) or (b) if the
     *      page is clean.
     *    - If ref->page->modify->inst_updates is not NULL, the page is instantiated and the
     *      transaction that deleted it has not resolved yet. The update list is used during commit
     *      or rollback to find the updates created during instantiation.
     *
     * The last two points of group (2) are orthogonal; that is, after instantiation the
     * instantiated flag and page_del structure (on the one hand) and the update list (on the other)
     * are used and discarded independently. The former persists only until the page is first
     * successfully reconciled; the latter persists until the transaction resolves. These events may
     * occur in either order.
     *
     * As described above, in any state in group (1) an access to the page may require it be read
     * into memory, at which point it moves into group (2). Instantiation always sets the
     * instantiated flag to true; the updates list is only created if the transaction has not yet
     * resolved at the point instantiation happens. (The ref is locked in both transaction
     * resolution and instantiation to make sure these events happen in a well-defined order.)
     *
     * Because internal pages with uncommitted (including prepared) deletions are not written to
     * disk, a page instantiated after its parent was read from disk will always have inst_updates
     * set to NULL.
     */
    wt_shared WT_PAGE_DELETED *page_del; /* Page-delete information for a deleted page. */

#ifdef HAVE_REF_TRACK
#define WT_REF_SAVE_STATE_MAX 3
    /* Capture history of ref state changes. */
    WT_REF_HIST hist[WT_REF_SAVE_STATE_MAX];
    uint64_t histoff;
#endif
};

#ifdef HAVE_REF_TRACK
/*
 * In DIAGNOSTIC mode we overwrite the WT_REF on free to force failures, but we want to retain ref
 * state history. Don't overwrite these fields.
 */
#define WT_REF_CLEAR_SIZE (offsetof(WT_REF, hist))
/*
 * WT_REF_SIZE is the expected structure size -- we verify the build to ensure the compiler hasn't
 * inserted padding which would break the world.
 */
#define WT_REF_SIZE (48 + WT_REF_SAVE_STATE_MAX * sizeof(WT_REF_HIST) + 8)
#else
#define WT_REF_SIZE 48
#define WT_REF_CLEAR_SIZE (sizeof(WT_REF))
#endif

/*
 * WT_ROW --
 * Each in-memory page row-store leaf page has an array of WT_ROW structures:
 * this is created from on-page data when a page is read from the file.  It's
 * sorted by key, fixed in size, and starts with a reference to on-page data.
 *
 * Multiple threads of control may be searching the in-memory row-store pages,
 * and the key may be instantiated at any time.  Code must be able to handle
 * both when the key has not been instantiated (the key field points into the
 * page's disk image), and when the key has been instantiated (the key field
 * points outside the page's disk image).  We don't need barriers because the
 * key is updated atomically, but code that reads the key field multiple times
 * is a very, very bad idea.  Specifically, do not do this:
 *
 *	key = rip->key;
 *	if (key_is_on_page(key)) {
 *		cell = rip->key;
 *	}
 *
 * The field is declared volatile (so the compiler knows it shouldn't read it
 * multiple times), and we obscure the field name and use a copy macro in all
 * references to the field (so the code doesn't read it multiple times), all
 * to make sure we don't introduce this bug (again).
 */
struct __wt_row { /* On-page key, on-page cell, or off-page WT_IKEY */
    wt_shared void *volatile __key;
};
#define WT_ROW_KEY_COPY(rip) ((rip)->__key)
#define WT_ROW_KEY_SET(rip, v) ((rip)->__key) = (void *)(v)

/*
 * WT_ROW_FOREACH --
 *	Walk the entries of an in-memory row-store leaf page.
 */
#define WT_ROW_FOREACH(page, rip, i) \
    for ((i) = (page)->entries, (rip) = (page)->pg_row; (i) > 0; ++(rip), --(i))
#define WT_ROW_FOREACH_REVERSE(page, rip, i)                                             \
    for ((i) = (page)->entries, (rip) = (page)->pg_row + ((page)->entries - 1); (i) > 0; \
         --(rip), --(i))

/*
 * WT_ROW_SLOT --
 *	Return the 0-based array offset based on a WT_ROW reference.
 */
#define WT_ROW_SLOT(page, rip) ((uint32_t)((rip) - (page)->pg_row))

/*
 * WT_COL -- Each in-memory variable-length column-store leaf page has an array of WT_COL
 * structures: this is created from on-page data when a page is read from the file. It's fixed in
 * size, and references data on the page.
 */
struct __wt_col {
    /*
     * Variable-length column-store data references are page offsets, not pointers (we boldly
     * re-invent short pointers). The trade-off is 4B per K/V pair on a 64-bit machine vs. a single
     * cycle for the addition of a base pointer. The on-page data is a WT_CELL (same as row-store
     * pages).
     *
     * Obscure the field name, code shouldn't use WT_COL->__col_value, the public interface is
     * WT_COL_PTR and WT_COL_PTR_SET.
     */
    uint32_t __col_value;
};

/*
 * WT_COL_PTR, WT_COL_PTR_SET --
 *	Return/Set a pointer corresponding to the data offset. (If the item does
 * not exist on the page, return a NULL.)
 */
#define WT_COL_PTR(page, cip) WT_PAGE_REF_OFFSET(page, (cip)->__col_value)
#define WT_COL_PTR_SET(cip, value) (cip)->__col_value = (value)

/*
 * WT_COL_FOREACH --
 *	Walk the entries of variable-length column-store leaf page.
 */
#define WT_COL_FOREACH(page, cip, i) \
    for ((i) = (page)->entries, (cip) = (page)->pg_var; (i) > 0; ++(cip), --(i))

/*
 * WT_COL_SLOT --
 *	Return the 0-based array offset based on a WT_COL reference.
 */
#define WT_COL_SLOT(page, cip) ((uint32_t)((cip) - (page)->pg_var))

/*
 * WT_IKEY --
 *  Instantiated key: row-store keys are usually prefix compressed or overflow objects.
 *  Normally, a row-store page in-memory key points to the on-page WT_CELL, but in some
 *  cases, we instantiate the key in memory, in which case the row-store page in-memory
 *  key points to a WT_IKEY structure.
 */
struct __wt_ikey {
    uint32_t size; /* Key length */

    /*
     * If we no longer point to the key's on-page WT_CELL, we can't find its
     * related value.  Save the offset of the key cell in the page.
     *
     * Row-store cell references are page offsets, not pointers (we boldly
     * re-invent short pointers).  The trade-off is 4B per K/V pair on a
     * 64-bit machine vs. a single cycle for the addition of a base pointer.
     */
    uint32_t cell_offset;

/* The key bytes immediately follow the WT_IKEY structure. */
#define WT_IKEY_DATA(ikey) ((void *)((uint8_t *)(ikey) + sizeof(WT_IKEY)))
};

/*
 * WT_UPDATE --
 *
 * Entries on leaf pages can be updated, either modified or deleted. Updates to entries in the
 * WT_ROW and WT_COL arrays are stored in the page's WT_UPDATE array. When the first element on a
 * page is updated, the WT_UPDATE array is allocated, with one slot for every existing element in
 * the page. A slot points to a WT_UPDATE structure; if more than one update is done for an entry,
 * WT_UPDATE structures are formed into a forward-linked list.
 */
struct __wt_update {
    /*
     * Transaction IDs are set when updates are created (before they become visible) and only change
     * when marked with WT_TXN_ABORTED. Transaction ID readers expect to copy a transaction ID into
     * a local variable and see a stable value. In case a compiler might re-read the transaction ID
     * from memory rather than using the local variable, mark the shared transaction IDs volatile to
     * prevent unexpected repeated/reordered reads.
     */
    wt_shared volatile uint64_t txnid; /* transaction ID */

    union {
        struct {
            wt_timestamp_t durable_ts; /* timestamps */
            wt_timestamp_t start_ts;
        } commit;

        struct {
            wt_timestamp_t rollback_ts; /* rollback timestamp */
            uint64_t saved_txnid;       /* transaction id before rollback */
        } prepare_rollback;
    } u;

#undef upd_durable_ts
#define upd_durable_ts u.commit.durable_ts
#undef upd_start_ts
#define upd_start_ts u.commit.start_ts
#undef upd_rollback_ts
#define upd_rollback_ts u.prepare_rollback.rollback_ts
#undef upd_saved_txnid
#define upd_saved_txnid u.prepare_rollback.saved_txnid

    /*
     * When transaction is prepared, both prepare_ts and start_ts should be assigned to prepare
     * timestamp. After commit, start_ts will store the commit_ts.
     */
    uint64_t prepared_id;
    wt_timestamp_t prepare_ts;

    /*
     * The durable timestamp of the previous update in the update chain. This timestamp is used for
     * diagnostic checks only, and could be removed to reduce the size of the structure should that
     * be necessary.
     */
    wt_timestamp_t prev_durable_ts;

    wt_shared WT_UPDATE *next; /* forward-linked list */

    uint32_t size; /* data length */

#define WT_UPDATE_INVALID 0   /* diagnostic check */
#define WT_UPDATE_MODIFY 1    /* partial-update modify value */
#define WT_UPDATE_RESERVE 2   /* reserved */
#define WT_UPDATE_STANDARD 3  /* complete value */
#define WT_UPDATE_TOMBSTONE 4 /* deleted */
    uint8_t type; /* type (one byte to conserve memory); also read-only after initialization */

/* If the update includes a complete value. */
#define WT_UPDATE_DATA_VALUE(upd) \
    ((upd)->type == WT_UPDATE_STANDARD || (upd)->type == WT_UPDATE_TOMBSTONE)

    /*
     * The update state is used for transaction prepare to manage visibility and transitioning
     * update structure state safely.
     */
    wt_shared volatile uint8_t prepare_state; /* prepare state */

/* When introducing a new flag, consider adding it to WT_UPDATE_SELECT_FOR_DS. */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_UPDATE_DELETE_DURABLE 0x0001u  /* Key has been removed from disk image. */
#define WT_UPDATE_DS 0x0002u              /* Update has been chosen to the data store. */
#define WT_UPDATE_DURABLE 0x0004u         /* Update has been durable. */
#define WT_UPDATE_HS 0x0008u              /* Update has been written to hs. */
#define WT_UPDATE_HS_MAX_STOP 0x0010u     /* Update has been written to hs with a max stop. */
#define WT_UPDATE_PREPARE_DURABLE 0x0020u /* Prepared update has been durable. */
#define WT_UPDATE_PREPARE_RESTORED_FROM_DS 0x0040u /* Prepared update restored from data store. */
#define WT_UPDATE_PREPARE_ROLLBACK 0x0080u /* Tombstone that rolled back by a prepared update.*/
#define WT_UPDATE_RESTORED_FAST_TRUNCATE 0x0100u /* Fast truncate instantiation. */
#define WT_UPDATE_RESTORED_FROM_DELTA 0x0200u    /* Update restored from delta. */
#define WT_UPDATE_RESTORED_FROM_DS 0x0400u       /* Update restored from data store. */
#define WT_UPDATE_RESTORED_FROM_HS 0x0800u       /* Update restored from history store. */
#define WT_UPDATE_RTS_DRYRUN_ABORT 0x1000u       /* Used by dry run to mark a would-be abort. */
                                                 /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    uint16_t flags;

/* There are several cases we should select the update irrespective of visibility to write to the
 * disk image:
 *
 * 1. A previous reconciliation selected this update as writing anything that is older
 * undoes the previous work.
 *
 * 2. The update is restored from the disk image as writing anything that is older undoes
 * the previous work.
 *
 * 3. An earlier reconciliation performed an update-restore eviction and this update was
 * restored from disk.
 *
 * 4. We rolled back a prepared transaction and restored an update from the history store.
 *
 * 5. We rolled back a prepared transaction and aim to delete the following update from the
 * history store.
 *
 * These scenarios can happen if the current reconciliation has a limited visibility of
 * updates compared to one of the previous reconciliations. This is important as it is never
 * ok to undo the work of the previous reconciliations.
 */
#define WT_UPDATE_SELECT_FOR_DS                                                      \
    WT_UPDATE_DS | WT_UPDATE_PREPARE_RESTORED_FROM_DS | WT_UPDATE_RESTORED_FROM_DS | \
      WT_UPDATE_RESTORED_FROM_HS | WT_UPDATE_RESTORED_FROM_DELTA
    /*
     * Zero or more bytes of value (the payload) immediately follows the WT_UPDATE structure. We use
     * a C99 flexible array member which has the semantics we want.
     */
    uint8_t data[]; /* start of the data */
};

/*
 * WT_UPDATE_SIZE is the expected structure size excluding the payload data -- we verify the build
 * to ensure the compiler hasn't inserted padding.
 */
#define WT_UPDATE_SIZE 64

/*
 * If there is no value, ensure that the memory allocation size matches that returned by sizeof().
 * Otherwise bit-exact tools like MSan may infer the structure is not completely initialized.
 */
#define WT_UPDATE_SIZE_NOVALUE (sizeof(struct __wt_update))

/*
 * The memory size of an update: include some padding because this is such a common case that
 * overhead of tiny allocations can swamp our cache overhead calculation.
 */
#define WT_UPDATE_MEMSIZE(upd) WT_ALIGN(WT_UPDATE_SIZE + (upd)->size, 32)

/*
 * WT_UPDATE_VALUE --
 *
 * A generic representation of an update's value regardless of where it exists. This structure is
 * used to represent both in-memory updates and updates that don't exist in an update list such as
 * reconstructed modify updates, updates in the history store and onpage values.
 *
 * The skip buffer flag is an optimization for callers of various read functions to communicate that
 * they just want to check that an update exists and not read its underlying value. This means that
 * the read functions can avoid the performance penalty of reconstructing modifies.
 */
struct __wt_update_value {
    WT_ITEM buf;
    WT_TIME_WINDOW tw;
    uint8_t type;
    bool skip_buf;
};

/*
 * WT_WITH_UPDATE_VALUE_SKIP_BUF --
 *
 * A helper macro to use for calling read functions when we're checking for the existence of a given
 * key. This means that read functions can avoid the performance penalty of reconstructing modifies.
 */
#define WT_WITH_UPDATE_VALUE_SKIP_BUF(op) \
    do {                                  \
        cbt->upd_value->skip_buf = true;  \
        op;                               \
        cbt->upd_value->skip_buf = false; \
    } while (0)

/*
 * WT_MODIFY_UPDATE_MIN/MAX, WT_MODIFY_VECTOR_STACK_SIZE
 *	Limit update chains value to avoid penalizing reads and permit truncation. Having a smaller
 * value will penalize the cases when history has to be maintained, resulting in multiplying cache
 * pressure.
 *
 * When threads race modifying a record, we can end up with more than the usual maximum number of
 * modifications in an update list. We use small vectors of modify updates in a couple of places to
 * avoid heap allocation, add a few additional slots to that array.
 */
#define WT_MODIFY_UPDATE_MIN 10  /* Update count before we bother checking anything else */
#define WT_MODIFY_UPDATE_MAX 200 /* Update count hard limit */
#define WT_UPDATE_VECTOR_STACK_SIZE (WT_MODIFY_UPDATE_MIN + 10)

/*
 * WT_UPDATE_VECTOR --
 * 	A resizable array for storing updates. The allocation strategy is similar to that of
 *	llvm::SmallVector<T> where we keep space on the stack for the regular case but fall back to
 *	dynamic allocation as needed.
 */
struct __wt_update_vector {
    WT_SESSION_IMPL *session;
    WT_UPDATE *list[WT_UPDATE_VECTOR_STACK_SIZE];
    WT_UPDATE **listp;
    size_t allocated_bytes;
    size_t size;
};

/*
 * WT_MODIFY_MEM_FRACTION
 *	Limit update chains to a fraction of the base document size.
 */
#define WT_MODIFY_MEM_FRACTION 10

/*
 * WT_INSERT --
 *
 * Row-store leaf pages support inserts of new K/V pairs. When the first K/V pair is inserted, the
 * WT_INSERT_HEAD array is allocated, with one slot for every existing element in the page, plus one
 * additional slot. A slot points to a WT_INSERT_HEAD structure for the items which sort after the
 * WT_ROW element that references it and before the subsequent WT_ROW element; the skiplist
 * structure has a randomly chosen depth of next pointers in each inserted node.
 *
 * The additional slot is because it's possible to insert items smaller than any existing key on the
 * page: for that reason, the first slot of the insert array holds keys smaller than any other key
 * on the page.
 *
 * In column-store variable-length run-length encoded pages, a single indx entry may reference a
 * large number of records, because there's a single on-page entry representing many identical
 * records. (We don't expand those entries when the page comes into memory, as that would require
 * resources as pages are moved to/from the cache, including read-only files.) Instead, a single
 * indx entry represents all of the identical records originally found on the page.
 *
 * Modifying (or deleting) run-length encoded column-store records is hard because the page's entry
 * no longer references a set of identical items. We handle this by "inserting" a new entry into the
 * insert array, with its own record number. (This is the only case where it's possible to insert
 * into a column-store: only appends are allowed, as insert requires re-numbering subsequent
 * records. Berkeley DB did support mutable records, but it won't scale and it isn't useful enough
 * to re-implement, IMNSHO.)
 */
struct __wt_insert {
    wt_shared WT_UPDATE *upd; /* value */

    union {
        uint64_t recno; /* column-store record number */
        struct {
            uint32_t offset; /* row-store key data start */
            uint32_t size;   /* row-store key data size */
        } key;
    } u;

#define WT_INSERT_KEY_SIZE(ins) (((WT_INSERT *)(ins))->u.key.size)
#define WT_INSERT_KEY(ins) ((void *)((uint8_t *)(ins) + ((WT_INSERT *)(ins))->u.key.offset))
#define WT_INSERT_RECNO(ins) (((WT_INSERT *)(ins))->u.recno)

    wt_shared WT_INSERT *next[0]; /* forward-linked skip list */
};

/*
 * Skiplist helper macros.
 */
#define WT_SKIP_FIRST(ins_head) \
    (((ins_head) == NULL) ? NULL : ((WT_INSERT_HEAD *)(ins_head))->head[0])
#define WT_SKIP_LAST(ins_head) \
    (((ins_head) == NULL) ? NULL : ((WT_INSERT_HEAD *)(ins_head))->tail[0])
#define WT_SKIP_NEXT(ins) ((ins)->next[0])
#define WT_SKIP_FOREACH(ins, ins_head) \
    for ((ins) = WT_SKIP_FIRST(ins_head); (ins) != NULL; (ins) = WT_SKIP_NEXT(ins))

/*
 * Atomically allocate and swap a structure or array into place.
 */
#define WT_PAGE_ALLOC_AND_SWAP(s, page, dest, v, count)                             \
    do {                                                                            \
        if (((v) = (dest)) == NULL) {                                               \
            WT_ERR(__wt_calloc_def(s, count, &(v)));                                \
            if (__wt_atomic_cas_ptr(&(dest), NULL, v))                              \
                __wt_cache_page_inmem_incr(s, page, (count) * sizeof(*(v)), false); \
            else                                                                    \
                __wt_free(s, v);                                                    \
        }                                                                           \
    } while (0)

/*
 * WT_INSERT_HEAD --
 * 	The head of a skiplist of WT_INSERT items.
 */
struct __wt_insert_head {
    wt_shared WT_INSERT *head[WT_SKIP_MAXDEPTH]; /* first item on skiplists */
    wt_shared WT_INSERT *tail[WT_SKIP_MAXDEPTH]; /* last item on skiplists */
};

/*
 * The row-store leaf page insert lists are arrays of pointers to structures, and may not exist. The
 * following macros return an array entry if the array of pointers and the specific structure exist,
 * else NULL.
 */
#define WT_ROW_INSERT_SLOT(page, slot)                                  \
    ((page)->modify == NULL || (page)->modify->mod_row_insert == NULL ? \
        NULL :                                                          \
        (page)->modify->mod_row_insert[slot])
#define WT_ROW_INSERT(page, ip) WT_ROW_INSERT_SLOT(page, WT_ROW_SLOT(page, ip))
#define WT_ROW_UPDATE(page, ip)                                         \
    ((page)->modify == NULL || (page)->modify->mod_row_update == NULL ? \
        NULL :                                                          \
        (page)->modify->mod_row_update[WT_ROW_SLOT(page, ip)])
/*
 * WT_ROW_INSERT_SMALLEST references an additional slot past the end of the "one per WT_ROW slot"
 * insert array. That's because the insert array requires an extra slot to hold keys that sort
 * before any key found on the original page.
 */
#define WT_ROW_INSERT_SMALLEST(page)                                    \
    ((page)->modify == NULL || (page)->modify->mod_row_insert == NULL ? \
        NULL :                                                          \
        (page)->modify->mod_row_insert[(page)->entries])

/*
 * The column-store leaf page update lists are arrays of pointers to structures, and may not exist.
 * The following macros return an array entry if the array of pointers and the specific structure
 * exist, else NULL.
 */
#define WT_COL_UPDATE_SLOT(page, slot)                                  \
    ((page)->modify == NULL || (page)->modify->mod_col_update == NULL ? \
        NULL :                                                          \
        (page)->modify->mod_col_update[slot])
#define WT_COL_UPDATE(page, ip) WT_COL_UPDATE_SLOT(page, WT_COL_SLOT(page, ip))

/*
 * WT_COL_UPDATE_SINGLE is a single WT_INSERT list, used for any fixed-length column-store updates
 * for a page.
 */
#define WT_COL_UPDATE_SINGLE(page) WT_COL_UPDATE_SLOT(page, 0)

/*
 * WT_COL_APPEND is an WT_INSERT list, used for fixed- and variable-length appends.
 */
#define WT_COL_APPEND(page)                                             \
    ((page)->modify == NULL || (page)->modify->mod_col_append == NULL ? \
        NULL :                                                          \
        (page)->modify->mod_col_append[0])

/* WT_COL_FIX_FOREACH_BITS walks fixed-length bit-fields on a disk page. */
#define WT_COL_FIX_FOREACH_BITS(btree, dsk, v, i)                            \
    for ((i) = 0,                                                            \
        (v) = (i) < (dsk)->u.entries ?                                       \
           __bit_getv(WT_PAGE_HEADER_BYTE(btree, dsk), 0, (btree)->bitcnt) : \
           0;                                                                \
         (i) < (dsk)->u.entries; ++(i),                                      \
        (v) = (i) < (dsk)->u.entries ?                                       \
           __bit_getv(WT_PAGE_HEADER_BYTE(btree, dsk), i, (btree)->bitcnt) : \
           0)

/*
 * FLCS pages with time information have a small additional header after the main page data that
 * holds a version number and cell count, plus the byte offset to the start of the cell data. The
 * latter values are limited by the page size, so need only be 32 bits. One hopes we'll never need
 * 2^32 versions.
 *
 * This struct is the in-memory representation. The number of entries is the number of time windows
 * (there are twice as many cells) and the offsets is from the beginning of the page. The space
 * between the empty offset and the data offset is not used and is expected to be zeroed.
 *
 * This structure is only used when handling on-disk pages; once the page is read in, one should
 * instead use the time window index in the page structure, which is a different type found above.
 */
struct __wt_col_fix_auxiliary_header {
    uint32_t version;
    uint32_t entries;
    uint32_t emptyoffset;
    uint32_t dataoffset;
};

/*
 * The on-disk auxiliary header uses a 1-byte version (the header must always begin with a nonzero
 * byte) and packed integers for the entry count and offset. To make the size of the offset entry
 * predictable (rather than dependent on the total page size) and also as small as possible, we
 * store the distance from the auxiliary data. To avoid complications computing the offset, we
 * include the offset's own storage space in the offset, and to make things simpler all around, we
 * include the whole auxiliary header in the offset; that is, the position of the auxiliary data is
 * computed as the position of the start of the auxiliary header plus the decoded stored offset.
 *
 * Both the entry count and the offset are limited to 32 bits because pages may not exceed 4G, so
 * their maximum encoded lengths are 5 each, so the maximum size of the on-disk header is 11 bytes.
 * It can be as small as 3 bytes, though.
 *
 * We reserve 7 bytes for the header on a full page (not 11) because on a full page the encoded
 * offset is the reservation size, and 7 encodes in one byte. This is enough for all smaller pages:
 * obviously if there's at least 4 extra bytes in the bitmap space any header will fit (4 + 7 = 11)
 * and if there's less the encoded offset is less than 11, which still encodes to one byte.
 */

#define WT_COL_FIX_AUXHEADER_RESERVATION 7
#define WT_COL_FIX_AUXHEADER_SIZE_MAX 11

/* Values for ->version. Version 0 never appears in an on-disk header. */
#define WT_COL_FIX_VERSION_NIL 0 /* Original page format with no timestamp data */
#define WT_COL_FIX_VERSION_TS 1  /* Upgraded format with cells carrying timestamp info */

/*
 * Manage split generation numbers. Splits walk the list of sessions to check when it is safe to
 * free structures that have been replaced. We also check that list periodically (e.g., when
 * wrapping up a transaction) to free any memory we can.
 *
 * Before a thread enters code that will examine page indexes (which are swapped out by splits), it
 * publishes a copy of the current split generation into its session. Don't assume that threads
 * never re-enter this code: if we already have a split generation, leave it alone. If our caller is
 * examining an index, we don't want the oldest split generation to move forward and potentially
 * free it.
 */
#define WT_ENTER_PAGE_INDEX(session) WT_ENTER_GENERATION((session), WT_GEN_SPLIT);

#define WT_LEAVE_PAGE_INDEX(session) WT_LEAVE_GENERATION((session), WT_GEN_SPLIT);

#define WT_WITH_PAGE_INDEX(session, e) \
    WT_ENTER_PAGE_INDEX(session);      \
    (e);                               \
    WT_LEAVE_PAGE_INDEX(session)

/*
 * Manage the given generation number with support for re-entry. Re-entry is allowed as the previous
 * generation as it must be as low as the current generation.
 */
#define WT_ENTER_GENERATION(session, generation)              \
    do {                                                      \
        bool __entered_##generation = false;                  \
        if (__wt_session_gen((session), (generation)) == 0) { \
            __wt_session_gen_enter((session), (generation));  \
            __entered_##generation = true;                    \
        }

#define WT_LEAVE_GENERATION(session, generation)         \
    if (__entered_##generation)                          \
        __wt_session_gen_leave((session), (generation)); \
    }                                                    \
    while (0)

/*
 * WT_VERIFY_INFO -- A structure to hold all the information related to a verify operation.
 */
struct __wt_verify_info {
    WT_SESSION_IMPL *session;

    const char *tag;           /* Identifier included in error messages */
    const WT_PAGE_HEADER *dsk; /* The disk header for the page being verified */
    WT_ADDR *page_addr;        /* An item representing a page entry being verified */
    size_t page_size;
    uint32_t cell_num; /* The current cell offset being verified */
    uint64_t recno;    /* The current record number in a column store page */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_VRFY_DISK_CONTINUE_ON_FAILURE 0x1u
#define WT_VRFY_DISK_EMPTY_PAGE_OK 0x2u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};
