/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* Get the session from any cursor. */
#define CUR2S(c) ((WT_SESSION_IMPL *)((WT_CURSOR *)c)->session)

/*
 * Initialize a static WT_CURSOR structure.
 */
#define WT_CURSOR_STATIC_INIT(n, get_key, get_value, get_raw_key_value, set_key, set_value,      \
  compare, equals, next, prev, reset, search, search_near, insert, modify, update, remove,       \
  reserve, reconfigure, largest_key, bound, cache, reopen, checkpoint_id, close)                 \
    static const WT_CURSOR n = {                                                                 \
      NULL, /* session */                                                                        \
      NULL, /* uri */                                                                            \
      NULL, /* key_format */                                                                     \
      NULL, /* value_format */                                                                   \
      get_key, get_value, get_raw_key_value, set_key, set_value, compare, equals, next, prev,    \
      reset, search, search_near, insert, modify, update, remove, reserve, checkpoint_id, close, \
      largest_key, reconfigure, bound, cache, reopen, 0, /* uri_hash */                          \
      {NULL, NULL},                                      /* TAILQ_ENTRY q */                     \
      0,                                                 /* recno key */                         \
      {0},                                               /* recno raw buffer */                  \
      NULL,                                              /* json_private */                      \
      NULL,                                              /* lang_private */                      \
      {NULL, 0, NULL, 0, 0},                             /* WT_ITEM key */                       \
      {NULL, 0, NULL, 0, 0},                             /* WT_ITEM value */                     \
      0,                                                 /* int saved_err */                     \
      NULL,                                              /* internal_uri */                      \
      {NULL, 0, NULL, 0, 0},                             /* WT_ITEM lower bound */               \
      {NULL, 0, NULL, 0, 0},                             /* WT_ITEM upper bound */               \
      0                                                  /* uint32_t flags */                    \
    }

/* Call a function without the evict reposition cursor flag, restore afterwards. */
#define WT_WITHOUT_EVICT_REPOSITION(e)                                              \
    do {                                                                            \
        bool __evict_reposition_flag = F_ISSET(cursor, WT_CURSTD_EVICT_REPOSITION); \
        F_CLR(cursor, WT_CURSTD_EVICT_REPOSITION);                                  \
        e;                                                                          \
        if (__evict_reposition_flag)                                                \
            F_SET(cursor, WT_CURSTD_EVICT_REPOSITION);                              \
    } while (0)

/* Maximum number of incremental backups. */
#define WT_BLKINCR_MAX 2

struct __wt_cursor_backup {
    WT_CURSOR iface;

    size_t next;     /* Cursor position */
    WT_FSTREAM *bfs; /* Backup file stream */

#define WT_CURSOR_BACKUP_ID(cursor) (((WT_CURSOR_BACKUP *)(cursor))->maxid)
    uint32_t maxid; /* Maximum log file ID seen */

    /*
     * Keep a list of configuration values for each file to be copied for an incremental backup. We
     * use it to keep the bitmap at the time the backup starts. This list should be kept and managed
     * in lock-step with the list of files.
     */
    char **cfg_list; /* List of metadata configs for files to be copied. */
    size_t cfg_allocated;
    char *cfg_current;
    char **list; /* List of files to be copied. */
    size_t list_allocated;
    size_t list_next;

    /* File offset-based incremental backup. */
    WT_BLKINCR *incr_src; /* Incremental backup source */
    char *incr_file;      /* File name */

    WT_CURSOR *incr_cursor; /* File cursor */

