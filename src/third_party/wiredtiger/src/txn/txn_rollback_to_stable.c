/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CHECK_RECOVERY_FLAG_TXNID(session, txnid)                                           \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) && S2C(session)->recovery_ckpt_snap_min != 0 && \
      (txnid) >= S2C(session)->recovery_ckpt_snap_min)

/* Enable rollback to stable verbose messaging during recovery. */
#define WT_VERB_RECOVERY_RTS(session)                                                              \
    (F_ISSET(S2C(session), WT_CONN_RECOVERING) ?                                                   \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RECOVERY, WT_VERB_RTS})) : \
        WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RTS})))

/*
 * __rollback_delete_hs --
 *     Delete the updates for a key in the history store until the first update (including) that is
 *     larger than or equal to the specified timestamp.
 */
static int
__rollback_delete_hs(WT_SESSION_IMPL *session, WT_ITEM *key, wt_timestamp_t ts)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;

    /* Open a history store table cursor. */
    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    /*
     * Rollback-to-stable operates exclusively (i.e., it is the only active operation in the system)
     * outside the constraints of transactions. Therefore, there is no need for snapshot based
     * visibility checks.
     */
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));

    /*
     * Scan the history store for the given btree and key with maximum start timestamp to let the
     * search point to the last version of the key and start traversing backwards to delete all the
     * records until the first update with the start timestamp larger than or equal to the specified
     * timestamp.
     */
    hs_cursor->set_key(hs_cursor, 4, S2BT(session)->id, key, WT_TS_MAX, UINT64_MAX);
    ret = __wt_curhs_search_near_before(session, hs_cursor);
    for (; ret == 0; ret = hs_cursor->prev(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));
        if (hs_start_ts < ts)
            break;
        WT_ERR(hs_cursor->remove(hs_cursor));
        WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        if (hs_start_ts == ts)
            WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts);
        else
            WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts_unstable);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &hs_key);
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __rollback_abort_update --
 *     Abort updates in an update change with timestamps newer than the rollback timestamp. Also,
 *     clear the history store flag for the first stable update in the update.
 */
static int
__rollback_abort_update(WT_SESSION_IMPL *session, WT_ITEM *key, WT_UPDATE *first_upd,
  wt_timestamp_t rollback_timestamp, bool *stable_update_found)
{
    WT_UPDATE *stable_upd, *tombstone, *upd;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    stable_upd = tombstone = NULL;
    if (stable_update_found != NULL)
        *stable_update_found = false;
    for (upd = first_upd; upd != NULL; upd = upd->next) {
        /* Skip the updates that are aborted. */
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        if (rollback_timestamp < upd->durable_ts || upd->prepare_state == WT_PREPARE_INPROGRESS) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "rollback to stable update aborted with txnid: %" PRIu64
              " durable timestamp: %s and stable timestamp: %s, prepared: %s",
              upd->txnid, __wt_timestamp_to_string(upd->durable_ts, ts_string[0]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[1]),
              rollback_timestamp < upd->durable_ts ? "false" : "true");

            upd->txnid = WT_TXN_ABORTED;
            WT_STAT_CONN_INCR(session, txn_rts_upd_aborted);
        } else {
            /* Valid update is found. */
            stable_upd = upd;
            break;
        }
    }

    /*
     * Clear the history store flag for the stable update to indicate that this update should not be
     * written into the history store later, when all the aborted updates are removed from the
     * history store. The next time when this update is moved into the history store, it will have a
     * different stop time point.
     */
    if (stable_upd != NULL) {
        if (F_ISSET(stable_upd, WT_UPDATE_HS)) {
            /* Find the update following a stable tombstone. */
            if (stable_upd->type == WT_UPDATE_TOMBSTONE) {
                tombstone = stable_upd;
                for (stable_upd = stable_upd->next; stable_upd != NULL;
                     stable_upd = stable_upd->next) {
                    if (stable_upd->txnid != WT_TXN_ABORTED) {
                        WT_ASSERT(session,
                          stable_upd->type != WT_UPDATE_TOMBSTONE &&
                            F_ISSET(stable_upd, WT_UPDATE_HS));
                        break;
                    }
                }
            }

            /*
             * Delete the first stable update and any newer update from the history store. If the
             * update following the stable tombstone is removed by obsolete check, no need to remove
             * that update from the history store as it has a globally visible tombstone. In that
             * case, it is enough to delete everything up until to the tombstone timestamp.
             */
            WT_RET(__rollback_delete_hs(
              session, key, stable_upd == NULL ? tombstone->start_ts : stable_upd->start_ts));

            /*
             * Clear the history store flag for the first stable update. Otherwise, it will not be
             * moved to history store again.
             */
            if (stable_upd != NULL)
                F_CLR(stable_upd, WT_UPDATE_HS);
            if (tombstone != NULL)
                F_CLR(tombstone, WT_UPDATE_HS);
        }
        if (stable_update_found != NULL)
            *stable_update_found = true;
    }

    return (0);
}

/*
 * __rollback_abort_insert_list --
 *     Apply the update abort check to each entry in an insert skip list. Return how many entries
 *     had stable updates.
 */
static int
__rollback_abort_insert_list(WT_SESSION_IMPL *session, WT_PAGE *page, WT_INSERT_HEAD *head,
  wt_timestamp_t rollback_timestamp, uint32_t *stable_updates_count)
{
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_INSERT *ins;
    uint64_t recno;
    uint8_t *memp;
    bool stable_update_found;

    WT_ERR(
      __wt_scr_alloc(session, page->type == WT_PAGE_ROW_LEAF ? 0 : WT_INTPACK64_MAXSIZE, &key));

    WT_SKIP_FOREACH (ins, head)
        if (ins->upd != NULL) {
            if (page->type == WT_PAGE_ROW_LEAF) {
                key->data = WT_INSERT_KEY(ins);
                key->size = WT_INSERT_KEY_SIZE(ins);
            } else {
                recno = WT_INSERT_RECNO(ins);
                memp = key->mem;
                WT_ERR(__wt_vpack_uint(&memp, 0, recno));
                key->size = WT_PTRDIFF(memp, key->data);
            }
            WT_ERR(__rollback_abort_update(
              session, key, ins->upd, rollback_timestamp, &stable_update_found));
            if (stable_update_found && stable_updates_count != NULL)
                (*stable_updates_count)++;
        }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __rollback_has_stable_update --
 *     Check if an update chain has a stable update on it. Assume the update chain has already been
 *     processed so all we need to do is look for a valid, non-aborted entry.
 */
static bool
__rollback_has_stable_update(WT_UPDATE *upd)
{
    while (upd != NULL && (upd->type == WT_UPDATE_INVALID || upd->txnid == WT_TXN_ABORTED))
        upd = upd->next;
    return (upd != NULL);
}

/*
 * __rollback_col_modify --
 *     Add the provided update to the head of the update list.
 */
static inline int
__rollback_col_modify(WT_SESSION_IMPL *session, WT_REF *ref, WT_UPDATE *upd, uint64_t recno)
{
    WT_CURSOR_BTREE cbt;
    WT_DECL_RET;

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    /* Search the page. */
    WT_ERR(__wt_col_search(&cbt, recno, ref, true, NULL));

    /* Apply the modification. */
#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__wt_col_modify(&cbt, recno, NULL, upd, WT_UPDATE_INVALID, true, false));
#else
    WT_ERR(__wt_col_modify(&cbt, recno, NULL, upd, WT_UPDATE_INVALID, true));
#endif

err:
    /* Free any resources that may have been cached in the cursor. */
    WT_TRET(__wt_btcur_close(&cbt, true));

    return (ret);
}

/*
 * __rollback_row_modify --
 *     Add the provided update to the head of the update list.
 */
static inline int
__rollback_row_modify(WT_SESSION_IMPL *session, WT_REF *ref, WT_UPDATE *upd, WT_ITEM *key)
{
    WT_CURSOR_BTREE cbt;
    WT_DECL_RET;

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    /* Search the page. */
    WT_ERR(__wt_row_search(&cbt, key, true, ref, true, NULL));

    /* Apply the modification. */
#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true, false));
#else
    WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true));
