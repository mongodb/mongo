/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rollback_abort_newer_update --
 *     Abort updates in an update change with timestamps newer than the rollback timestamp. Also,
 *     clear the history store flag for the first stable update in the update.
 */
static void
__rollback_abort_newer_update(WT_SESSION_IMPL *session, WT_UPDATE *first_upd,
  wt_timestamp_t rollback_timestamp, bool *stable_update_found)
{
    WT_UPDATE *upd;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    *stable_update_found = false;
    for (upd = first_upd; upd != NULL; upd = upd->next) {
        /* Skip the updates that are aborted. */
        if (upd->txnid == WT_TXN_ABORTED) {
            if (upd == first_upd)
                first_upd = upd->next;
        } else if (rollback_timestamp < upd->durable_ts) {
            /*
             * If any updates are aborted, all newer updates better be aborted as well.
             *
             * Timestamp ordering relies on the validations at the time of commit. Thus if the table
             * is not configured for key consistency check, the timestamps could be out of order
             * here.
             */
            WT_ASSERT(session, !FLD_ISSET(S2BT(session)->assert_flags, WT_ASSERT_COMMIT_TS_KEYS) ||
                upd == first_upd);
            first_upd = upd->next;

            __wt_verbose(session, WT_VERB_RTS,
              "rollback to stable update aborted with txnid: %" PRIu64
              " durable timestamp: %s and stable timestamp: %s",
              upd->txnid, __wt_timestamp_to_string(upd->durable_ts, ts_string[0]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[1]));

            upd->txnid = WT_TXN_ABORTED;
            WT_STAT_CONN_INCR(session, txn_rts_upd_aborted);
            upd->durable_ts = upd->start_ts = WT_TS_NONE;
        } else {
            /* Valid update is found. */
            WT_ASSERT(session, first_upd == upd);
            break;
        }
    }

    /*
     * Clear the history store flag for the stable update to indicate that this update should not be
     * written into the history store later, when all the aborted updates are removed from the
     * history store. The next time when this update is moved into the history store, it will have a
     * different stop time pair.
     */
    if (first_upd != NULL) {
        F_CLR(first_upd, WT_UPDATE_HS);
        *stable_update_found = true;
    }
}

/*
 * __rollback_abort_newer_insert --
 *     Apply the update abort check to each entry in an insert skip list.
 */
static void
__rollback_abort_newer_insert(
  WT_SESSION_IMPL *session, WT_INSERT_HEAD *head, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT *ins;
    bool stable_update_found;

    WT_SKIP_FOREACH (ins, head)
        if (ins->upd != NULL)
            __rollback_abort_newer_update(
              session, ins->upd, rollback_timestamp, &stable_update_found);
}

/*
 * __rollback_row_add_update --
 *     Add the provided update to the head of the update list.
 */
static inline int
__rollback_row_add_update(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_UPDATE *upd)
{
    WT_DECL_RET;
    WT_PAGE_MODIFY *mod;
    WT_UPDATE *old_upd, **upd_entry;
    size_t upd_size;

    /* If we don't yet have a modify structure, we'll need one. */
    WT_RET(__wt_page_modify_init(session, page));
    mod = page->modify;

    /* Allocate an update array as necessary. */
    WT_PAGE_ALLOC_AND_SWAP(session, page, mod->mod_row_update, upd_entry, page->entries);

    /* Set the WT_UPDATE array reference. */
    upd_entry = &mod->mod_row_update[WT_ROW_SLOT(page, rip)];
    upd_size = __wt_update_list_memsize(upd);

    /*
     * If it's a full update list, we're trying to instantiate the row. Otherwise, it's just a
     * single update that we'd like to append to the update list.
     *
     * Set the "old" entry to the second update in the list so that the serialization function
     * succeeds in swapping the first update into place.
     */
    if (upd->next != NULL)
        *upd_entry = upd->next;
    old_upd = *upd_entry;

    /*
     * Point the new WT_UPDATE item to the next element in the list. The serialization function acts
     * as our memory barrier to flush this write.
     */
    upd->next = old_upd;

    /*
     * Serialize the update. Rollback to stable doesn't need to check the visibility of the on page
     * value to detect conflict.
     */
    WT_ERR(__wt_update_serial(session, NULL, page, upd_entry, &upd, upd_size, true));

err:
    return (ret);
}

/*
 * __rollback_row_ondisk_fixup_key --
 *     Abort updates in the history store and replace the on-disk value with an update that
 *     satisfies the given timestamp.
 */
