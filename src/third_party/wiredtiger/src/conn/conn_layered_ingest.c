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
 * __layered_assert_stable_btree_state --
 *     Assert stable btree invariants before applying ingest updates for a key: (1) no unresolved
 *     preserved prepared update exists; and (2) if the ingest chain ends with a tombstone, a
 *     corresponding value exists to delete.
 */
static WT_INLINE void
__layered_assert_stable_btree_state(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *last_upd)
{
    WT_UPDATE *upd;
    bool has_value;

    if (cbt->compare != 0) {
        if (last_upd->type != WT_UPDATE_TOMBSTONE)
            return;
        /* No on-page value to check; rely solely on visibility. */
        has_value = false;
    } else {
        WT_ASSERT_ALWAYS(session, cbt->ins == NULL,
          "The stable btree should not contain inserts prior to draining");

        if (cbt->ref->page->modify != NULL && cbt->ref->page->modify->mod_row_update != NULL)
            upd = cbt->ref->page->modify->mod_row_update[cbt->slot];
        else
            upd = NULL;

        /*
         * Walk the chain: assert no unresolved preserved prepared update exists, and advance past
         * any rolled-back preserved prepared updates to find the first visible update.
         */
        for (; upd != NULL; upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED) {
                WT_ASSERT_ALWAYS(session, upd->prepare_state == WT_PREPARE_INPROGRESS,
                  "During ingest drain, aborted updates on the stable btree must be "
                  "rolled-back preserved prepared transactions");
                continue;
            }

            WT_ASSERT_ALWAYS(session, upd->prepare_state != WT_PREPARE_INPROGRESS,
              "During ingest drain, found an unresolved prepared update on the stable btree; "
              "prepared transactions must be resolved before step-up");
            break;
        }

        if (last_upd->type != WT_UPDATE_TOMBSTONE)
            return;

        if (upd != NULL)
            has_value = upd->type != WT_UPDATE_TOMBSTONE;
        else {
            WT_TIME_WINDOW tw;
            bool tw_found = __wt_read_cell_time_window(cbt, &tw);
            has_value =
              tw_found && !WT_TIME_WINDOW_HAS_PREPARE(&tw) && !WT_TIME_WINDOW_HAS_STOP(&tw);
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
  WT_UPDATE *upds, WT_UPDATE *last_upd, wt_timestamp_t from_ts)
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

    /* We only need to check on the first pass. */
    if (from_ts == WT_TS_NONE)
        __layered_assert_stable_btree_state(session, cbt, last_upd);

    /*
     * If the oldest update being moved is an aborted prepared update and the stable btree has no
     * existing value for this key, append a globally visible tombstone after the chain. Any newer
     * updates may themselves be non-stable while the update's rollback timestamp has already become
     * stable; without a fallback below, reconciliation has nothing to write in place of the aborted
     * prepared update, leaving an orphaned prepared value on the disk image. The tombstone keeps
     * the post-rollback state well-defined (the key never existed).
     */
    if (cbt->compare != 0 && last_upd->txnid == WT_TXN_ABORTED) {
        WT_ASSERT(session, last_upd->prepared_id != WT_PREPARED_ID_NONE);
        WT_UPDATE *tombstone;
        WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
        last_upd->next = tombstone;
    }

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
    WT_DECL_RET;

    WT_ASSERT(session, WT_URI_IS_INGEST(uri));

    /*
     * Clearing the ingest table is final and owned by no transaction. The session flag makes the
     * truncate write globally visible tombstones that are immediately visible to every reader.
     */
    F_SET(session, WT_SESSION_NON_TRANSACTIONAL_TRUNCATE);
    ret = session->iface.truncate(&session->iface, uri, NULL, NULL, NULL);
    F_CLR(session, WT_SESSION_NON_TRANSACTIONAL_TRUNCATE);

    return (ret);
}

/*
 * __layered_reset_ingest_table_prune_timestamp --
 *     Reset the prune timestamp for the ingest table.
 *
 * This is used when connection steps up from follower to leader. Resetting the prune timestamp to
 *     WT_TS_NONE will allow immediate eviction of dirty ingest pages. These dirty pages are not
 *     needed any more since the new leader just drained all the ingest content to the stable table.
 */