#endif

err:
    /* Free any resources that may have been cached in the cursor. */
    WT_TRET(__wt_btcur_close(&cbt, true));

    return (ret);
}

/*
 * __rollback_txn_visible_id --
 *     Check if the transaction id is visible or not.
 */
static bool
__rollback_txn_visible_id(WT_SESSION_IMPL *session, uint64_t id)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* If not recovery then assume all the data as visible. */
    if (!F_ISSET(conn, WT_CONN_RECOVERING))
        return (true);

    /*
     * Only full checkpoint writes the metadata with snapshot. If the recovered checkpoint snapshot
     * details are none then return false i.e, updates are visible.
     */
    if (conn->recovery_ckpt_snap_min == WT_TXN_NONE && conn->recovery_ckpt_snap_max == WT_TXN_NONE)
        return (true);

    return (
      __wt_txn_visible_id_snapshot(id, conn->recovery_ckpt_snap_min, conn->recovery_ckpt_snap_max,
        conn->recovery_ckpt_snapshot, conn->recovery_ckpt_snapshot_count));
}

/*
 * __rollback_ondisk_fixup_key --
 *     Abort updates in the history store and replace the on-disk value with an update that
 *     satisfies the given timestamp.
 */
static int
__rollback_ondisk_fixup_key(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip, uint64_t recno,
  WT_ITEM *row_key, WT_CELL_UNPACK_KV *unpack, wt_timestamp_t rollback_timestamp)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(full_value);
    WT_DECL_ITEM(hs_key);
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_TIME_WINDOW *hs_tw;
    WT_UPDATE *tombstone, *upd;
    wt_timestamp_t hs_durable_ts, hs_start_ts, hs_stop_durable_ts, newer_hs_durable_ts, pinned_ts;
    uint64_t hs_counter, type_full;
    uint32_t hs_btree_id;
    uint8_t *memp;
    uint8_t type;
    char ts_string[4][WT_TS_INT_STRING_SIZE];
    char tw_string[WT_TIME_STRING_SIZE];
    bool valid_update_found;
#ifdef HAVE_DIAGNOSTIC
    bool first_record;
#endif

    page = ref->page;

    hs_cursor = NULL;
    tombstone = upd = NULL;
    hs_durable_ts = hs_start_ts = hs_stop_durable_ts = WT_TS_NONE;
    hs_btree_id = S2BT(session)->id;
    valid_update_found = false;
#ifdef HAVE_DIAGNOSTIC
    first_record = true;
#endif

    /* Allocate buffers for the data store and history store key. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    if (rip != NULL) {
        if (row_key != NULL)
            key = row_key;
        else {
            /* Unpack a row key. */
            WT_ERR(__wt_scr_alloc(session, 0, &key));
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
        }
    } else {
        /* Manufacture a column key. */
        WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));
        memp = key->mem;
        WT_ERR(__wt_vpack_uint(&memp, 0, recno));
        key->size = WT_PTRDIFF(memp, key->data);
    }

    WT_ERR(__wt_scr_alloc(session, 0, &full_value));
    WT_ERR(__wt_page_cell_data_ref(session, page, unpack, full_value));
    WT_ERR(__wt_buf_set(session, full_value, full_value->data, full_value->size));
    newer_hs_durable_ts = unpack->tw.durable_start_ts;

    __wt_txn_pinned_timestamp(session, &pinned_ts);

    /* Open a history store table cursor. */
    WT_ERR(__wt_curhs_open(session, NULL, &hs_cursor));
    /*
     * Rollback-to-stable operates exclusively (i.e., it is the only active operation in the system)
     * outside the constraints of transactions. Therefore, there is no need for snapshot based
     * visibility checks.
     */
    F_SET(hs_cursor, WT_CURSTD_HS_READ_ALL);

    /*
     * Scan the history store for the given btree and key with maximum start timestamp to let the
     * search point to the last version of the key and start traversing backwards to find out the
     * satisfying record according the given timestamp. Any satisfying history store record is moved
     * into data store and removed from history store. If none of the history store records satisfy
     * the given timestamp, the key is removed from data store.
     */
    hs_cursor->set_key(hs_cursor, 4, hs_btree_id, key, WT_TS_MAX, UINT64_MAX);
    ret = __wt_curhs_search_near_before(session, hs_cursor);
    for (; ret == 0; ret = hs_cursor->prev(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

        /* Get current value and convert to full update if it is a modify. */
        WT_ERR(hs_cursor->get_value(
          hs_cursor, &hs_stop_durable_ts, &hs_durable_ts, &type_full, hs_value));
        type = (uint8_t)type_full;

        /* Retrieve the time window from the history cursor. */
        __wt_hs_upd_time_window(hs_cursor, &hs_tw);

        /*
         * We have a tombstone on the history update and it is obsolete according to the timestamp
         * and txnid, so no need to restore it. These obsolete updates are written to the disk when
         * they are not obsolete at the time of reconciliation by an eviction thread and later they
         * become obsolete according to the checkpoint.
         */
        if (__rollback_txn_visible_id(session, hs_tw->stop_txn) &&
          hs_stop_durable_ts <= pinned_ts) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "history store stop is obsolete with time window: %s and pinned timestamp: %s",
              __wt_time_window_to_string(hs_tw, tw_string),
              __wt_timestamp_to_string(pinned_ts, ts_string[0]));
            WT_ERR(hs_cursor->remove(hs_cursor));
            WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
            continue;
        }

        /*
         * Do not include history store updates greater than on-disk data store version to construct
         * a full update to restore except when the on-disk update is prepared. Including more
         * recent updates than the on-disk version shouldn't be problem as the on-disk version in
         * history store is always a full update. It is better to not to include those updates as it
         * unnecessarily increases the rollback to stable time.
         *
         * Comparing with timestamps here has no problem unlike in search flow where the timestamps
         * may be reset during reconciliation. RTS detects an on-disk update is unstable based on
         * the written proper timestamp, so comparing against it with history store shouldn't have
         * any problem.
         */
        if (hs_start_ts <= unpack->tw.start_ts || unpack->tw.prepare) {
            if (type == WT_UPDATE_MODIFY)
                WT_ERR(__wt_modify_apply_item(
                  session, S2BT(session)->value_format, full_value, hs_value->data));
            else {
                WT_ASSERT(session, type == WT_UPDATE_STANDARD);
                WT_ERR(__wt_buf_set(session, full_value, hs_value->data, hs_value->size));
            }
        } else
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "history store update more recent than on-disk update with start timestamp: %s,"
              " durable timestamp: %s, stop timestamp: %s and type: %" PRIu8,
              __wt_timestamp_to_string(hs_start_ts, ts_string[0]),
              __wt_timestamp_to_string(hs_durable_ts, ts_string[1]),
              __wt_timestamp_to_string(hs_stop_durable_ts, ts_string[2]), type);

        /*
         * Verify the history store timestamps are in order. The start timestamp may be equal to the
         * stop timestamp if the original update's commit timestamp is out of order. We may see
         * records newer than or equal to the onpage value if eviction runs concurrently with
         * checkpoint. In that case, don't verify the first record.
         *
         * It is possible during a prepared transaction rollback, the history store update that have
         * its own stop timestamp doesn't get removed leads to duplicate records in history store
         * after further operations on that same key. Rollback to stable should ignore such records
         * for timestamp ordering verification.
         *
         * If we have fixed the out-of-order timestamps, then the newer update reinserted with an
         * older timestamp may have a durable timestamp that is smaller than the current stop
         * durable timestamp.
         *
         * It is possible that there can be an update in the history store with a max stop timestamp
         * in the middle of the same key updates. This occurs when the checkpoint writes the
         * committed prepared update and further updates on that key including the history store
         * changes before the transaction fixes the history store update to have a proper stop
         * timestamp. It is a rare scenario.
         */
        WT_ASSERT(session,
          hs_stop_durable_ts <= newer_hs_durable_ts || hs_start_ts == hs_stop_durable_ts ||
            hs_start_ts == newer_hs_durable_ts || newer_hs_durable_ts == hs_durable_ts ||
            first_record || hs_stop_durable_ts == WT_TS_MAX);

        if (hs_stop_durable_ts < newer_hs_durable_ts)
            WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_stop_older_than_newer_start);

        /*
         * Stop processing when we find a stable update according to the given timestamp and
         * transaction id.
         */
        if (__rollback_txn_visible_id(session, hs_tw->start_txn) &&
          hs_durable_ts <= rollback_timestamp) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "history store update valid with start timestamp: %s, durable timestamp: %s, stop "
              "timestamp: %s, stable timestamp: %s, txnid: %" PRIu64 " and type: %" PRIu8,
              __wt_timestamp_to_string(hs_start_ts, ts_string[0]),
              __wt_timestamp_to_string(hs_durable_ts, ts_string[1]),
              __wt_timestamp_to_string(hs_stop_durable_ts, ts_string[2]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[3]), hs_tw->start_txn, type);
            WT_ASSERT(session, unpack->tw.prepare || hs_tw->start_ts <= unpack->tw.start_ts);
            valid_update_found = true;
            break;
        }

        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "history store update aborted with start timestamp: %s, durable timestamp: %s, stop "
          "timestamp: %s, stable timestamp: %s, start txnid: %" PRIu64 ", stop txnid: %" PRIu64
          " and type: %" PRIu8,
          __wt_timestamp_to_string(hs_start_ts, ts_string[0]),
          __wt_timestamp_to_string(hs_durable_ts, ts_string[1]),
          __wt_timestamp_to_string(hs_stop_durable_ts, ts_string[2]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[3]), hs_tw->start_txn,
          hs_tw->stop_txn, type);

        /*
         * Start time point of the current record may be used as stop time point of the previous
         * record. Save it to verify against the previous record and check if we need to append the
         * stop time point as a tombstone when we rollback the history store record.
         */
        newer_hs_durable_ts = hs_durable_ts;
