/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_DEFAULT_PENDING_PREPARED_DISCOVER_HASHSIZE 256

/*
 * __wt_prepared_discover_find_item --
 *     Find a pending prepared item by its ID in the pending prepared items hash map.
 */
int
__wt_prepared_discover_find_item(
  WT_SESSION_IMPL *session, uint64_t prepared_id, WT_PENDING_PREPARED_ITEM **prepared_item)
{
    WT_CONNECTION_IMPL *conn;
    WT_PENDING_PREPARED_ITEM *item;
    WT_PENDING_PREPARED_MAP *pending_prepare_items;
    WT_TXN_GLOBAL *txn_global;
    uint64_t bucket;
    conn = S2C(session);
    txn_global = &conn->txn_global;
    pending_prepare_items = &txn_global->pending_prepare_items;
    if (pending_prepare_items->hash != NULL) {
        bucket = prepared_id & (pending_prepare_items->hash_size - 1);
        TAILQ_FOREACH (item, &pending_prepare_items->hash[bucket], hashq) {
            if (item->prepared_id == prepared_id) {
                *prepared_item = item;
                return (0);
            }
        }
    }
    return (WT_NOTFOUND);
}

/*
 * __prepare_discover_alloc_upd --
 *     Create the actual update for a pending prepared value.
 */
static int
__prepare_discover_alloc_upd(WT_SESSION_IMPL *session, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack,
  WT_UPDATE **updp, size_t *sizep)
{
    WT_UPDATE *upd;

    *sizep = 0;
    upd = NULL;
    if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&(unpack->tw))) {
        /*
         * Usually we would allocate a tombstone update when seeing a stop timestamp. However in
         * this code flow, we're restoring the update into ingest table with no tombstone allowed,
         * create a standard update with a special tombstone value instead of a tombstone. In the
         * case where the update has both start and stop prepared, no need to restore the start
         * prepared.
         */
        WT_RET(__wt_upd_alloc(session, &__wt_tombstone, WT_UPDATE_STANDARD, &upd, sizep));
        upd->txnid = unpack->tw.stop_txn;
        upd->prepared_id = unpack->tw.stop_prepared_id;
        upd->prepare_ts = unpack->tw.stop_prepare_ts;
        upd->upd_durable_ts = WT_TS_NONE;
        upd->upd_start_ts = unpack->tw.stop_prepare_ts;
        upd->prepare_state = WT_PREPARE_INPROGRESS;
    } else {
        WT_ASSERT(session, WT_TIME_WINDOW_HAS_START_PREPARE(&(unpack->tw)));
        WT_RET(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, sizep));
        upd->txnid = unpack->tw.start_txn;
        upd->prepared_id = unpack->tw.start_prepared_id;
        upd->prepare_ts = unpack->tw.start_prepare_ts;
        upd->upd_durable_ts = WT_TS_NONE;
        upd->upd_start_ts = unpack->tw.start_prepare_ts;
        upd->prepare_state = WT_PREPARE_INPROGRESS;
    }
    *updp = upd;
    return (0);
}

/*
 * __pending_prepare_items_init --
 *     Initialize pending prepared txn hash map.
 */
static int
__pending_prepare_items_init(
  WT_SESSION_IMPL *session, WT_PENDING_PREPARED_MAP *pending_prepare_items, u_int hash_size)
{
    /* Hash size must be a power of 2 for efficient bucket calculation. */
    WT_ASSERT(session, (hash_size & (hash_size - 1)) == 0);

    pending_prepare_items->hash_size = hash_size;
    WT_RET(
      __wt_calloc_def(session, pending_prepare_items->hash_size, &pending_prepare_items->hash));
    for (uint64_t i = 0; i < pending_prepare_items->hash_size; i++) {
        TAILQ_INIT(&pending_prepare_items->hash[i]); /* hash lists */
    }
    return (0);
}

/*
 * __prepared_discover_find_or_create_item --
 *     We have learned that a prepared transaction with a particular ID exists. If this is the first
 *     time it's been noticed, create an item corresponding to it. Otherwise return the matching
 *     item.
 */
static int
__prepared_discover_find_or_create_item(WT_SESSION_IMPL *session, uint64_t prepared_id,
  wt_timestamp_t prepare_timestamp, WT_PENDING_PREPARED_ITEM **prepared_item)
{
    WT_CONNECTION_IMPL *conn;
    WT_PENDING_PREPARED_ITEM *item;
    WT_PENDING_PREPARED_MAP *pending_prepare_items;
    WT_TXN_GLOBAL *txn_global;
    uint64_t bucket;

    if (__wt_prepared_discover_find_item(session, prepared_id, prepared_item) == 0)
        return (0);

    conn = S2C(session);
    txn_global = &conn->txn_global;
    pending_prepare_items = &txn_global->pending_prepare_items;
    if (pending_prepare_items->hash == NULL) {
        WT_RET(__pending_prepare_items_init(session, pending_prepare_items,
          /* hash size*/ WT_DEFAULT_PENDING_PREPARED_DISCOVER_HASHSIZE));
    }

    WT_RET(__wt_calloc_one(session, &item));
    item->prepared_id = prepared_id;
    item->prepare_timestamp = prepare_timestamp;
    bucket = prepared_id & (pending_prepare_items->hash_size - 1);
    TAILQ_INSERT_HEAD(&pending_prepare_items->hash[bucket], item, hashq);
    *prepared_item = item;
    return (0);
}

