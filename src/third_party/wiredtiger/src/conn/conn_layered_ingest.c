/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order);

/*
 * __layered_assert_tombstone_has_value_on_stable_btree --
 *     Assert that a value exists on the stable btree before moving a tombstone intended to delete
 *     it.
 */
static WT_INLINE void
__layered_assert_tombstone_has_value_on_stable_btree(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *last_upd)
{
    bool has_value;

    if (last_upd->type != WT_UPDATE_TOMBSTONE)
        return;

    /*
     * If the last update is a tombstone, ensure that there is a corresponding value on the stable
     * table that it deletes.
     */
    if (cbt->compare != 0)
        /* No on-page value to check; rely solely on visibility. */
        has_value = false;
    else {
        WT_ASSERT_ALWAYS(session, cbt->ins == NULL,
          "The stable btree should not contain inserts prior to draining");
        WT_UPDATE *upd = NULL;
        if (cbt->ref->page->modify != NULL && cbt->ref->page->modify->mod_row_update != NULL)
            upd = cbt->ref->page->modify->mod_row_update[cbt->slot];

        if (upd != NULL) {
            WT_ASSERT_ALWAYS(session, upd->txnid != WT_TXN_ABORTED,
              "The stable btree should not contain aborted updates prior to draining");
            has_value = upd->type != WT_UPDATE_TOMBSTONE;
        } else {
            WT_TIME_WINDOW tw;
            bool tw_found = __wt_read_cell_time_window(cbt, &tw);
            has_value = tw_found && !WT_TIME_WINDOW_HAS_STOP(&tw);
        }
    }

    /*
     * If a globally visible tombstone is observed at the end, the update it deletes may have been
     * removed during the obsolete check.
     */
    WT_ASSERT_ALWAYS(session, has_value || __wt_txn_upd_visible_all(session, last_upd),
      "No corresponding value exists on the stable table to delete");
}

/*
 * __layered_move_updates --
 *     Move the updates of a key to the stable table. Any unresolved prepared update on the stable
 *     table should now have been resolved.
 */
static int
__layered_move_updates(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key,
  WT_UPDATE *upds, WT_UPDATE *last_upd)
{
    WT_DECL_RET;

    /*
     * Disable bulk load if the btree is empty. Otherwise, checkpoint may skip this btree if it has
     * never been checkpointed.
     */
    __wt_btree_disable_bulk(session);

    /* Search the page. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, key, true, NULL, false, NULL));
    WT_ERR(ret);

    __layered_assert_tombstone_has_value_on_stable_btree(session, cbt, last_upd);

    /* Apply the modification. */
    WT_ERR(__wt_row_modify(cbt, key, NULL, &upds, WT_UPDATE_INVALID, false, false));

err:
    WT_TRET(__wt_btcur_reset(cbt));
    return (ret);
}

/*
 * __layered_clear_ingest_table --
 *     After ingest content has been drained to the stable table, clear out the ingest table.
 */
static int
__layered_clear_ingest_table(WT_SESSION_IMPL *session, const char *uri)
{
    WT_ASSERT(session, WT_SUFFIX_MATCH(uri, ".wt_ingest"));

    /*
     * Truncate needs a running txn. We should probably do something more like the history store and
     * make this non-transactional -- this happens during step-up, so we know there are no other
     * transactions running, so it's safe.
     */
    WT_RET(__wt_txn_begin(session, NULL));

    /*
     * No other transactions are running, we're only doing this truncate, and it should become
     * immediately visible. So this transaction doesn't have to care about timestamps.
     */
    F_SET(session->txn, WT_TXN_TS_NOT_SET);

    WT_RET(session->iface.truncate(&session->iface, uri, NULL, NULL, NULL));

    WT_RET(__wt_txn_commit(session, NULL));

    return (0);
}

/*
 * __layered_table_get_constituent_cursor --
 *     Retrieve or open a constituent cursor for a layered tree.
 */