static int
__rollback_row_ondisk_fixup_key(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip,
  wt_timestamp_t rollback_timestamp, bool replace)
{
    WT_CELL_UNPACK *unpack, _unpack;
    WT_CURSOR *hs_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(hs_key);
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_ITEM full_value;
    WT_UPDATE *hs_upd, *upd;
    wt_timestamp_t durable_ts, hs_start_ts, hs_stop_ts, newer_hs_ts;
    size_t size;
    uint64_t hs_counter, type_full;
    uint32_t hs_btree_id, session_flags;
    uint8_t type;
    int cmp;
    char ts_string[4][WT_TS_INT_STRING_SIZE];
    bool is_owner, valid_update_found;

    hs_cursor = NULL;
    hs_upd = upd = NULL;
    durable_ts = hs_start_ts = newer_hs_ts = WT_TS_NONE;
    hs_btree_id = S2BT(session)->id;
    session_flags = 0;
    is_owner = valid_update_found = false;

    /* Allocate buffers for the data store and history store key. */
    WT_RET(__wt_scr_alloc(session, 0, &key));
    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));

    /* Get the full update value from the data store. */
    WT_CLEAR(full_value);
    if (!__wt_row_leaf_value(page, rip, &full_value)) {
        unpack = &_unpack;
        __wt_row_leaf_value_cell(session, page, rip, NULL, unpack);
        WT_ERR(__wt_page_cell_data_ref(session, page, unpack, &full_value));
    }
    WT_ERR(__wt_buf_set(session, &full_value, full_value.data, full_value.size));

    /* Open a history store table cursor. */
    WT_ERR(__wt_hs_cursor(session, &session_flags, &is_owner));
    hs_cursor = session->hs_cursor;
    cbt = (WT_CURSOR_BTREE *)hs_cursor;

    /*
     * Scan the history store for the given btree and key with maximum start and stop time pair to
     * let the search point to the last version of the key and start traversing backwards to find
     * out the satisfying record according the given timestamp. Any satisfying history store record
     * is moved into data store and removed from history store. If none of the history store records
     * satisfy the given timestamp, the key is removed from data store.
     */
    ret = __wt_hs_cursor_position(session, hs_cursor, hs_btree_id, key, WT_TS_MAX);
    for (; ret == 0; ret = hs_cursor->prev(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

        /* Stop before crossing over to the next btree */
        if (hs_btree_id != S2BT(session)->id)
            break;

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        WT_ERR(__wt_compare(session, NULL, hs_key, key, &cmp));
        if (cmp != 0)
            break;

        /*
         * As part of the history store search, we never get an exact match based on our search
         * criteria as we always search for a maximum record for that key. Make sure that we set the
         * comparison result as an exact match to remove this key as part of rollback to stable. In
         * case if we don't mark the comparison result as same, later the __wt_row_modify function
         * will not properly remove the update from history store.
         */
        cbt->compare = 0;

        /* Get current value and convert to full update if it is a modify. */
        WT_ERR(hs_cursor->get_value(hs_cursor, &hs_stop_ts, &durable_ts, &type_full, hs_value));
        type = (uint8_t)type_full;
        if (type == WT_UPDATE_MODIFY)
            WT_ERR(__wt_modify_apply_item(session, &full_value, hs_value->data, false));
        else {
            WT_ASSERT(session, type == WT_UPDATE_STANDARD);
            WT_ERR(__wt_buf_set(session, &full_value, hs_value->data, hs_value->size));
        }

        /*
         * Verify the history store timestamps are in order. The start timestamp may be equal to the
         * stop timestamp if the original update's commit timestamp is out of order.
         */
        WT_ASSERT(session,
          (newer_hs_ts == WT_TS_NONE || hs_stop_ts <= newer_hs_ts || hs_start_ts == hs_stop_ts));

        /*
         * Stop processing when we find the newer version value of this key is stable according to
         * the current version stop timestamp. Also it confirms that history store doesn't contains
         * any newer version than the current version for the key.
         */
        if (hs_stop_ts <= rollback_timestamp) {
            __wt_verbose(session, WT_VERB_RTS,
              "history store update valid with stop timestamp: %s and stable timestamp: %s",
              __wt_timestamp_to_string(hs_stop_ts, ts_string[0]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[1]));
            break;
        }

        /* Stop processing when we find a stable update according to the given timestamp. */
        if (durable_ts <= rollback_timestamp) {
            __wt_verbose(session, WT_VERB_RTS,
              "history store update valid with start timestamp: %s, durable timestamp: %s, "
              "stop timestamp: %s and stable timestamp: %s",
              __wt_timestamp_to_string(hs_start_ts, ts_string[0]),
              __wt_timestamp_to_string(durable_ts, ts_string[1]),
              __wt_timestamp_to_string(hs_stop_ts, ts_string[2]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[3]));
            valid_update_found = true;
            break;
        }

        __wt_verbose(session, WT_VERB_RTS,
          "history store update aborted with start timestamp: %s, durable timestamp: %s, stop "
          "timestamp: %s and stable timestamp: %s",
          __wt_timestamp_to_string(hs_start_ts, ts_string[0]),
          __wt_timestamp_to_string(durable_ts, ts_string[1]),
          __wt_timestamp_to_string(hs_stop_ts, ts_string[2]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[3]));

        /*
         * Durable timestamp of the current record is used as stop timestamp of previous record.
         * Save it to verify against previous record.
         */
        newer_hs_ts = durable_ts;
        WT_ERR(__wt_upd_alloc_tombstone(session, &hs_upd));
        WT_ERR(__wt_hs_modify(cbt, hs_upd));
        WT_STAT_CONN_INCR(session, txn_rts_hs_removed);
        hs_upd = NULL;
    }

    if (replace) {
        /*
         * If we found a history value that satisfied the given timestamp, add it to the update
         * list. Otherwise remove the key by adding a tombstone.
         */
        if (valid_update_found) {
            WT_ERR(__wt_update_alloc(session, &full_value, &upd, &size, WT_UPDATE_STANDARD));

            upd->txnid = WT_TXN_NONE;
            upd->durable_ts = durable_ts;
            upd->start_ts = hs_start_ts;
            __wt_verbose(session, WT_VERB_RTS, "update restored from history store (txnid: %" PRIu64
                                               ", start_ts: %s, durable_ts: %s",
              upd->txnid, __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
              __wt_timestamp_to_string(upd->durable_ts, ts_string[1]));

            /*
             * Set the flag to indicate that this update has been restored from history store for
             * the rollback to stable operation.
             */
            F_SET(upd, WT_UPDATE_RESTORED_FOR_ROLLBACK);
        } else {
            WT_ERR(__wt_upd_alloc_tombstone(session, &upd));
            WT_STAT_CONN_INCR(session, txn_rts_keys_removed);
            __wt_verbose(session, WT_VERB_RTS, "%p: key removed", (void *)key);
        }

        WT_ERR(__rollback_row_add_update(session, page, rip, upd));
        upd = NULL;
    }

    /* Finally remove that update from history store. */
    if (valid_update_found) {
        WT_ERR(__wt_upd_alloc_tombstone(session, &hs_upd));
        WT_ERR(__wt_hs_modify(cbt, hs_upd));
        WT_STAT_CONN_INCR(session, txn_rts_hs_removed);
        hs_upd = NULL;
    }

err:
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &hs_key);
    __wt_scr_free(session, &hs_value);
    __wt_buf_free(session, &full_value);
    __wt_free(session, hs_upd);
    __wt_free(session, upd);
    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));

    return (ret);
}

