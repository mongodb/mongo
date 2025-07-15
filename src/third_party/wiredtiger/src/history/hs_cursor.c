/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
      ret = __wt_row_modify(
        hs_cbt, &hs_cbt->iface.key, NULL, &hs_upd, WT_UPDATE_INVALID, false, false));
    return (ret);
}

/*
 * __wt_hs_upd_time_window --
 *     Get the underlying time window of the update history store cursor is positioned at.
 */
void
__wt_hs_upd_time_window(WT_CURSOR *hs_cursor, WT_TIME_WINDOW **twp)
{
    WT_CURSOR_BTREE *hs_cbt;

    hs_cbt = __wt_curhs_get_cbt(hs_cursor);
    *twp = &hs_cbt->upd_value->tw;
}

/*
 * __wt_hs_find_upd --
 *     Scan the history store for a record the btree cursor wants to position on. Create an update
 *     for the record and return to the caller.
 */
int
__wt_hs_find_upd(WT_SESSION_IMPL *session, uint32_t btree_id, WT_ITEM *key,
  const char *value_format, uint64_t recno, WT_UPDATE_VALUE *upd_value, WT_ITEM *base_value_buf)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(orig_hs_value_buf);
    WT_DECL_RET;
    WT_ITEM hs_key, recno_key;
    WT_TXN_SHARED *txn_shared;
    WT_UPDATE *mod_upd;
    WT_UPDATE_VECTOR modifies;
    wt_timestamp_t durable_timestamp, durable_timestamp_tmp;
    wt_timestamp_t hs_stop_durable_ts, hs_stop_durable_ts_tmp, read_timestamp;
    size_t max_memsize;
    uint64_t upd_type_full;
    uint8_t *p, recno_key_buf[WT_INTPACK64_MAXSIZE], upd_type;
    bool upd_found;

    hs_cursor = NULL;
    mod_upd = NULL;
    orig_hs_value_buf = NULL;
    WT_CLEAR(hs_key);
    __wt_update_vector_init(session, &modifies);
    txn_shared = WT_SESSION_TXN_SHARED(session);
    upd_found = false;

    WT_STAT_CONN_DSRC_INCR(session, cursor_search_hs);

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

    /*
     * If reading from a checkpoint, it is possible to get here because the history store is
     * currently open, but not be able to get a cursor because there was no history store in the
     * checkpoint. We know this is the case if there's no history store checkpoint name stashed in
     * the session. In this case, behave the same as if we searched and found nothing. Otherwise, we
     * should be able to open a cursor on the selected checkpoint; if we fail because it's somehow
     * disappeared, that's a problem and we shouldn't just silently return no data.
     */
    if (WT_READING_CHECKPOINT(session) && session->hs_checkpoint == NULL) {
        ret = 0;
        goto done;
    }

    WT_ERR_NOTFOUND_OK(__wt_curhs_open(session, btree_id, NULL, &hs_cursor), true);
    /* Do this separately for now because the behavior below is confusing if it triggers. */
    WT_ASSERT(session, ret != WT_NOTFOUND);
    WT_ERR(ret);

    /*
     * After positioning our cursor, we're stepping backwards to find the correct update. Since the
     * timestamp is part of the key, our cursor needs to go from the newest record (further in the
     * history store) to the oldest (earlier in the history store) for a given key.
     *
     * A reader without a timestamp should read the largest timestamp in the range, however cursor
     * search near if given a 0 timestamp will place at the top of the range and hide the records
     * below it. As such we need to adjust a 0 timestamp to the timestamp max value.
     *
     * If reading a checkpoint, use the checkpoint read timestamp instead.
     */
    read_timestamp = WT_READING_CHECKPOINT(session) ? session->txn->checkpoint_read_timestamp :
                                                      txn_shared->read_timestamp;
    read_timestamp = read_timestamp == WT_TS_NONE ? WT_TS_MAX : read_timestamp;

    hs_cursor->set_key(hs_cursor, 4, btree_id, key, read_timestamp, UINT64_MAX);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_before(session, hs_cursor), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }

    /* Allocate buffer for the history store value. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));
    WT_ERR(hs_cursor->get_value(
      hs_cursor, &hs_stop_durable_ts, &durable_timestamp, &upd_type_full, hs_value));
    upd_type = (uint8_t)upd_type_full;

    /* We do not have tombstones in the history store anymore. */
    WT_ASSERT(session, upd_type != WT_UPDATE_TOMBSTONE);

    upd_found = true;

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
        /* Store this so that we don't have to make a special case for the first modify. */
        hs_stop_durable_ts_tmp = hs_stop_durable_ts;

        /*
         * Resolving update chains of reverse deltas requires the current transaction to look beyond
         * its current snapshot in certain scenarios. This flag allows us to ignore transaction
         * visibility checks when reading in order to construct the modify chain, so we can create
         * the value we expect.
         */
        F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

        while (upd_type == WT_UPDATE_MODIFY) {
            WT_ERR(__wt_upd_alloc(session, hs_value, upd_type, &mod_upd, NULL));
            WT_ERR(__wt_update_vector_push(&modifies, mod_upd));
            mod_upd = NULL;

            /*
             * Find the base update to apply the reverse deltas. If our cursor next fails to find an
             * update here we fall back to the datastore version. If its timestamp doesn't match our
             * timestamp then we return not found.
             */
            WT_ERR_NOTFOUND_OK(hs_cursor->next(hs_cursor), true);
            if (ret == WT_NOTFOUND) {
                /*
                 * Fallback to the provided value as the base value.
                 *
                 * Work around of clang analyzer complaining the value is never read as it is reset
                 * again by the following WT_ERR macro.
                 */
                WT_NOT_READ(ret, 0);
                orig_hs_value_buf = hs_value;
                hs_value = base_value_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }

            WT_ERR(hs_cursor->get_value(hs_cursor, &hs_stop_durable_ts_tmp, &durable_timestamp_tmp,
              &upd_type_full, hs_value));
            upd_type = (uint8_t)upd_type_full;
        }
        WT_ASSERT(session, upd_type == WT_UPDATE_STANDARD);

        if (modifies.size > 0) {
            __wt_modifies_max_memsize(&modifies, value_format, hs_value->size, &max_memsize);
            WT_ERR(__wt_buf_set_and_grow(
              session, hs_value, hs_value->data, hs_value->size, max_memsize));
        }
        while (modifies.size > 0) {
            __wt_update_vector_pop(&modifies, &mod_upd);
            WT_ERR(__wt_modify_apply_item(session, value_format, hs_value, mod_upd->data));
            __wt_free_update_list(session, &mod_upd);
        }
        WT_STAT_CONN_DSRC_INCR(session, cache_hs_read_squash);
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

    __wt_free_update_list(session, &mod_upd);
    while (modifies.size > 0) {
        __wt_update_vector_pop(&modifies, &mod_upd);
        __wt_free_update_list(session, &mod_upd);
    }
    __wt_update_vector_free(&modifies);

    if (ret == 0) {
        if (upd_found)
            WT_STAT_CONN_DSRC_INCR(session, cache_hs_read);
        else {
            upd_value->type = WT_UPDATE_INVALID;
            WT_STAT_CONN_DSRC_INCR(session, cache_hs_read_miss);
        }
    }

    /* Mark the buffer as invalid if there is an error. */
    if (ret != 0)
        upd_value->type = WT_UPDATE_INVALID;

    WT_ASSERT(session, ret != WT_NOTFOUND);

    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));

    return (ret);
}

