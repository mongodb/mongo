/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_hs_row_search --
 *     Search the history store for a given key and position the cursor on it.
 */
int
__wt_hs_row_search(WT_CURSOR_BTREE *hs_cbt, WT_ITEM *srch_key, bool insert)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_RET;
    bool leaf_found;

    hs_cursor = &hs_cbt->iface;
    leaf_found = false;

    /*
     * Check whether the search key can be find in the provided leaf page, if exists. Otherwise
     * perform a full search.
     */
    if (hs_cbt->ref != NULL) {
        WT_WITH_BTREE(CUR2S(hs_cbt), CUR2BT(hs_cbt),
          ret = __wt_row_search(hs_cbt, srch_key, insert, hs_cbt->ref, false, &leaf_found));
        WT_RET(ret);

        /*
         * Only use the pinned page search results if search returns an exact match or a slot other
         * than the page's boundary slots, if that's not the case, the record might belong on an
         * entirely different page.
         */
        if (leaf_found &&
          (hs_cbt->compare != 0 &&
            (hs_cbt->slot == 0 || hs_cbt->slot == hs_cbt->ref->page->entries - 1)))
            leaf_found = false;
        if (!leaf_found)
            hs_cursor->reset(hs_cursor);
    }

    if (!leaf_found)
        WT_WITH_BTREE(CUR2S(hs_cbt), CUR2BT(hs_cbt),
          ret = __wt_row_search(hs_cbt, srch_key, insert, NULL, false, NULL));

#ifdef HAVE_DIAGNOSTIC
    WT_TRET(__wt_cursor_key_order_init(hs_cbt));
#endif
    return (ret);
}

/*
 * __wt_hs_modify --
 *     Make an update to the history store.
 *
 * History store updates don't use transactions as those updates should be immediately visible and
 *     don't follow normal transaction semantics. For this reason, history store updates are
 *     directly modified using the low level api instead of the ordinary cursor api.
 */
int
__wt_hs_modify(WT_CURSOR_BTREE *hs_cbt, WT_UPDATE *hs_upd)
{
    WT_DECL_RET;

    /*
     * We don't have exclusive access to the history store page so we need to pass "false" here to
     * ensure that we're locking when inserting new keys to an insert list.
     */
    WT_WITH_BTREE(CUR2S(hs_cbt), CUR2BT(hs_cbt),
      ret = __wt_row_modify(hs_cbt, &hs_cbt->iface.key, NULL, hs_upd, WT_UPDATE_INVALID, false));
    return (ret);
}

/*
 * __hs_cursor_position_int --
 *     Internal function to position a history store cursor at the end of a set of updates for a
 *     given btree id, record key and timestamp.
 */
static int
__hs_cursor_position_int(WT_SESSION_IMPL *session, WT_CURSOR *cursor, uint32_t btree_id,
  const WT_ITEM *key, wt_timestamp_t timestamp, WT_ITEM *user_srch_key)
{
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    int cmp, exact;

    if (user_srch_key == NULL)
        WT_RET(__wt_scr_alloc(session, 0, &srch_key));
    else
        srch_key = user_srch_key;

    /*
     * Because of the special visibility rules for the history store, a new key can appear in
     * between our search and the set of updates that we're interested in. Keep trying until we find
     * it.
     *
     * There may be no history store entries for the given btree id and record key if they have been
     * removed by WT_CONNECTION::rollback_to_stable.
     *
     * Note that we need to compare the raw key off the cursor to determine where we are in the
     * history store as opposed to comparing the embedded data store key since the ordering is not
     * guaranteed to be the same.
     */
    cursor->set_key(cursor, btree_id, key, timestamp, UINT64_MAX);
    /* Copy the raw key before searching as a basis for comparison. */
    WT_ERR(__wt_buf_set(session, srch_key, cursor->key.data, cursor->key.size));
    WT_ERR(cursor->search_near(cursor, &exact));
    if (exact > 0) {
        /*
         * It's possible that we may race with a history store insert for another key. So we may be
         * more than one record away the end of our target key/timestamp range. Keep iterating
         * backwards until we land on our key.
         */
        while ((ret = cursor->prev(cursor)) == 0) {
            WT_STAT_CONN_INCR(session, cursor_skip_hs_cur_position);
            WT_STAT_DATA_INCR(session, cursor_skip_hs_cur_position);

            WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
            if (cmp <= 0)
                break;
        }
    }
#ifdef HAVE_DIAGNOSTIC
    if (ret == 0) {
        WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
        WT_ASSERT(session, cmp <= 0);
    }
#endif
err:
    if (user_srch_key == NULL)
        __wt_scr_free(session, &srch_key);
    return (ret);
}