#ifdef HAVE_DIAGNOSTIC
        first_record = false;
#endif

        WT_ERR(hs_cursor->remove(hs_cursor));
        WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts_unstable);
    }

    /*
     * If we found a history value that satisfied the given timestamp, add it to the update list.
     * Otherwise remove the key by adding a tombstone.
     */
    if (valid_update_found) {
        /* Retrieve the time window from the history cursor. */
        __wt_hs_upd_time_window(hs_cursor, &hs_tw);
        WT_ASSERT(session,
          hs_tw->start_ts < unpack->tw.start_ts || hs_tw->start_txn < unpack->tw.start_txn);
        WT_ERR(__wt_upd_alloc(session, full_value, WT_UPDATE_STANDARD, &upd, NULL));

        /*
         * Set the transaction id of updates to WT_TXN_NONE when called from recovery, because the
         * connections write generation will be initialized after rollback to stable and the updates
         * in the cache will be problematic. The transaction id of pages which are in disk will be
         * automatically reset as part of unpacking cell when loaded to cache.
         */
        if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
            upd->txnid = WT_TXN_NONE;
        else
            upd->txnid = hs_tw->start_txn;
        upd->durable_ts = hs_tw->durable_start_ts;
        upd->start_ts = hs_tw->start_ts;
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "update restored from history store txnid: %" PRIu64 ", start_ts: %s and durable_ts: %s",
          upd->txnid, __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
          __wt_timestamp_to_string(upd->durable_ts, ts_string[1]));

        /*
         * Set the flag to indicate that this update has been restored from history store for the
         * rollback to stable operation.
         */
        F_SET(upd, WT_UPDATE_RESTORED_FROM_HS);
        WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_restore_updates);

        /*
         * We have a tombstone on the original update chain and it is stable according to the
         * timestamp and txnid, we need to restore that as well.
         */
        if (__rollback_txn_visible_id(session, hs_tw->stop_txn) &&
          hs_stop_durable_ts <= rollback_timestamp) {
            /*
             * The restoring tombstone timestamp must be zero or less than previous update start
             * timestamp or the on-disk update is an out of order prepared.
             */
            WT_ASSERT(session,
              hs_stop_durable_ts == WT_TS_NONE || hs_stop_durable_ts < newer_hs_durable_ts ||
                unpack->tw.prepare);

            WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
            /*
             * Set the transaction id of updates to WT_TXN_NONE when called from recovery, because
             * the connections write generation will be initialized after rollback to stable and the
             * updates in the cache will be problematic. The transaction id of pages which are in
             * disk will be automatically reset as part of unpacking cell when loaded to cache.
             */
            if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
                tombstone->txnid = WT_TXN_NONE;
            else
                tombstone->txnid = hs_tw->stop_txn;
            tombstone->durable_ts = hs_tw->durable_stop_ts;
            tombstone->start_ts = hs_tw->stop_ts;
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "tombstone restored from history store txnid: %" PRIu64
              ", start_ts: %s, durable_ts: %s",
              tombstone->txnid, __wt_timestamp_to_string(tombstone->start_ts, ts_string[0]),
              __wt_timestamp_to_string(tombstone->durable_ts, ts_string[1]));

            /*
             * Set the flag to indicate that this update has been restored from history store for
             * the rollback to stable operation.
             */
            F_SET(tombstone, WT_UPDATE_RESTORED_FROM_HS);

            tombstone->next = upd;
            upd = tombstone;
            WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_restore_tombstones);
        }
    } else {
        WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
        WT_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session), "%p: key removed", (void *)key);
    }

    if (rip != NULL)
        WT_ERR(__rollback_row_modify(session, ref, upd, key));
    else
        WT_ERR(__rollback_col_modify(session, ref, upd, recno));

    /* Finally remove that update from history store. */
    if (valid_update_found) {
        /* Avoid freeing the updates while still in use if hs_cursor->remove fails. */
        upd = tombstone = NULL;

        WT_ERR(hs_cursor->remove(hs_cursor));
        WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts);
    }

    if (0) {
err:
        WT_ASSERT(session, tombstone == NULL || upd == tombstone);
        __wt_free_update_list(session, &upd);
    }
    __wt_scr_free(session, &full_value);
    __wt_scr_free(session, &hs_key);
    __wt_scr_free(session, &hs_value);
    if (rip == NULL || row_key == NULL)
        __wt_scr_free(session, &key);
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __rollback_abort_ondisk_kv --
 *     Fix the on-disk K/V version according to the given timestamp.
 */