/*
 * __rollback_abort_row_ondisk_kv --
 *     Fix the on-disk row K/V version according to the given timestamp.
 */
static int
__rollback_abort_row_ondisk_kv(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, wt_timestamp_t rollback_timestamp)
{
    WT_CELL_UNPACK *vpack, _vpack;
    WT_DECL_RET;
    WT_ITEM buf;
    WT_UPDATE *upd;
    size_t size;
    char ts_string[3][WT_TS_INT_STRING_SIZE];

    vpack = &_vpack;
    upd = NULL;
    __wt_row_leaf_value_cell(session, page, rip, NULL, vpack);
    if (vpack->durable_start_ts > rollback_timestamp) {
        __wt_verbose(session, WT_VERB_RTS,
          "on-disk update aborted with start durable timestamp: %s, commit timestamp: %s and "
          "stable timestamp: %s",
          __wt_timestamp_to_string(vpack->durable_start_ts, ts_string[0]),
          __wt_timestamp_to_string(vpack->start_ts, ts_string[1]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[2]));
        if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
            return (__rollback_row_ondisk_fixup_key(session, page, rip, rollback_timestamp, true));
        else {
            /*
             * In-memory database don't have a history store to provide a stable update, so remove
             * the key.
             */
            WT_RET(__wt_upd_alloc_tombstone(session, &upd));
            WT_STAT_CONN_INCR(session, txn_rts_keys_removed);
        }
    } else if (vpack->durable_stop_ts != WT_TS_NONE &&
      vpack->durable_stop_ts > rollback_timestamp) {
        /*
         * Clear the remove operation from the key by inserting the original on-disk value as a
         * standard update.
         */
        WT_CLEAR(buf);

        /*
         * If a value is simple(no compression), and is globally visible at the time of reading a
         * page into cache, we encode its location into the WT_ROW.
         */
        if (!__wt_row_leaf_value(page, rip, &buf))
            /* Take the value from the original page cell. */
            WT_RET(__wt_page_cell_data_ref(session, page, vpack, &buf));

        WT_RET(__wt_update_alloc(session, &buf, &upd, &size, WT_UPDATE_STANDARD));
        upd->txnid = vpack->start_txn;
        upd->durable_ts = vpack->durable_start_ts;
        upd->start_ts = vpack->start_ts;
        WT_STAT_CONN_INCR(session, txn_rts_keys_restored);
        __wt_verbose(session, WT_VERB_RTS,
          "key restored (txnid: %" PRIu64 ", start_ts: %s, durable_ts: %s", upd->txnid,
          __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
          __wt_timestamp_to_string(upd->durable_ts, ts_string[1]));
    } else
        /* Stable version according to the timestamp. */
        return (0);

    WT_ERR(__rollback_row_add_update(session, page, rip, upd));
    return (0);

err:
    __wt_free(session, upd);
    return (ret);
}