static int
__layered_table_get_constituent_cursor(
  WT_SESSION_IMPL *session, uint32_t ingest_id, WT_CURSOR **cursorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *stable_cursor;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL, NULL};

    conn = S2C(session);
    entry = conn->layered_table_manager.entries[ingest_id];

    *cursorp = NULL;

    if (entry == NULL)
        return (0);

    /* Open the cursor and keep a reference in the manager entry and our caller */
    WT_RET(__wt_open_cursor(session, entry->stable_uri, NULL, cfg, &stable_cursor));
    *cursorp = stable_cursor;

    return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __layered_assert_ingest_table_empty --
 *     Verify that the ingest table has no records. Called after truncation as a post-condition
 *     check.
 */
static int
__layered_assert_ingest_table_empty(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cursor_config[] = {
      WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "readonly", NULL, NULL};

    WT_RET(__wt_open_cursor(session, uri, NULL, cursor_config, &cursor));
    ret = cursor->next(cursor);
    WT_ASSERT(session, ret == WT_NOTFOUND);
    WT_TRET(cursor->close(cursor));

    return (ret == WT_NOTFOUND ? 0 : ret);
}
#endif

/*
 * __layered_copy_ingest_table --
 *     Moving all the data from a single ingest table to the corresponding stable table
 */
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session, WT_LAYERED_TABLE_MANAGER_ENTRY *entry)
{
    WT_CURSOR *ingest_version_cursor, *prepare_cursor, *stable_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(tmp_key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_UPDATE *last_upd, *prev_upd, *upd, *upds;
    wt_timestamp_t last_checkpoint_timestamp;
    wt_timestamp_t durable_start_ts, durable_stop_ts, start_prepare_ts, start_ts, stop_prepare_ts,
      stop_ts;
    uint64_t start_prepared_id, start_txn, stop_prepared_id, stop_txn;
    uint8_t flags, location, prepare, type;
    int cmp;
    char buf[256], buf2[64];
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL, NULL};
    bool is_prepare_rollback, prepare_resolved, preserve_prepared;

    ingest_version_cursor = prepare_cursor = stable_cursor = NULL;
    last_upd = prev_upd = upd = upds = NULL;
    prepare_resolved = false;
    preserve_prepared = F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED);

    last_checkpoint_timestamp = __wt_atomic_load_uint64_acquire(
      &S2C(session)->disaggregated_storage.last_checkpoint_timestamp);
    WT_RET(__layered_table_get_constituent_cursor(session, entry->ingest_id, &stable_cursor));
    cbt = (WT_CURSOR_BTREE *)stable_cursor;
    if (last_checkpoint_timestamp != WT_TS_NONE)
        WT_ERR(__wt_snprintf(
          buf2, sizeof(buf2), "start_timestamp=%" PRIx64 "", last_checkpoint_timestamp));
    else
        buf2[0] = '\0';
    WT_ERR(__wt_snprintf(buf, sizeof(buf),
      "debug=(dump_version=(enabled=true,raw_key_value=true,visible_only=true,timestamp_order=true,"
      "cross_key=true,show_prepared_rollback=%s,%s))",
      preserve_prepared ? "true" : "false", buf2));
    cfg[1] = buf;
    WT_ERR(__wt_open_cursor(session, entry->ingest_uri, NULL, cfg, &ingest_version_cursor));

    WT_ERR(__wt_scr_alloc(session, 0, &key));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp_key));
    WT_ERR(__wt_scr_alloc(session, 0, &value));

    for (;;) {
        upd = NULL;
        WT_ERR_NOTFOUND_OK(ingest_version_cursor->next(ingest_version_cursor), true);
        if (ret == WT_NOTFOUND) {
            if (key->size > 0 && upds != NULL) {
                WT_WITH_DHANDLE(session, cbt->dhandle,
                  ret = __layered_move_updates(session, cbt, key, upds, last_upd));
                WT_ERR(ret);
                upds = NULL;
            } else
                ret = 0;
            break;
        }

        WT_ERR(ingest_version_cursor->get_key(ingest_version_cursor, tmp_key));
        WT_ERR(__wt_compare(session, CUR2BT(cbt)->collator, key, tmp_key, &cmp));
        if (cmp != 0) {
            /*
             * Ensure keys returned are in correctly sorted order. Only perform this check when key
             * has been initialized.
             */
            WT_ASSERT(session, key->size == 0 || cmp <= 0);

            if (upds != NULL) {
                WT_WITH_DHANDLE(session, cbt->dhandle,
                  ret = __layered_move_updates(session, cbt, key, upds, last_upd));
                WT_ERR(ret);
            }

            upds = NULL;
            prev_upd = NULL;
            prepare_resolved = false;
            WT_ERR(__wt_buf_set(session, key, tmp_key->data, tmp_key->size));
        }

        WT_ERR(ingest_version_cursor->get_value(ingest_version_cursor, &start_txn, &start_ts,
          &durable_start_ts, &start_prepare_ts, &start_prepared_id, &stop_txn, &stop_ts,
          &durable_stop_ts, &stop_prepare_ts, &stop_prepared_id, &type, &prepare, &flags, &location,
          value));

        is_prepare_rollback = start_txn == WT_TXN_ABORTED;
        /*
         * It is possible to see a full value that is smaller than or equal to the last checkpoint
         * timestamp with a stop timestamp that is larger than the last checkpoint timestamp. Ignore
         * the update in this case.
         */
        if (durable_start_ts > last_checkpoint_timestamp) {
            /*
             * If the "preserve prepared" option is enabled and the ingest btree contains a resolved
             * prepared update for this key whose prepared timestamp is less than or equal to the
             * last checkpoint timestamp, the stable btree must still contain an unresolved prepared
             * cell from a previous checkpoint. To ensure data consistency, resolve the unresolved
             * prepared cell before applying the ingest updates.
             */
            if (preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE &&
              start_prepare_ts <= last_checkpoint_timestamp) {
                /* Only resolve the updates from the same prepared transaction once. */
                if (!prepare_resolved) {
                    if (is_prepare_rollback) {
                        /*
                         * The original transaction id is stored in start timestamp and the rollback
                         * timestamp is stored in durable timestamp.
                         */
                        WT_TXN_TIME_POINT txn_time_point;
                        txn_time_point.id = start_ts;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.rollback_timestamp = durable_start_ts;
                        WT_ERR(__wt_txn_resolve_prepared_op(session, CUR2BT(cbt), &txn_time_point,
                          key, WT_RECNO_OOB, false, &prepare_cursor));
                    } else {
                        WT_TXN_TIME_POINT txn_time_point;
                        txn_time_point.id = start_txn;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.commit_timestamp = start_ts;
                        txn_time_point.durable_timestamp = durable_start_ts;
                        WT_ERR(__wt_txn_resolve_prepared_op(session, CUR2BT(cbt), &txn_time_point,
                          key, WT_RECNO_OOB, true, &prepare_cursor));
                    }
                    prepare_resolved = true;
                }
            } else {
                /*
                 * If the update is not a prepared update or a resolved prepared update that has
                 * never been written to the checkpoint as a prepared update, move it to the stable
                 * table directly.
                 */
                /*
                 * FIXME-WT-14732: this is an ugly layering violation. But I can't think of a better
                 * way now.
                 */
                if (__wt_clayered_deleted(value))
                    WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
                else
                    WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));
                /*
                 * If the prepared update is aborted, move the aborted update to the stable table
                 * because we may write a prepared update to the disk in a future reconciliation.
                 */
                if (is_prepare_rollback) {
                    /* Prepared transactions must have a prepared id in disagg. */
                    WT_ASSERT(
                      session, preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE);
                    /*
                     * The original transaction id is stored in start timestamp and the rollback
                     * timestamp is stored in durable timestamp.
                     */
                    upd->txnid = WT_TXN_ABORTED;
                    upd->prepare_state = WT_PREPARE_INPROGRESS;
                    upd->prepare_ts = start_prepare_ts;
                    upd->prepared_id = start_prepared_id;
                    upd->upd_saved_txnid = start_ts;
                    upd->upd_rollback_ts = durable_start_ts;
                } else {
                    upd->txnid = start_txn;
                    if (start_prepared_id != WT_PREPARED_ID_NONE)
                        upd->prepare_state = WT_PREPARE_RESOLVED;
                    upd->prepare_ts = start_prepare_ts;
                    upd->prepared_id = start_prepared_id;
                    upd->upd_start_ts = start_ts;
                    upd->upd_durable_ts = durable_start_ts;
                }
                /* This is for debugging purpose and it is not checked in the code. */
                F_SET(upd, WT_UPDATE_RESTORED_FROM_INGEST);
                last_upd = upd;
            }
        }

        if (upd != NULL) {
            /* If a prepared update is resolved, it must be the final update to be drained. */
            WT_ASSERT(session, !prepare_resolved);
            if (prev_upd != NULL)
                prev_upd->next = upd;
            else
                upds = upd;

            prev_upd = upd;
        }
    }

