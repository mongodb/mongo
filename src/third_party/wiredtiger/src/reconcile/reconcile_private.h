/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WTI_CROSSING_MIN_BND(r, next_len) \
    ((r)->cur_ptr->min_offset == 0 && (next_len) > (r)->min_space_avail)
#define WTI_CROSSING_SPLIT_BND(r, next_len) ((next_len) > (r)->space_avail)
#define WTI_CHECK_CROSSING_BND(r, next_len) \
    (WTI_CROSSING_MIN_BND(r, next_len) || WTI_CROSSING_SPLIT_BND(r, next_len))

/*
 * WTI_REC_SPLIT_MIN_ITEMS_USE_MEM
 *     The minimum number of page items (entries on the disk image or saved updates) associated with
 *     a page required to consider in-memory updates in the split calculation.
 */
#define WTI_REC_SPLIT_MIN_ITEMS_USE_MEM 10

/*
 * WTI_REC_TW_START_VISIBLE_ALL
 *     Check if the provided time window's start is globally visible as per the saved state on the
 *     reconciliation structure.
 *
 *     An update is considered to be globally visible when its transaction id is less than the
 *     pinned id, and when its start timestamp is less than or equal to the pinned timestamp.
 *     Due to a difference in transaction id based visibility and timestamp visibility the timestamp
 *     comparison is inclusive whereas the transaction id comparison isn't.
 */
#define WTI_REC_TW_START_VISIBLE_ALL(r, tw)          \
    (((tw)->start_txn < (r)->rec_start_oldest_id) && \
      ((tw)->durable_start_ts == WT_TS_NONE ||       \
        ((r)->rec_start_pinned_ts != WT_TS_NONE &&   \
          (tw)->durable_start_ts <= (r)->rec_start_pinned_ts)))

/*
 * WTI_CHILD_MODIFY_STATE --
 *	We review child pages (while holding the child page's WT_REF lock), during internal-page
 * reconciliation. This structure encapsulates the child page's returned information/state.
 */
typedef struct {
    enum {
        WTI_CHILD_IGNORE,   /* Ignored child */
        WTI_CHILD_MODIFIED, /* Modified child */
        WTI_CHILD_ORIGINAL, /* Original child */
        WTI_CHILD_PROXY     /* Deleted child: proxy */
    } state;                /* Returned child state */

    WT_PAGE_DELETED del; /* WTI_CHILD_PROXY state fast-truncate information */

    bool hazard; /* If currently holding a child hazard pointer */
} WTI_CHILD_MODIFY_STATE;

/*
 * WTI_CHILD_RELEASE, WTI_CHILD_RELEASE_ERR --
 *	Macros to clean up during internal-page reconciliation, releasing the hazard pointer we're
 * holding on a child page.
 */
#define WTI_CHILD_RELEASE(session, hazard, ref)                         \
    do {                                                                \
        if (hazard) {                                                   \
            (hazard) = false;                                           \
            WT_TRET(__wt_page_release(session, ref, WT_READ_NO_EVICT)); \
        }                                                               \
    } while (0)
#define WTI_CHILD_RELEASE_ERR(session, hazard, ref) \
    do {                                            \
        WTI_CHILD_RELEASE(session, hazard, ref);    \
        WT_ERR(ret);                                \
    } while (0)

/*
 * WTI_REC_KV--
 *	An on-page key/value item we're building.
 */
struct __wti_rec_kv {
    WT_ITEM buf;  /* Data */
    WT_CELL cell; /* Cell and cell's length */
    size_t cell_len;
    size_t len; /* Total length of cell + data */
};

/*
 * WTI_REC_DICTIONARY --
 *  We optionally build a dictionary of values for leaf pages. Where
 * two value cells are identical, only write the value once, the second
 * and subsequent copies point to the original cell. The dictionary is
 * fixed size, but organized in a skip-list to make searches faster.
 */
struct __wti_rec_dictionary {
    uint64_t hash;   /* Hash value */
    uint32_t offset; /* Matching cell */

    u_int depth; /* Skiplist */
    WTI_REC_DICTIONARY *next[0];
};