    WT_ITEM bitstring;    /* List of modified blocks */
    uint64_t nbits;       /* Number of bits in bitstring */
    uint64_t offset;      /* Zero bit offset in bitstring */
    uint64_t bit_offset;  /* Current offset */
    uint64_t granularity; /* Length, transfer size */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CURBACKUP_CKPT_FAKE 0x0001u   /* Object has fake checkpoint */
#define WT_CURBACKUP_COMPRESSED 0x0002u  /* Object uses compression */
#define WT_CURBACKUP_CONSOLIDATE 0x0004u /* Consolidate returned info on this object */
#define WT_CURBACKUP_DUP 0x0008u         /* Duplicated backup cursor */
#define WT_CURBACKUP_EXPORT 0x0010u      /* Special backup cursor for export operation */
#define WT_CURBACKUP_FORCE_FULL 0x0020u  /* Force full file copy for this cursor */
#define WT_CURBACKUP_FORCE_STOP 0x0040u  /* Force stop incremental backup */
#define WT_CURBACKUP_HAS_CB_INFO 0x0080u /* Object has checkpoint backup info */
#define WT_CURBACKUP_INCR 0x0100u        /* Incremental backup cursor */
#define WT_CURBACKUP_INCR_INIT 0x0200u   /* Cursor traversal initialized */
#define WT_CURBACKUP_LOCKER 0x0400u      /* Hot-backup started */
#define WT_CURBACKUP_QUERYID 0x0800u     /* Backup cursor for incremental ids */
#define WT_CURBACKUP_RENAME 0x1000u      /* Object had a rename */
                                         /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

struct __wt_cursor_prepare_discovered {
    WT_CURSOR iface;

    size_t next; /* Cursor position */

    wt_timestamp_t *list; /* List of prepared transactions available to be claimed. */
    size_t list_allocated;
    size_t list_next;
};

struct __wt_cursor_btree {
    WT_CURSOR iface;

    WT_DATA_HANDLE *dhandle; /* Data handle for the btree */

    /*
     * The following fields are set by the search functions as a precursor to page modification: we
     * have a page, a WT_COL/WT_ROW slot on the page, an insert head, insert list and a skiplist
     * stack (the stack of skiplist entries leading to the insert point). The search functions also
     * return the relationship of the search key to the found key.
     */
    WT_REF *ref;   /* Current page */
    uint32_t slot; /* WT_COL/WT_ROW 0-based slot */

    WT_INSERT_HEAD *ins_head; /* Insert chain head */
    WT_INSERT *ins;           /* Current insert node */
    /* Search stack */
    WT_INSERT **ins_stack[WT_SKIP_MAXDEPTH];

    /* Next item(s) found during search */
    WT_INSERT *next_stack[WT_SKIP_MAXDEPTH];

    uint32_t page_deleted_count; /* Deleted items on the page */

    uint64_t recno; /* Record number */

    /*
     * Next-random cursors can optionally be configured to step through a percentage of the total
     * leaf pages to their next value. Note the configured value and the calculated number of leaf
     * pages to skip.
     */
    uint64_t next_random_leaf_skip;
    u_int next_random_sample_size;

    /*
     * The search function sets compare to:
     *	< 1 if the found key is less than the specified key
     *	  0 if the found key matches the specified key
     *	> 1 if the found key is larger than the specified key
     */
    int compare;

    /*
     * A key returned from a binary search or cursor movement on a row-store page; if we find an
     * exact match on a row-store leaf page in a search operation, keep a copy of key we built
     * during the search to avoid doing the additional work of getting the key again for return to
     * the application. Note, this only applies to exact matches when searching disk-image
     * structures, so it's not, for example, a key from an insert list. Additionally, this structure
     * is used to build keys when moving a cursor through a row-store leaf page.
     */
    WT_ITEM *row_key, _row_key;

    /*
     * It's relatively expensive to calculate the last record on a variable- length column-store
     * page because of the repeat values. Calculate it once per page and cache it. This value
     * doesn't include the skiplist of appended entries on the last page.
     */
    uint64_t last_standard_recno;