/*
 * __rollback_abort_newer_col_var --
 *     Abort updates on a variable length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static void
__rollback_abort_newer_col_var(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_COL *cip;
    WT_INSERT_HEAD *ins;
    uint32_t i;

    /* Review the changes to the original on-page data items */
    WT_COL_FOREACH (page, cip, i)
        if ((ins = WT_COL_UPDATE(page, cip)) != NULL)
            __rollback_abort_newer_insert(session, ins, rollback_timestamp);

    /* Review the append list */
    if ((ins = WT_COL_APPEND(page)) != NULL)
        __rollback_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __rollback_abort_newer_col_fix --
 *     Abort updates on a fixed length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static void
__rollback_abort_newer_col_fix(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT_HEAD *ins;

    /* Review the changes to the original on-page data items */
    if ((ins = WT_COL_UPDATE_SINGLE(page)) != NULL)
        __rollback_abort_newer_insert(session, ins, rollback_timestamp);

    /* Review the append list */
    if ((ins = WT_COL_APPEND(page)) != NULL)
        __rollback_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __rollback_abort_row_reconciled_page_internal --
 *     Abort updates on a history store using the in-memory build reconciled page of data store.
 */
static int
__rollback_abort_row_reconciled_page_internal(WT_SESSION_IMPL *session, const void *image,
  const uint8_t *addr, size_t addr_size, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_ITEM tmp;
    WT_PAGE *mod_page;
    WT_ROW *rip;
    uint32_t i, page_flags;
    const void *image_local;

    /*
     * Don't pass an allocated buffer to the underlying block read function, force allocation of new
     * memory of the appropriate size.
     */
    WT_CLEAR(tmp);

    mod_page = NULL;
    image_local = image;

    if (image_local == NULL) {
        WT_RET(__wt_bt_read(session, &tmp, addr, addr_size));
        image_local = tmp.data;
    }

    /* Don't free the passed image later. */
    page_flags = image != NULL ? 0 : WT_PAGE_DISK_ALLOC;
    WT_ERR(__wt_page_inmem(session, NULL, image_local, page_flags, &mod_page));
    tmp.mem = NULL;
    WT_ROW_FOREACH (mod_page, rip, i)
        WT_ERR_NOTFOUND_OK(
          __rollback_row_ondisk_fixup_key(session, mod_page, rip, rollback_timestamp, false),
          false);

err:
    if (mod_page != NULL)
        __wt_page_out(session, &mod_page);
    __wt_buf_free(session, &tmp);

    return (ret);
}

/*
 * __rollback_abort_row_reconciled_page --
 *     Abort updates on a history store using the reconciled pages of data store.
 */
static int
__rollback_abort_row_reconciled_page(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t multi_entry;
    char ts_string[3][WT_TS_INT_STRING_SIZE];

    if ((mod = page->modify) == NULL)
        return (0);

    if (mod->rec_result == WT_PM_REC_REPLACE &&
      (mod->mod_replace.newest_start_durable_ts > rollback_timestamp ||
          mod->mod_replace.newest_stop_durable_ts > rollback_timestamp)) {
        __wt_verbose(session, WT_VERB_RTS,
          "reconciled replace block page history store update removal On-disk with start "
          "durable timestamp: %s, stop durable timestamp: %s and stable timestamp: %s",
          __wt_timestamp_to_string(mod->mod_replace.newest_start_durable_ts, ts_string[0]),
          __wt_timestamp_to_string(mod->mod_replace.newest_stop_durable_ts, ts_string[1]),
          __wt_timestamp_to_string(rollback_timestamp, ts_string[2]));

        WT_RET(__rollback_abort_row_reconciled_page_internal(session, mod->u1.r.disk_image,
          mod->u1.r.replace.addr, mod->u1.r.replace.size, rollback_timestamp));

        /*
         * As this page has newer aborts that are aborted, make sure to mark the page as dirty to
         * let the reconciliation happens again on the page. Otherwise, the eviction may pick the
         * already reconciled page to write to disk with newer updates.
         */
        __wt_page_modify_set(session, page);
    } else if (mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        for (multi = mod->mod_multi, multi_entry = 0; multi_entry < mod->mod_multi_entries;
             ++multi, ++multi_entry)
            if (multi->addr.newest_start_durable_ts > rollback_timestamp ||
              multi->addr.newest_stop_durable_ts > rollback_timestamp) {
                __wt_verbose(session, WT_VERB_RTS,
                  "reconciled multi block page history store update removal On-disk with "
                  "start durable timestamp: %s, stop durable timestamp: %s and stable "
                  "timestamp: %s",
                  __wt_timestamp_to_string(multi->addr.newest_start_durable_ts, ts_string[0]),
                  __wt_timestamp_to_string(multi->addr.newest_stop_durable_ts, ts_string[1]),
                  __wt_timestamp_to_string(rollback_timestamp, ts_string[2]));

                WT_RET(__rollback_abort_row_reconciled_page_internal(session, multi->disk_image,
                  multi->addr.addr, multi->addr.size, rollback_timestamp));

                /*
                 * As this page has newer aborts that are aborted, make sure to mark the page as
                 * dirty to let the reconciliation happens again on the page. Otherwise, the
                 * eviction may pick the already reconciled page to write to disk with newer
                 * updates.
                 */
                __wt_page_modify_set(session, page);
            }
    }

    return (0);
}

/*
 * __rollback_abort_newer_row_leaf --
 *     Abort updates on a row leaf page with timestamps newer than the rollback timestamp.
 */
static int
__rollback_abort_newer_row_leaf(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT_HEAD *insert;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint32_t i;
    bool stable_update_found;

    /*
     * Review the insert list for keys before the first entry on the disk page.
     */
    if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
        __rollback_abort_newer_insert(session, insert, rollback_timestamp);

    /*
     * Review updates that belong to keys that are on the disk image, as well as for keys inserted
     * since the page was read from disk.
     */
    WT_ROW_FOREACH (page, rip, i) {
        stable_update_found = false;
        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
            __rollback_abort_newer_update(session, upd, rollback_timestamp, &stable_update_found);

        if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
            __rollback_abort_newer_insert(session, insert, rollback_timestamp);

        /*
         * If there is no stable update found in the update list, abort any on-disk value.
         */
        if (!stable_update_found)
            WT_RET(__rollback_abort_row_ondisk_kv(session, page, rip, rollback_timestamp));
    }

    /*
     * If the configuration is not in-memory, abort history store updates from the reconciled pages
     * of data store.
     */
    if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        WT_RET(__rollback_abort_row_reconciled_page(session, page, rollback_timestamp));
    return (0);
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
    WT_CELL_UNPACK vpack;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    wt_timestamp_t durable_ts;
    uint32_t i;
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *tag;
    bool result;

    addr = ref->addr;
    mod = ref->page == NULL ? NULL : ref->page->modify;
    durable_ts = WT_TS_NONE;
    tag = "undefined state";
    result = false;

    /*
     * The rollback operation should be performed on this page when any one of the following is
     * greater than the given timestamp:
     * 1. The reconciled replace page max durable timestamp.
     * 2. The reconciled multi page max durable timestamp.
     * 3. The on page address max durable timestamp.
     * 4. The off page address max durable timestamp.
     */
    if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE) {
        tag = "reconciled replace block";
        durable_ts =
          WT_MAX(mod->mod_replace.newest_start_durable_ts, mod->mod_replace.newest_stop_durable_ts);
        result = (durable_ts > rollback_timestamp);
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        tag = "reconciled multi block";
        /* Calculate the max durable timestamp by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            durable_ts = WT_MAX(durable_ts, multi->addr.newest_start_durable_ts);
            durable_ts = WT_MAX(durable_ts, multi->addr.newest_stop_durable_ts);
        }
        result = (durable_ts > rollback_timestamp);
    } else if (!__wt_off_page(ref->home, addr)) {
        tag = "on page cell";
        /* Check if the page is obsolete using the page disk address. */
        __wt_cell_unpack(session, ref->home, (WT_CELL *)addr, &vpack);
        durable_ts = WT_MAX(vpack.newest_start_durable_ts, vpack.newest_stop_durable_ts);
        result = (durable_ts > rollback_timestamp);
    } else if (addr != NULL) {
        tag = "address";
        durable_ts = WT_MAX(addr->newest_start_durable_ts, addr->newest_stop_durable_ts);
        result = (durable_ts > rollback_timestamp);
    }

    __wt_verbose(session, WT_VERB_RTS, "%p: page with %s durable timestamp: %s", (void *)ref, tag,
      __wt_timestamp_to_string(durable_ts, ts_string));

    return (result);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __rollback_verify_ondisk_page --
 *     Verify the on-disk page that it doesn't have updates newer than the timestamp.
 */
static void
__rollback_verify_ondisk_page(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_CELL_UNPACK *vpack, _vpack;
    WT_ROW *rip;
    uint32_t i;

    vpack = &_vpack;

    /* Review updates that belong to keys that are on the disk image. */
    WT_ROW_FOREACH (page, rip, i) {
        __wt_row_leaf_value_cell(session, page, rip, NULL, vpack);
        WT_ASSERT(session, vpack->start_ts <= rollback_timestamp);
        if (vpack->stop_ts != WT_TS_MAX)
            WT_ASSERT(session, vpack->stop_ts <= rollback_timestamp);
    }
}
#endif

/*
 * __rollback_abort_newer_updates --
 *     Abort updates on this page newer than the timestamp.
 */
static int
__rollback_abort_newer_updates(
  WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_PAGE *page;
    bool local_read;

    local_read = false;

    /* Review deleted page saved to the ref. */
    if (ref->page_del != NULL && rollback_timestamp < ref->page_del->durable_timestamp) {
        __wt_verbose(session, WT_VERB_RTS, "%p: deleted page rolled back", (void *)ref);
        WT_RET(__wt_delete_page_rollback(session, ref));
    }

    /*
     * If we have a ref with no page, or the page is clean, find out whether the page has any
     * modifications that are newer than the given timestamp. As eviction writes the newest version
     * to page, even a clean page may also contain modifications that need rollback. Such pages are
     * read back into memory and processed like other modified pages.
     */
    if ((page = ref->page) == NULL || !__wt_page_is_modified(page)) {
        if (!__rollback_page_needs_abort(session, ref, rollback_timestamp)) {
            __wt_verbose(session, WT_VERB_RTS, "%p: page skipped", (void *)ref);
#ifdef HAVE_DIAGNOSTIC
            if (ref->page == NULL && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY)) {
                WT_RET(__wt_page_in(session, ref, 0));
                __rollback_verify_ondisk_page(session, ref->page, rollback_timestamp);
                WT_TRET(__wt_page_release(session, ref, 0));
            }
#endif
            return (0);
        }

        /* Page needs rollback, read it into cache. */
        if (page == NULL) {
            WT_RET(__wt_page_in(session, ref, 0));
            local_read = true;
        }
        page = ref->page;
    }
    WT_STAT_CONN_INCR(session, txn_rts_pages_visited);
    __wt_verbose(session, WT_VERB_RTS, "%p: page rolled back when page is modified: %s",
      (void *)ref, __wt_page_is_modified(page) ? "true" : "false");

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        __rollback_abort_newer_col_fix(session, page, rollback_timestamp);
        break;
    case WT_PAGE_COL_VAR:
        __rollback_abort_newer_col_var(session, page, rollback_timestamp);
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        /*
         * There is nothing to do for internal pages, since we aren't rolling back far enough to
         * potentially include reconciled changes - and thus won't need to roll back structure
         * changes on internal pages.
         */
        break;
    case WT_PAGE_ROW_LEAF:
        WT_ERR(__rollback_abort_newer_row_leaf(session, page, rollback_timestamp));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, page->type));
    }

