/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rts_btree_abort_update --
 *     Abort updates in an update change with timestamps newer than the rollback timestamp. Also,
 *     clear the history store flag for the first stable update in the update.
 */
static int
__rts_btree_abort_update(WT_SESSION_IMPL *session, WT_ITEM *key, WT_UPDATE *first_upd,
  wt_timestamp_t rollback_timestamp, bool *stable_update_found)
{
    WT_UPDATE *stable_upd, *tombstone, *upd;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool dryrun;
    bool txn_id_visible;

    dryrun = S2C(session)->rts->dryrun;

    stable_upd = tombstone = NULL;
    txn_id_visible = false;
    if (stable_update_found != NULL)
        *stable_update_found = false;
    for (upd = first_upd; upd != NULL; upd = upd->next) {
        /* Skip the updates that are aborted. */
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        /*
         * An unstable update needs to be aborted if any of the following are true:
         * 1. An update is invisible based on the checkpoint snapshot during recovery.
         * 2. The update durable timestamp is greater than the stable timestamp.
         * 3. The update is a prepared update.
         *
         * Usually during recovery, there are no in memory updates present on the page. But
         * whenever an unstable fast truncate operation is written to the disk, as part
         * of the rollback to stable page read, it instantiates the tombstones on the page.
         * The transaction id validation is ignored in all scenarios except recovery.
         */
        txn_id_visible = __wt_rts_visibility_txn_visible_id(session, upd->txnid);
        if (!txn_id_visible || rollback_timestamp < upd->durable_ts ||
          upd->prepare_state == WT_PREPARE_INPROGRESS) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              WT_RTS_VERB_TAG_UPDATE_ABORT
              "rollback to stable aborting update with txnid=%" PRIu64
              ", txnid_not_visible=%s"
              ", stable_timestamp=%s < durable_timestamp=%s: %s, prepare_state=%s",
              upd->txnid, !txn_id_visible ? "true" : "false",
              __wt_timestamp_to_string(rollback_timestamp, ts_string[1]),
              __wt_timestamp_to_string(upd->durable_ts, ts_string[0]),
              rollback_timestamp < upd->durable_ts ? "true" : "false",
              __wt_prepare_state_str(upd->prepare_state));

            if (!dryrun)
                upd->txnid = WT_TXN_ABORTED;
            WT_RTS_STAT_CONN_INCR(session, txn_rts_upd_aborted);
        } else {
            /* Valid update is found. */
            stable_upd = upd;
            break;
        }
    }

    /*
     * Clear the history store flags for the stable update to indicate that this update should be
     * written to the history store later. The next time when this update is moved into the history
     * store, it will have a different stop time point.
     */
    if (stable_upd != NULL) {
        if (F_ISSET(stable_upd, WT_UPDATE_HS | WT_UPDATE_TO_DELETE_FROM_HS)) {
            /* Find the update following a stable tombstone. */
            if (stable_upd->type == WT_UPDATE_TOMBSTONE) {
                tombstone = stable_upd;
                for (stable_upd = stable_upd->next; stable_upd != NULL;
                     stable_upd = stable_upd->next) {
                    if (stable_upd->txnid != WT_TXN_ABORTED) {
                        WT_ASSERT(session,
                          stable_upd->type != WT_UPDATE_TOMBSTONE &&
                            F_ISSET(stable_upd, WT_UPDATE_HS | WT_UPDATE_TO_DELETE_FROM_HS));
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
            WT_RET(__wt_rts_history_delete_hs(
              session, key, stable_upd == NULL ? tombstone->start_ts : stable_upd->start_ts));

            /*
             * Clear the history store flags for the first stable update. Otherwise, it will not be
             * moved to history store again.
             */
            if (!dryrun) {
                if (stable_upd != NULL)
                    F_CLR(stable_upd, WT_UPDATE_HS | WT_UPDATE_TO_DELETE_FROM_HS);
                if (tombstone != NULL)
                    F_CLR(tombstone, WT_UPDATE_HS | WT_UPDATE_TO_DELETE_FROM_HS);
            }
        }
        if (stable_update_found != NULL)
            *stable_update_found = true;
    }

    return (0);
}

/*
 * __rts_btree_abort_insert_list --
 *     Apply the update abort check to each entry in an insert skip list. Return how many entries
 *     had stable updates.
 */
static int
__rts_btree_abort_insert_list(WT_SESSION_IMPL *session, WT_PAGE *page, WT_INSERT_HEAD *head,
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
            WT_ERR(__rts_btree_abort_update(
              session, key, ins->upd, rollback_timestamp, &stable_update_found));
            if (stable_update_found && stable_updates_count != NULL)
                (*stable_updates_count)++;
            if (!stable_update_found && page->type == WT_PAGE_ROW_LEAF &&
              !F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
                /*
                 * When a new key is added to a page and the page is then checkpointed, updates for
                 * that key can be present in the History Store while the key isn't present in the
                 * disk image. RTS will then only remove these updates when there is a stable update
                 * on-chain. These updates still need removing when no stable updates are on-chain,
                 * so do so here explicitly. Pass in rollback_timestamp + 1 as history store cleanup
                 * removes updates inclusive of the provided timestamp, but we only want to remove
                 * unstable updates.
                 *
                 * FIXME-WT-10017: WT-9846 is an interim fix only for row-store while we investigate
                 * the impacts of a long term correction in WT-10017. Once completed this change can
                 * be reverted.
                 */
                WT_ERR(__wt_rts_history_delete_hs(session, key, rollback_timestamp + 1));
        }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __rts_btree_col_modify --
 *     Add the provided update to the head of the update list.
 */
static inline int
__rts_btree_col_modify(WT_SESSION_IMPL *session, WT_REF *ref, WT_UPDATE *upd, uint64_t recno)
{
    WT_CURSOR_BTREE cbt;
    WT_DECL_RET;
    bool dryrun;

    dryrun = S2C(session)->rts->dryrun;

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    /* Search the page. */
    WT_ERR(__wt_col_search(&cbt, recno, ref, true, NULL));

    /* Apply the modification. */
    if (!dryrun)
        WT_ERR(__wt_col_modify(&cbt, recno, NULL, upd, WT_UPDATE_INVALID, true, false));

err:
    /* Free any resources that may have been cached in the cursor. */
    WT_TRET(__wt_btcur_close(&cbt, true));

    return (ret);
}

/*
 * __rts_btree_row_modify --
 *     Add the provided update to the head of the update list.
 */
static inline int
__rts_btree_row_modify(WT_SESSION_IMPL *session, WT_REF *ref, WT_UPDATE *upd, WT_ITEM *key)
{
    WT_CURSOR_BTREE cbt;
    WT_DECL_RET;
    bool dryrun;

    dryrun = S2C(session)->rts->dryrun;

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    /* Search the page. */
    WT_ERR(__wt_row_search(&cbt, key, true, ref, true, NULL));

    /* Apply the modification. */
    if (!dryrun)
        WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true, false));

err:
    /* Free any resources that may have been cached in the cursor. */
    WT_TRET(__wt_btcur_close(&cbt, true));

    return (ret);
}

/*
 * __rts_btree_ondisk_fixup_key --
 *     Abort updates in the history store and replace the on-disk value with an update that
 *     satisfies the given timestamp.
 */
static int
__rts_btree_ondisk_fixup_key(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip, uint64_t recno,
  WT_ITEM *row_key, WT_CELL_UNPACK_KV *unpack, wt_timestamp_t rollback_timestamp)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(full_value);
    WT_DECL_ITEM(hs_key);
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(key_string);
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
    bool dryrun;
    bool first_record;
    bool valid_update_found;

    dryrun = S2C(session)->rts->dryrun;

    page = ref->page;

    hs_cursor = NULL;
    tombstone = upd = NULL;
    hs_durable_ts = hs_start_ts = hs_stop_durable_ts = WT_TS_NONE;
    hs_btree_id = S2BT(session)->id;
    valid_update_found = false;
    first_record = true;

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

    WT_ERR(__wt_scr_alloc(session, 0, &key_string));
    __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_2,
      WT_RTS_VERB_TAG_ONDISK_KEY_ROLLBACK "rolling back the on-disk key=%s",
      __wt_key_string(session, key->data, key->size, S2BT(session)->key_format, key_string));

    WT_ERR(__wt_scr_alloc(session, 0, &full_value));
    WT_ERR(__wt_page_cell_data_ref_kv(session, page, unpack, full_value));
    /*
     * We can read overflow removed value if checkpoint has run before rollback to stable. In this
     * case, we have already appended the on page value to the update chain. At this point, we have
     * visited the update chain and decided the value is not stable. In addition, checkpoint must
     * have moved this value to the history store as a full value. Therefore, we can safely ignore
     * the on page value if it is overflow removed.
     */
    if (__wt_cell_type_raw(unpack->cell) == WT_CELL_VALUE_OVFL_RM)
        ret = 0;
    else
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
        if (__wt_rts_visibility_txn_visible_id(session, hs_tw->stop_txn) &&
          hs_tw->durable_stop_ts <= pinned_ts) {
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              WT_RTS_VERB_TAG_HS_STOP_OBSOLETE
              "history store stop is obsolete with time_window=%s and pinned_timestamp=%s",
              __wt_time_window_to_string(hs_tw, tw_string),
              __wt_timestamp_to_string(pinned_ts, ts_string[0]));
            if (!dryrun)
                WT_ERR(hs_cursor->remove(hs_cursor));
            WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);

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
        if (hs_tw->start_ts <= unpack->tw.start_ts || unpack->tw.prepare) {
            if (type == WT_UPDATE_MODIFY)
                WT_ERR(__wt_modify_apply_item(
                  session, S2BT(session)->value_format, full_value, hs_value->data));
            else {
                WT_ASSERT(session, type == WT_UPDATE_STANDARD);
                WT_ERR(__wt_buf_set(session, full_value, hs_value->data, hs_value->size));
            }
        } else
            __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_2,
              WT_RTS_VERB_TAG_HS_GT_ONDISK
              "history store update more recent than on-disk update with time_window=%s and "
              "type=%s",
              __wt_time_window_to_string(hs_tw, tw_string), __wt_update_type_str(type));

        /*
         * Verify the history store timestamps are in order. The start timestamp may be equal to the
         * stop timestamp if the original update's commit timestamp is in order. We may see records
         * newer than or equal to the onpage value if eviction runs concurrently with checkpoint. In
         * that case, don't verify the first record.
         *
         * It is possible during a prepared transaction rollback, the history store update that have
         * its own stop timestamp doesn't get removed leads to duplicate records in history store
         * after further operations on that same key. Rollback to stable should ignore such records
         * for timestamp ordering verification.
         *
         * It is possible that there can be an update in the history store with a max stop timestamp
         * in the middle of the same key updates. This occurs when the checkpoint writes the
         * committed prepared update and further updates on that key including the history store
         * changes before the transaction fixes the history store update to have a proper stop
         * timestamp. It is a rare scenario.
         */
        WT_ASSERT_ALWAYS(session,
          hs_stop_durable_ts <= newer_hs_durable_ts || hs_start_ts == hs_stop_durable_ts ||
            hs_start_ts == newer_hs_durable_ts || newer_hs_durable_ts == hs_durable_ts ||
            first_record || hs_stop_durable_ts == WT_TS_MAX,
          "Out of order history store updates detected");

        if (hs_stop_durable_ts < newer_hs_durable_ts)
            WT_STAT_CONN_DATA_INCR(session, txn_rts_hs_stop_older_than_newer_start);

        /*
         * Validate the timestamps in the key and the cell are same. This must be validated only
         * after verifying it's stop time window is not globally visible. The start timestamps of
         * the time window are cleared when they are globally visible and there will be no stop
         * timestamp in the history store whenever a prepared update is written to the data store.
         */
        WT_ASSERT(session,
          (hs_tw->start_ts == WT_TS_NONE || hs_tw->start_ts == hs_start_ts) &&
            (hs_tw->durable_start_ts == WT_TS_NONE || hs_tw->durable_start_ts == hs_durable_ts) &&
            ((hs_tw->durable_stop_ts == 0 && hs_stop_durable_ts == WT_TS_MAX) ||
              hs_tw->durable_stop_ts == hs_stop_durable_ts));

        /*
         * Stop processing when we find a stable update according to the given timestamp and
         * transaction id.
         */
        if (__wt_rts_visibility_txn_visible_id(session, hs_tw->start_txn) &&
          hs_tw->durable_start_ts <= rollback_timestamp) {
            __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_2,
              WT_RTS_VERB_TAG_HS_UPDATE_VALID
              "history store update valid with time_window=%s, type=%s and stable_timestamp=%s",
              __wt_time_window_to_string(hs_tw, tw_string), __wt_update_type_str(type),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[0]));
            WT_ASSERT(session, unpack->tw.prepare || hs_tw->start_ts <= unpack->tw.start_ts);
            valid_update_found = true;
            break;
        }

        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_HS_UPDATE_ABORT
          "history store update aborted with time_window=%s, type=%s and stable_timestamp=%s",
          __wt_time_window_to_string(hs_tw, tw_string), __wt_update_type_str(type),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[3]));

        /*
         * Start time point of the current record may be used as stop time point of the previous
         * record. Save it to verify against the previous record and check if we need to append the
         * stop time point as a tombstone when we rollback the history store record.
         */
        newer_hs_durable_ts = hs_durable_ts;
        first_record = false;

        if (!dryrun)
            WT_ERR(hs_cursor->remove(hs_cursor));
        WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        WT_RTS_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts_unstable);
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
          WT_RTS_VERB_TAG_HS_UPDATE_RESTORED "history store update restored txnid=%" PRIu64
                                             ", start_ts=%s and durable_ts=%s",
          upd->txnid, __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
          __wt_timestamp_to_string(upd->durable_ts, ts_string[1]));

        /*
         * Set the flag to indicate that this update has been restored from history store for the
         * rollback to stable operation.
         */
        F_SET(upd, WT_UPDATE_RESTORED_FROM_HS);
        WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_restore_updates);

        /*
         * We have a tombstone on the original update chain and it is stable according to the
         * timestamp and txnid, we need to restore that as well.
         */
        if (__wt_rts_visibility_txn_visible_id(session, hs_tw->stop_txn) &&
          hs_tw->durable_stop_ts <= rollback_timestamp) {
            /*
             * The restoring tombstone timestamp must be zero or less than previous update start
             * timestamp.
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
              WT_RTS_VERB_TAG_HS_RESTORE_TOMBSTONE
              "history store tombstone restored, txnid=%" PRIu64 ", start_ts=%s and durable_ts=%s",
              tombstone->txnid, __wt_timestamp_to_string(tombstone->start_ts, ts_string[0]),
              __wt_timestamp_to_string(tombstone->durable_ts, ts_string[1]));

            /*
             * Set the flag to indicate that this update has been restored from history store for
             * the rollback to stable operation.
             */
            F_SET(tombstone, WT_UPDATE_RESTORED_FROM_HS);

            tombstone->next = upd;
            upd = tombstone;
            WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_restore_tombstones);
        }
    } else {
        WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
        WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
        __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_3,
          WT_RTS_VERB_TAG_KEY_REMOVED "%s", "key removed");
    }

    if (rip != NULL)
        WT_ERR(__rts_btree_row_modify(session, ref, upd, key));
    else
        WT_ERR(__rts_btree_col_modify(session, ref, upd, recno));

    /* Finally remove that update from history store. */
    if (valid_update_found) {
        /* Avoid freeing the updates while still in use if hs_cursor->remove fails. */
        upd = tombstone = NULL;

        if (!dryrun)
            WT_ERR(hs_cursor->remove(hs_cursor));
        WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_hs_removed);
        WT_RTS_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate_rts);
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
    __wt_scr_free(session, &key_string);
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));
    if (dryrun) {
        WT_ASSERT(session, !valid_update_found || upd == NULL);
        __wt_free_update_list(session, &upd);
    }
    return (ret);
}