    /*
     * For row-store pages, we need a single item that tells us the part of the page we're walking
     * (otherwise switching from next to prev and vice-versa is just too complicated), so we map the
     * WT_ROW and WT_INSERT_HEAD insert array slots into a single name space: slot 1 is the
     * "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is WT_INSERT_HEAD[0], and so on. This
     * means WT_INSERT lists are odd-numbered slots, and WT_ROW array slots are even-numbered slots.
     */
    uint32_t row_iteration_slot; /* Row-store iteration slot */

    /*
     * Variable-length column-store values are run-length encoded and may be overflow values. To
     * avoid repeatedly reading overflow values or decompressing encoded values, process it once and
     * store the result in a temporary buffer. The cip_saved field is used to determine if we've
     * switched columns since our last cursor call. Note however that this result caching is not
     * necessarily safe for all RLE cells. The flag WT_CBT_CACHEABLE_RLE_CELL indicates that the
     * value is uniform across the whole cell. If it is not set (e.g. if the cell is not globally
     * visible yet), the cached values should not be used.
     */
    WT_COL *cip_saved; /* Last iteration reference */

    /*
     * We don't instantiate prefix-compressed keys on pages because we don't want to waste memory if
     * only moving a cursor through the page, and it's faster to build keys while moving through the
     * page than to roll-forward from a previously instantiated key (we don't instantiate all of the
     * keys, just the ones at binary search points). We can't use the application's WT_CURSOR key
     * field as a copy of the last-returned key because it may have been altered by the API layer,
     * for example, dump cursors. Instead we store the last-returned key in a temporary buffer. The
     * rip_saved field is used to determine if the key in the temporary buffer has the prefix needed
     * for building the current key.
     */
    WT_ROW *rip_saved; /* Last-returned key reference */

    /*
     * A temporary buffer, used in a few different ways:
     *
     * 1) For caching RLE values for column-store files (if RLE is non-zero, then we don't unpack
     * the value every time we move to the next cursor position, we re-use the unpacked value we
     * stored here the first time we hit the value).
     *
     * 2) For building on-page keys when searching row-store files.
     *
     * 3) For tracking random return values to avoid repetition.
     */
    WT_ITEM *tmp, _tmp;

    /*
     * The update structure allocated by the row- and column-store modify functions, used to avoid a
     * data copy in the WT_CURSOR.update call.
     */
    WT_UPDATE_VALUE *modify_update, _modify_update;

    /* An intermediate structure to hold the update value to be assigned to the cursor buffer. */
    WT_UPDATE_VALUE *upd_value, _upd_value;

    /*
     * Bits used by checkpoint cursor: a private transaction, used to provide the proper read
     * snapshot; a reference to the corresponding history store checkpoint, which keeps it from
     * disappearing under us if it's unnamed and also tracks its identity for use in history store
     * accesses; a write generation, used to override the tree's base write generation in the
     * unpacking cleanup code; and a checkpoint ID, which is available to applications through an
     * undocumented interface to allow them to open cursors on multiple files and check if they got
     * the same checkpoint in all of them.
     */
    WT_TXN *checkpoint_txn;
    WT_DATA_HANDLE *checkpoint_hs_dhandle;
    uint64_t checkpoint_write_gen;
    uint64_t checkpoint_id;

    /*
     * Fixed-length column-store items are a single byte, and it's simpler and cheaper to allocate
     * the space for it now than keep checking to see if we need to grow the buffer.
     */
    uint8_t v; /* Fixed-length return value */

    uint8_t append_tree; /* Cursor appended to the tree */

    /*
     * We have to restart cursor next/prev after a prepare conflict. Keep the state of the cursor
     * separately so we can restart at exactly the right point.
     */
    enum { WT_CBT_RETRY_NOTSET = 0, WT_CBT_RETRY_INSERT, WT_CBT_RETRY_PAGE } iter_retry;