err:
    if (local_read)
        WT_TRET(__wt_page_release(session, ref, 0));
    return (ret);
}

/*
 * __rollback_to_stable_btree_walk --
 *     Called for each open handle - choose to either skip or wipe the commits
 */
static int
__rollback_to_stable_btree_walk(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_REF *child_ref, *ref;

    /* Walk the tree, marking commits aborted where appropriate. */
    ref = NULL;
    while ((ret = __wt_tree_walk(
              session, &ref, WT_READ_CACHE_LEAF | WT_READ_NO_EVICT | WT_READ_WONT_NEED)) == 0 &&
      ref != NULL)
        if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
            WT_INTL_FOREACH_BEGIN (session, ref->page, child_ref) {
                WT_RET(__rollback_abort_newer_updates(session, child_ref, rollback_timestamp));
            }
            WT_INTL_FOREACH_END;
        }

    return (ret);
}

/*
 * __rollback_eviction_drain --
 *     Wait for eviction to drain from a tree.
 */
static int
__rollback_eviction_drain(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_UNUSED(cfg);

    WT_RET(__wt_evict_file_exclusive_on(session));
    __wt_evict_file_exclusive_off(session);
    return (0);
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
    WT_DECL_RET;

    btree = S2BT(session);
    conn = S2C(session);

    __wt_verbose(session, WT_VERB_RTS,
      "rollback to stable connection logging enabled: %s and btree logging enabled: %s",
      FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) ? "true" : "false",
      !F_ISSET(btree, WT_BTREE_NO_LOGGING) ? "true" : "false");

    /*
     * Immediately durable files don't get their commits wiped. This case mostly exists to support
     * the semantic required for the oplog in MongoDB - updates that have been made to the oplog
     * should not be aborted. It also wouldn't be safe to roll back updates for any table that had
     * it's records logged, since those updates would be recovered after a crash making them
     * inconsistent.
     */
    if (__wt_btree_immediately_durable(session)) {
        if (btree->id >= conn->stable_rollback_maxfile)
            WT_PANIC_RET(session, EINVAL, "btree file ID %" PRIu32 " larger than max %" PRIu32,
              btree->id, conn->stable_rollback_maxfile);
        return (0);
    }

    /* There is never anything to do for checkpoint handles */
    if (session->dhandle->checkpoint != NULL)
        return (0);

    /* There is nothing to do on an empty tree. */
    if (btree->root.page == NULL)
        return (0);
    /*
     * Ensure the eviction server is out of the file - we don't want it messing with us. This step
     * shouldn't be required, but it simplifies some of the reasoning about what state trees can be
     * in.
     */
    WT_RET(__wt_evict_file_exclusive_on(session));
    WT_WITH_PAGE_INDEX(session, ret = __rollback_to_stable_btree_walk(session, rollback_timestamp));
    __wt_evict_file_exclusive_off(session);

    return (ret);
}