err:
    if (upd != NULL)
        __wt_free(session, upd);
    if (upds != NULL)
        __wt_free_update_list(session, &upds);
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &tmp_key);
    __wt_scr_free(session, &value);
    if (ingest_version_cursor != NULL)
        WT_TRET(ingest_version_cursor->close(ingest_version_cursor));
    if (prepare_cursor != NULL)
        WT_TRET(prepare_cursor->close(prepare_cursor));
    if (stable_cursor != NULL)
        WT_TRET(stable_cursor->close(stable_cursor));
    return (ret);
}

/*
 * __layered_drain_worker_run --
 *     Run function for drain workers.
 */
static int
__layered_drain_worker_run(WT_SESSION_IMPL *session, WT_THREAD *ctx)
{
    WT_DECL_RET;
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_UNUSED(ctx);
    __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
    /* If the queue is empty we are done. */
    if (TAILQ_EMPTY(&conn->layered_drain_data.work_queue)) {
        __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        return (0);
    }

    WT_LAYERED_DRAIN_ENTRY *work_item = TAILQ_FIRST(&conn->layered_drain_data.work_queue);
    WT_ASSERT(session, work_item != NULL);
    TAILQ_REMOVE(&conn->layered_drain_data.work_queue, work_item, q);
    __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    WT_ERR_MSG_CHK(session, __layered_copy_ingest_table(session, work_item->entry),
      "Failed to copy ingest table \"%s\" to stable table \"%s\"", work_item->entry->ingest_uri,
      work_item->entry->stable_uri);
    WT_ERR_MSG_CHK(session, __layered_clear_ingest_table(session, work_item->entry->ingest_uri),
      "Failed to clear ingest table \"%s\"", work_item->entry->ingest_uri);

#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__layered_assert_ingest_table_empty(session, work_item->entry->ingest_uri));
#endif

    WT_ASSERT(session, work_item->entry->pinned_dhandle != NULL);
    WT_WITH_DHANDLE(session, work_item->entry->pinned_dhandle, {
        work_item->entry->pinned_dhandle = NULL;
        __wt_cursor_dhandle_decr_use(session);
    });

err:
    __wt_free(session, work_item);
    return (ret);
}