/*
 * __rts_btree_abort_ondisk_kv --
 *     Fix the on-disk K/V version according to the given timestamp.
 */
static int
__rts_btree_abort_ondisk_kv(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip, uint64_t recno,
  WT_ITEM *row_key, WT_CELL_UNPACK_KV *vpack, wt_timestamp_t rollback_timestamp,
  bool *is_ondisk_stable)
{
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(key_string);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_UPDATE *upd;
    uint8_t *memp;
    char time_string[WT_TIME_STRING_SIZE];
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
              WT_RTS_VERB_TAG_HS_ABORT_STOP
              "history store update aborted with start_durable/commit_timestamp=%s, %s, "
              "stop_durable/commit_timestamp=%s, %s and stable_timestamp=%s",
              __wt_timestamp_to_string(vpack->tw.durable_start_ts, ts_string[0]),
              __wt_timestamp_to_string(vpack->tw.start_ts, ts_string[1]),
              __wt_timestamp_to_string(vpack->tw.durable_stop_ts, ts_string[2]),
              __wt_timestamp_to_string(vpack->tw.stop_ts, ts_string[3]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[4]));
            WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
            WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_sweep_hs_keys);
        } else
            return (0);
    } else if (vpack->tw.durable_start_ts > rollback_timestamp ||
      !__wt_rts_visibility_txn_visible_id(session, vpack->tw.start_txn) ||
      (!WT_TIME_WINDOW_HAS_STOP(&vpack->tw) && prepared)) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
          WT_RTS_VERB_TAG_ONDISK_ABORT_TW
          "on-disk update aborted with time_window=%s. "
          "Start durable_timestamp > stable_timestamp: %s, or txnid_not_visible=%s, "
          "or tw_has_no_stop_and_is_prepared=%s",
          __wt_time_point_to_string(
            vpack->tw.start_ts, vpack->tw.durable_start_ts, vpack->tw.start_txn, time_string),
          vpack->tw.durable_start_ts > rollback_timestamp ? "true" : "false",
          !__wt_rts_visibility_txn_visible_id(session, vpack->tw.start_txn) ? "true" : "false",
          !WT_TIME_WINDOW_HAS_STOP(&vpack->tw) && prepared ? "true" : "false");
        if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
            return (__rts_btree_ondisk_fixup_key(
              session, ref, rip, recno, row_key, vpack, rollback_timestamp));
        else {
            /*
             * In-memory database don't have a history store to provide a stable update, so remove
             * the key. Note that an in-memory database will have saved old values in the update
             * chain, so we should only get here for a key/value that never existed at all as of the
             * rollback timestamp; thus, deleting it is the correct response.
             */
            WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
            WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
        }
    } else if (WT_TIME_WINDOW_HAS_STOP(&vpack->tw) &&
      (vpack->tw.durable_stop_ts > rollback_timestamp ||
        !__wt_rts_visibility_txn_visible_id(session, vpack->tw.stop_txn) || prepared)) {
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
                return (__rts_btree_ondisk_fixup_key(
                  session, ref, rip, recno, row_key, vpack, rollback_timestamp));
            else {
                /*
                 * In-memory database don't have a history store to provide a stable update, so
                 * remove the key.
                 */
                WT_RET(__wt_upd_alloc_tombstone(session, &upd, NULL));
                WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_keys_removed);
            }
        } else {
            /*
             * Clear the remove operation from the key by inserting the original on-disk value as a
             * standard update.
             */
            WT_RET(__wt_scr_alloc(session, 0, &tmp));
            if ((ret = __wt_page_cell_data_ref_kv(session, page, vpack, tmp)) == 0)
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
            WT_RTS_STAT_CONN_DATA_INCR(session, txn_rts_keys_restored);
            __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
              WT_RTS_VERB_TAG_KEY_CLEAR_REMOVE
              "key restored with commit_timestamp=%s, durable_timestamp=%s, stable_timestamp=%s, "
              "txnid=%" PRIu64
              " and removed commit_timestamp=%s, durable_timestamp=%s, txnid=%" PRIu64
              ", prepared=%s",
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
    } else {
        /* Manufacture a column key. */
        WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));
        memp = key->mem;
        WT_ERR(__wt_vpack_uint(&memp, 0, recno));
        key->size = WT_PTRDIFF(memp, key->data);
    }

    WT_ERR(__wt_scr_alloc(session, 0, &key_string));
    __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_2,
      WT_RTS_VERB_TAG_ONDISK_KV_REMOVE "removing the key, tombstone=%s, key=%s",
      upd->type == WT_UPDATE_TOMBSTONE ? "true" : "false",
      __wt_key_string(session, key->data, key->size, S2BT(session)->key_format, key_string));

    if (rip != NULL)
        WT_ERR(__rts_btree_row_modify(session, ref, upd, key));
    else
        WT_ERR(__rts_btree_col_modify(session, ref, upd, recno));

    if (S2C(session)->rts->dryrun) {
err:
        __wt_free(session, upd);
    }
    if (rip == NULL || row_key == NULL)
        __wt_scr_free(session, &key);
    __wt_scr_free(session, &key_string);
    return (ret);
}