/*
 * __rollback_to_stable_check --
 *     Ensure the rollback request is reasonable.
 */
static int
__rollback_to_stable_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    bool txn_active;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    if (!txn_global->has_stable_timestamp)
        WT_RET_MSG(session, EINVAL, "rollback_to_stable requires a stable timestamp");

    /*
     * Help the user comply with the requirement that there are no concurrent operations. Protect
     * against spurious conflicts with the sweep server: we exclude it from running concurrent with
     * rolling back the history store contents.
     */
    ret = __wt_txn_activity_check(session, &txn_active);
#ifdef HAVE_DIAGNOSTIC
    if (txn_active)
        WT_TRET(__wt_verbose_dump_txn(session));
#endif

    if (ret == 0 && txn_active)
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
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    WT_ITEM key;
    WT_UPDATE *hs_upd;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id, session_flags;
    int exact;
    char ts_string[WT_TS_INT_STRING_SIZE];
    bool is_owner;

    hs_cursor = NULL;
    WT_CLEAR(key);
    hs_upd = NULL;
    session_flags = 0;
    is_owner = false;

    WT_RET(__wt_scr_alloc(session, 0, &hs_key));

    /* Open a history store table cursor. */
    WT_ERR(__wt_hs_cursor(session, &session_flags, &is_owner));
    hs_cursor = session->hs_cursor;
    cbt = (WT_CURSOR_BTREE *)hs_cursor;

    /* Walk the history store for the given btree. */
    hs_cursor->set_key(hs_cursor, btree_id, &key, WT_TS_NONE, 0);
    ret = hs_cursor->search_near(hs_cursor, &exact);

    /*
     * The search should always end up pointing to the start of the required btree or end of the
     * previous btree on success. Move the cursor based on the result.
     */
    WT_ASSERT(session, (ret != 0 || exact != 0));
    if (ret == 0 && exact < 0)
        ret = hs_cursor->next(hs_cursor);

    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

        /* Stop crossing into the next btree boundary. */
        if (btree_id != hs_btree_id)
            break;

        /* Set this comparison as exact match of the search for later use. */
        cbt->compare = 0;
        __wt_verbose(session, WT_VERB_RTS,
          "rollback to stable history store cleanup of update with start timestamp: %s",
          __wt_timestamp_to_string(hs_start_ts, ts_string));

        WT_ERR(__wt_upd_alloc_tombstone(session, &hs_upd));
        WT_ERR(__wt_hs_modify(cbt, hs_upd));
        WT_STAT_CONN_INCR(session, txn_rts_hs_removed);
        hs_upd = NULL;
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &hs_key);
    __wt_free(session, hs_upd);
    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));

    return (ret);
}