/*
 * __wt_prepared_discover_remove_item --
 *     Find and remove a pending prepared item by its ID in the pending prepared items hash map.
 */
int
__wt_prepared_discover_remove_item(WT_SESSION_IMPL *session, uint64_t prepared_id)
{
    WT_CONNECTION_IMPL *conn;
    WT_PENDING_PREPARED_ITEM *item;
    WT_PENDING_PREPARED_MAP *pending_prepare_items;
    WT_TXN_GLOBAL *txn_global;
    uint64_t bucket;
    conn = S2C(session);
    txn_global = &conn->txn_global;
    pending_prepare_items = &txn_global->pending_prepare_items;

    if (pending_prepare_items->hash != NULL) {
        bucket = prepared_id & (pending_prepare_items->hash_size - 1);
        TAILQ_FOREACH (item, &pending_prepare_items->hash[bucket], hashq) {
            if (item->prepared_id == prepared_id) {
                TAILQ_REMOVE(&pending_prepare_items->hash[bucket], item, hashq);
                /* Clean up memory of unclaimed mod array */
                WT_ASSERT_ALWAYS(
                  session, item->mod_count == 0, "Removing an unclaimed prepared item.");
                __wt_free(session, item->mod);
                __wt_free(session, item);
                return (0);
            }
        }
    }
    return (WT_NOTFOUND);
}

/*
 * __wti_prepared_discover_add_artifact_upd --
 *     Add an artifact to a pending prepared transaction.
 */
int
__wti_prepared_discover_add_artifact_upd(WT_SESSION_IMPL *session, WT_UPDATE *upd, WT_ITEM *key)
{
    WT_PENDING_PREPARED_ITEM *prepared_item;
    WT_TXN_OP *op;
    WT_RET(__prepared_discover_find_or_create_item(
      session, upd->prepared_id, upd->prepare_ts, &prepared_item));
    /*
     * We need the key and btree information to help with the search of the update when resolving
     * txn.
     */
    WT_RET(__wt_pending_prepared_next_op(session, &op, prepared_item, key));
    WT_RET(__wt_op_modify(session, upd, op));

    WT_ASSERT(session, op->type == WT_TXN_OP_BASIC_ROW || op->type == WT_TXN_OP_INMEM_ROW);

#ifdef HAVE_DIAGNOSTIC
    ++prepared_item->prepare_count;
#endif
    return (0);
}

/*
 * __prepared_discover_apply_upd_on_ingest --
 *     Search the ingest btree for the key, write the update, and register the prepared artifact.
 *     Must be called with session->dhandle set to the ingest btree.
 *
 * The cursor is reused across keys in the same walk: the row search overwrites cbt->ref without
 *     releasing the prior hazard pointer, so it must be released here before each search.
 *
 * Artifact registration must run with the ingest dhandle active so that op->btree is captured as
 *     the ingest btree; the prepared transaction commit searches that btree to resolve the op.
 */
static int
__prepared_discover_apply_upd_on_ingest(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key, WT_UPDATE *upd)
{
    WT_DECL_RET;

    if (cbt->ref != NULL) {
        WT_RET(__wt_page_release(session, cbt->ref, 0));
        cbt->ref = NULL;
    }
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, key, true, NULL, false, NULL));
    WT_RET(ret);
    WT_RET(__wt_row_modify(cbt, key, NULL, &upd, WT_UPDATE_INVALID, true, true));
    return (__wti_prepared_discover_add_artifact_upd(session, upd, key));
}

/*
 * __wti_prepared_discover_restore_and_add_artifact_upd --
 *     In disaggregated storage, follower nodes cannot modify the stable table, so a prepared update
 *     found on disk must be restored onto the ingest table for later commit. The ingest cursor is
 *     supplied by the caller and reused across all prepared keys in the same btree walk.
 */
int
__wti_prepared_discover_restore_and_add_artifact_upd(WT_SESSION_IMPL *session,
  WT_CURSOR *ingest_cursor, WT_ITEM *key, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_UPDATE *upd;
    size_t size;

    cbt = (WT_CURSOR_BTREE *)ingest_cursor;
    WT_RET(__prepare_discover_alloc_upd(session, value, unpack, &upd, &size));
    WT_WITH_DHANDLE(
      session, cbt->dhandle, ret = __prepared_discover_apply_upd_on_ingest(session, cbt, key, upd));
    return (ret);
}