static int
__rollback_abort_ondisk_kv(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip, uint64_t recno,
  WT_ITEM *row_key, WT_CELL_UNPACK_KV *vpack, wt_timestamp_t rollback_timestamp,
  bool *is_ondisk_stable)
{
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_UPDATE *upd;
    char ts_string[5][WT_TS_INT_STRING_SIZE];
    bool prepared;

    page = ref->page;
    upd = NULL;

    /* Initialize the on-disk stable version flag. */
    if (is_ondisk_stable != NULL)
        *is_ondisk_stable = false;

    prepared = vpack->tw.prepare;
    if (WT_IS_HS(session->dhandle)) {
        /*
         * Abort the history store update with stop durable timestamp greater than the stable
         * timestamp or the updates with max stop timestamp which implies that they are associated
         * with prepared transactions.
         */
        if (vpack->tw.durable_stop_ts > rollback_timestamp || vpack->tw.stop_ts == WT_TS_MAX) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "hs update aborted with start durable/commit timestamp: %s, %s, stop durable/commit "
              "timestamp: %s, %s and stable timestamp: %s",
              __wt_timestamp_to_string(vpack->tw.durable_start_ts, ts_string[0]),
              __wt_timestamp_to_string(vpack->tw.start_ts, ts_string[1]),
              __wt_timestamp_to_string(vpack->tw.durable_stop_ts, ts_string[2]),
              __wt_timestamp_to_string(vpack->tw.stop_ts, ts_string[3]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[4]));
            WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
            WT_STAT_CONN_DATA_INCR(session, txn_rts_sweep_hs_keys);
        } else
            return (0);
    } else if (vpack->tw.durable_start_ts > rollback_timestamp ||
      !__rollback_txn_visible_id(session, vpack->tw.start_txn) ||
      (!WT_TIME_WINDOW_HAS_STOP(&vpack->tw) && prepared)) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "on-disk update aborted with start durable timestamp: %s, commit timestamp: %s, "
          "prepared: %s, stable timestamp: %s and txnid : %" PRIu64,
          __wt_timestamp_to_string(vpack->tw.durable_start_ts, ts_string[0]),
          __wt_timestamp_to_string(vpack->tw.start_ts, ts_string[1]), prepared ? "true" : "false",
          __wt_timestamp_to_string(rollback_timestamp, ts_string[2]), vpack->tw.start_txn);
        if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
            return (__rollback_ondisk_fixup_key(
              session, ref, rip, recno, row_key, vpack, rollback_timestamp));
        else {
            /*
             * In-memory database don't have a history store to provide a stable update, so remove
             * the key. Note that an in-memory database will have saved old values in the update
             * chain, so we should only get here for a key/value that never existed at all as of the
             * rollback timestamp; thus, deleting it is the correct response.
             */
            WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
            WT_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
        }
    } else if (WT_TIME_WINDOW_HAS_STOP(&vpack->tw) &&
      (vpack->tw.durable_stop_ts > rollback_timestamp ||
        !__rollback_txn_visible_id(session, vpack->tw.stop_txn) || prepared)) {
        /*
         * For prepared transactions, it is possible that both the on-disk key start and stop time
         * windows can be the same. To abort these updates, check for any stable update from history
         * store or remove the key.
         */
        if (vpack->tw.start_ts == vpack->tw.stop_ts &&
          vpack->tw.durable_start_ts == vpack->tw.durable_stop_ts &&
          vpack->tw.start_txn == vpack->tw.stop_txn) {
            WT_ASSERT(session, prepared == true);
            if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
                return (__rollback_ondisk_fixup_key(
                  session, ref, rip, recno, row_key, vpack, rollback_timestamp));
            else {
                /*
                 * In-memory database don't have a history store to provide a stable update, so
                 * remove the key.
                 */
                WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
                WT_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
            }
        } else {
            /*
             * Clear the remove operation from the key by inserting the original on-disk value as a
             * standard update.
             */
            WT_RET(__wt_scr_alloc(session, 0, &tmp));
            if ((ret = __wt_page_cell_data_ref(session, page, vpack, tmp)) == 0)
                ret = __wt_upd_alloc(session, tmp, WT_UPDATE_STANDARD, &upd, NULL);
            __wt_scr_free(session, &tmp);
            WT_RET(ret);

            /*
             * Set the transaction id of updates to WT_TXN_NONE when called from recovery, because
             * the connections write generation will be initialized after rollback to stable and the
             * updates in the cache will be problematic. The transaction id of pages which are in
             * disk will be automatically reset as part of unpacking cell when loaded to cache.
             */
            if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
                upd->txnid = WT_TXN_NONE;
            else
                upd->txnid = vpack->tw.start_txn;
            upd->durable_ts = vpack->tw.durable_start_ts;
            upd->start_ts = vpack->tw.start_ts;
            F_SET(upd, WT_UPDATE_RESTORED_FROM_DS);
            WT_STAT_CONN_DATA_INCR(session, txn_rts_keys_restored);
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "key restored with commit timestamp: %s, durable timestamp: %s, stable timestamp: "
              "%s, "
              "txnid: %" PRIu64
              " and removed commit timestamp: %s, durable timestamp: %s, txnid: %" PRIu64
              ", prepared: %s",
              __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
              __wt_timestamp_to_string(upd->durable_ts, ts_string[1]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[2]), upd->txnid,
              __wt_timestamp_to_string(vpack->tw.stop_ts, ts_string[3]),
              __wt_timestamp_to_string(vpack->tw.durable_stop_ts, ts_string[4]), vpack->tw.stop_txn,
              prepared ? "true" : "false");
        }
    } else {
        /* Stable version according to the timestamp. */
        if (is_ondisk_stable != NULL)
            *is_ondisk_stable = true;
        return (0);
    }

    if (rip != NULL) {
        if (row_key != NULL)
            key = row_key;
        else {
            /* Unpack a row key. */
            WT_ERR(__wt_scr_alloc(session, 0, &key));
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
        }
        WT_ERR(__rollback_row_modify(session, ref, upd, key));
    } else
        WT_ERR(__rollback_col_modify(session, ref, upd, recno));

    if (0) {
err:
        __wt_free(session, upd);
    }
    if (rip != NULL && row_key == NULL)
        __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __rollback_abort_col_var --
 *     Abort updates on a variable length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static int
