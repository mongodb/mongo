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
__wti_prepared_discover_add_artifact_upd(
  WT_SESSION_IMPL *session, uint64_t prepared_id, WT_ITEM *key, WT_UPDATE *upd)
{
    WT_PENDING_PREPARED_ITEM *prepared_item;
    WT_TXN_OP *op;

    WT_RET(__prepared_discover_find_or_create_item(
      session, prepared_id, upd->prepare_ts, &prepared_item));

    WT_RET(__wt_pending_prepared_next_op(session, &op, prepared_item, key));
    WT_RET(__wt_op_modify(session, upd, op));

    WT_ASSERT(session, op->type == WT_TXN_OP_BASIC_ROW || op->type == WT_TXN_OP_INMEM_ROW);

#ifdef HAVE_DIAGNOSTIC
    ++prepared_item->prepare_count;
#endif
    return (0);
}

/*
 * __wti_prepared_discover_add_artifact_ondisk_row --
 *     Add an artifact to a pending prepared transaction.
 */
int
__wti_prepared_discover_add_artifact_ondisk_row(
  WT_SESSION_IMPL *session, uint64_t prepared_id, WT_TIME_WINDOW *tw, WT_ITEM *key)
{
    WT_DECL_RET;
    WT_UPDATE *upd;

    /*
     * Create an update structure with the time information and state populated - that allows this
     * code to reuse existing machinery for installing transaction operations. FIXME-WT-15346 Handle
     * claiming prepared delete.
     */
    WT_RET(__wt_upd_alloc(session, NULL, WT_UPDATE_STANDARD, &upd, NULL));
    upd->txnid = session->txn->id;
    upd->upd_durable_ts = tw->durable_start_ts;
    upd->prepare_state = WT_PREPARE_INPROGRESS;
    upd->prepared_id = prepared_id;
    upd->upd_start_ts = upd->prepare_ts = tw->start_prepare_ts;
    WT_ERR(__wti_prepared_discover_add_artifact_upd(session, prepared_id, key, upd));
err:
    /*
     * It's OK to free the update now, the transaction structure will lookup using the key since
     * this is for a prepared transaction.
     */
    __wt_free_update_list(session, &upd);
    return (ret);
}