static int
__layered_reset_ingest_table_prune_timestamp(WT_SESSION_IMPL *session, const char *ingest_uri)
{
    WT_BTREE *btree = NULL;
    WT_DECL_RET;
    wt_timestamp_t btree_prune_timestamp;

    WT_RET_ERROR_OK(ret = __wt_session_get_dhandle(session, ingest_uri, NULL, NULL, 0), ENOENT);
    if (ret == ENOENT) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "Handle not found for ingest table uri: %s", ingest_uri);
        return (0);
    }

    btree = (WT_BTREE *)session->dhandle->handle;
    btree_prune_timestamp = __wt_atomic_load_uint64_relaxed(&btree->prune_timestamp);

    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "Reset prune timestamp from %" PRIu64 " to WT_TS_NONE(%d)", btree_prune_timestamp,
      WT_TS_NONE);

    __wt_atomic_store_uint64_relaxed(&btree->prune_timestamp, WT_TS_NONE);

    WT_RET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __layered_derive_stable_uri --
 *     Derive the stable constituent URI corresponding to an ingest constituent URI. The result is
 *     written into the caller's scratch buffer, which must already be allocated.
 */
static int
__layered_derive_stable_uri(WT_SESSION_IMPL *session, const char *ingest_uri, WT_ITEM *buf)
{
    static const char ingest_suffix[] = ".wt_ingest";
    size_t prefix_len, uri_len;

    uri_len = strlen(ingest_uri);
    WT_ASSERT_ALWAYS(session, uri_len > sizeof(ingest_suffix) - 1,
      "Ingest URI is too short to contain an ingest suffix");
    prefix_len = uri_len - (sizeof(ingest_suffix) - 1);
    WT_ASSERT_ALWAYS(session, strcmp(ingest_uri + prefix_len, ingest_suffix) == 0,
      "Ingest URI does not end in the expected ingest suffix");
    return (__wt_buf_fmt(session, buf, "%.*s.wt_stable", (int)prefix_len, ingest_uri));
}

/*
 * __layered_derive_layered_uri --
 *     Derive the parent layered URI from a constituent ingest URI.
 */
static int
__layered_derive_layered_uri(WT_SESSION_IMPL *session, const char *ingest_uri, WT_ITEM *buf)
{
    static const char file_prefix[] = "file:";
    static const char ingest_suffix[] = ".wt_ingest";
    size_t uri_len = strlen(ingest_uri);
    size_t prefix_len = strlen(file_prefix);
    size_t suffix_len = strlen(ingest_suffix);

    if (!WT_PREFIX_MATCH(ingest_uri, file_prefix) || !WT_URI_IS_INGEST(ingest_uri))
        WT_RET_MSG(session, EINVAL,
          "Ingest URI \"%s\" does not match expected file:<name>.wt_ingest shape", ingest_uri);
    WT_ASSERT(session, uri_len > prefix_len + suffix_len);
    size_t name_len = uri_len - prefix_len - suffix_len;
    return (__wt_buf_fmt(session, buf, "layered:%.*s", (int)name_len, ingest_uri + prefix_len));
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
 * __layered_fix_prepared_transaction_callback --
 *     Callback for session walk to fix prepared transactions that may be active during the ingest
 *     btree drain.
 */
static int
__layered_fix_prepared_transaction_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_FIX_PREPARED_COOKIE *cookie;
    WT_TXN *txn;
    bool patched;

    cookie = (WT_FIX_PREPARED_COOKIE *)cookiep;
    txn = array_session->txn;
    *exit_walkp = false;
    patched = false;

    if (!F_ISSET(txn, WT_TXN_PREPARE))
        return (0);

    /*
     * Prefer matching by transaction id: a live in-flight prepared transaction that survived
     * step-up shares its session's transaction id with the on-disk start record. Only fall back to
     * the prepared id when the transaction id does not match, which covers sessions that reclaimed
     * the prepared transaction from a checkpoint at startup recovery -- those sessions have no
     * transaction id assigned but do carry a prepared id.
     */
    if (!F_ISSET(&txn->time_point, WT_TXN_TIME_POINT_HAS_ID)) {
        WT_ASSERT(session, F_ISSET(&txn->time_point, WT_TXN_TIME_POINT_HAS_PREPARED_ID));
        if (txn->time_point.prepared_id != cookie->prepared_id)
            return (0);
    } else if (txn->time_point.id != cookie->txnid)
        return (0);
    else
        WT_ASSERT(session,
          !F_ISSET(&txn->time_point, WT_TXN_TIME_POINT_HAS_PREPARED_ID) ||
            txn->time_point.prepared_id == cookie->prepared_id);

    for (size_t i = 0; i < txn->mod_count; i++) {
        WT_TXN_OP *op = &txn->mod[i];

        if (op->type == WT_TXN_OP_NONE)
            continue;

        if (op->btree != cookie->ingest_btree)
            continue;

        int cmp;
        WT_RET(__wt_compare(session, op->btree->collator, &op->u.op_row.key, cookie->key, &cmp));

        if (cmp < 0)
            continue;

        /*
         * The operation keys in a prepared transaction are sorted. We have passed the key we're
         * looking for.
         */
        if (cmp > 0)
            break;

        /*
         * Mark the original update on the ingest btree as aborted. Otherwise, we may get a
         * WT_ROLLBACK error when we try to truncate the ingest btree.
         */
        op->u.op_upd->txnid = WT_TXN_ABORTED;
        /* Point the operation to the stable btree. */
        op->btree = cookie->stable_btree;

        /*
         * Transfer the session_inuse reference from the ingest btree to the stable btree. The
         * ingest btree's session_inuse was incremented when this operation was recorded in the
         * transaction, and op->btree's (now the stable btree) session_inuse will be decremented
         * when the operation is freed. Adjust both counts to keep them balanced.
         */
        (void)__wt_atomic_sub_int32(&cookie->ingest_btree->dhandle->session_inuse, 1);
        (void)__wt_atomic_add_int32(&cookie->stable_btree->dhandle->session_inuse, 1);
        patched = true;
    }

    /*
     * Only stop the walk when this session actually owned the key. In a split-prepared scenario two
     * sessions can share the same prepared_id: one session reclaimed the id from a checkpoint (no
     * transaction id assigned) and holds mods for some tables, while a second live session holds
     * mods for different tables with the same id. If the first session matched by prepared_id but
     * had no mods for this ingest btree, the walk must continue so the second session can be found.
     */
    *exit_walkp = patched;
    return (0);
}