__rollback_abort_col_var(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_CELL *kcell;
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
    WT_INSERT *ins;
    WT_INSERT_HEAD *inshead;
    WT_PAGE *page;
    uint64_t ins_recno, recno, rle;
    uint32_t i, j, stable_updates_count;
    bool is_ondisk_stable;

    page = ref->page;
    /*
     * If a disk image exists, start from the provided recno; or else start from 0.
     */
    if (page->dsk != NULL)
        recno = page->dsk->recno;
    else
        recno = 0;

    /* Review the changes to the original on-page data items. */
    WT_COL_FOREACH (page, cip, i) {
        stable_updates_count = 0;

        if ((inshead = WT_COL_UPDATE(page, cip)) != NULL)
            WT_RET(__rollback_abort_insert_list(
              session, page, inshead, rollback_timestamp, &stable_updates_count));

        if (page->dsk != NULL) {
            /* Unpack the cell. We need its RLE count whether or not we're going to iterate it. */
            kcell = WT_COL_PTR(page, cip);
            __wt_cell_unpack_kv(session, page->dsk, kcell, &unpack);
            rle = __wt_cell_rle(&unpack);

            /*
             * Each key whose on-disk value is not stable and has no stable update on the update
             * list must be processed downstream.
             *
             * If we can determine that the cell's on-disk value is stable, we can skip iterating
             * over the cell; likewise, if we can determine that every key in the cell has a stable
             * update on the update list, we can skip the iteration. Otherwise we have to try each
             * key.
             *
             * If the on-disk cell is deleted, it is stable, because cells only appear as deleted
             * when there is no older value that might need to be restored.
             *
             * Note that in a purely timestamped world, the presence of any stable update for any
             * key in the cell means the on-disk value must be stable, because the update must be
             * newer than the on-disk value. However, this is no longer true if the stable update
             * has no timestamp. It may also not be true if the on-disk value is prepared, or other
             * corner cases. Therefore, we must iterate the cell unless _every_ key has a stable
             * update.
             *
             * We can, however, stop iterating as soon as the downstream code reports back that the
             * on-disk value is actually stable.
             */
            if (unpack.type == WT_CELL_DEL)
                WT_STAT_CONN_DATA_INCR(session, txn_rts_delete_rle_skipped);
            else if (stable_updates_count == rle)
                WT_STAT_CONN_DATA_INCR(session, txn_rts_stable_rle_skipped);
            else {
                j = 0;
                if (inshead != NULL) {
                    WT_SKIP_FOREACH (ins, inshead) {
                        /* If the update list goes past the end of the cell, something's wrong. */
                        WT_ASSERT(session, j < rle);
                        ins_recno = WT_INSERT_RECNO(ins);
                        /* Process all the keys before this update. */
                        while (recno + j < ins_recno) {
                            WT_RET(__rollback_abort_ondisk_kv(session, ref, NULL, recno + j, NULL,
                              &unpack, rollback_timestamp, &is_ondisk_stable));
                            /* We can stop right away if the on-disk version is stable. */
                            if (is_ondisk_stable) {
                                if (rle > 1)
                                    WT_STAT_CONN_DATA_INCR(session, txn_rts_stable_rle_skipped);
                                goto stop;
                            }
                            j++;
                        }
                        /* If this key has a stable update, skip over it. */
                        if (recno + j == ins_recno && __rollback_has_stable_update(ins->upd))
                            j++;
                    }
                }
                /* Process the rest of the keys. */
                while (j < rle) {
                    WT_RET(__rollback_abort_ondisk_kv(session, ref, NULL, recno + j, NULL, &unpack,
                      rollback_timestamp, &is_ondisk_stable));
                    /* We can stop right away if the on-disk version is stable. */
                    if (is_ondisk_stable) {
                        if (rle > 1)
                            WT_STAT_CONN_DATA_INCR(session, txn_rts_stable_rle_skipped);
                        goto stop;
                    }
                    j++;
                }
            }
stop:
            recno += rle;
        }
    }

    /* Review the append list */
    if ((inshead = WT_COL_APPEND(page)) != NULL)
        WT_RET(__rollback_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

    return (0);
}

/*
 * __rollback_abort_col_fix_one --
 *     Handle one possibly unstable on-disk time window.
 */
static int
__rollback_abort_col_fix_one(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t tw,
  uint32_t recno_offset, wt_timestamp_t rollback_timestamp)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_PAGE *page;
    uint8_t value;

    btree = S2BT(session);
    page = ref->page;

    /* Unpack the cell to get the time window. */
    cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
    __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

    /* Fake up the value (which is not physically in the cell) in case it's wanted. */
    value = __bit_getv(page->pg_fix_bitf, recno_offset, btree->bitcnt);
    unpack.data = &value;
    unpack.size = 1;

    return (__rollback_abort_ondisk_kv(session, ref, NULL, page->dsk->recno + recno_offset, NULL,
      &unpack, rollback_timestamp, NULL));
}

/*
 * __rollback_abort_col_fix --
 *     Abort updates on a fixed length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static int
__rollback_abort_col_fix(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT *ins;
    WT_INSERT_HEAD *inshead;
    WT_PAGE *page;
    uint32_t ins_recno_offset, recno_offset, numtws, tw;

    page = ref->page;

    /*
     * Review the changes to the original on-page data items. Note that while this can report back
     * to us whether it saw a stable update, that information doesn't do us any good -- unlike in
     * VLCS where the uniformity of cells lets us reason about the timestamps of all of them based
     * on the timestamp of an update to any of them, in FLCS everything is just thrown together, so
     * we'll need to iterate over all the keys anyway.
     */
    if ((inshead = WT_COL_UPDATE_SINGLE(page)) != NULL)
        WT_RET(__rollback_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

    /*
     * Iterate over all the keys, stopping only on keys that (a) have a time window on disk, and
     * also (b) do not have a stable update remaining in the update list. Keys with no on-disk time
     * window are stable. And we must not try to adjust the on-disk value for keys with stable
     * updates, because the downstream code assumes that has already been checked and in some cases
     * (e.g. in-memory databases) the wrong thing will happen.
     *
     * Iterate over the update list and carry along the iteration over the time window list in
     * parallel, even though the code would perhaps make more sense the other way around, because
     * this allows using the skiplist iterator macro instead of an open-coded mess.
     */
    numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;
    WT_ASSERT(session, numtws == 0 || page->dsk != NULL);
    tw = 0;
    if (inshead != NULL) {
        WT_SKIP_FOREACH (ins, inshead) {
            /* Process all the keys before this update entry. */
            ins_recno_offset = (uint32_t)(WT_INSERT_RECNO(ins) - ref->ref_recno);
            while (tw < numtws &&
              (recno_offset = page->pg_fix_tws[tw].recno_offset) < ins_recno_offset) {

                WT_RET(
                  __rollback_abort_col_fix_one(session, ref, tw, recno_offset, rollback_timestamp));
                tw++;
            }
            /* If this key has a stable update, skip over it. */
            if (tw < numtws && page->pg_fix_tws[tw].recno_offset == ins_recno_offset &&
              ins->upd != NULL && __rollback_has_stable_update(ins->upd))
                tw++;
        }
    }
    /* Process the rest of the keys with time windows. */
    while (tw < numtws) {
        recno_offset = page->pg_fix_tws[tw].recno_offset;
        WT_RET(__rollback_abort_col_fix_one(session, ref, tw, recno_offset, rollback_timestamp));
        tw++;
    }

    /* Review the append list. */
    if ((inshead = WT_COL_APPEND(page)) != NULL)
        WT_RET(__rollback_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

    return (0);
}

/*
 * __rollback_abort_row_leaf --
 *     Abort updates on a row leaf page with timestamps newer than the rollback timestamp.
 */
static int
__rollback_abort_row_leaf(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_CELL_UNPACK_KV *vpack, _vpack;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_INSERT_HEAD *insert;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint32_t i;
    bool have_key, stable_update_found;

    page = ref->page;

    WT_RET(__wt_scr_alloc(session, 0, &key));

    /*
     * Review the insert list for keys before the first entry on the disk page.
     */
    if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
        WT_ERR(__rollback_abort_insert_list(session, page, insert, rollback_timestamp, NULL));

    /*
     * Review updates that belong to keys that are on the disk image, as well as for keys inserted
     * since the page was read from disk.
     */
    WT_ROW_FOREACH (page, rip, i) {
        stable_update_found = false;
        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
            WT_ERR(
              __rollback_abort_update(session, key, upd, rollback_timestamp, &stable_update_found));
            have_key = true;
        } else
            have_key = false;

        if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
            WT_ERR(__rollback_abort_insert_list(session, page, insert, rollback_timestamp, NULL));

        /*
         * If there is no stable update found in the update list, abort any on-disk value.
         */
        if (!stable_update_found) {
            vpack = &_vpack;
            __wt_row_leaf_value_cell(session, page, rip, vpack);
            WT_ERR(__rollback_abort_ondisk_kv(
              session, ref, rip, 0, have_key ? key : NULL, vpack, rollback_timestamp, NULL));
        }
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __rollback_get_ref_max_durable_timestamp --
 *     Returns the ref aggregated max durable timestamp. The max durable timestamp is calculated
 *     between both start and stop durable timestamps except for history store, because most of the
 *     history store updates have stop timestamp either greater or equal to the start timestamp
 *     except for the updates written for the prepared updates on the data store. To abort the
 *     updates with no stop timestamp, we must include the newest stop timestamp also into the
 *     calculation of maximum durable timestamp of the history store.
 */
static wt_timestamp_t
__rollback_get_ref_max_durable_timestamp(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta)
{
    if (WT_IS_HS(session->dhandle))
        return (WT_MAX(ta->newest_stop_durable_ts, ta->newest_stop_ts));
    return (WT_MAX(ta->newest_start_durable_ts, ta->newest_stop_durable_ts));
}

/*
 * __rollback_page_needs_abort --
 *     Check whether the page needs rollback. Return true if the page has modifications newer than
 *     the given timestamp Otherwise return false.
 */
static bool
__rollback_page_needs_abort(
  WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_ADDR *addr;
    WT_CELL_UNPACK_ADDR vpack;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    wt_timestamp_t durable_ts;
    uint64_t newest_txn;
    uint32_t i;
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *tag;
    bool prepared, result;

    addr = ref->addr;
    mod = ref->page == NULL ? NULL : ref->page->modify;
    durable_ts = WT_TS_NONE;
    newest_txn = WT_TXN_NONE;
    tag = "undefined state";
    prepared = result = false;

    /*
     * The rollback operation should be performed on this page when any one of the following is
     * greater than the given timestamp or during recovery if the newest transaction id on the page
     * is greater than or equal to recovered checkpoint snapshot min:
     * 1. The reconciled replace page max durable timestamp.
     * 2. The reconciled multi page max durable timestamp.
     * 3. The on page address max durable timestamp.
     * 4. The off page address max durable timestamp.
     */
    if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE) {
        tag = "reconciled replace block";
        durable_ts = __rollback_get_ref_max_durable_timestamp(session, &mod->mod_replace.ta);
        prepared = mod->mod_replace.ta.prepare;
        result = (durable_ts > rollback_timestamp) || prepared;
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        tag = "reconciled multi block";
        /* Calculate the max durable timestamp by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            durable_ts = WT_MAX(
              durable_ts, __rollback_get_ref_max_durable_timestamp(session, &multi->addr.ta));
            if (multi->addr.ta.prepare)
                prepared = true;
        }
        result = (durable_ts > rollback_timestamp) || prepared;
    } else if (!__wt_off_page(ref->home, addr)) {
        tag = "on page cell";
        /* Check if the page is obsolete using the page disk address. */
        __wt_cell_unpack_addr(session, ref->home->dsk, (WT_CELL *)addr, &vpack);
        durable_ts = __rollback_get_ref_max_durable_timestamp(session, &vpack.ta);
        prepared = vpack.ta.prepare;
        newest_txn = vpack.ta.newest_txn;
        result = (durable_ts > rollback_timestamp) || prepared ||
          WT_CHECK_RECOVERY_FLAG_TXNID(session, newest_txn);
    } else if (addr != NULL) {
        tag = "address";
        durable_ts = __rollback_get_ref_max_durable_timestamp(session, &addr->ta);
        prepared = addr->ta.prepare;
        newest_txn = addr->ta.newest_txn;
        result = (durable_ts > rollback_timestamp) || prepared ||
          WT_CHECK_RECOVERY_FLAG_TXNID(session, newest_txn);
    }

    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      "%p: page with %s durable timestamp: %s, newest txn: %" PRIu64 " and prepared updates: %s",
      (void *)ref, tag, __wt_timestamp_to_string(durable_ts, ts_string), newest_txn,
      prepared ? "true" : "false");

    return (result);
}