/*
 * __layered_drain_worker_check --
 *     Check function for drain workers.
 */
static bool
__layered_drain_worker_check(WT_SESSION_IMPL *session)
{
    return (__wt_atomic_load_bool_relaxed(&S2C(session)->layered_drain_data.running));
}

/*
 * __layered_drain_clear_work_queue --
 *     Clear the work queue for ingest table drain.
 */
static void
__layered_drain_clear_work_queue(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
    if (!TAILQ_EMPTY(&conn->layered_drain_data.work_queue)) {
        WT_LAYERED_DRAIN_ENTRY *work_item = NULL, *work_item_tmp = NULL;
        TAILQ_FOREACH_SAFE(work_item, &conn->layered_drain_data.work_queue, q, work_item_tmp)
        {
            TAILQ_REMOVE(&conn->layered_drain_data.work_queue, work_item, q);
            __wt_free(session, work_item);
        }
    }
    WT_ASSERT_ALWAYS(session, TAILQ_EMPTY(&conn->layered_drain_data.work_queue),
      "Layered drain work queue failed to drain");
    __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    __wt_spin_destroy(session, &conn->layered_drain_data.queue_lock);
}

/*
 * __wti_layered_drain_ingest_tables --
 *     Moving all the data from the ingest tables to the stable tables
 */