/*
 * __rts_btree_abort_col_var --
 *     Abort updates on a variable length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static int
__rts_btree_abort_col_var(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
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
            WT_RET(__rts_btree_abort_insert_list(
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
                            WT_RET(__rts_btree_abort_ondisk_kv(session, ref, NULL, recno + j, NULL,
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
                        if (recno + j == ins_recno &&
                          __wt_rts_visibility_has_stable_update(ins->upd))
                            j++;
                    }
                }
                /* Process the rest of the keys. */
                while (j < rle) {
                    WT_RET(__rts_btree_abort_ondisk_kv(session, ref, NULL, recno + j, NULL, &unpack,
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
        WT_RET(__rts_btree_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

    return (0);
}

/*
 * __rts_btree_abort_col_fix_one --
 *     Handle one possibly unstable on-disk time window.
 */
static int
__rts_btree_abort_col_fix_one(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t tw,
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

    return (__rts_btree_abort_ondisk_kv(session, ref, NULL, page->dsk->recno + recno_offset, NULL,
      &unpack, rollback_timestamp, NULL));
}

/*
 * __rts_btree_abort_col_fix --
 *     Abort updates on a fixed length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static int
__rts_btree_abort_col_fix(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT *ins;
    WT_INSERT_HEAD *inshead;
    WT_PAGE *page;
    uint32_t ins_recno_offset, recno_offset, numtws, tw;

    page = ref->page;
    WT_ASSERT(session, page != NULL);

    /*
     * Review the changes to the original on-page data items. Note that while this can report back
     * to us whether it saw a stable update, that information doesn't do us any good -- unlike in
     * VLCS where the uniformity of cells lets us reason about the timestamps of all of them based
     * on the timestamp of an update to any of them, in FLCS everything is just thrown together, so
     * we'll need to iterate over all the keys anyway.
     */
    if ((inshead = WT_COL_UPDATE_SINGLE(page)) != NULL)
        WT_RET(__rts_btree_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

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

                WT_RET(__rts_btree_abort_col_fix_one(
                  session, ref, tw, recno_offset, rollback_timestamp));
                tw++;
            }
            /* If this key has a stable update, skip over it. */
            if (tw < numtws && page->pg_fix_tws[tw].recno_offset == ins_recno_offset &&
              ins->upd != NULL && __wt_rts_visibility_has_stable_update(ins->upd))
                tw++;
        }
    }
    /* Process the rest of the keys with time windows. */
    while (tw < numtws) {
        recno_offset = page->pg_fix_tws[tw].recno_offset;
        WT_RET(__rts_btree_abort_col_fix_one(session, ref, tw, recno_offset, rollback_timestamp));
        tw++;
    }

    /* Review the append list. */
    if ((inshead = WT_COL_APPEND(page)) != NULL)
        WT_RET(__rts_btree_abort_insert_list(session, page, inshead, rollback_timestamp, NULL));

    return (0);
}