/*
 * __rollback_abort_updates --
 *     Abort updates on this page newer than the timestamp.
 */
static int
__rollback_abort_updates(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_PAGE *page;

    /*
     * If we have a ref with clean page, find out whether the page has any modifications that are
     * newer than the given timestamp. As eviction writes the newest version to page, even a clean
     * page may also contain modifications that need rollback.
     */
    page = ref->page;
    if (!__wt_page_is_modified(page) &&
      !__rollback_page_needs_abort(session, ref, rollback_timestamp)) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session), "%p: page skipped", (void *)ref);
        return (0);
    }

    WT_STAT_CONN_INCR(session, txn_rts_pages_visited);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      "%p: page rolled back when page is modified: %s", (void *)ref,
      __wt_page_is_modified(page) ? "true" : "false");

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        WT_RET(__rollback_abort_col_fix(session, ref, rollback_timestamp));
        break;
    case WT_PAGE_COL_VAR:
        WT_RET(__rollback_abort_col_var(session, ref, rollback_timestamp));
        break;
    case WT_PAGE_ROW_LEAF:
        WT_RET(__rollback_abort_row_leaf(session, ref, rollback_timestamp));
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        /* This function is not called for internal pages. */
        WT_ASSERT(session, false);
        /* Fall through. */
    default:
        WT_RET(__wt_illegal_value(session, page->type));
    }

    /* Mark the page as dirty to reconcile the page. */
    if (page->modify)
        __wt_page_modify_set(session, page);
    return (0);
}

/*
 * __rollback_abort_fast_truncate --
 *     Abort fast truncate for an internal page of leaf pages.
 */
static int
__rollback_abort_fast_truncate(
  WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_REF *child_ref;

    WT_INTL_FOREACH_BEGIN (session, ref->page, child_ref) {
        /*
         * A fast-truncate page is either in the WT_REF_DELETED state (where the WT_PAGE_DELETED
         * structure has the timestamp information), or in an in-memory state where it started as a
         * fast-truncate page which was then instantiated and the timestamp information moved to the
         * individual WT_UPDATE structures. When reviewing internal pages, ignore the second case,
         * an instantiated page is handled when the leaf page is visited.
         */
        if (child_ref->state == WT_REF_DELETED && child_ref->ft_info.del != NULL &&
          rollback_timestamp < child_ref->ft_info.del->durable_timestamp) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "%p: deleted page rolled back", (void *)child_ref);
            WT_RET(__wt_delete_page_rollback(session, child_ref));
        }
    }
    WT_INTL_FOREACH_END;
    return (0);
}

/*
 * __wt_rts_page_skip --
 *     Skip if rollback to stable doesn't requires to read this page.
 */
int
__wt_rts_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool *skipp)
{
    wt_timestamp_t rollback_timestamp;

    rollback_timestamp = *(wt_timestamp_t *)(context);
    *skipp = false; /* Default to reading */

    /* If the page state is other than on disk, we want to look at it. */
    if (ref->state != WT_REF_DISK)
        return (0);

    /* Check whether this ref has any possible updates to be aborted. */
    if (!__rollback_page_needs_abort(session, ref, rollback_timestamp)) {
        *skipp = true;
        __wt_verbose_multi(
          session, WT_VERB_RECOVERY_RTS(session), "%p: page walk skipped", (void *)ref);
        WT_STAT_CONN_INCR(session, txn_rts_tree_walk_skip_pages);
    }

    return (0);
}

/*
 * __rollback_to_stable_btree_walk --
 *     Called for each open handle - choose to either skip or wipe the commits
 */
static int
__rollback_to_stable_btree_walk(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_REF *ref;

    /* Walk the tree, marking commits aborted where appropriate. */
    ref = NULL;
    while ((ret = __wt_tree_walk_custom_skip(session, &ref, __wt_rts_page_skip, &rollback_timestamp,
              WT_READ_NO_EVICT | WT_READ_WONT_NEED)) == 0 &&
      ref != NULL)
        if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
            WT_WITH_PAGE_INDEX(
              session, ret = __rollback_abort_fast_truncate(session, ref, rollback_timestamp));
        else
            WT_RET(__rollback_abort_updates(session, ref, rollback_timestamp));

    return (ret);
}

/*
 * __rollback_to_stable_btree --
 *     Called for each object handle - choose to either skip or wipe the commits
 */