int
__wti_layered_drain_ingest_tables(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    size_t i, table_count;
    bool empty, group_created;

    conn = S2C(session);
    manager = &conn->layered_table_manager;
    group_created = false;

    __wt_spin_lock(session, &manager->layered_table_lock);

    table_count = manager->open_layered_table_count;

    /*
     * FIXME-WT-14734: shouldn't we hold this lock longer, e.g. manager->entries could get
     * reallocated, or individual entries could get removed or freed.
     */
    __wt_spin_unlock(session, &manager->layered_table_lock);
    /* Initialize the work queue. */
    TAILQ_INIT(&conn->layered_drain_data.work_queue);
    WT_RET(__wt_spin_init(
      session, &conn->layered_drain_data.queue_lock, "layered drain work queue lock"));

    __wt_atomic_store_bool(&conn->layered_drain_data.running, true);

    bool multithreaded = conn->layered_drain_data.thread_count > 1;

    /*
     * Create the thread group. The application thread is also a drain thread so the configured
     * thread count needs to be greater than 1 for this to be meaningful. We still lock and queue
     * work for single threaded mode, as such single threaded is only recommended for testing.
     */
    if (multithreaded) {
        WT_ERR(__wt_thread_group_create(session, &conn->layered_drain_data.threads, "disagg-drain",
          conn->layered_drain_data.thread_count - 1, conn->layered_drain_data.thread_count - 1,
          WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL, __layered_drain_worker_check,
          __layered_drain_worker_run, NULL));
        group_created = true;
    }

    /* FIXME-WT-14735: skip empty ingest tables. */
    for (i = 0; i < table_count; i++) {
        if ((entry = manager->entries[i]) != NULL) {
            /*
             * Mark the layered table in use, we don't want it to be closed between now and when the
             * drain takes place, otherwise this entry would be freed.
             */
            WT_ERR(__wt_cursor_uri_incr_use(session, entry->layered_uri, &entry->pinned_dhandle));

            WT_LAYERED_DRAIN_ENTRY *work_item;
            WT_ERR(__wt_calloc_one(session, &work_item));
            work_item->entry = entry;
            __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
            TAILQ_INSERT_HEAD(&conn->layered_drain_data.work_queue, work_item, q);
            __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        }
    }

    /*
     * We can be lazy here and use the current thread as a worker thread. Then once this loop exits
     * we can kill our thread group.
     */
    while (true) {
        __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
        empty = TAILQ_EMPTY(&conn->layered_drain_data.work_queue);
        __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
        if (empty) {
            /*
             * Notify the other threads to exit. Relaxed is okay here as the worker threads will
             * observe this change eventually.
             */
            __wt_atomic_store_bool_relaxed(&conn->layered_drain_data.running, false);
            break;
        }
        WT_ERR(__layered_drain_worker_run(session, NULL));
    }

err:
    /* Let any running threads finish up. */
    if (group_created) {
        __wt_cond_signal(session, conn->layered_drain_data.threads.wait_cond);
        __wt_writelock(session, &conn->layered_drain_data.threads.lock);
        WT_TRET(__wt_thread_group_destroy(session, &conn->layered_drain_data.threads));
    }
    /* Cleanup and release resources. */
    __layered_drain_clear_work_queue(session);
    return (ret);
}

/*
 * __layered_update_ingest_table_prune_timestamp --
 *     Update the prune timestamp of the specified ingest table.
 *
 * We want to see what is the oldest checkpoint on the provided table that is in use by any open
 *     cursor. Even if there are no open cursors on it, the most recent checkpoint on the table is
 *     always considered in use. The basic plan is to start with the last checkpoint in use that we
 *     knew about, and check it again. If it's no longer in use, we go to the next one, etc. This
 *     gives us a list (possibly zero length), of checkpoints that are no longer in use by cursors
 *     on this table. Thus, the timestamp associated with the newest such checkpoint can be used for
 *     garbage collection pruning. Any item in the ingest table older than that timestamp must be
 *     including in one of the checkpoints we're saving, and thus can be removed.
 *
 * The `uri_at_checkpoint_buf` argument is used only to avoid extra allocations between consecutive
 *     calls.
 */