/*
 * __rollback_to_stable_btree_apply --
 *     Perform rollback to stable to all files listed in the metadata, apart from the metadata and
 *     history store files.
 */
static int
__rollback_to_stable_btree_apply(WT_SESSION_IMPL *session)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, durableval, key;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t max_durable_ts, start_durable_ts, stop_durable_ts, rollback_timestamp;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    const char *config, *uri;
    bool durable_ts_found;

    txn_global = &S2C(session)->txn_global;

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    WT_ORDERED_READ(rollback_timestamp, txn_global->stable_timestamp);
    __wt_verbose(session, WT_VERB_RTS,
      "performing rollback to stable with stable timestamp: %s and oldest timestamp: %s",
      __wt_timestamp_to_string(rollback_timestamp, ts_string[0]),
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[1]));

    WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SCHEMA));
    WT_RET(__wt_metadata_cursor(session, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &uri));

        /* Ignore metadata and history store files. */
        if (strcmp(uri, WT_METAFILE_URI) == 0 || strcmp(uri, WT_HS_URI) == 0)
            continue;

        if (!WT_PREFIX_MATCH(uri, "file:"))
            continue;

        WT_ERR(cursor->get_value(cursor, &config));

        /* Find out the max durable timestamp of the object from checkpoint. */
        start_durable_ts = stop_durable_ts = WT_TS_NONE;
        durable_ts_found = false;
        WT_ERR(__wt_config_getones(session, config, "checkpoint", &cval));
        __wt_config_subinit(session, &ckptconf, &cval);
        for (; __wt_config_next(&ckptconf, &key, &cval) == 0;) {
            ret = __wt_config_subgets(session, &cval, "start_durable_ts", &durableval);
            if (ret == 0) {
                start_durable_ts = WT_MAX(start_durable_ts, (wt_timestamp_t)durableval.val);
                durable_ts_found = true;
            }
            WT_ERR_NOTFOUND_OK(ret, false);
            ret = __wt_config_subgets(session, &cval, "stop_durable_ts", &durableval);
            if (ret == 0) {
                stop_durable_ts = WT_MAX(stop_durable_ts, (wt_timestamp_t)durableval.val);
                durable_ts_found = true;
            }
            WT_ERR_NOTFOUND_OK(ret, false);
        }
        max_durable_ts = WT_MAX(start_durable_ts, stop_durable_ts);
        ret = __wt_session_get_dhandle(session, uri, NULL, NULL, 0);
        /* Ignore performing rollback to stable on files that don't exist. */
        if (ret == ENOENT)
            continue;
        WT_ERR(ret);

        /*
         * The rollback operation should be performed on this file based on the following:
         * 1. The tree is modified.
         * 2. The checkpoint durable start/stop timestamp is greater than the rollback timestamp.
         * 3. There is no durable timestamp in any checkpoint.
         */
        if (S2BT(session)->modified || max_durable_ts > rollback_timestamp || !durable_ts_found) {
            __wt_verbose(session, WT_VERB_RTS,
              "tree rolled back with durable timestamp: %s, or when tree is modified: %s or "
              "when durable time is not found: %s",
              __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
              S2BT(session)->modified ? "true" : "false", !durable_ts_found ? "true" : "false");
            WT_TRET(__rollback_to_stable_btree(session, rollback_timestamp));
        } else
            __wt_verbose(session, WT_VERB_RTS,
              "tree skipped with durable timestamp: %s and stable timestamp: %s",
              __wt_timestamp_to_string(max_durable_ts, ts_string[0]),
              __wt_timestamp_to_string(rollback_timestamp, ts_string[1]));

        /*
         * Truncate history store entries for the non-timestamped table.
         * Exceptions:
         * 1. Modified tree - Scenarios where the tree is never checkpointed lead to zero
         * durable timestamp even they are timestamped tables. Until we have a special indication
         * of letting to know the table type other than checking checkpointed durable timestamp
         * to WT_TS_NONE, We need this exception.
         * 2. In-memory database - In this scenario, there is no history store to truncate.
         */
        if (!S2BT(session)->modified && max_durable_ts == WT_TS_NONE &&
          !F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
            WT_TRET(__rollback_to_stable_btree_hs_truncate(session, S2BT(session)->id));

        WT_TRET(__wt_session_release_dhandle(session));
        WT_ERR(ret);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __rollback_to_stable --
 *     Rollback all modifications with timestamps more recent than the passed in timestamp.
 */
static int
__rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Mark that a rollback operation is in progress and wait for eviction to drain. */
    WT_RET(__wt_conn_btree_apply(session, NULL, __rollback_eviction_drain, NULL, cfg));

    WT_RET(__rollback_to_stable_check(session));

    /*
     * Allocate a non-durable btree bitstring. We increment the global value before using it, so the
     * current value is already in use, and hence we need to add one here.
     */
    conn->stable_rollback_maxfile = conn->next_file_id + 1;
    WT_WITH_SCHEMA_LOCK(session, ret = __rollback_to_stable_btree_apply(session));

    return (ret);
}