static int
__rollback_to_stable_btree(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;

    btree = S2BT(session);
    conn = S2C(session);

    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      "rollback to stable connection logging enabled: %s and btree logging enabled: %s",
      FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) ? "true" : "false",
      !F_ISSET(btree, WT_BTREE_NO_LOGGING) ? "true" : "false");

    /*
     * Immediately durable files don't get their commits wiped. This case mostly exists to support
     * the semantic required for the oplog in MongoDB - updates that have been made to the oplog
     * should not be aborted. It also wouldn't be safe to roll back updates for any table that had
     * its records logged: those updates would be recovered after a crash, making them inconsistent.
     */
    if (__wt_btree_immediately_durable(session))
        return (0);

    /* There is never anything to do for checkpoint handles. */
    if (session->dhandle->checkpoint != NULL)
        return (0);

    /* There is nothing to do on an empty tree. */
    if (btree->root.page == NULL)
        return (0);

    return (__rollback_to_stable_btree_walk(session, rollback_timestamp));
}

/*
 * __txn_user_active --
 *     Return if there are any running user transactions.
 */
static bool
__txn_user_active(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session_in_list;
    uint32_t i, session_cnt;
    bool txn_active;

    conn = S2C(session);
    txn_active = false;

    WT_STAT_CONN_INCR(session, txn_walk_sessions);

    /*
     * WT_TXN structures are allocated and freed as sessions are activated and closed. Lock the
     * session open/close to ensure we don't race. This call is a rarely used RTS-only function,
     * acquiring the lock shouldn't be an issue.
     */
    __wt_spin_lock(session, &conn->api_lock);

    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (i = 0, session_in_list = conn->sessions; i < session_cnt; i++, session_in_list++) {

        /* Skip inactive or internal sessions. */
        if (!session_in_list->active || F_ISSET(session_in_list, WT_SESSION_INTERNAL))
            continue;

        /* Check if a user session has a running transaction. */
        if (F_ISSET(session_in_list->txn, WT_TXN_RUNNING)) {
            txn_active = true;
            break;
        }
    }
    __wt_spin_unlock(session, &conn->api_lock);

    /*
     * A new transaction may start after we return from this call and callers should be aware of
     * this limitation.
     */
    return (txn_active);
}

/*
 * __rollback_to_stable_check --
 *     Ensure the rollback request is reasonable.
 */
static int
__rollback_to_stable_check(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    bool txn_active;

    /*
     * Help the user comply with the requirement that there are no concurrent user operations. It is
     * okay to have a transaction in prepared state.
     */
    txn_active = __txn_user_active(session);
#ifdef HAVE_DIAGNOSTIC
    if (txn_active)
        WT_TRET(__wt_verbose_dump_txn(session));
#endif

    if (txn_active)
        WT_RET_MSG(session, EINVAL, "rollback_to_stable illegal with active transactions");

    return (ret);
}

/*
 * __rollback_to_stable_btree_hs_truncate --
 *     Wipe all history store updates for the btree (non-timestamped tables)
 */
static int
__rollback_to_stable_btree_hs_truncate(WT_SESSION_IMPL *session, uint32_t btree_id)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    char ts_string[WT_TS_INT_STRING_SIZE];

    hs_cursor = NULL;

    WT_RET(__wt_scr_alloc(session, 0, &hs_key));

    /* Open a history store table cursor. */
    WT_ERR(__wt_curhs_open(session, NULL, &hs_cursor));

    /* Walk the history store for the given btree. */
    hs_cursor->set_key(hs_cursor, 1, btree_id);
    ret = __wt_curhs_search_near_after(session, hs_cursor);

    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

        /* We shouldn't cross the btree search space. */
        WT_ASSERT(session, btree_id == hs_btree_id);

        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "rollback to stable history store cleanup of update with start timestamp: %s",
          __wt_timestamp_to_string(hs_start_ts, ts_string));

        WT_ERR(hs_cursor->remove(hs_cursor));
        WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &hs_key);
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));

    return (ret);
}

/*
 * __rollback_to_stable_hs_final_pass --
 *     Perform rollback to stable on the history store to remove any entries newer than the stable
 *     timestamp.
 */
static int
__rollback_to_stable_hs_final_pass(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, durableval, key;
    WT_DECL_RET;
    wt_timestamp_t max_durable_ts, newest_stop_durable_ts, newest_stop_ts;
    char *config;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    config = NULL;

    WT_RET(__wt_metadata_search(session, WT_HS_URI, &config));

    /*
     * Find out the max durable timestamp of the history store from checkpoint. Most of the history
     * store updates have stop timestamp either greater or equal to the start timestamp except for
     * the updates written for the prepared updates on the data store. To abort the updates with no
     * stop timestamp, we must include the newest stop timestamp also into the calculation of
     * maximum timestamp of the history store.
     */
    newest_stop_durable_ts = newest_stop_ts = WT_TS_NONE;
    WT_ERR(__wt_config_getones(session, config, "checkpoint", &cval));
    __wt_config_subinit(session, &ckptconf, &cval);
    for (; __wt_config_next(&ckptconf, &key, &cval) == 0;) {
        ret = __wt_config_subgets(session, &cval, "newest_stop_durable_ts", &durableval);
        if (ret == 0)
            newest_stop_durable_ts = WT_MAX(newest_stop_durable_ts, (wt_timestamp_t)durableval.val);
        WT_ERR_NOTFOUND_OK(ret, false);
        ret = __wt_config_subgets(session, &cval, "newest_stop_ts", &durableval);
        if (ret == 0)
            newest_stop_ts = WT_MAX(newest_stop_ts, (wt_timestamp_t)durableval.val);
        WT_ERR_NOTFOUND_OK(ret, false);
    }
    max_durable_ts = WT_MAX(newest_stop_ts, newest_stop_durable_ts);
    WT_ERR(__wt_session_get_dhandle(session, WT_HS_URI, NULL, NULL, 0));

    /*
     * The rollback operation should be performed on the history store file when the checkpoint
     * durable start/stop timestamp is greater than the rollback timestamp. But skip if there is no
     * stable timestamp.
     *
     * Note that the corresponding code in __rollback_to_stable_btree_apply also checks whether
     * there _are_ timestamped updates by checking max_durable_ts; that check is redundant here for
     * several reasons, the most immediate being that max_durable_ts cannot be none (zero) because
     * it's greater than rollback_timestamp, which is itself greater than zero.
     */
    if (max_durable_ts > rollback_timestamp && rollback_timestamp != WT_TS_NONE) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "tree rolled back with durable timestamp: %s",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]));
        WT_TRET(__rollback_to_stable_btree(session, rollback_timestamp));
    } else
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "tree skipped with durable timestamp: %s and stable timestamp: %s",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[1]));

    WT_TRET(__wt_session_release_dhandle(session));

err:
    __wt_free(session, config);
    return (ret);
}

/*
 * __rollback_progress_msg --
 *     Log a verbose message about the progress of the current rollback to stable.
 */
static void
__rollback_progress_msg(WT_SESSION_IMPL *session, struct timespec rollback_start,
  uint64_t rollback_count, uint64_t *rollback_msg_count)
{
    struct timespec cur_time;
    uint64_t time_diff;

    __wt_epoch(session, &cur_time);

    /* Time since the rollback started. */
    time_diff = WT_TIMEDIFF_SEC(cur_time, rollback_start);

    if ((time_diff / WT_PROGRESS_MSG_PERIOD) > *rollback_msg_count) {
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
          "Rollback to stable has been running for %" PRIu64 " seconds and has inspected %" PRIu64
          " files. For more detailed logging, enable WT_VERB_RTS",
          time_diff, rollback_count);
        ++(*rollback_msg_count);
    }
}

/*
 * __rollback_to_stable_btree_apply --
 *     Perform rollback to stable on a single file.
 */