/*
 * __layered_fix_prepared_transaction --
 *     During ingest drain, a key that was prepared on the ingest btree is being moved to the stable
 *     btree. If the owning transaction is still in-flight (not yet committed or rolled back), its
 *     WT_TXN_OP entries still reference the ingest btree and the in-memory update on it. This
 *     function patches those entries so that commit/rollback will operate on the stable btree
 *     instead. For each matching operation it: (1) aborts the original in-memory update on the
 *     ingest btree so that a subsequent truncate of the ingest table does not trip over a live
 *     prepared update, (2) redirects op->btree to the stable btree, and (3) transfers the
 *     session_inuse reference from the ingest dhandle to the stable dhandle to keep reference
 *     counts balanced.
 *
 * The owning session is identified by either the on-disk transaction id (set on a session whose
 *     prepared transaction remained in-flight across step-up) or the on-disk prepared id (set on a
 *     session that reclaimed the prepared transaction from a checkpoint at startup recovery, where
 *     no transaction id is assigned).
 *
 * This is a temporary solution. It assumes no concurrent commit/rollback of the prepared
 *     transaction and no prepared fast-truncate operations.
 */
static int
__layered_fix_prepared_transaction(WT_SESSION_IMPL *session, WT_ITEM *key, WT_BTREE *ingest_btree,
  WT_BTREE *stable_btree, uint64_t txnid, uint64_t prepared_id)
{
    WT_FIX_PREPARED_COOKIE cookie;

    cookie.key = key;
    cookie.ingest_btree = ingest_btree;
    cookie.stable_btree = stable_btree;
    cookie.txnid = txnid;
    cookie.prepared_id = prepared_id;

    return (
      __wt_session_array_walk(session, __layered_fix_prepared_transaction_callback, true, &cookie));
}

/*
 * __layered_apply_truncate_to_stable --
 *     Replay a single follower-recorded truncate against stable. This needs to be done after all
 *     older ingest updates have been drained.
 */
static int
__layered_apply_truncate_to_stable(WT_SESSION_IMPL *session, WT_TRUNCATE *t)
{
    WT_DECL_RET;

    WT_ASSERT(session, t->start_key.size > 0 && t->stop_key.size > 0);
    WT_ASSERT(session, t->start_ts > WT_TS_NONE);
    WT_ASSERT(session, t->durable_ts >= t->start_ts);

    const char *open_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "raw=true", NULL};
    WT_CURSOR *trunc_start = NULL, *trunc_stop = NULL;
    WT_ERR(__wt_open_cursor(session, t->layered_table->stable_uri, NULL, open_cfg, &trunc_start));
    WT_ERR(__wt_open_cursor(session, t->layered_table->stable_uri, NULL, open_cfg, &trunc_stop));

    trunc_start->set_key(trunc_start, &t->start_key);
    trunc_stop->set_key(trunc_stop, &t->stop_key);

    session->replay_trunc_ctx.txn_id = t->txn_id;
    session->replay_trunc_ctx.commit_ts = t->start_ts;
    session->replay_trunc_ctx.durable_ts = t->durable_ts;

    F_SET(session, WT_SESSION_INGEST_REPLAY);
    ret = __wt_session_range_truncate(session, NULL, trunc_start, trunc_stop);
    F_CLR(session, WT_SESSION_INGEST_REPLAY);