    /*
     * The random number state is used for random cursor operations. The random number can be seeded
     * by the user or is randomly set based on the time and thread ID.
     */
    wt_shared WT_RAND_STATE rnd; /* Random number generation state */

#ifdef HAVE_DIAGNOSTIC
    /* Check that cursor next/prev never returns keys out-of-order. */
    WT_ITEM *lastkey, _lastkey;
    uint64_t lastrecno;

    /* Record where the last key is when we see it to help debugging out of order issues. */
    WT_REF *lastref;    /* The page where the last key is */
    uint32_t lastslot;  /* WT_COL/WT_ROW 0-based slot */
    WT_INSERT *lastins; /* The last insert list */
#endif

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CBT_ACTIVE 0x001u             /* Active in the tree */
#define WT_CBT_CACHEABLE_RLE_CELL 0x002u /* Col-store: value in RLE cell valid for its keys */
#define WT_CBT_ITERATE_APPEND 0x004u     /* Col-store: iterating append list */
#define WT_CBT_ITERATE_NEXT 0x008u       /* Next iteration configuration */
#define WT_CBT_ITERATE_PREV 0x010u       /* Prev iteration configuration */
#define WT_CBT_ITERATE_RETRY_NEXT 0x020u /* Prepare conflict by next. */
#define WT_CBT_ITERATE_RETRY_PREV 0x040u /* Prepare conflict by prev. */
#define WT_CBT_READ_ONCE 0x080u          /* Page in with WT_READ_WONT_NEED */
#define WT_CBT_SEARCH_SMALLEST 0x100u    /* Row-store: small-key insert list */
#define WT_CBT_VAR_ONPAGE_MATCH 0x200u   /* Var-store: on-page recno match */
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

#define WT_CBT_POSITION_MASK /* Flags associated with position */                      \
    (WT_CBT_ITERATE_APPEND | WT_CBT_ITERATE_NEXT | WT_CBT_ITERATE_PREV |               \
      WT_CBT_ITERATE_RETRY_NEXT | WT_CBT_ITERATE_RETRY_PREV | WT_CBT_SEARCH_SMALLEST | \
      WT_CBT_VAR_ONPAGE_MATCH)

    uint32_t flags;
};

/* Get the WT_BTREE from any WT_CURSOR/WT_CURSOR_BTREE. */
#ifdef INLINE_FUNCTIONS_INSTEAD_OF_MACROS
/*
 * __wt_curbt2bt --
 *     Safely return the WT_BTREE pointed to by the cursor_btree's dhandle.
 */
static WT_INLINE WT_BTREE *
__wt_curbt2bt(WT_CURSOR_BTREE *cursor_btree)
{
    WT_DATA_HANDLE *dhandle;

    dhandle = cursor_btree->dhandle;

    return (dhandle == NULL ? NULL : (WT_BTREE *)(dhandle->handle));
}

#define CUR2BT(c) __wt_curbt2bt((WT_CURSOR_BTREE *)(c))
#else
#define CUR2BT(c)                                \
    (((WT_CURSOR_BTREE *)(c))->dhandle == NULL ? \
        NULL :                                   \
        (WT_BTREE *)((WT_CURSOR_BTREE *)(c))->dhandle->handle)
#endif /* INLINE_FUNCTIONS_INSTEAD_OF_MACROS */

struct __wt_cursor_bulk {
    WT_CURSOR_BTREE cbt;

    /*
     * Variable-length column store compares values during bulk load as part of RLE compression,
     * row-store compares keys during bulk load to avoid corruption.
     */
    bool first_insert; /* First insert */
    WT_ITEM *last;     /* Last key/value inserted */

    /*
     * Additional column-store bulk load support.
     */
    uint64_t recno; /* Record number */
    uint64_t rle;   /* Variable-length RLE counter */

    /*
     * Additional fixed-length column store bitmap bulk load support: current entry in memory chunk
     * count, and the maximum number of records per chunk.
     */
    bool bitmap;    /* Bitmap bulk load */
    uint32_t entry; /* Entry count */
    uint32_t nrecs; /* Max records per chunk */