static int
__rollback_to_stable_btree_apply(
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
    bool dhandle_allocated, durable_ts_found, has_txn_updates_gt_than_ckpt_snap, perform_rts;
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
     * 1. Empty table.
     * 2. Table has timestamped updates without a stable timestamp.
     */
    if ((F_ISSET(S2C(session), WT_CONN_RECOVERING) ||
          F_ISSET(S2C(session), WT_CONN_CLOSING_CHECKPOINT)) &&
      (addr_size == 0 || (rollback_timestamp == WT_TS_NONE && max_durable_ts != WT_TS_NONE))) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "skip rollback to stable on file %s because %s", uri,
          addr_size == 0 ? "its checkpoint address length is 0" :
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
    WT_WITH_HANDLE_LIST_READ_LOCK(session, (ret = __wt_conn_dhandle_find(session, uri, NULL)));

    perform_rts = ret == 0 && S2BT(session)->modified;

    WT_ERR_NOTFOUND_OK(ret, false);

    if (perform_rts || max_durable_ts > rollback_timestamp || prepared_updates ||
      !durable_ts_found || has_txn_updates_gt_than_ckpt_snap) {
        ret = __wt_session_get_dhandle(session, uri, NULL, NULL, 0);
        if (ret != 0)
            WT_ERR_MSG(session, ret, "%s: unable to open handle%s", uri,
              ret == EBUSY ? ", error indicates handle is unavailable due to concurrent use" : "");
        dhandle_allocated = true;

        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "tree rolled back with durable timestamp: %s, or when tree is modified: %s or "
          "prepared updates: %s or when durable time is not found: %s or txnid: %" PRIu64
          " is greater than recovery checkpoint snap min: %s",
          __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
          S2BT(session)->modified ? "true" : "false", prepared_updates ? "true" : "false",
          !durable_ts_found ? "true" : "false", rollback_txnid,
          has_txn_updates_gt_than_ckpt_snap ? "true" : "false");
        WT_ERR(__rollback_to_stable_btree(session, rollback_timestamp));
    } else
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "%s: tree skipped with durable timestamp: %s and stable timestamp: %s or txnid: %" PRIu64,
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
        WT_ERR(__rollback_to_stable_btree_hs_truncate(session, btree_id));
    }

err:
    if (dhandle_allocated)
        WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_rollback_to_stable_one --
 *     Perform rollback to stable on a single object.
 */
int
__wt_rollback_to_stable_one(WT_SESSION_IMPL *session, const char *uri, bool *skipp)
{
    WT_DECL_RET;
    wt_timestamp_t rollback_timestamp;
    char *config;

    /*
     * This is confusing: the caller's boolean argument "skip" stops the schema-worker loop from
     * processing this object and any underlying objects it may have (for example, a table with
     * multiple underlying file objects). We rollback-to-stable all of the file objects an object
     * may contain, so set the caller's skip argument to true on all file objects, else set the
     * caller's skip argument to false so our caller continues down the tree of objects.
     */
    *skipp = WT_BTREE_PREFIX(uri);
    if (!*skipp)
        return (0);

    WT_RET(__wt_metadata_search(session, uri, &config));

    /* Read the stable timestamp once, when we first start up. */
    WT_ORDERED_READ(rollback_timestamp, S2C(session)->txn_global.stable_timestamp);

    F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
    ret = __rollback_to_stable_btree_apply(session, uri, config, rollback_timestamp);
    F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

    __wt_free(session, config);

    return (ret);
}

/*
 * __rollback_to_stable_btree_apply_all --
 *     Perform rollback to stable to all files listed in the metadata, apart from the metadata and
 *     history store files.
 */
static int
__rollback_to_stable_btree_apply_all(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    struct timespec rollback_timer;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t rollback_count, rollback_msg_count;
    const char *config, *uri;

    /* Initialize the verbose tracking timer. */
    __wt_epoch(session, &rollback_timer);
    rollback_count = 0;
    rollback_msg_count = 0;

    WT_RET(__wt_metadata_cursor(session, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        /* Log a progress message. */
        __rollback_progress_msg(session, rollback_timer, rollback_count, &rollback_msg_count);
        ++rollback_count;

        WT_ERR(cursor->get_key(cursor, &uri));
        WT_ERR(cursor->get_value(cursor, &config));

        F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
        ret = __rollback_to_stable_btree_apply(session, uri, config, rollback_timestamp);
        F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);

        /*
         * Ignore rollback to stable failures on files that don't exist or files where corruption is
         * detected.
         */
        if (ret == ENOENT || (ret == WT_ERROR && F_ISSET(S2C(session), WT_CONN_DATA_CORRUPTION))) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              "%s: skipped performing rollback to stable because the file %s", uri,
              ret == ENOENT ? "does not exist" : "is corrupted.");
            continue;
        }
        WT_ERR(ret);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
        WT_ERR(__rollback_to_stable_hs_final_pass(session, rollback_timestamp));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __rollback_to_stable --
 *     Rollback all modifications with timestamps more recent than the passed in timestamp.
 */
static int
__rollback_to_stable(WT_SESSION_IMPL *session, bool no_ckpt)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t rollback_timestamp;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    conn = S2C(session);
    txn_global = &conn->txn_global;

    /*
     * Rollback to stable should ignore tombstones in the history store since it needs to scan the
     * entire table sequentially.
     */
    F_SET(session, WT_SESSION_ROLLBACK_TO_STABLE);

    WT_ERR(__rollback_to_stable_check(session));

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    WT_ORDERED_READ(rollback_timestamp, txn_global->stable_timestamp);
    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      "performing rollback to stable with stable timestamp: %s and oldest timestamp: %s",
      __wt_timestamp_to_string(rollback_timestamp, ts_string[0]),
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[1]));

    if (F_ISSET(conn, WT_CONN_RECOVERING))
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          "recovered checkpoint snapshot min:  %" PRIu64 ", snapshot max: %" PRIu64
          ", snapshot count: %" PRIu32,
          conn->recovery_ckpt_snap_min, conn->recovery_ckpt_snap_max,
          conn->recovery_ckpt_snapshot_count);

    WT_ERR(__rollback_to_stable_btree_apply_all(session, rollback_timestamp));

    /* Rollback the global durable timestamp to the stable timestamp. */
    txn_global->has_durable_timestamp = txn_global->has_stable_timestamp;
    txn_global->durable_timestamp = txn_global->stable_timestamp;

    /*
     * If the configuration is not in-memory, forcibly log a checkpoint after rollback to stable to
     * ensure that both in-memory and on-disk versions are the same unless caller requested for no
     * checkpoint.
     */
    if (!F_ISSET(conn, WT_CONN_IN_MEMORY) && !no_ckpt)
        WT_ERR(session->iface.checkpoint(&session->iface, "force=1"));

err:
    F_CLR(session, WT_SESSION_ROLLBACK_TO_STABLE);
    return (ret);
}

/*
 * __wt_rollback_to_stable --
 *     Rollback the database to the stable timestamp.
 */
int
__wt_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[], bool no_ckpt)
{
    WT_DECL_RET;

    WT_UNUSED(cfg);

    /*
     * Don't use the connection's default session: we are working on data handles and (a) don't want
     * to cache all of them forever, plus (b) can't guarantee that no other method will be called
     * concurrently. Copy parent session no logging option to the internal session to make sure that
     * rollback to stable doesn't generate log records.
     */
    WT_RET(__wt_open_internal_session(S2C(session), "txn rollback_to_stable", true,
      F_MASK(session, WT_SESSION_NO_LOGGING), 0, &session));

    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 1);
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __rollback_to_stable(session, no_ckpt)));
    WT_STAT_CONN_SET(session, txn_rollback_to_stable_running, 0);

    WT_TRET(__wt_session_close_internal(session));

    return (ret);
}