/*
 * WTI_REC_CHUNK --
 *	Reconciliation split chunk. If the total chunk size crosses the split size additional
 *  information is stored about where that is.
 */
struct __wti_rec_chunk {
    /*
     * These fields track the amount of entries and their associated timestamps prior to the split
     * boundary.
     */
    uint32_t entries_before_split_boundary;
    WT_TIME_AGGREGATE ta_before_split_boundary;

    /* These fields track the key or recno of the very first entry past the split boundary. */
    uint64_t recno_at_split_boundary;
    WT_ITEM key_at_split_boundary;

    /*
     * This time aggregate tracks the aggregated timestamps of all the entries past the split
     * boundary. Merged with the "before" entry it will equal the full time aggregate for the chunk.
     */
    WT_TIME_AGGREGATE ta_after_split_boundary;

    /*
     * The recno and entries fields are the starting record number of the chunk (for column-store
     * splits), and the number of entries in the chunk.
     *
     * The key for a row-store page; no column-store key is needed because the page's recno, stored
     * in the recno field, is the column-store key.
     */
    uint32_t entries;
    uint64_t recno;
    WT_ITEM key;
    WT_TIME_AGGREGATE ta;

    size_t min_offset; /* byte offset */

    WT_ITEM image; /* disk-image */
};

/*
 * WTI_DELETE_HS_UPD --
 *	Update that needs to be deleted from the history store.
 */
struct __wti_delete_hs_upd {
    WT_INSERT *ins; /* Insert list reference */
    WT_ROW *rip;    /* Original on-page reference */
    WT_UPDATE *upd;
    WT_UPDATE *tombstone;
};

/*
 * Reconciliation is the process of taking an in-memory page, walking each entry
 * in the page, building a backing disk image in a temporary buffer representing
 * that information, and writing that buffer to disk.  What could be simpler?
 *
 * WTI_RECONCILE --
 *	Information tracking a single page reconciliation.
 */
struct __wti_reconcile {
    WT_REF *ref; /* Page being reconciled */
    WT_PAGE *page;

    /* Track the pinned id for the reconciliation if without a snapshot. */
    uint64_t rec_start_pinned_id;

    /* Track the oldest id that is needed. */
    uint64_t rec_start_oldest_id;

    /* Track the pinned timestamp at the time reconciliation started. */
    wt_timestamp_t rec_start_pinned_ts;

    /* Track the pinned stable timestamp at the time reconciliation started. */
    wt_timestamp_t rec_start_pinned_stable_ts;

    /*
     * When eviction is reconciling under the published checkpoint snapshot, the identity of that
     * snapshot; otherwise WT_CKPT_SNAP_GEN_NONE.
     */
    uint64_t rec_ckpt_snap_gen;

    /* Track the prune timestamp at the time reconciliation started. */
    wt_timestamp_t rec_prune_timestamp;

    /* Track the page's maximum transaction/timestamp. */
    uint64_t max_txn;
    wt_timestamp_t max_ts;

    /* Track the total time in msecs spent in block writes */
    uint64_t page_write_time;

    /*
     * Reconciliation gets tricky if we have to split a page, which happens when the disk image we
     * create exceeds the page type's maximum disk image size.
     *
     * First, the target size of the page we're building.
     */
    uint32_t page_size; /* Page size */

    /*
     * Second, the split size: if we're doing the page layout, split to a smaller-than-maximum page
     * size when a split is required so we don't repeatedly split a packed page.
     */
    uint32_t split_size;     /* Split page size */
    uint32_t min_split_size; /* Minimum split page size */
    uint32_t flags;          /* Caller's configuration */

    /*
     * We maintain two split chunks in the memory during reconciliation to be written out as pages.
     * As we get to the end of the data, if the last one turns out to be smaller than the minimum
     * split size, we go back into the penultimate chunk and split at this minimum split size
     * boundary. This moves some data from the penultimate chunk to the last chunk, hence increasing
     * the size of the last page written without decreasing the penultimate page size beyond the
     * minimum split size. For this reason, we maintain an expected split percentage boundary and a
     * minimum split percentage boundary.
     *
     * Chunks are referenced by current and previous pointers. In case of a split, previous
     * references the first chunk and current switches to the second chunk. If reconciliation
     * generates more split chunks, the previous chunk is written to the disk and current and
     * previous swap.
     */
    WTI_REC_CHUNK chunk_A, chunk_B, *cur_ptr, *prev_ptr;

