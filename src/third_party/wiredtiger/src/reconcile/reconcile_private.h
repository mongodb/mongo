/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_CHILD_MODIFY_STATE --
 *	We review child pages (while holding the child page's WT_REF lock), during internal-page
 * reconciliation. This structure encapsulates the child page's returned information/state.
 */
typedef struct {
    enum {
        WT_CHILD_IGNORE,   /* Ignored child */
        WT_CHILD_MODIFIED, /* Modified child */
        WT_CHILD_ORIGINAL, /* Original child */
        WT_CHILD_PROXY     /* Deleted child: proxy */
    } state;               /* Returned child state */

    WT_PAGE_DELETED del; /* WT_CHILD_PROXY state fast-truncate information */

    bool hazard; /* If currently holding a child hazard pointer */
} WT_CHILD_MODIFY_STATE;

/*
 * WT_CHILD_RELEASE, WT_CHILD_RELEASE_ERR --
 *	Macros to clean up during internal-page reconciliation, releasing the hazard pointer we're
 * holding on a child page.
 */
#define WT_CHILD_RELEASE(session, hazard, ref)                          \
    do {                                                                \
        if (hazard) {                                                   \
            (hazard) = false;                                           \
            WT_TRET(__wt_page_release(session, ref, WT_READ_NO_EVICT)); \
        }                                                               \
    } while (0)
#define WT_CHILD_RELEASE_ERR(session, hazard, ref) \
    do {                                           \
        WT_CHILD_RELEASE(session, hazard, ref);    \
        WT_ERR(ret);                               \
    } while (0)

/*
 * WT_REC_KV--
 *	An on-page key/value item we're building.
 */
struct __wt_rec_kv {
    WT_ITEM buf;  /* Data */
    WT_CELL cell; /* Cell and cell's length */
    size_t cell_len;
    size_t len; /* Total length of cell + data */
};

/*
 * WT_REC_DICTIONARY --
 *  We optionally build a dictionary of values for leaf pages. Where
 * two value cells are identical, only write the value once, the second
 * and subsequent copies point to the original cell. The dictionary is
 * fixed size, but organized in a skip-list to make searches faster.
 */
struct __wt_rec_dictionary {
    uint64_t hash;   /* Hash value */
    uint32_t offset; /* Matching cell */

    u_int depth; /* Skiplist */
    WT_REC_DICTIONARY *next[0];
};

/*
 * WT_REC_CHUNK --
 *	Reconciliation split chunk. If the total chunk size crosses the split size additional
 *  information is stored about where that is.
 */
struct __wt_rec_chunk {
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

    /* For fixed-length column store, track where the time windows start and how many we have. */
    uint32_t aux_start_offset;
    uint32_t auxentries;
};

/*
 * Reconciliation tracks two time aggregates per chunk, one for the full chunk and one for the part
 * of the chunk past the split boundary. In every situation that we write to the main aggregate we
 * need to write to the "after" aggregate. These helper macros were added with that in mind.
 */
#define WT_REC_CHUNK_TA_UPDATE(session, chunk, tw)                                    \
    do {                                                                              \
        WT_TIME_AGGREGATE_UPDATE((session), &(chunk)->ta, (tw));                      \
        WT_TIME_AGGREGATE_UPDATE((session), &(chunk)->ta_after_split_boundary, (tw)); \
    } while (0)
#define WT_REC_CHUNK_TA_MERGE(session, chunk, ta_agg)                                    \
    do {                                                                                 \
        WT_TIME_AGGREGATE_MERGE((session), &(chunk)->ta, (ta_agg));                      \
        WT_TIME_AGGREGATE_MERGE((session), &(chunk)->ta_after_split_boundary, (ta_agg)); \
    } while (0)

typedef struct {
    WT_UPDATE *upd;       /* Update to write (or NULL) */
    WT_UPDATE *tombstone; /* The tombstone to write (or NULL) */

    WT_TIME_WINDOW tw;

    bool upd_saved;       /* An element on the row's update chain was saved */
    bool no_ts_tombstone; /* Tombstone without a timestamp */
} WT_UPDATE_SELECT;

#define WT_UPDATE_SELECT_INIT(upd_select)       \
    do {                                        \
        (upd_select)->upd = NULL;               \
        (upd_select)->tombstone = NULL;         \
        (upd_select)->upd_saved = false;        \
        (upd_select)->no_ts_tombstone = false;  \
        WT_TIME_WINDOW_INIT(&(upd_select)->tw); \
    } while (0)

/*
 * Macros from fixed-length entries to/from bytes.
 */
#define WT_COL_FIX_BYTES_TO_ENTRIES(btree, bytes) ((uint32_t)((((bytes)*8) / (btree)->bitcnt)))
#define WT_COL_FIX_ENTRIES_TO_BYTES(btree, entries) \
    ((uint32_t)WT_ALIGN((entries) * (btree)->bitcnt, 8))

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
extern int __wti_rec_child_modify(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref,
  WT_CHILD_MODIFY_STATE *cmsp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_col_fix(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_col_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_col_var(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_dictionary_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, u_int slots)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_hs_clear_on_tombstone(WT_SESSION_IMPL *session, WT_RECONCILE *r,
  uint64_t recno, WT_ITEM *rowkey, bool reinsert) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_row_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_row_leaf(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_crossing_bnd(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_finish(WT_SESSION_IMPL *session, WT_RECONCILE *r)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t add_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_split_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page,
  uint64_t recno, uint64_t primary_size, uint32_t auxiliary_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rec_upd_select(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins,
  WT_ROW *rip, WT_CELL_UNPACK_KV *vpack, WT_UPDATE_SELECT *upd_select)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wti_rec_col_fix_write_auxheader(WT_SESSION_IMPL *session, uint32_t entries,
  uint32_t aux_start_offset, uint32_t auxentries, uint8_t *image, size_t size);
extern void __wti_rec_dictionary_free(WT_SESSION_IMPL *session, WT_RECONCILE *r);
extern void __wti_rec_dictionary_reset(WT_RECONCILE *r);

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