/*
 * __wt_hs_btree_truncate --
 *     Wipe all history store updates for the btree.
 */
int
__wt_hs_btree_truncate(WT_SESSION_IMPL *session, uint32_t btree_id)
{
    WT_CURSOR *hs_cursor_start, *hs_cursor_stop;
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    WT_SESSION *truncate_session;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;

    hs_cursor_start = hs_cursor_stop = NULL;
    hs_btree_id = 0;
    truncate_session = (WT_SESSION *)session;

    WT_RET(__wt_scr_alloc(session, 0, &hs_key));

    /* Open a history store start cursor. */
    WT_ERR(__wt_curhs_open(session, btree_id, NULL, &hs_cursor_start));
    F_SET(hs_cursor_start, WT_CURSTD_HS_READ_COMMITTED);

    hs_cursor_start->set_key(hs_cursor_start, 1, btree_id);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor_start), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }

    /* Open a history store stop cursor. */
    WT_ERR(__wt_curhs_open(session, btree_id, NULL, &hs_cursor_stop));
    F_SET(hs_cursor_stop, WT_CURSTD_HS_READ_COMMITTED | WT_CURSTD_HS_READ_ACROSS_BTREE);

    hs_cursor_stop->set_key(hs_cursor_stop, 1, btree_id + 1);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor_stop), true);

#ifdef HAVE_DIAGNOSTIC
    /* If we get not found, we are at the largest btree id in the history store. */
    if (ret == 0) {
        hs_cursor_stop->get_key(hs_cursor_stop, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter);
        WT_ASSERT(session, hs_btree_id > btree_id);
    }
#endif

    do {
        WT_ASSERT(session, ret == WT_NOTFOUND || hs_btree_id > btree_id);

        WT_ERR_NOTFOUND_OK(hs_cursor_stop->prev(hs_cursor_stop), true);
        /* We can find the start point then we must be able to find the stop point. */
        if (ret == WT_NOTFOUND)
            WT_ERR_PANIC(
              session, ret, "cannot locate the stop point to truncate the history store.");
        hs_cursor_stop->get_key(hs_cursor_stop, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter);
    } while (hs_btree_id != btree_id);

    WT_ERR(
      truncate_session->truncate(truncate_session, NULL, hs_cursor_start, hs_cursor_stop, NULL));

done:
err:
    __wt_scr_free(session, &hs_key);
    if (hs_cursor_start != NULL)
        WT_TRET(hs_cursor_start->close(hs_cursor_start));
    if (hs_cursor_stop != NULL)
        WT_TRET(hs_cursor_stop->close(hs_cursor_stop));

    return (ret);
}