/*
 * __wt_rollback_to_stable --
 *     Rollback all modifications with timestamps more recent than the passed in timestamp.
 */
int
__wt_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[], bool no_ckpt)
{
    WT_DECL_RET;

    /*
     * Don't use the connection's default session: we are working on data handles and (a) don't want
     * to cache all of them forever, plus (b) can't guarantee that no other method will be called
     * concurrently. Copy parent session no logging option to the internal session to make sure that
     * rollback to stable doesn't generate log records.
     */
    WT_RET(__wt_open_internal_session(S2C(session), "txn rollback_to_stable", true,
      F_MASK(session, WT_SESSION_NO_LOGGING), &session));

    /*
     * Rollback to stable should ignore tombstones in the history store since it needs to scan the
     * entire table sequentially.
     */
    F_SET(session, WT_SESSION_ROLLBACK_TO_STABLE | WT_SESSION_IGNORE_HS_TOMBSTONE);
    ret = __rollback_to_stable(session, cfg);
    F_CLR(session, WT_SESSION_ROLLBACK_TO_STABLE | WT_SESSION_IGNORE_HS_TOMBSTONE);
    WT_RET(ret);

    /*
     * If the configuration is not in-memory, forcibly log a checkpoint after rollback to stable to
     * ensure that both in-memory and on-disk versions are the same unless caller requested for no
     * checkpoint.
     */
    if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY) && !no_ckpt)
        WT_TRET(session->iface.checkpoint(&session->iface, "force=1"));
    WT_TRET(session->iface.close(&session->iface, NULL));

    return (ret);
}