    WT_ITEM delta;

    size_t disk_img_buf_size; /* Base size needed for a chunk memory image */

    /*
     * We track current information about the current record number, the number of entries copied
     * into the disk image buffer, where we are in the buffer, how much memory remains, and the
     * current min/max of the timestamps. Those values are packaged here rather than passing
     * pointers to stack locations around the code.
     */
    uint64_t recno;   /* Current record number */
    uint32_t entries; /* Current number of entries */
    /* Keys omitted from the disk image; used to decide whether to write a delta or a full page. */
    uint32_t keys_removed_from_disk_image_count;
    uint8_t *first_free;    /* Current first free byte */
    size_t space_avail;     /* Remaining space in this chunk */
    size_t min_space_avail; /* Remaining space in this chunk to put a minimum size boundary */

    /*
     * Counters tracking how much time information is included in reconciliation for each page that
     * is written to disk. The number of entries on a page is limited to a 32 bit number so these
     * counters can be too.
     */
    uint32_t count_durable_start_ts;
    uint32_t count_start_ts;
    uint32_t count_start_txn;
    uint32_t count_durable_stop_ts;
    uint32_t count_stop_ts;
    uint32_t count_stop_txn;
    uint32_t count_prepare;

    uint32_t count_delta_durable_start_ts;
    uint32_t count_delta_start_ts;
    uint32_t count_delta_start_txn;
    uint32_t count_delta_durable_stop_ts;
    uint32_t count_delta_stop_ts;
    uint32_t count_delta_stop_txn;
    uint32_t count_delta_prepare;

    /*
     * Counters for tracking the number of key deletions and key insertions or updates in internal
     * page deltas.
     */
    uint32_t count_internal_page_delta_key_deleted;
    uint32_t count_internal_page_delta_key_updated;

    /* Stats for key bytes discarded using prefix compression.*/
    uint32_t bytes_prefix_compression_delta;
    uint32_t bytes_prefix_compression_full;

    /*
     * Saved update list, supporting WT_REC_HS configurations. While reviewing updates for each
     * page, we save WT_UPDATE lists here, and then move them to per-block areas as the blocks are
     * defined.
     */
    WT_SAVE_UPD *supd; /* Saved updates */
    uint32_t supd_next;
    uint32_t supd_onpage_or_restore; /* Number of the saved updates have onpage value or should be
                                        restored */
    size_t supd_allocated;
    size_t supd_memsize; /* Size of saved update structures */

    /*
     * List of updates to be deleted from the history store. While reviewing updates for each page,
     * we save the updates that needs to be deleted from history store here, and then delete them
     * after we have built the disk image.
     */
    WTI_DELETE_HS_UPD *delete_hs_upd; /* Updates to delete from history store */
    size_t delete_hs_upd_allocated;

    /* List of pages we've written so far. */
    WT_MULTI *multi;
    uint32_t multi_next;
    uint32_t delete_hs_upd_next;
    size_t multi_allocated;

    /*
     * Root pages are written when wrapping up the reconciliation, remember the image we're going to
     * write.
     */
    WT_ITEM *wrapup_checkpoint;
    WT_PAGE_BLOCK_META wrapup_checkpoint_block_meta;

    WTI_REC_DICTIONARY **dictionary;         /* Dictionary */
    u_int dictionary_next, dictionary_slots; /* Next, max entries */
                                             /* Skiplist head. */
    WTI_REC_DICTIONARY *dictionary_head[WT_SKIP_MAXDEPTH];

    WTI_REC_KV k, v; /* Key/Value being built */

    WT_ITEM *cur, _cur;   /* Key/Value being built */
    WT_ITEM *last, _last; /* Last key/value built */

