/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rts_btree_walk_check_btree_modified --
 *     Check that the rollback to stable btree is modified or not.
 */
static int
__rts_btree_walk_check_btree_modified(WT_SESSION_IMPL *session, const char *uri, bool *modified)
{
    WT_DECL_RET;

    ret = __wt_conn_dhandle_find(session, uri, NULL);
    *modified = ret == 0 && S2BT(session)->modified;
    return (ret);
}

/*
 * __rts_btree_walk_page_skip --
 *     Skip if rollback to stable doesn't require reading this page.
 */
static int
__rts_btree_walk_page_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_PAGE_DELETED *page_del;
    wt_timestamp_t rollback_timestamp;
    char time_string[WT_TIME_STRING_SIZE];

    rollback_timestamp = *(wt_timestamp_t *)context;
    WT_UNUSED(visible_all);

    *skipp = false; /* Default to reading */

    /*
     * Skip pages truncated at or before the RTS timestamp. (We could read the page, but that would
     * unnecessarily instantiate it). If the page has no fast-delete information, that means either
     * it was discarded because the delete is globally visible, or the internal page holding the
     * cell was an old format page so none was loaded. In the latter case we should skip the page as
     * there's no way to get correct behavior and skipping matches the historic behavior. Note that
     * eviction is running; we must lock the WT_REF before examining the fast-delete information.
     */
    if (ref->state == WT_REF_DELETED &&
      WT_REF_CAS_STATE(session, ref, WT_REF_DELETED, WT_REF_LOCKED)) {
        page_del = ref->page_del;
        if (page_del == NULL ||
          (__wt_rts_visibility_txn_visible_id(session, page_del->txnid) &&
            page_del->durable_timestamp <= rollback_timestamp)) {
            /*
             * We should never see a prepared truncate here; not at recovery time because prepared
             * truncates can't be written to disk, and not during a runtime RTS either because it
             * should not be possible to do that with an unresolved prepared transaction.
             */
            WT_ASSERT(session,
              page_del == NULL || page_del->prepare_state == WT_PREPARE_INIT ||
                page_del->prepare_state == WT_PREPARE_RESOLVED);

            if (page_del == NULL)
                __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
                  WT_RTS_VERB_TAG_SKIP_DEL_NULL "ref=%p: deleted page walk skipped", (void *)ref);
            else {
                __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
                  "%p: deleted page walk skipped page_del %s", (void *)ref,
                  __wt_time_point_to_string(page_del->timestamp, page_del->durable_timestamp,
                    page_del->txnid, time_string));
            }
            WT_STAT_CONN_INCR(session, txn_rts_tree_walk_skip_pages);
            *skipp = true;
        }
        WT_REF_SET_STATE(ref, WT_REF_DELETED);
        return (0);
    }

    /* Otherwise, if the page state is other than on disk, we want to look at it. */
    if (ref->state != WT_REF_DISK)
        return (0);

    /*
     * Check whether this on-disk page has any updates to be aborted. We are not holding a hazard
     * reference on the page and so we rely on there being no other threads of control in the tree,
     * that is, eviction ignores WT_REF_DISK pages and no other thread is reading pages, this page
     * cannot change state from on-disk to something else.
     */
    if (!__wt_rts_visibility_page_needs_abort(session, ref, rollback_timestamp)) {
        *skipp = true;
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_STABLE_PG_WALK_SKIP "ref=%p: stable page walk skipped", (void *)ref);
        WT_STAT_CONN_INCR(session, txn_rts_tree_walk_skip_pages);
    }

    return (0);
}

/*
 * __rts_btree_walk --
 *     Called for each open handle - choose to either skip or wipe the commits
 */
static int
__rts_btree_walk(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_REF *ref;

    /* Walk the tree, marking commits aborted where appropriate. */
    ref = NULL;
    while (
      (ret = __wt_tree_walk_custom_skip(session, &ref, __rts_btree_walk_page_skip,
         &rollback_timestamp, WT_READ_NO_EVICT | WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED)) == 0 &&
      ref != NULL)
        if (F_ISSET(ref, WT_REF_FLAG_LEAF))
            WT_RET(__wt_rts_btree_abort_updates(session, ref, rollback_timestamp));

    return (ret);
}