/*
 * __rts_btree_abort_row_leaf --
 *     Abort updates on a row leaf page with timestamps newer than the rollback timestamp.
 */
static int
__rts_btree_abort_row_leaf(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
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
        WT_ERR(__rts_btree_abort_insert_list(session, page, insert, rollback_timestamp, NULL));

    /*
     * Review updates that belong to keys that are on the disk image, as well as for keys inserted
     * since the page was read from disk.
     */
    WT_ROW_FOREACH (page, rip, i) {
        stable_update_found = false;
        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
            WT_ERR(__rts_btree_abort_update(
              session, key, upd, rollback_timestamp, &stable_update_found));
            have_key = true;
        } else
            have_key = false;

        if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
            WT_ERR(__rts_btree_abort_insert_list(session, page, insert, rollback_timestamp, NULL));

        /*
         * If there is no stable update found in the update list, abort any on-disk value.
         */
        if (!stable_update_found) {
            vpack = &_vpack;
            __wt_row_leaf_value_cell(session, page, rip, vpack);
            WT_ERR(__rts_btree_abort_ondisk_kv(
              session, ref, rip, 0, have_key ? key : NULL, vpack, rollback_timestamp, NULL));
        }
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __wt_rts_btree_abort_updates --
 *     Abort updates on this page newer than the timestamp.
 */
int
__wt_rts_btree_abort_updates(
  WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_PAGE *page;
    bool dryrun, modified;

    dryrun = S2C(session)->rts->dryrun;

    /*
     * If we have a ref with clean page, find out whether the page has any modifications that are
     * newer than the given timestamp. As eviction writes the newest version to page, even a clean
     * page may also contain modifications that need rollback.
     */
    page = ref->page;
    modified = __wt_page_is_modified(page);
    if (!modified && !__wt_rts_visibility_page_needs_abort(session, ref, rollback_timestamp)) {
        __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_3,
          WT_RTS_VERB_TAG_SKIP_UNMODIFIED "ref=%p: unmodified stable page skipped", (void *)ref);
        return (0);
    }

    WT_STAT_CONN_INCR(session, txn_rts_pages_visited);
    __wt_verbose_level_multi(session, WT_VERB_RECOVERY_RTS(session), WT_VERBOSE_DEBUG_2,
      WT_RTS_VERB_TAG_PAGE_ROLLBACK "rolling back page, addr=%p modified=%s", (void *)ref,
      modified ? "true" : "false");

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        WT_RET(__rts_btree_abort_col_fix(session, ref, rollback_timestamp));
        break;
    case WT_PAGE_COL_VAR:
        WT_RET(__rts_btree_abort_col_var(session, ref, rollback_timestamp));
        break;
    case WT_PAGE_ROW_LEAF:
        WT_RET(__rts_btree_abort_row_leaf(session, ref, rollback_timestamp));
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
    if (!dryrun && page->modify)
        __wt_page_modify_set(session, page);
    return (0);
}