    uint8_t key_pfx_last; /* Last prefix compression */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_REC_TIME_NEWEST_START_DURABLE_TS 0x01u
#define WTI_REC_TIME_NEWEST_STOP_DURABLE_TS 0x02u
#define WTI_REC_TIME_NEWEST_STOP_TS 0x04u
#define WTI_REC_TIME_NEWEST_STOP_TXN 0x08u
#define WTI_REC_TIME_NEWEST_TXN 0x10u
#define WTI_REC_TIME_OLDEST_START_TS 0x20u
#define WTI_REC_TIME_PREPARE 0x40u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t ts_usage_flags;
    uint8_t delta_ts_usage_flags;

/*
 * Reconciliation state flags:
 *
 * UPDATE_USED: when no update is found to be written for the whole page, update-restore eviction is
 * failed unless all the updates for a key were aborted; considering it a success could force the
 * page through eviction repeatedly.
 *
 * LEAVE_DIRTY: the page can't be marked clean after reconciliation, for example checkpoint or
 * eviction found some uncommitted updates.
 *
 * OVFL_ITEMS: if a leaf page with no overflow items is written, the parent's address cell is set to
 * the leaf-no-overflow type so the leaf can be deleted without reading it. The test is per-page
 * reconciliation: once an overflow item is seen, all subsequent leaf pages written for the page
 * lose the leaf-no-overflow type, whether or not they contain overflow items.
 *
 * ALL_EMPTY_VALUE, ANY_EMPTY_VALUE: empty (zero length) values aren't written, so the page's
 * on-disk header records whether all or any values are empty, avoiding a pass over the page to
 * count keys on read. Per-page reconciliation, as for OVFL_ITEMS.
 *
 * CELL_ZERO: the 0th key isn't kept on internal pages, the search code ignores it as nothing can
 * sort less by definition. See the code for how this works.
 *
 * REC_PAGE_CELL_WITH_*: track reconciliation calls for pages with cells containing time window
 * values and prepared transactions.
 *
 * HS_CLEAR_ON_TOMBSTONE: removing a key due to a tombstone with a "none" durable timestamp also
 * removes the key's history store contents, using the cached history store cursor.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_REC_ALL_EMPTY_VALUE 0x00001u
#define WTI_REC_ANY_EMPTY_VALUE 0x00002u
#define WTI_REC_CACHE_WRITE_HS 0x00004u                /* Used the history store table */
#define WTI_REC_CACHE_WRITE_RESTORE_INVISIBLE 0x00008u /* Restored an invisible update */
#define WTI_REC_CELL_ZERO 0x00010u                     /* Row-store internal page 0th key */
#define WTI_REC_HAS_UPD_CHAIN_ALL_ABORTED 0x00020u     /* Update chain with all aborted */
#define WTI_REC_HS_CLEAR_ON_TOMBSTONE 0x00040u
#define WTI_REC_IS_BULK_LOAD 0x00080u          /* Bulk load */
#define WTI_REC_KEY_PFX_COMPRESS 0x00100u      /* Can prefix-compress next key */
#define WTI_REC_KEY_PFX_COMPRESS_CONF 0x00200u /* Prefix compression configured */
#define WTI_REC_KEY_SFX_COMPRESS 0x00400u      /* Can suffix-compress next key */
#define WTI_REC_KEY_SFX_COMPRESS_CONF 0x00800u /* Suffix compression configured */
#define WTI_REC_LEAVE_DIRTY 0x01000u
#define WTI_REC_NEWER_UPDATES_THAN_LAST_REC_USED 0x02000u /* Wrote newer updates */
#define WTI_REC_OVFL_ITEMS 0x04000u
#define WTI_REC_REC_PAGE_CELL_WITH_PREPARED_TXN 0x08000u
#define WTI_REC_REC_PAGE_CELL_WITH_TS 0x10000u
#define WTI_REC_REC_PAGE_CELL_WITH_TXN_ID 0x20000u
#define WTI_REC_UPDATE_USED 0x40000u
#define WTI_REC_WRAPUP_CHECKPOINT_COMPRESSED 0x80000u /* Wrapped-up checkpoint image compressed */
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t state_flags;

    WT_SALVAGE_COOKIE *salvage; /* If it's a salvage operation */

    WT_REF_STATE tested_ref_state; /* Debugging information */