err:
    if (trunc_start != NULL)
        WT_TRET(trunc_start->close(trunc_start));
    if (trunc_stop != NULL)
        WT_TRET(trunc_stop->close(trunc_stop));
    return (ret);
}

/*
 * __layered_copy_ingest_table --
 *     Move ingest updates whose durable timestamp falls in (from_ts, to_ts) to the corresponding
 *     stable table.
 */
static int
__layered_copy_ingest_table(
  WT_SESSION_IMPL *session, const char *ingest_uri, wt_timestamp_t from_ts, wt_timestamp_t to_ts)
{
    WT_BTREE *ingest_btree, *stable_btree;
    WT_CURSOR *ingest_btree_cursor, *ingest_version_cursor, *prepare_cursor, *stable_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(stable_uri_buf);
    WT_DECL_ITEM(tmp_key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_UPDATE *last_upd, *prev_upd, *upd, *upds;
    wt_timestamp_t cursor_start_ts, last_checkpoint_timestamp;
    wt_timestamp_t durable_start_ts, durable_stop_ts, start_prepare_ts, start_ts, stop_prepare_ts,
      stop_ts;
    uint64_t start_prepared_id, start_txn, stop_prepared_id, stop_txn;
    uint8_t flags, location, prepare, type;
    int cmp;
    char buf[256], buf2[64];
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL, NULL};
    const char *open_cfg[] = {
      WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL, NULL};
    bool in_ts_range, is_prepare_rollback, prepare_resolved, preserve_prepared, prepare_txn_fixed;

    ingest_version_cursor = prepare_cursor = stable_cursor = NULL;
    last_upd = prev_upd = upd = upds = NULL;
    prepare_resolved = prepare_txn_fixed = false;
    preserve_prepared = F_ISSET(S2C(session), WT_CONN_PRESERVE_PREPARED);

    WT_RET(__wt_scr_alloc(session, 0, &stable_uri_buf));
    WT_ERR(__layered_derive_stable_uri(session, ingest_uri, stable_uri_buf));

    last_checkpoint_timestamp = __wt_atomic_load_uint64_acquire(
      &S2C(session)->disaggregated_storage.last_checkpoint_timestamp);
    WT_ERR(__wt_open_cursor(session, stable_uri_buf->data, NULL, open_cfg, &stable_cursor));
    cbt = (WT_CURSOR_BTREE *)stable_cursor;
    stable_btree = CUR2BT(cbt);

    /*
     * The version cursor skips updates at or below cursor_start_ts to avoid re-draining data
     * already covered by a previous pass or a checkpoint.
     */
    cursor_start_ts = (from_ts > last_checkpoint_timestamp) ? from_ts : last_checkpoint_timestamp;
    if (cursor_start_ts != WT_TS_NONE)
        WT_ERR(__wt_snprintf(buf2, sizeof(buf2), "start_timestamp=%" PRIx64 "", cursor_start_ts));
    else
        buf2[0] = '\0';
    WT_ERR(__wt_snprintf(buf, sizeof(buf),
      "debug=(dump_version=(enabled=true,raw_key_value=true,timestamp_order=true,cross_key=true,"
      "show_prepared_rollback=%s,%s))",
      preserve_prepared ? "true" : "false", buf2));
    cfg[1] = buf;
    WT_ERR(__wt_open_cursor(session, ingest_uri, NULL, cfg, &ingest_version_cursor));
    ingest_btree_cursor = ((WT_CURSOR_VERSION *)ingest_version_cursor)->file_cursor;
    ingest_btree = CUR2BT(ingest_btree_cursor);

    WT_ERR(__wt_scr_alloc(session, 0, &key));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp_key));
    WT_ERR(__wt_scr_alloc(session, 0, &value));

    for (;;) {
        upd = NULL;
        WT_ERR_NOTFOUND_OK(ingest_version_cursor->next(ingest_version_cursor), true);
        if (ret == WT_NOTFOUND) {
            if (key->size > 0 && upds != NULL) {
                WT_WITH_DHANDLE(session, cbt->dhandle,
                  ret = __layered_move_updates(session, cbt, key, upds, last_upd, from_ts));
                WT_ERR(ret);
                upds = NULL;
            } else
                ret = 0;
            break;
        }

        WT_ERR(ingest_version_cursor->get_key(ingest_version_cursor, tmp_key));
        WT_ERR(__wt_compare(session, stable_btree->collator, key, tmp_key, &cmp));
        if (cmp != 0) {
            /*
             * Ensure keys returned are in correctly sorted order. Only perform this check when key
             * has been initialized.
             */
            WT_ASSERT(session, key->size == 0 || cmp <= 0);

            if (upds != NULL) {
                WT_WITH_DHANDLE(session, cbt->dhandle,
                  ret = __layered_move_updates(session, cbt, key, upds, last_upd, from_ts));
                WT_ERR(ret);
            }

            upds = NULL;
            prev_upd = NULL;
            prepare_txn_fixed = false;
            prepare_resolved = false;
            WT_ERR(__wt_buf_set(session, key, tmp_key->data, tmp_key->size));
        }

        WT_ERR(ingest_version_cursor->get_value(ingest_version_cursor, &start_txn, &start_ts,
          &durable_start_ts, &start_prepare_ts, &start_prepared_id, &stop_txn, &stop_ts,
          &durable_stop_ts, &stop_prepare_ts, &stop_prepared_id, &type, &prepare, &flags, &location,
          value));

        is_prepare_rollback = start_txn == WT_TXN_ABORTED;
        /*
         * Only process updates whose durable timestamp falls in the range. Prepared updates are
         * included only in the final pass since their commit timestamp is not yet resolved.
         */
        in_ts_range = prepare ? (to_ts == WT_TS_MAX) :
                                (durable_start_ts > from_ts && durable_start_ts <= to_ts);
        if (in_ts_range) {
            /*
             * If the "preserve prepared" option is enabled and the ingest btree contains a resolved
             * prepared update for this key whose prepared timestamp is less than or equal to the
             * last checkpoint timestamp, the stable btree must still contain an unresolved prepared
             * cell from a previous checkpoint. To ensure data consistency, resolve the unresolved
             * prepared cell before applying the ingest updates.
             */
            if (preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE &&
              start_prepare_ts <= last_checkpoint_timestamp) {
                if (prepare) {
                    if (!prepare_txn_fixed) {
                        WT_ASSERT(session, upds == NULL);
                        WT_ERR(__layered_fix_prepared_transaction(
                          session, key, ingest_btree, stable_btree, start_txn, start_prepared_id));
                        prepare_txn_fixed = true;
                    }
                } else if (!prepare_resolved) {
                    /* Only resolve the updates from the same prepared transaction once. */
                    WT_ASSERT(session, to_ts == WT_TS_MAX);
                    if (is_prepare_rollback) {
                        /*
                         * The original transaction id is stored in start timestamp and the rollback
                         * timestamp is stored in durable timestamp.
                         */
                        WT_TXN_TIME_POINT txn_time_point;
                        WT_ASSERT(session, start_prepared_id != WT_PREPARED_ID_NONE);
                        WT_ASSERT(session, start_prepare_ts != WT_TS_NONE);
                        WT_ASSERT(session, durable_start_ts != WT_TS_NONE);
                        WT_CLEAR(txn_time_point);
                        txn_time_point.id = start_ts;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.rollback_timestamp = durable_start_ts;
                        F_SET(&txn_time_point,
                          WT_TXN_TIME_POINT_HAS_PREPARED_ID | WT_TXN_TIME_POINT_HAS_TS_PREPARE |
                            WT_TXN_TIME_POINT_HAS_TS_ROLLBACK);
                        /* Sessions that claimed by prepared id alone carry no transaction id. */
                        if (start_ts != WT_TXN_NONE)
                            F_SET(&txn_time_point, WT_TXN_TIME_POINT_HAS_ID);
                        WT_ERR(__wt_txn_resolve_prepared_op(session, stable_btree, &txn_time_point,
                          key, WT_RECNO_OOB, false, &prepare_cursor));
                    } else {
                        WT_TXN_TIME_POINT txn_time_point;
                        WT_ASSERT(session, start_prepared_id != WT_PREPARED_ID_NONE);
                        WT_ASSERT(session, start_prepare_ts != WT_TS_NONE);
                        WT_ASSERT(session, start_ts != WT_TS_NONE);
                        WT_ASSERT(session, durable_start_ts != WT_TS_NONE);
                        WT_CLEAR(txn_time_point);
                        txn_time_point.id = start_txn;
                        txn_time_point.prepared_id = start_prepared_id;
                        txn_time_point.prepare_timestamp = start_prepare_ts;
                        txn_time_point.commit_timestamp = start_ts;
                        txn_time_point.durable_timestamp = durable_start_ts;
                        F_SET(&txn_time_point,
                          WT_TXN_TIME_POINT_HAS_PREPARED_ID | WT_TXN_TIME_POINT_HAS_TS_PREPARE |
                            WT_TXN_TIME_POINT_HAS_TS_COMMIT | WT_TXN_TIME_POINT_HAS_TS_DURABLE);
                        /* Sessions that claimed by prepared id alone carry no transaction id. */
                        if (start_txn != WT_TXN_NONE)
                            F_SET(&txn_time_point, WT_TXN_TIME_POINT_HAS_ID);
                        WT_ERR(__wt_txn_resolve_prepared_op(session, stable_btree, &txn_time_point,
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
                    WT_ASSERT(session,
                      !prepare && preserve_prepared && start_prepared_id != WT_PREPARED_ID_NONE);
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
                    WT_ASSERT(session, !prepare || durable_start_ts == WT_TS_NONE);
                    upd->txnid = start_txn;
                    if (prepare)
                        upd->prepare_state = WT_PREPARE_INPROGRESS;
                    else if (start_prepared_id != WT_PREPARED_ID_NONE)
                        upd->prepare_state = WT_PREPARE_RESOLVED;
                    upd->prepare_ts = start_prepare_ts;
                    upd->prepared_id = start_prepared_id;
                    upd->upd_start_ts = start_ts;
                    upd->upd_durable_ts = durable_start_ts;
                }
                /* This is for debugging purpose and it is not checked in the code. */
                F_SET(upd, WT_UPDATE_RESTORED_FROM_INGEST);
                last_upd = upd;

                if (prepare && !prepare_txn_fixed) {
                    WT_ASSERT(session, upds == NULL);
                    WT_ERR(__layered_fix_prepared_transaction(
                      session, key, ingest_btree, stable_btree, start_txn, start_prepared_id));
                    prepare_txn_fixed = true;
                }
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
    __wt_scr_free(session, &stable_uri_buf);
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
 * __truncate_cmp_by_start_ts --
 *     qsort comparator: ascending order by truncate start timestamp and txn id.
 */
static int
__truncate_cmp_by_start_ts(const void *a, const void *b)
{
    const WT_TRUNCATE *ta = *(const WT_TRUNCATE *const *)a;
    const WT_TRUNCATE *tb = *(const WT_TRUNCATE *const *)b;

    if (ta->start_ts < tb->start_ts)
        return (-1);
    if (ta->start_ts > tb->start_ts)
        return (1);
    if (ta->txn_id < tb->txn_id)
        return (-1);
    if (ta->txn_id > tb->txn_id)
        return (1);
    return (0);
}

/*
 * __layered_build_sorted_truncates --
 *     Create a sorted array of committed truncates from the table's truncate list.
 */
static int
__layered_build_sorted_truncates(WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table,
  WT_TRUNCATE ***sortedp, size_t *ntruncatesp)
{
    WT_DECL_RET;
    WT_TRUNCATE *t = NULL, **sorted = NULL;
    size_t i = 0, ntruncates = 0;

    *sortedp = NULL;
    *ntruncatesp = 0;

    __wt_readlock(session, &layered_table->truncate_lock);
    TAILQ_FOREACH (t, &layered_table->truncateqh, q)
        if (t->txn_id != WT_TXN_NONE)
            ++ntruncates;

    /* Early exit if there are no committed truncates. */
    if (ntruncates == 0)
        goto err;

    WT_ERR(__wt_calloc(session, ntruncates, sizeof(WT_TRUNCATE *), &sorted));
    /* Populate the array with committed truncates. */
    TAILQ_FOREACH (t, &layered_table->truncateqh, q)
        if (t->txn_id != WT_TXN_NONE)
            sorted[i++] = t;

    /* Sort the array of committed truncates by start timestamp. */
    __wt_qsort(sorted, ntruncates, sizeof(WT_TRUNCATE *), __truncate_cmp_by_start_ts);
    *sortedp = sorted;
    *ntruncatesp = ntruncates;

err:
    __wt_readunlock(session, &layered_table->truncate_lock);
    if (ret != 0)
        __wt_free(session, sorted);
    return (ret);
}

/*
 * __layered_drain_ingest_table_and_truncate_list --
 *     Drain ingest to stable in timestamp order, interleaving committed follower truncates.
 */
static int
__layered_drain_ingest_table_and_truncate_list(WT_SESSION_IMPL *session, const char *ingest_uri)
{
    WT_DATA_HANDLE *layered_dhandle = NULL;
    WT_DECL_ITEM(layered_uri_buf);
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table = NULL;
    WT_TRUNCATE **sorted_truncates = NULL;
    wt_timestamp_t prev_ts = WT_TS_NONE;

    WT_RET(__wt_scr_alloc(session, 0, &layered_uri_buf));
    WT_ERR(__layered_derive_layered_uri(session, ingest_uri, layered_uri_buf));

    WT_ERR_ERROR_OK(
      __wt_session_get_dhandle(session, layered_uri_buf->data, NULL, NULL, 0), ENOENT, true);
    if (ret == ENOENT) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "No layered handle found for ingest table \"%s\", only performing ingest drain",
          ingest_uri);
        WT_ERR(__layered_copy_ingest_table(session, ingest_uri, WT_TS_NONE, WT_TS_MAX));
        ret = 0;
        goto err;
    }
    /*
     * For each committed truncate, copy ingest updates with upper and lower bounds. The lower bound
     * is the timestamp of the previous truncate and the upper bound is the timestamp of the current
     * truncate. After copying ingest updates, apply the range truncate.
     */
    layered_dhandle = session->dhandle;
    layered_table = (WT_LAYERED_TABLE *)layered_dhandle;
    size_t ntruncates;
    WT_ERR(
      __layered_build_sorted_truncates(session, layered_table, &sorted_truncates, &ntruncates));

    for (size_t i = 0; i < ntruncates; i++) {
        WT_TRUNCATE *t = sorted_truncates[i];
        WT_ERR(__layered_copy_ingest_table(session, ingest_uri, prev_ts, t->start_ts));
        WT_ERR(__layered_apply_truncate_to_stable(session, t));
        prev_ts = t->start_ts;
    }
    WT_ERR(__layered_copy_ingest_table(session, ingest_uri, prev_ts, WT_TS_MAX));

err:
    if (layered_table != NULL)
        __wt_layered_table_truncate_clear(session, layered_table);

    __wt_free(session, sorted_truncates);
    /*
     * Cursor opens and closes can leave session->dhandle pointing at a file dhandle, so scope the
     * release explicitly back onto the layered dhandle we acquired above.
     */
    if (layered_dhandle != NULL)
        WT_WITH_DHANDLE(session, layered_dhandle, WT_TRET(__wt_session_release_dhandle(session)));
    __wt_scr_free(session, &layered_uri_buf);
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

    const char *ingest_uri = work_item->ingest_dhandle->name;
    WT_ERR_MSG_CHK(session, __layered_drain_ingest_table_and_truncate_list(session, ingest_uri),
      "Failed to drain ingest and truncate list for \"%s\"", ingest_uri);
    WT_ERR_MSG_CHK(session, __layered_clear_ingest_table(session, ingest_uri),
      "Failed to clear ingest table \"%s\"", ingest_uri);

#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__layered_assert_ingest_table_empty(session, ingest_uri));
#endif

    WT_ERR_MSG_CHK(session, __layered_reset_ingest_table_prune_timestamp(session, ingest_uri),
      "Failed to reset ingest table prune timestamp \"%s\"", ingest_uri);

err:
    /*
     * Balance the pin acquired when queueing. The work item has already been removed from the
     * queue, so the cleanup helper won't see it on the error path either.
     */
    WT_WITH_DHANDLE(session, work_item->ingest_dhandle, __wt_cursor_dhandle_decr_use(session));
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
            if (work_item->ingest_dhandle != NULL)
                WT_WITH_DHANDLE(
                  session, work_item->ingest_dhandle, __wt_cursor_dhandle_decr_use(session));
            __wt_free(session, work_item);
        }
    }
    WT_ASSERT_ALWAYS(session, TAILQ_EMPTY(&conn->layered_drain_data.work_queue),
      "Layered drain work queue failed to drain");
    __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    __wt_spin_destroy(session, &conn->layered_drain_data.queue_lock);
}

/*
 * __layered_queue_ingest_dhandles --
 *     Walk the connection's open dhandle list, queue any open ingest btrees for draining, and pin
 *     each via session_inuse so it survives until the worker processes it. Sourcing the work list
 *     from the dhandle list catches ingest btrees that have been touched (e.g. by prepared
 *     discovery) without their parent `layered:` URI ever being opened.
 */
static int
__layered_queue_ingest_dhandles(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_LAYERED_DRAIN_ENTRY *work_item;

    conn = S2C(session);

    for (dhandle = NULL;;) {
        WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q);
        if (dhandle == NULL)
            break;

        if (!WT_DHANDLE_BTREE(dhandle) || !F_ISSET(dhandle, WT_DHANDLE_OPEN))
            continue;
        if (!WT_URI_IS_INGEST(dhandle->name))
            continue;

        /*
         * Pin via session_inuse so the dhandle survives sweep across the lock release and worker
         * processing. The worker decrements after the drain completes.
         */
        WT_WITH_DHANDLE(session, dhandle, __wt_cursor_dhandle_incr_use(session));

        if ((ret = __wt_calloc_one(session, &work_item)) != 0) {
            WT_WITH_DHANDLE(session, dhandle, __wt_cursor_dhandle_decr_use(session));
            WT_RET(ret);
        }
        work_item->ingest_dhandle = dhandle;
        __wt_spin_lock(session, &conn->layered_drain_data.queue_lock);
        TAILQ_INSERT_HEAD(&conn->layered_drain_data.work_queue, work_item, q);
        __wt_spin_unlock(session, &conn->layered_drain_data.queue_lock);
    }
    return (0);
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

    bool empty, group_created;

    conn = S2C(session);
    group_created = false;

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
    WT_WITH_HANDLE_LIST_READ_LOCK(session, ret = __layered_queue_ingest_dhandles(session));
    WT_ERR(ret);

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
    wt_timestamp_t btree_prune_timestamp, prune_timestamp;
    int64_t ckpt_inuse, last_ckpt;
    int32_t layered_dhandle_inuse, stable_dhandle_inuse;

    layered_table = NULL;
    prune_timestamp = WT_TS_NONE;
    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     */
    WT_RET_ERROR_OK(ret = __wt_session_get_dhandle(session, layered_uri, NULL, NULL, 0), ENOENT);
    if (ret == ENOENT) {
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

    if (prune_timestamp == WT_TS_NONE) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: No checkpoint is eligible for pruning. The last checkpoint in use is %" PRId64,
          layered_table->iface.name, ckpt_inuse);
        ret = 0;
        goto err;
    }

    /*
     * Set the prune timestamp in the btree if it is open, typically it is. However, it's possible
     * that it hasn't been opened yet. In that case, we need to skip updating its timestamp for
     * pruning, and we'll get another chance to update the prune timestamp at the next checkpoint.
     */
    WT_ERR_ERROR_OK(
      __wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0), ENOENT, true);
    if (ret == ENOENT) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "GC %s: Handle not found for ingest table uri: %s", layered_table->iface.name,
          layered_table->ingest_uri);
        ret = 0;
        goto err;
    }

    btree = (WT_BTREE *)session->dhandle->handle;

    btree_prune_timestamp = __wt_atomic_load_uint64_relaxed(&btree->prune_timestamp);
    WT_ASSERT(session, prune_timestamp >= btree_prune_timestamp);

    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "GC %s: update prune timestamp from %" PRIu64 " to %" PRIu64
      " and checkpoint in use from %" PRId64 " to %" PRId64,
      layered_table->iface.name, btree_prune_timestamp, prune_timestamp,
      layered_table->last_ckpt_inuse, ckpt_inuse);

    /*
     * The prune timestamp should be monotonically increasing. It is fine for the user to read the
     * obsolete value. Therefore, no synchronization is required.
     */
    __wt_atomic_store_uint64_relaxed(&btree->prune_timestamp, prune_timestamp);
    layered_table->last_ckpt_inuse = ckpt_inuse;

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

#ifdef HAVE_UNITTEST

/*
 * __ut_layered_derive_layered_uri --
 *     Unit test wrapper for __layered_derive_layered_uri.
 */
int
__ut_layered_derive_layered_uri(WT_SESSION_IMPL *session, const char *ingest_uri, WT_ITEM *buf)
{
    return (__layered_derive_layered_uri(session, ingest_uri, buf));
}

#endif