/*
 * __wt_hs_cursor_position --
 *     Position a history store cursor at the end of a set of updates for a given btree id, record
 *     key and timestamp. There may be no history store entries for the given btree id and record
 *     key if they have been removed by WT_CONNECTION::rollback_to_stable. There is an optional
 *     argument to store the key that we used to position the cursor which can be used to assess
 *     where the cursor is relative to it. The function executes with isolation level set as
 *     WT_ISO_READ_UNCOMMITTED.
 */
int
__wt_hs_cursor_position(WT_SESSION_IMPL *session, WT_CURSOR *cursor, uint32_t btree_id,
  const WT_ITEM *key, wt_timestamp_t timestamp, WT_ITEM *user_srch_key)
{
    WT_DECL_RET;
    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED,
      ret = __hs_cursor_position_int(session, cursor, btree_id, key, timestamp, user_srch_key));
    return (ret);
}

/*
 * __wt_hs_find_upd --
 *     Scan the history store for a record the btree cursor wants to position on. Create an update
 *     for the record and return to the caller. The caller may choose to optionally allow prepared
 *     updates to be returned regardless of whether prepare is being ignored globally. Otherwise, a
 *     prepare conflict will be returned upon reading a prepared update.
 */
int
__wt_hs_find_upd(WT_SESSION_IMPL *session, WT_ITEM *key, const char *value_format, uint64_t recno,
  WT_UPDATE_VALUE *upd_value, bool allow_prepare, WT_ITEM *on_disk_buf, WT_TIME_WINDOW *on_disk_tw)
{
    WT_CURSOR *hs_cursor;
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(orig_hs_value_buf);
    WT_DECL_RET;
    WT_ITEM hs_key, recno_key;
    WT_MODIFY_VECTOR modifies;
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;
    WT_UPDATE *mod_upd, *upd;
    wt_timestamp_t durable_timestamp, durable_timestamp_tmp, hs_start_ts, hs_start_ts_tmp;
    wt_timestamp_t hs_stop_durable_ts, hs_stop_durable_ts_tmp, read_timestamp;
    uint64_t hs_counter, hs_counter_tmp, upd_type_full;
    uint32_t hs_btree_id;
    uint8_t *p, recno_key_buf[WT_INTPACK64_MAXSIZE], upd_type;
    int cmp;
    bool modify;

    hs_cursor = NULL;
    mod_upd = upd = NULL;
    orig_hs_value_buf = NULL;
    WT_CLEAR(hs_key);
    __wt_modify_vector_init(session, &modifies);
    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);
    hs_btree_id = S2BT(session)->id;
    WT_NOT_READ(modify, false);

    WT_STAT_CONN_INCR(session, cursor_search_hs);
    WT_STAT_DATA_INCR(session, cursor_search_hs);

    /* Row-store key is as passed to us, create the column-store key as needed. */
    WT_ASSERT(
      session, (key == NULL && recno != WT_RECNO_OOB) || (key != NULL && recno == WT_RECNO_OOB));
    if (key == NULL) {
        p = recno_key_buf;
        WT_RET(__wt_vpack_uint(&p, 0, recno));
        memset(&recno_key, 0, sizeof(recno_key));
        key = &recno_key;
        key->data = recno_key_buf;
        key->size = WT_PTRDIFF(p, recno_key_buf);
    }

    /* Allocate buffer for the history store value. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    /* Open a history store table cursor. */
    WT_ERR(__wt_hs_cursor_open(session));
    hs_cursor = session->hs_cursor;
    hs_cbt = (WT_CURSOR_BTREE *)hs_cursor;

    /*
     * After positioning our cursor, we're stepping backwards to find the correct update. Since the
     * timestamp is part of the key, our cursor needs to go from the newest record (further in the
     * history store) to the oldest (earlier in the history store) for a given key.
     */
    read_timestamp = allow_prepare ? txn->prepare_timestamp : txn_shared->read_timestamp;

    /*
     * A reader without a timestamp should read the largest timestamp in the range, however cursor
     * search near if given a 0 timestamp will place at the top of the range and hide the records
     * below it. As such we need to adjust a 0 timestamp to the timestamp max value.
     */
    if (read_timestamp == WT_TS_NONE)
        read_timestamp = WT_TS_MAX;

    WT_ERR_NOTFOUND_OK(
      __wt_hs_cursor_position(session, hs_cursor, hs_btree_id, key, read_timestamp, NULL), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }
    for (;; ret = __wt_hs_cursor_prev(session, hs_cursor)) {
        WT_ERR_NOTFOUND_OK(ret, true);
        /* If we hit the end of the table, let's get out of here. */
        if (ret == WT_NOTFOUND) {
            ret = 0;
            goto done;
        }
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));

        /* Stop before crossing over to the next btree */
        if (hs_btree_id != S2BT(session)->id)
            goto done;

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        if (cmp != 0)
            goto done;

        /*
         * If the stop time pair on the tombstone in the history store is already globally visible
         * we can skip it.
         */
        if (__wt_txn_tw_stop_visible_all(session, &hs_cbt->upd_value->tw)) {
            WT_STAT_CONN_INCR(session, cursor_prev_hs_tombstone);
            WT_STAT_DATA_INCR(session, cursor_prev_hs_tombstone);
            continue;
        }
        /*
         * If the stop time point of a record is visible to us, we won't be able to see anything for
         * this entire key. Just jump straight to the end.
         */
        if (__wt_txn_tw_stop_visible(session, &hs_cbt->upd_value->tw))
            goto done;
        /* If the start time point is visible to us, let's return that record. */
        if (__wt_txn_tw_start_visible(session, &hs_cbt->upd_value->tw))
            break;
    }

    WT_ERR(hs_cursor->get_value(
      hs_cursor, &hs_stop_durable_ts, &durable_timestamp, &upd_type_full, hs_value));
    upd_type = (uint8_t)upd_type_full;

    /* We do not have tombstones in the history store anymore. */
    WT_ASSERT(session, upd_type != WT_UPDATE_TOMBSTONE);

    /*
     * If the caller has signalled they don't need the value buffer, don't bother reconstructing a
     * modify update or copying the contents into the value buffer.
     */
    if (upd_value->skip_buf)
        goto skip_buf;

    /*
     * Keep walking until we get a non-modify update. Once we get to that point, squash the updates
     * together.
     */
    if (upd_type == WT_UPDATE_MODIFY) {
        WT_NOT_READ(modify, true);
        /* Store this so that we don't have to make a special case for the first modify. */
        hs_stop_durable_ts_tmp = hs_stop_durable_ts;

        /*
         * Resolving update chains of reverse deltas requires the current transaction to look beyond
         * its current snapshot in certain scenarios. This flag allows us to ignore transaction
         * visibility checks when reading in order to construct the modify chain, so we can create
         * the value we expect.
         */
        while (upd_type == WT_UPDATE_MODIFY) {
            WT_ERR(__wt_upd_alloc(session, hs_value, upd_type, &mod_upd, NULL));
            WT_ERR(__wt_modify_vector_push(&modifies, mod_upd));
            mod_upd = NULL;

            /*
             * Find the base update to apply the reverse deltas. If our cursor next fails to find an
             * update here we fall back to the datastore version. If its timestamp doesn't match our
             * timestamp then we return not found.
             */
            WT_ERR_NOTFOUND_OK(__wt_hs_cursor_next(session, hs_cursor), true);
            if (ret == WT_NOTFOUND) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }
            hs_start_ts_tmp = WT_TS_NONE;
            /*
             * Make sure we use the temporary variants of these variables. We need to retain the
             * timestamps of the original modify we saw.
             *
             * We keep looking back into history store until we find a base update to apply the
             * reverse deltas on top of.
             */
            WT_ERR(hs_cursor->get_key(
              hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts_tmp, &hs_counter_tmp));

            if (hs_btree_id != S2BT(session)->id) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }

            WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));

            if (cmp != 0) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }

            /*
             * If we find a history store record that either corresponds to the on-disk value or is
             * newer than it then we should use the on-disk value as the base value and apply our
             * modifies on top of it.
             */
            if (on_disk_tw->start_ts < hs_start_ts_tmp ||
              (on_disk_tw->start_ts == hs_start_ts_tmp &&
                on_disk_tw->start_txn <= hs_cbt->upd_value->tw.start_txn)) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }

            WT_ERR(hs_cursor->get_value(hs_cursor, &hs_stop_durable_ts_tmp, &durable_timestamp_tmp,
              &upd_type_full, hs_value));
            upd_type = (uint8_t)upd_type_full;
        }
        WT_ASSERT(session, upd_type == WT_UPDATE_STANDARD);
        while (modifies.size > 0) {
            __wt_modify_vector_pop(&modifies, &mod_upd);
            WT_ERR(__wt_modify_apply_item(session, value_format, hs_value, mod_upd->data));
            __wt_free_update_list(session, &mod_upd);
            mod_upd = NULL;
        }
        WT_STAT_CONN_INCR(session, cache_hs_read_squash);
        WT_STAT_DATA_INCR(session, cache_hs_read_squash);
    }

    /*
     * Potential optimization: We can likely get rid of this copy and the update allocation above.
     * We already have buffers containing the modify values so there's no good reason to allocate an
     * update other than to work with our modify vector implementation.
     */
    WT_ERR(__wt_buf_set(session, &upd_value->buf, hs_value->data, hs_value->size));