    /*
     * XXX In the case of a modified update, we may need a copy of the current value as a set of
     * bytes. We call back into the btree code using a fake cursor to do that work. This a layering
     * violation and fragile, we need a better solution.
     */
    WT_CURSOR_BTREE update_modify_cbt;

    WT_CURSOR *hs_cursor;
};

/*
 * Reconciliation tracks two time aggregates per chunk, one for the full chunk and one for the part
 * of the chunk past the split boundary. In every situation that we write to the main aggregate we
 * need to write to the "after" aggregate. These helper macros were added with that in mind.
 */
#define WTI_REC_CHUNK_TA_UPDATE(session, chunk, tw)                                   \
    do {                                                                              \
        WT_TIME_AGGREGATE_UPDATE((session), &(chunk)->ta, (tw));                      \
        WT_TIME_AGGREGATE_UPDATE((session), &(chunk)->ta_after_split_boundary, (tw)); \
    } while (0)
#define WTI_REC_CHUNK_TA_MERGE(session, chunk, ta_agg)                                   \
    do {                                                                                 \
        WT_TIME_AGGREGATE_MERGE((session), &(chunk)->ta, (ta_agg));                      \
        WT_TIME_AGGREGATE_MERGE((session), &(chunk)->ta_after_split_boundary, (ta_agg)); \
    } while (0)

struct __wti_update_select {
    WT_UPDATE *upd;       /* Update to write (or NULL) */
    WT_UPDATE *tombstone; /* The tombstone to write (or NULL) */

    WT_TIME_WINDOW tw;

    bool upd_saved;       /* An element on the row's update chain was saved */
    bool no_ts_tombstone; /* Tombstone without a timestamp */

    /*
     * A prepared update that was rolled back after the stable timestamp and skipped by this
     * reconciliation, kept so the fallback append can anchor its walk on it rather than the
     * concurrently-mutating update chain head.
     */
    WT_UPDATE *prepare_rollback_upd;

    bool modify_needs_onpage_value; /* A retained MODIFY needs the on-page value */
};

#define WTI_UPDATE_SELECT_INIT(upd_select)               \
    do {                                                 \
        (upd_select)->upd = NULL;                        \
        (upd_select)->tombstone = NULL;                  \
        (upd_select)->upd_saved = false;                 \
        (upd_select)->no_ts_tombstone = false;           \
        (upd_select)->prepare_rollback_upd = NULL;       \
        (upd_select)->modify_needs_onpage_value = false; \
        WT_TIME_WINDOW_INIT(&(upd_select)->tw);          \
    } while (0)

#define WT_REC_RESULT_SINGLE_PAGE(session, r)                                    \
    (((r)->ref->page->modify->rec_result == 0 && (r)->ref->page->dsk != NULL) || \
      (r)->ref->page->modify->rec_result == WT_PM_REC_REPLACE ||                 \
      ((r)->ref->page->modify->rec_result == WT_PM_REC_MULTIBLOCK &&             \
        (r)->ref->page->modify->mod_multi_entries == 1))

/* Called after building the disk image. */
#define WT_BUILD_DELTA_LEAF(session, r)                                                        \
    (WT_DELTA_LEAF_ENABLED((session)) && !F_ISSET_ATOMIC_16((r)->page, WT_PAGE_INMEM_SPLIT) && \
      (r)->multi_next == 1 && WT_REC_RESULT_SINGLE_PAGE((session), (r)))

/*
 * Called when building the internal page image to indicate should we start to build a delta for the
 * page. We are still building so multi_next should still be 0 instead of 1.
 */
#define WT_BUILD_DELTA_INT(session, r)                                                    \
    WT_DELTA_INT_ENABLED((S2BT(session)), (S2C(session))) && !__wt_ref_is_root(r->ref) && \
      (r)->multi_next == 0 &&                                                             \
      !F_ISSET_ATOMIC_16(r->ref->page, WT_PAGE_REC_FAIL | WT_PAGE_INTL_PINDEX_UPDATE) &&  \
      WT_REC_RESULT_SINGLE_PAGE((session), (r))
/*
 * Macro to check if an update's transaction ID and timestamp is obsolete and can be pruned.
 */