static int
__layered_update_ingest_table_prune_timestamp(WT_SESSION_IMPL *session, const char *layered_uri,
  wt_timestamp_t checkpoint_timestamp, WT_ITEM *uri_at_checkpoint_buf)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table;
    wt_timestamp_t prune_timestamp;
    int64_t ckpt_inuse, last_ckpt;
    int32_t layered_dhandle_inuse, stable_dhandle_inuse;

    layered_table = NULL;
    prune_timestamp = WT_TS_NONE;

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     */
    WT_RET_NOTFOUND_OK(__wt_session_get_dhandle(session, layered_uri, NULL, NULL, 0));
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Layered table was not found.", layered_uri);
        return (0);
    }
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    /*
     * Get the last existing checkpoint. If we've never seen a checkpoint, then there's nothing in
     * the ingest table we can remove. Move on.
     */
    WT_ERR_NOTFOUND_OK(
      __layered_last_checkpoint_order(session, layered_table->stable_uri, &last_ckpt), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Layered table checkpoint does not exist: %s", layered_table->iface.name,
          layered_table->stable_uri);
        ret = 0;
        goto err;
    }

    /*
     * If we are setting a prune timestamp the first time, the previous checkpoint could still be in
     * use, so start from it.
     */
    ckpt_inuse = layered_table->last_ckpt_inuse;
    if (ckpt_inuse == 0)
        ckpt_inuse = (last_ckpt > 1) ? last_ckpt - 1 : last_ckpt;

    /* Find the last checkpoint which is still in use. */
    while (ckpt_inuse < last_ckpt) {
        stable_dhandle_inuse = 0;
        WT_ERR(__wt_buf_fmt(session, uri_at_checkpoint_buf, "%s/%s.%" PRId64,
          layered_table->stable_uri, WT_CHECKPOINT, ckpt_inuse));

        /* If it's in use, then it must be in the connection cache. */
        WT_WITH_HANDLE_LIST_READ_LOCK(session,
          if ((ret = __wt_conn_dhandle_find(session, uri_at_checkpoint_buf->data, NULL)) == 0)
            WT_DHANDLE_ACQUIRE(session->dhandle));

        /* If one exists, read all the required info, then release. */
        if (ret == 0) {
            stable_dhandle_inuse = __wt_atomic_load_int32_acquire(&session->dhandle->session_inuse);
            WT_ASSERT(session, prune_timestamp <= S2BT(session)->checkpoint_timestamp);
            prune_timestamp = S2BT(session)->checkpoint_timestamp;
            WT_DHANDLE_RELEASE(session->dhandle);
        }

        WT_ERR_NOTFOUND_OK(ret, false);

        /* If it's in use by any session, then we're done. */
        if (stable_dhandle_inuse > 0)
            break;

        ++ckpt_inuse;
    }

    layered_dhandle_inuse =
      __wt_atomic_load_int32_acquire(&((WT_DATA_HANDLE *)layered_table)->session_inuse);
    if (ckpt_inuse == last_ckpt && (last_ckpt != 1 || layered_dhandle_inuse == 0))
        prune_timestamp = checkpoint_timestamp;

    if (ckpt_inuse == layered_table->last_ckpt_inuse) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Nothing to update - the last checkpoint is still in use %" PRId64,
          layered_table->iface.name, ckpt_inuse);
        ret = 0;
        goto err;
    }

    /*
     * Set the prune timestamp in the btree if it is open, typically it is. However, it's possible
     * that it hasn't been opened yet. In that case, we need to skip updating its timestamp for
     * pruning, and we'll get another chance to update the prune timestamp at the next checkpoint.
     */
    WT_ERR_NOTFOUND_OK(
      __wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0), true);
    if (ret == WT_NOTFOUND) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Handle not found for ingest table uri: %s", layered_table->iface.name,
          layered_table->ingest_uri);
        ret = 0;
        goto err;
    }

    btree = (WT_BTREE *)session->dhandle->handle;

    if (prune_timestamp != WT_TS_NONE) {
        uint64_t btree_prune_timestamp = __wt_atomic_load_uint64_relaxed(&btree->prune_timestamp);
        WT_ASSERT(session, prune_timestamp >= btree_prune_timestamp);

        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: update prune timestamp from %" PRIu64 " to %" PRIu64
          " and checkpoint in use from %" PRId64 " to %" PRId64,
          layered_table->iface.name, btree_prune_timestamp, prune_timestamp,
          layered_table->last_ckpt_inuse, ckpt_inuse);

        /*
         * The prune timestamp should be monotonically increasing. It is fine for the user to read
         * the obsolete value. Therefore, no synchronization is required.
         */
        __wt_atomic_store_uint64_relaxed(&btree->prune_timestamp, prune_timestamp);
        layered_table->last_ckpt_inuse = ckpt_inuse;
    }

    WT_ERR(__wt_session_release_dhandle(session));