skip_buf:
    upd_value->tw.durable_start_ts = durable_timestamp;
    upd_value->tw.start_txn = WT_TXN_NONE;
    upd_value->type = upd_type;

done:
err:
    if (orig_hs_value_buf != NULL)
        __wt_scr_free(session, &orig_hs_value_buf);
    else
        __wt_scr_free(session, &hs_value);
    WT_ASSERT(session, hs_key.mem == NULL && hs_key.memsize == 0);

    WT_TRET(__wt_hs_cursor_close(session));

    __wt_free_update_list(session, &mod_upd);
    while (modifies.size > 0) {
        __wt_modify_vector_pop(&modifies, &upd);
        __wt_free_update_list(session, &upd);
    }
    __wt_modify_vector_free(&modifies);

    if (ret == 0) {
        /* Couldn't find a record. */
        if (upd == NULL) {
            ret = WT_NOTFOUND;
            WT_STAT_CONN_INCR(session, cache_hs_read_miss);
            WT_STAT_DATA_INCR(session, cache_hs_read_miss);
        } else {
            WT_STAT_CONN_INCR(session, cache_hs_read);
            WT_STAT_DATA_INCR(session, cache_hs_read);
        }
    }

    WT_ASSERT(session, upd != NULL || ret != 0);

    return (ret);
}
