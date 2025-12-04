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
 * __wti_prepared_discover_restore_and_add_artifact_upd --
 *     In disaggregated storage, in follower mode, stable table cannot be modified, therefore a
 *     prepared update needs to be restored onto ingest table so that the follower node can then
 *     commit the prepared transaction. This function opens the ingest table and inserts the update
 *     restored from disk onto the ingest table.
 */
int
__wti_prepared_discover_restore_and_add_artifact_upd(WT_SESSION_IMPL *session,
  const char *stable_uri, WT_ITEM *key, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    WT_UPDATE *upd;
    uint32_t i, table_count;

    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL, NULL};

    cursor = NULL;
    entry = NULL;
    conn = S2C(session);
    manager = &conn->layered_table_manager;
    table_count = manager->open_layered_table_count;
    for (i = 0; i < table_count; i++) {
        /* Find the entry with stable uri that matches the currently opened dhandle. */
        if (manager->entries[i] != NULL) {
            if (WT_PREFIX_MATCH(stable_uri, manager->entries[i]->stable_uri)) {
                entry = manager->entries[i];
                break;
            }
        }
    }
    WT_ASSERT_ALWAYS(
      session, entry != NULL, "Unable to find matching ingest table to restore prepared update");
    /* Open cursor on the ingest table */
    WT_ERR(__wt_open_cursor(session, entry->ingest_uri, NULL, cfg, &cursor));

    cbt = (WT_CURSOR_BTREE *)cursor;
    size_t size;
    WT_ERR(__wt_page_inmem_update(session, value, unpack, &upd, &size));

    /* Search the page and apply the modification. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, key, true, NULL, false, NULL));
    WT_ERR(ret);
    WT_ERR(__wt_row_modify(cbt, key, NULL, &upd, WT_UPDATE_INVALID, true, true));
    WT_ERR(__wti_prepared_discover_add_artifact_upd(session, upd, key));
err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}