err:
    WT_ASSERT(session, layered_table != NULL);
    session->dhandle = (WT_DATA_HANDLE *)layered_table;
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wti_layered_iterate_ingest_tables_for_gc_pruning --
 *     Iterate over all ingest tables and check whether their prune timestamps could be updated.
 */
int
__wti_layered_iterate_ingest_tables_for_gc_pruning(
  WT_SESSION_IMPL *session, wt_timestamp_t checkpoint_timestamp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(layered_table_uri_buf);
    WT_DECL_ITEM(uri_at_checkpoint_buf);
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    size_t i;

    conn = S2C(session);
    manager = &conn->layered_table_manager;
    WT_RET(__wt_scr_alloc(session, 0, &layered_table_uri_buf));
    WT_RET(__wt_scr_alloc(session, 0, &uri_at_checkpoint_buf));

    WT_ASSERT(session, manager->init);

    __wt_spin_lock(session, &manager->layered_table_lock);
    for (i = 0; i < manager->open_layered_table_count; i++) {
        if ((entry = manager->entries[i]) == NULL)
            continue;
        ret = __wt_buf_setstr(session, layered_table_uri_buf, entry->layered_uri);

        /*
         * Unlock the mutex while handling a table since while updating the prune timestamp we get a
         * dhandle lock which could cause a deadlock.
         *
         * Releasing the mutex may allow the table to grow, shrink or be modified during this
         * operation. It's okay to prune an element twice in a loop (the second pruning will
         * probably do nothing), or miss an element to prune (it will be visited next time).
         */
        __wt_spin_unlock(session, &manager->layered_table_lock);

        /* Check the buffer-copy result here to avoid returning with the mutex held. */
        WT_ERR(ret);

        WT_ERR(__layered_update_ingest_table_prune_timestamp(
          session, layered_table_uri_buf->data, checkpoint_timestamp, uri_at_checkpoint_buf));

        __wt_spin_lock(session, &manager->layered_table_lock);
    }
    __wt_spin_unlock(session, &manager->layered_table_lock);

err:
    if (ret != 0)
        __wt_verbose_level(
          session, WT_VERB_LAYERED, WT_VERBOSE_ERROR, "GC ingest tables prune failed by: %d", ret);

    __wt_scr_free(session, &layered_table_uri_buf);
    __wt_scr_free(session, &uri_at_checkpoint_buf);
    return (ret);
}

/*
 * __layered_last_checkpoint_order --
 *     For a URI, get the order number for the most recent checkpoint.
 */
static int
__layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order)
{
    int scanf_ret;

    const char *checkpoint_name;
    int64_t order_from_name;

    *ckpt_order = 0;

    /* Pull up the last checkpoint for this URI. It could return WT_NOTFOUND. */
    WT_RET(__wt_meta_checkpoint_last_name(session, shared_uri, &checkpoint_name, ckpt_order, NULL));

    /* Sanity check: we make sure that the name returned matches the order number. */
    scanf_ret = sscanf(checkpoint_name, WT_CHECKPOINT ".%" PRId64, &order_from_name);
    __wt_free(session, checkpoint_name);

    if (scanf_ret != 1)
        WT_RET_MSG(session, EINVAL,
          "shared metadata checkpoint unknown format: %s, scan returns %d", checkpoint_name,
          scanf_ret);

    /* These should always be the same. */
    WT_ASSERT(session, *ckpt_order == order_from_name);

    return (0);
}