    void *reconcile; /* Reconciliation support */
    WT_REF *ref;     /* The leaf page */
    WT_PAGE *leaf;
};

struct __wt_cursor_config {
    WT_CURSOR iface;
};

struct __wt_cursor_bounds_state {
    WT_ITEM *lower_bound;
    WT_ITEM *upper_bound;
    uint64_t bound_flags;
};

struct __wt_cursor_data_source {
    WT_CURSOR iface;

    WT_COLLATOR *collator; /* Configured collator */
    int collator_owned;    /* Collator needs to be terminated */

    WT_CURSOR *source; /* Application-owned cursor */
};

struct __wt_cursor_dump {
    WT_CURSOR iface;

    WT_CURSOR *child;
};

struct __wt_cursor_hs {
    WT_CURSOR iface;

    WT_CURSOR *file_cursor; /* Queries of regular history store data */
    WT_TIME_WINDOW time_window;
    uint32_t btree_id;
    uint32_t hs_id;
    WT_ITEM *datastore_key;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_HS_CUR_BTREE_ID_SET 0x1u
#define WT_HS_CUR_COUNTER_SET 0x2u
#define WT_HS_CUR_KEY_SET 0x4u
#define WT_HS_CUR_TS_SET 0x8u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

struct __wt_cursor_index {
    WT_CURSOR iface;

    WT_TABLE *table;
    WT_INDEX *index;
    const char *key_plan, *value_plan;

    WT_CURSOR *child;
    WT_CURSOR **cg_cursors;
    uint8_t *cg_needvalue;
};

struct __wt_cursor_metadata {
    WT_CURSOR iface;

    WT_CURSOR *file_cursor;   /* Queries of regular metadata */
    WT_CURSOR *create_cursor; /* Extra cursor for create option */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_MDC_CREATEONLY 0x1u
#define WT_MDC_ONMETADATA 0x2u
#define WT_MDC_POSITIONED 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

struct __wt_cursor_stat {
    WT_CURSOR iface;

    bool notinitialized; /* Cursor not initialized */
    bool notpositioned;  /* Cursor not positioned */

    int64_t *stats;  /* Statistics */
    int stats_base;  /* Base statistics value */
    int stats_count; /* Count of statistics values */
    int (*stats_desc)(WT_CURSOR_STAT *, int, const char **);
    /* Statistics descriptions */
    int (*next_set)(WT_SESSION_IMPL *, WT_CURSOR_STAT *, bool, bool); /* Advance to next set */

    union { /* Copies of the statistics */
        WT_DSRC_STATS dsrc_stats;
        WT_CONNECTION_STATS conn_stats;
        WT_SESSION_STATS session_stats;
    } u;

    const char **cfg; /* Original cursor configuration */
    char *desc_buf;   /* Saved description string */

    int key;    /* Current stats key */
    uint64_t v; /* Current stats value */
    WT_ITEM pv; /* Current stats value (string) */

    /* Options declared in flags.py, shared by WT_CONNECTION::stat_flags */
    uint32_t flags;
};

/*
 * WT_CURSOR_STATS --
 *	Return a reference to a statistic cursor's stats structures.
 */
#define WT_CURSOR_STATS(cursor) (((WT_CURSOR_STAT *)(cursor))->stats)

struct __wt_cursor_table {
    WT_CURSOR iface;

    WT_TABLE *table;
    const char *plan;

    const char **cfg; /* Saved configuration string */

    WT_CURSOR **cg_cursors;
    WT_ITEM *cg_valcopy; /*
                          * Copies of column group values, for
                          * overlapping set_value calls.
                          */
    WT_CURSOR **idx_cursors;
};

struct __wt_cursor_version {
    WT_CURSOR iface;

    WT_CURSOR *hs_cursor;   /* Queries of history cursor. */
    WT_CURSOR *file_cursor; /* Queries of regular file cursor. */
    WT_UPDATE *next_upd;