#define WT_REC_CAN_PRUNE_UPD(txnid, timestamp, r)                                    \
    ((r)->rec_prune_timestamp != WT_TS_NONE && (txnid) < (r)->rec_start_oldest_id && \
      (timestamp) <= r->rec_prune_timestamp)

#define WT_REC_HAS_ON_DISK(vpack) (vpack != NULL && vpack->type != WT_CELL_DEL)
/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wti_ovfl_reuse_add(WT_SESSION_IMPL *session, WT_PAGE *page, const uint8_t *addr,
  size_t addr_size, const void *value, size_t value_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_ovfl_reuse_search(WT_SESSION_IMPL *session, WT_PAGE *page, uint8_t **addrp,
  size_t *addr_sizep, const void *value, size_t value_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_ovfl_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_ovfl_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_build_delta_init(WT_SESSION_IMPL *session, WTI_RECONCILE *r)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_cell_build_ovfl(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WTI_REC_KV *kv,
  uint8_t type, WT_TIME_WINDOW *tw, uint64_t rle) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_child_modify(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *ref,
  WTI_CHILD_MODIFY_STATE *cmsp, bool *build_delta) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_col_int(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_col_var(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_dictionary_init(WT_SESSION_IMPL *session, WTI_RECONCILE *r, u_int slots)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_dictionary_lookup(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WTI_REC_KV *val,
  WTI_REC_DICTIONARY **dpp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_hs_clear_on_tombstone(WT_SESSION_IMPL *session, WTI_RECONCILE *r,
  uint64_t recno, WT_ITEM *rowkey, bool reinsert) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_hs_delete_key(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  uint32_t btree_id, const WT_ITEM *key, bool reinsert, bool error_on_ts_ordering)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_hs_delete_updates(WT_SESSION_IMPL *session, WTI_RECONCILE *r)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_hs_insert_updates(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_MULTI *multi)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_pack_delta_row_leaf(WT_SESSION_IMPL *session, WTI_RECONCILE *r,
  WT_SAVE_UPD *supd, WT_ITEM *key, WT_ITEM *custom_value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_row_int(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_row_leaf(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_crossing_bnd(WT_SESSION_IMPL *session, WTI_RECONCILE *r, size_t next_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_finish(WT_SESSION_IMPL *session, WTI_RECONCILE *r)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_init(WT_SESSION_IMPL *session, WTI_RECONCILE *r, uint64_t recno,
  uint64_t primary_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_upd_select(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins,
  WT_ROW *rip, WT_CELL_UNPACK_KV *vpack, WTI_UPDATE_SELECT *upd_select)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wti_rec_dictionary_free(WT_SESSION_IMPL *session, WTI_RECONCILE *r);
extern void __wti_rec_dictionary_reset(WTI_RECONCILE *r);
static WT_INLINE bool __wti_rec_need_split(WTI_RECONCILE *r, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wti_rec_cell_build_val(WT_SESSION_IMPL *session, WTI_RECONCILE *r,
  const void *data, size_t size, WT_TIME_WINDOW *tw, uint64_t rle, bool *ovfl_val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wti_rec_dict_replace(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_TIME_WINDOW *tw, uint64_t rle, WTI_REC_KV *val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wti_rec_get_row_leaf_key(WT_SESSION_IMPL *session, WT_BTREE *btree,
  WTI_RECONCILE *r, WT_INSERT *ins, WT_ROW *rip, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE void __wti_rec_cell_build_addr(WT_SESSION_IMPL *session, WTI_RECONCILE *r,
  WT_ADDR *addr, WT_CELL_UNPACK_ADDR *vpack, uint64_t recno, WT_PAGE_DELETED *page_del,
  bool is_prepared_fast_truncate);
static WT_INLINE void __wti_rec_image_copy(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WTI_REC_KV *kv);
static WT_INLINE void __wti_rec_kv_copy(WT_SESSION_IMPL *session, uint8_t *p, WTI_REC_KV *kv);
static WT_INLINE void __wti_rec_time_window_clear_obsolete(WT_SESSION_IMPL *session,
  WTI_UPDATE_SELECT *upd_select, WT_CELL_UNPACK_KV *vpack, WTI_RECONCILE *r);

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