/*
 * __wt_rts_btree_walk_btree_apply --
 *     Perform rollback to stable on a single file.
 */
int
__wt_rts_btree_walk_btree_apply(
  WT_SESSION_IMPL *session, const char *uri, const char *config, wt_timestamp_t rollback_timestamp)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, value, key;
    WT_DECL_RET;
    wt_timestamp_t max_durable_ts, newest_start_durable_ts, newest_stop_durable_ts;
    size_t addr_size;
    uint64_t rollback_txnid, write_gen;
    uint32_t btree_id;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool dhandle_allocated, durable_ts_found, has_txn_updates_gt_than_ckpt_snap, modified;
    bool prepared_updates;

    /* Ignore non-btree objects as well as the metadata and history store files. */
    if (!WT_BTREE_PREFIX(uri) || strcmp(uri, WT_HS_URI) == 0 || strcmp(uri, WT_METAFILE_URI) == 0)
        return (0);

    addr_size = 0;
    rollback_txnid = 0;
    write_gen = 0;
    dhandle_allocated = false;

    /* Find out the max durable timestamp of the object from checkpoint. */
    newest_start_durable_ts = newest_stop_durable_ts = WT_TS_NONE;
    durable_ts_found = prepared_updates = has_txn_updates_gt_than_ckpt_snap = false;

    WT_RET(__wt_config_getones(session, config, "checkpoint", &cval));
    __wt_config_subinit(session, &ckptconf, &cval);
    for (; __wt_config_next(&ckptconf, &key, &cval) == 0;) {
        ret = __wt_config_subgets(session, &cval, "newest_start_durable_ts", &value);
        if (ret == 0) {
            newest_start_durable_ts = WT_MAX(newest_start_durable_ts, (wt_timestamp_t)value.val);
            durable_ts_found = true;
        }
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "newest_stop_durable_ts", &value);
        if (ret == 0) {
            newest_stop_durable_ts = WT_MAX(newest_stop_durable_ts, (wt_timestamp_t)value.val);
            durable_ts_found = true;
        }
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "prepare", &value);
        if (ret == 0) {
            if (value.val)
                prepared_updates = true;
        }
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "newest_txn", &value);
        if (value.len != 0)
            rollback_txnid = (uint64_t)value.val;
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "addr", &value);
        if (ret == 0)
            addr_size = value.len;
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "write_gen", &value);
        if (ret == 0)
            write_gen = (uint64_t)value.val;
        WT_RET_NOTFOUND_OK(ret);
    }
    max_durable_ts = WT_MAX(newest_start_durable_ts, newest_stop_durable_ts);

    /*
     * Perform rollback to stable when the newest written transaction of the btree is greater than
     * or equal to the checkpoint snapshot. The snapshot comparison is valid only when the btree
     * write generation number is greater than the last checkpoint connection base write generation
     * to confirm that the btree is modified in the previous restart cycle.
     */
    if (WT_CHECK_RECOVERY_FLAG_TXNID(session, rollback_txnid) &&
      (write_gen >= S2C(session)->last_ckpt_base_write_gen)) {
        has_txn_updates_gt_than_ckpt_snap = true;
        /* Increment the inconsistent checkpoint stats counter. */
        WT_STAT_CONN_DATA_INCR(session, txn_rts_inconsistent_ckpt);
    }

    /*
     * The rollback to stable will skip the tables during recovery and shutdown in the following
     * conditions.
     * 1. Empty table or newly-created table.
     * 2. Table has timestamped updates without a stable timestamp.
     */
    if ((F_ISSET(S2C(session), WT_CONN_RECOVERING) ||
          F_ISSET(S2C(session), WT_CONN_CLOSING_CHECKPOINT)) &&
      (addr_size == 0 || (rollback_timestamp == WT_TS_NONE && max_durable_ts != WT_TS_NONE))) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_FILE_SKIP "skipping rollback to stable on file=%s because %s", uri,
          addr_size == 0 ? "it has never been checkpointed" :
                           "it has timestamped updates and the stable timestamp is 0");
        return (0);
    }

    /*
     * The rollback operation should be performed on this file based on the following:
     * 1. The dhandle is present in the cache and tree is modified.
     * 2. The checkpoint durable start/stop timestamp is greater than the rollback timestamp.
     * 3. The checkpoint has prepared updates written to disk.
     * 4. There is no durable timestamp in any checkpoint.
     * 5. The checkpoint newest txn is greater than snapshot min txn id.
     */
    WT_WITHOUT_DHANDLE(session,
      WT_WITH_HANDLE_LIST_READ_LOCK(
        session, (ret = __rts_btree_walk_check_btree_modified(session, uri, &modified))));

    WT_ERR_NOTFOUND_OK(ret, false);

    if (modified || max_durable_ts > rollback_timestamp || prepared_updates || !durable_ts_found ||
      has_txn_updates_gt_than_ckpt_snap) {
        /*
         * Open a handle; we're potentially opening a lot of handles and there's no reason to cache
         * all of them for future unknown use, discard on close.
         */
        ret = __wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_DISCARD);
        if (ret != 0)
            WT_ERR_MSG(session, ret, "%s: unable to open handle%s", uri,
              ret == EBUSY ? ", error indicates handle is unavailable due to concurrent use" : "");
        dhandle_allocated = true;

        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_TREE
          "rolling back tree. modified=%s, durable_timestamp=%s > stable_timestamp=%s: %s, "
          "has_prepared_updates=%s, durable_timestamp_not_found=%s, txnid=%" PRIu64
          " > recovery_checkpoint_snap_min=%" PRIu64 ": %s",
          S2BT(session)->modified ? "true" : "false",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[1]),
          max_durable_ts > rollback_timestamp ? "true" : "false",
          prepared_updates ? "true" : "false", !durable_ts_found ? "true" : "false", rollback_txnid,
          S2C(session)->recovery_ckpt_snap_min,
          has_txn_updates_gt_than_ckpt_snap ? "true" : "false");

        WT_ERR(__wt_rts_btree_walk_btree(session, rollback_timestamp));
    } else
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_TREE_SKIP
          "%s: tree skipped with durable_timestamp=%s and stable_timestamp=%s or txnid=%" PRIu64,
          uri, __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[1]), rollback_txnid);

    /*
     * Truncate history store entries for the non-timestamped table.
     * Exceptions:
     * 1. Modified tree - Scenarios where the tree is never checkpointed lead to zero
     * durable timestamp even they are timestamped tables. Until we have a special
     * indication of letting to know the table type other than checking checkpointed durable
     * timestamp to WT_TS_NONE, we need this exception.
     * 2. In-memory database - In this scenario, there is no history store to truncate.
     */
    if ((!dhandle_allocated || !S2BT(session)->modified) && max_durable_ts == WT_TS_NONE &&
      !F_ISSET(S2C(session), WT_CONN_IN_MEMORY)) {
        WT_ERR(__wt_config_getones(session, config, "id", &cval));
        btree_id = (uint32_t)cval.val;
        WT_ERR(__wt_rts_history_btree_hs_truncate(session, btree_id));
    }

err:
    if (dhandle_allocated)
        WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_rts_btree_walk_btree --
 *     Called for each object handle - choose to either skip or wipe the commits
 */
int
__wt_rts_btree_walk_btree(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;

    btree = S2BT(session);
    conn = S2C(session);

    __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_4,
      WT_RTS_VERB_TAG_TREE_LOGGING
      "rollback to stable connection_logging_enabled=%s and btree_logging_enabled=%s",
      FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) ? "true" : "false",
      F_ISSET(btree, WT_BTREE_LOGGED) ? "true" : "false");

    /* Files with commit-level durability (without timestamps), don't get their commits wiped. */
    if (F_ISSET(btree, WT_BTREE_LOGGED))
        return (0);

    /* There is never anything to do for checkpoint handles. */
    if (WT_READING_CHECKPOINT(session))
        return (0);

    /* There is nothing to do on an empty tree. */
    if (btree->root.page == NULL)
        return (0);

    return (__rts_btree_walk(session, rollback_timestamp));
}