    /*
     * While we are iterating through updates on the update list, we need to remember information
     * about the previous update we have just traversed so that we can record this as part of the
     * debug metadata in the version cursor's key.
     */
    uint64_t upd_stop_txnid;
    /* The previous traversed update's durable_ts will become the durable_stop_ts. */
    wt_timestamp_t upd_durable_stop_ts;
    /* The previous traversed update's start_ts will become the stop_ts. */
    wt_timestamp_t upd_stop_ts;
    /* The previous traversed update's prepare_ts will become the stop_prepare_ts. */
    wt_timestamp_t upd_stop_prepare_ts;
    /* Whether The previous traversed update is prepared. */
    uint8_t upd_stop_prepared;

    /* Don't show the user any keys from before this time. */
    wt_timestamp_t start_timestamp;

#define WT_CURVERSION_UPDATE_CHAIN 0
#define WT_CURVERSION_DISK_IMAGE 1
#define WT_CURVERSION_HISTORY_STORE 2

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CURVERSION_HS_EXHAUSTED 0x01u
#define WT_CURVERSION_ON_DISK_EXHAUSTED 0x02u
#define WT_CURVERSION_TIMESTAMP_ORDER 0x04u
#define WT_CURVERSION_UPDATE_EXHAUSTED 0x08u
#define WT_CURVERSION_VISIBLE_ONLY 0x10u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/*
 * WT_CURSOR_LAYERED --
 *	A layered table cursor.
 */
struct __wt_cursor_layered {
    WT_CURSOR iface;

    WT_DATA_HANDLE *dhandle;

    WT_CURSOR *current_cursor; /* The current cursor for iteration */
    WT_CURSOR *ingest_cursor;  /* The ingest table */
    WT_CURSOR *stable_cursor;  /* The stable table */

    int64_t next_random_seed;
    u_int next_random_sample_size;

    uint64_t snapshot_gen;        /* Snapshot generation on last update */
    uint64_t checkpoint_meta_lsn; /* The LSN of the last checkpoint metadata */
    bool leader;                  /* Leader/follower state on last update */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CLAYERED_ACTIVE 0x01u         /* Incremented the session count */
#define WT_CLAYERED_ITERATE_NEXT 0x02u   /* Forward iteration */
#define WT_CLAYERED_ITERATE_PREV 0x04u   /* Backward iteration */
#define WT_CLAYERED_MULTIPLE 0x08u       /* Multiple cursors have values */
#define WT_CLAYERED_OPEN_READ 0x10u      /* Open for reads */
#define WT_CLAYERED_RANDOM 0x20u         /* Random cursor operations only */
#define WT_CLAYERED_STABLE_NO_CKPT 0x40u /* Stable constituent didn't have a checkpoint */
                                         /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

#define WT_CURSOR_PRIMARY(cursor) (((WT_CURSOR_TABLE *)(cursor))->cg_cursors[0])

#define WT_CURSOR_RECNO(cursor) WT_STREQ((cursor)->key_format, "r")

#define WT_CURSOR_RAW_OK \
    (WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRETTY | WT_CURSTD_DUMP_PRINT | WT_CURSTD_RAW)

/*
 * This macro provides a consistent way of checking if a cursor has either its lower or upper bound
 * set.
 */
#define WT_CURSOR_BOUNDS_SET(cursor) \
    F_ISSET((cursor), WT_CURSTD_BOUND_LOWER | WT_CURSTD_BOUND_UPPER)

/*
 * A positioned cursor must have a page, this is a requirement of the cursor logic within the
 * wiredtiger API. As such if the page on the cursor is not null we can safely assume that the
 * cursor is positioned.
 *
 * This is primarily used by cursor bound checking logic.
 */
#define WT_CURSOR_IS_POSITIONED(cbt) (cbt->ref != NULL && cbt->ref->page != NULL)
