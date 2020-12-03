
/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curhs_prev_visible(WT_SESSION_IMPL *, WT_CURSOR_HS *);
static int __curhs_next_visible(WT_SESSION_IMPL *, WT_CURSOR_HS *);

/*
 * __hs_cursor_open_int --
 *     Open a new history store table cursor, internal function.
 */
static int
__hs_cursor_open_int(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_HS_URI, NULL, open_cursor_cfg, &cursor));
    WT_RET(ret);

    /* History store cursors should always ignore tombstones. */
    F_SET(cursor, WT_CURSTD_IGNORE_TOMBSTONE);

    *cursorp = cursor;
    return (0);
}

/*
 * __wt_hs_cursor_cache --
 *     Cache a new history store table cursor. Open and then close a history store cursor without
 *     saving it in the session.
 */
int
__wt_hs_cursor_cache(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;

    conn = S2C(session);

    /*
     * Make sure this session has a cached history store cursor, otherwise we can deadlock with a
     * session wanting exclusive access to a handle: that session will have a handle list write lock
     * and will be waiting on eviction to drain, we'll be inside eviction waiting on a handle list
     * read lock to open a history store cursor.
     *
     * The test for the no-reconciliation flag is necessary because the session may already be doing
     * history store operations and if we open/close the existing history store cursor, we can
     * affect those already-running history store operations by changing the cursor state. When
     * doing history store operations, we set the no-reconciliation flag, use it as short-hand to
     * avoid that problem. This doesn't open up the window for the deadlock because setting the
     * no-reconciliation flag limits eviction to in-memory splits.
     *
     * The test for the connection's default session is because there are known problems with using
     * cached cursors from the default session. The metadata does not have history store content and
     * is commonly handled specially. We won't need a history store cursor if we are evicting
     * metadata.
     *
     * FIXME-WT-6037: This isn't reasonable and needs a better fix.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY) || F_ISSET(session, WT_SESSION_NO_RECONCILE) ||
      (session->dhandle != NULL && WT_IS_METADATA(S2BT(session)->dhandle)) ||
      session == conn->default_session)
        return (0);
    WT_RET(__hs_cursor_open_int(session, &cursor));
    WT_RET(cursor->close(cursor));
    return (0);
}

/*
 * __wt_hs_cursor_open --
 *     Open a new history store table cursor wrapper function.
 */
int
__wt_hs_cursor_open(WT_SESSION_IMPL *session)
{
    /* Not allowed to open a cursor if you already have one */
    WT_ASSERT(session, session->hs_cursor == NULL);

    return (__hs_cursor_open_int(session, &session->hs_cursor));
}

/*
 * __wt_hs_cursor_close --
 *     Discard a history store cursor.
 */
int
__wt_hs_cursor_close(WT_SESSION_IMPL *session)
{
    /* Should only be called when session has an open history store cursor */
    WT_ASSERT(session, session->hs_cursor != NULL);

    WT_RET(session->hs_cursor->close(session->hs_cursor));
    session->hs_cursor = NULL;
    return (0);
}

/*
 * __wt_hs_cursor_next --
 *     Execute a next operation on a history store cursor with the appropriate isolation level.
 */
int
__wt_hs_cursor_next(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = cursor->next(cursor));
    return (ret);
}

/*
 * __wt_hs_cursor_prev --
 *     Execute a prev operation on a history store cursor with the appropriate isolation level.
 */
int
__wt_hs_cursor_prev(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = cursor->prev(cursor));
    return (ret);
}

/*
 * __wt_hs_cursor_search_near --
 *     Execute a search near operation on a history store cursor with the appropriate isolation
 *     level.
 */
int
__wt_hs_cursor_search_near(WT_SESSION_IMPL *session, WT_CURSOR *cursor, int *exactp)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(
      session, WT_ISO_READ_UNCOMMITTED, ret = cursor->search_near(cursor, exactp));
    return (ret);
}

/*
 * __curhs_next --
 *     WT_CURSOR->next method for the hs cursor type.
 */
static int
__curhs_next(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, next, CUR2BT(file_cursor));

    WT_ERR(__wt_hs_cursor_next(session, file_cursor));
    /*
     * We need to check if the history store record is visible to the current session. If not, the
     * __curhs_next_visible() will also keep iterating forward through the records until it finds a
     * visible record or bail out if records stop satisfying the fields set in cursor.
     */
    WT_ERR(__curhs_next_visible(session, hs_cursor));

err:
    API_END_RET(session, ret);
}

/*
 * __curhs_prev --
 *     WT_CURSOR->prev method for the hs cursor type.
 */
static int
__curhs_prev(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, prev, CUR2BT(file_cursor));

    WT_ERR(__wt_hs_cursor_prev(session, file_cursor));
    /*
     * We need to check if the history store record is visible to the current session. If not, the
     * __curhs_prev_visible() will also keep iterating backwards through the records until it finds
     * a visible record or bail out if records stop satisfying the fields set in cursor.
     */
    WT_ERR(__curhs_prev_visible(session, hs_cursor));

err:
    API_END_RET(session, ret);
}

/*
 * __curhs_close --
 *     WT_CURSOR->close method for the hs cursor type.
 */
static int
__curhs_close(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_ITEM *datastore_key;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(
      cursor, session, close, file_cursor == NULL ? NULL : CUR2BT(file_cursor));
err:
    if (file_cursor != NULL)
        WT_TRET(file_cursor->close(file_cursor));
    datastore_key = &hs_cursor->datastore_key;
    __wt_scr_free(session, &datastore_key);
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __curhs_reset --
 *     Reset a history store cursor.
 */
static int
__curhs_reset(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, CUR2BT(file_cursor));

    ret = file_cursor->reset(file_cursor);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);
    hs_cursor->btree_id = 0;
    hs_cursor->datastore_key.data = NULL;
    hs_cursor->datastore_key.size = 0;
    hs_cursor->flags = 0;

err:
    API_END_RET(session, ret);
}

/*
 * __curhs_set_key --
 *     WT_CURSOR->set_key method for the hs cursor type.
 */
static void
__curhs_set_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_ITEM *datastore_key;
    WT_SESSION_IMPL *session;
    wt_timestamp_t start_ts;
    uint64_t counter;
    uint32_t arg_count;
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    session = CUR2S(cursor);
    start_ts = WT_TS_NONE;
    counter = 0;

    va_start(ap, cursor);
    arg_count = va_arg(ap, uint32_t);

    WT_ASSERT(session, arg_count >= 1 && arg_count <= 4);

    hs_cursor->btree_id = va_arg(ap, uint32_t);
    F_SET(hs_cursor, WT_HS_CUR_BTREE_ID_SET);
    if (arg_count > 1) {
        datastore_key = va_arg(ap, WT_ITEM *);
        WT_IGNORE_RET(__wt_buf_set(
          session, &hs_cursor->datastore_key, datastore_key->data, datastore_key->size));
        F_SET(hs_cursor, WT_HS_CUR_KEY_SET);
    } else {
        hs_cursor->datastore_key.data = NULL;
        hs_cursor->datastore_key.size = 0;
        F_CLR(hs_cursor, WT_HS_CUR_KEY_SET);
    }

    if (arg_count > 2) {
        start_ts = va_arg(ap, wt_timestamp_t);
        F_SET(hs_cursor, WT_HS_CUR_TS_SET);
    } else
        F_CLR(hs_cursor, WT_HS_CUR_TS_SET);

    if (arg_count > 3) {
        counter = va_arg(ap, uint64_t);
        F_SET(hs_cursor, WT_HS_CUR_COUNTER_SET);
    } else
        F_CLR(hs_cursor, WT_HS_CUR_COUNTER_SET);

    va_end(ap);

    file_cursor->set_key(
      file_cursor, hs_cursor->btree_id, &hs_cursor->datastore_key, start_ts, counter);
}

/*
 * __curhs_prev_visible --
 *     Check the visibility of the current history store record. If it is not visible, find the
 *     previous visible history store record.
 */
static int
__curhs_prev_visible(WT_SESSION_IMPL *session, WT_CURSOR_HS *hs_cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR *std_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(datastore_key);
    WT_DECL_RET;
    wt_timestamp_t start_ts;
    uint64_t counter;
    uint32_t btree_id;
    int cmp;

    file_cursor = hs_cursor->file_cursor;
    std_cursor = (WT_CURSOR *)hs_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;

    WT_ERR(__wt_scr_alloc(session, 0, &datastore_key));

    for (; ret == 0; ret = __wt_hs_cursor_prev(session, file_cursor)) {
        WT_ERR(file_cursor->get_key(file_cursor, &btree_id, &datastore_key, &start_ts, &counter));

        /* Stop before crossing over to the next btree. */
        if (F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET) && btree_id != hs_cursor->btree_id) {
            ret = WT_NOTFOUND;
            goto done;
        }

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
            WT_ERR(__wt_compare(session, NULL, datastore_key, &hs_cursor->datastore_key, &cmp));
            if (cmp != 0) {
                ret = WT_NOTFOUND;
                goto done;
            }
        }

        /*
         * If the stop time pair on the tombstone in the history store is already globally visible
         * we can skip it.
         */
        if (__wt_txn_tw_stop_visible_all(session, &cbt->upd_value->tw)) {
            WT_STAT_CONN_INCR(session, cursor_prev_hs_tombstone);
            WT_STAT_DATA_INCR(session, cursor_prev_hs_tombstone);
            continue;
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record that
         * is not obsolete.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_COMMITTED))
            break;

        if (__wt_txn_tw_stop_visible(session, &cbt->upd_value->tw)) {
            /*
             * If the stop time point of a record is visible to us, we won't be able to see anything
             * for this entire key.
             */
            if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
                ret = WT_NOTFOUND;
                goto done;
            } else
                continue;
        }

        /* If the start time point is visible to us, let's return that record. */
        if (__wt_txn_tw_start_visible(session, &cbt->upd_value->tw))
            break;
    }

done:
err:
    __wt_scr_free(session, &datastore_key);
    return (ret);
}

/*
 * __curhs_next_visible --
 *     Check the visibility of the current history store record. If it is not visible, find the next
 *     visible history store record.
 */
static int
__curhs_next_visible(WT_SESSION_IMPL *session, WT_CURSOR_HS *hs_cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR *std_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(datastore_key);
    WT_DECL_RET;
    wt_timestamp_t start_ts;
    uint64_t counter;
    uint32_t btree_id;
    int cmp;

    file_cursor = hs_cursor->file_cursor;
    std_cursor = (WT_CURSOR *)hs_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;

    WT_ERR(__wt_scr_alloc(session, 0, &datastore_key));

    for (; ret == 0; ret = __wt_hs_cursor_next(session, file_cursor)) {
        WT_ERR(file_cursor->get_key(file_cursor, &btree_id, &datastore_key, &start_ts, &counter));

        /* Stop before crossing over to the next btree. */
        if (F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET) && btree_id != hs_cursor->btree_id) {
            ret = WT_NOTFOUND;
            goto done;
        }

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
            WT_ERR(__wt_compare(session, NULL, datastore_key, &hs_cursor->datastore_key, &cmp));
            if (cmp != 0) {
                ret = WT_NOTFOUND;
                goto done;
            }
        }

        /*
         * If the stop time pair on the tombstone in the history store is already globally visible
         * we can skip it.
         */
        if (__wt_txn_tw_stop_visible_all(session, &cbt->upd_value->tw)) {
            WT_STAT_CONN_INCR(session, cursor_next_hs_tombstone);
            WT_STAT_DATA_INCR(session, cursor_next_hs_tombstone);
            continue;
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record that
         * is not obsolete.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_COMMITTED))
            break;

        /*
         * If the stop time point of a record is visible to us, check the next one.
         */
        if (__wt_txn_tw_stop_visible(session, &cbt->upd_value->tw))
            continue;

        /* If the start time point is visible to us, let's return that record. */
        if (__wt_txn_tw_start_visible(session, &cbt->upd_value->tw))
            break;
    }

done:
err:
    __wt_scr_free(session, &datastore_key);
    return (ret);
}

/*
 * __curhs_search_near --
 *     WT_CURSOR->search_near method for the hs cursor type.
 */
static int
__curhs_search_near(WT_CURSOR *cursor, int *exactp)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int cmp;
    int exact;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    *exactp = 0;
    cmp = 0;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, search_near, CUR2BT(file_cursor));

    WT_ERR(__wt_scr_alloc(session, 0, &srch_key));
    /* At least we have the btree id set. */
    WT_ASSERT(session, F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET));
    WT_ERR(__wt_buf_set(session, srch_key, file_cursor->key.data, file_cursor->key.size));
    WT_ERR_NOTFOUND_OK(__wt_hs_cursor_search_near(session, file_cursor, &exact), true);

    /* Empty history store is fine. */
    if (ret == WT_NOTFOUND)
        goto done;

    /*
     * There are some key fields missing so we are searching a range of keys. Place the cursor at
     * the start of the range.
     */
    if (!F_ISSET(hs_cursor, WT_HS_CUR_COUNTER_SET)) {
        /*
         * If we raced with a history store insert, we may be two or more records away from our
         * target. Keep iterating forwards until we are on or past our target key.
         *
         * We can't use the cursor positioning helper that we use for regular reads since that will
         * place us at the end of a particular key/timestamp range whereas we want to be placed at
         * the beginning.
         */
        if (exact < 0) {
            while ((ret = __wt_hs_cursor_next(session, file_cursor)) == 0) {
                WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
                if (cmp >= 0)
                    break;
            }
            /* No entries greater than or equal to the key we searched for. */
            WT_ERR_NOTFOUND_OK(ret, true);
            if (ret == WT_NOTFOUND)
                goto done;

            *exactp = cmp;
        } else
            *exactp = 1;

        WT_ERR(__curhs_next_visible(session, hs_cursor));
    }
    /* Search the closest match that is smaller or equal to the search key. */
    else {
        /*
         * Because of the special visibility rules for the history store, a new key can appear in
         * between our search and the set of updates that we're interested in. Keep trying until we
         * find it.
         *
         * There may be no history store entries for the given btree id and record key if they have
         * been removed by rollback to stable.
         *
         * Note that we need to compare the raw key off the cursor to determine where we are in the
         * history store as opposed to comparing the embedded data store key since the ordering is
         * not guaranteed to be the same.
         */
        if (exact > 0) {
            /*
             * It's possible that we may race with a history store insert for another key. So we may
             * be more than one record away the end of our target key/timestamp range. Keep
             * iterating backwards until we land on our key.
             */
            while ((ret = __wt_hs_cursor_prev(session, file_cursor)) == 0) {
                WT_STAT_CONN_INCR(session, cursor_skip_hs_cur_position);
                WT_STAT_DATA_INCR(session, cursor_skip_hs_cur_position);

                WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
                if (cmp <= 0)
                    break;
            }

            *exactp = cmp;
        } else
            *exactp = -1;
#ifdef HAVE_DIAGNOSTIC
        if (ret == 0) {
            WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
            WT_ASSERT(session, cmp <= 0);
        }
#endif

        WT_ERR(__curhs_prev_visible(session, hs_cursor));
    }

done:
err:
    __wt_scr_free(session, &srch_key);
    API_END_RET(session, ret);
}

/*
 * __curhs_get_key --
 *     WT_CURSOR->get_key method for the hs cursor type.
 */
static int
__curhs_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;

    va_start(ap, cursor);
    ret = file_cursor->get_key(file_cursor, va_arg(ap, uint32_t *), va_arg(ap, WT_ITEM **),
      va_arg(ap, wt_timestamp_t *), va_arg(ap, uint64_t *));
    va_end(ap);

    return (ret);
}

/*
 * __curhs_get_value --
 *     WT_CURSOR->get_value method for the hs cursor type.
 */
static int
__curhs_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;

    va_start(ap, cursor);
    ret = file_cursor->get_value(file_cursor, va_arg(ap, wt_timestamp_t *),
      va_arg(ap, wt_timestamp_t *), va_arg(ap, uint64_t *), va_arg(ap, WT_ITEM **));
    va_end(ap);

    return (ret);
}

/*
 * __curhs_set_value --
 *     WT_CURSOR->set_value method for the hs cursor type.
 */
static void
__curhs_set_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    va_start(ap, cursor);
    hs_cursor->time_window = *va_arg(ap, WT_TIME_WINDOW *);

    file_cursor->set_value(file_cursor, va_arg(ap, wt_timestamp_t), va_arg(ap, wt_timestamp_t),
      va_arg(ap, uint64_t), va_arg(ap, WT_ITEM *));
    va_end(ap);
}

/*
 * __curhs_insert --
 *     WT_CURSOR->insert method for the hs cursor type.
 */
static int
__curhs_insert(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *hs_tombstone, *hs_upd;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    hs_tombstone = hs_upd = NULL;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, insert, CUR2BT(file_cursor));

    /* Allocate a tombstone only when there is a valid stop time point. */
    if (WT_TIME_WINDOW_HAS_STOP(&hs_cursor->time_window)) {
        /*
         * Insert a delete record to represent stop time point for the actual record to be inserted.
         * Set the stop time point as the commit time point of the history store delete record.
         */
        WT_ERR(__wt_upd_alloc_tombstone(session, &hs_tombstone, NULL));
        hs_tombstone->start_ts = hs_cursor->time_window.stop_ts;
        hs_tombstone->durable_ts = hs_cursor->time_window.durable_stop_ts;
        hs_tombstone->txnid = hs_cursor->time_window.stop_txn;
    }

    /*
     * Append to the delete record, the actual record to be inserted into the history store. Set the
     * current update start time point as the commit time point to the history store record.
     */
    WT_ERR(__wt_upd_alloc(session, &file_cursor->value, WT_UPDATE_STANDARD, &hs_upd, NULL));
    hs_upd->start_ts = hs_cursor->time_window.start_ts;
    hs_upd->durable_ts = hs_cursor->time_window.durable_start_ts;
    hs_upd->txnid = hs_cursor->time_window.start_txn;

    /* Insert the standard update as next update if there is a tombstone. */
    if (hs_tombstone != NULL) {
        hs_tombstone->next = hs_upd;
        hs_upd = hs_tombstone;
        hs_tombstone = NULL;
    }

retry:
    /* Search the page and insert the updates. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_hs_row_search(cbt, &file_cursor->key, true));
    WT_ERR(ret);
    ret = __wt_hs_modify(cbt, hs_upd);
    if (ret == WT_RESTART)
        goto retry;
    WT_ERR(ret);

    if (0) {
err:
        __wt_free(session, hs_tombstone);
        __wt_free(session, hs_upd);
    }

    API_END_RET(session, ret);
}

/*
 * __curhs_remove --
 *     WT_CURSOR->remove method for the hs cursor type.
 */
static int
__curhs_remove(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *hs_tombstone;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    hs_tombstone = NULL;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, insert, CUR2BT(file_cursor));

    /* Remove must be called with cursor positioned. */
    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));

    /*
     * Since we're using internal functions to modify the row structure, we need to manually set the
     * comparison to an exact match.
     */
    cbt->compare = 0;
    /* Add a tombstone with WT_TXN_NONE transaction id and WT_TS_NONE timestamps. */
    WT_ERR(__wt_upd_alloc_tombstone(session, &hs_tombstone, NULL));
    hs_tombstone->txnid = WT_TXN_NONE;
    hs_tombstone->start_ts = hs_tombstone->durable_ts = WT_TS_NONE;
    while ((ret = __wt_hs_modify(cbt, hs_tombstone)) == WT_RESTART) {
        WT_WITH_PAGE_INDEX(session, ret = __wt_hs_row_search(cbt, &file_cursor->key, false));
        WT_ERR(ret);
    }

    WT_ERR(ret);

    if (0) {
err:
        __wt_free(session, hs_tombstone);
    }

    API_END_RET(session, ret);
}

/*
 * __curhs_update --
 *     WT_CURSOR->update method for the hs cursor type.
 */
static int
__curhs_update(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_ITEM(hs_value);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *hs_tombstone, *hs_upd;

    uint64_t hs_upd_type;
    wt_timestamp_t hs_durable_ts, hs_stop_durable_ts;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    hs_tombstone = hs_upd = NULL;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, update, CUR2BT(file_cursor));

    /* We are assuming that the caller has already searched and found the key. */
    WT_ASSERT(
      session, F_ISSET(file_cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_INT));
    WT_ASSERT(session, F_ISSET(hs_cursor, WT_HS_CUR_COUNTER_SET | WT_HS_CUR_TS_SET));

    /*
     * Only valid scenario to update the history store is to add the stop timestamp. Any other case
     * should fail.
     */
    WT_ASSERT(session, !WT_TIME_WINDOW_IS_EMPTY(&hs_cursor->time_window));
    WT_ASSERT(session, WT_TIME_WINDOW_HAS_STOP(&hs_cursor->time_window));

    /*
     * Ideally we want to check if we are positioned on the newest value for user key. However, we
     * can't check if the timestamp was set to WT_TS_MAX when we searched for the key. We can can a
     * next() on cursor to confirm there is no newer value but that would disturb our cursor. A more
     * expensive method would be to search again and verify.
     */

    /* The tombstone to represent the stop time window. */
    WT_ERR(__wt_upd_alloc_tombstone(session, &hs_tombstone, NULL));
    hs_tombstone->start_ts = hs_cursor->time_window.stop_ts;
    hs_tombstone->durable_ts = hs_cursor->time_window.durable_stop_ts;
    hs_tombstone->txnid = hs_cursor->time_window.stop_txn;

    /* Modify the existing value with a new stop timestamp. */

    /* Allocate a buffer for the history store value. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    /* Retrieve the existing update value and stop timestamp. */
    WT_ERR(file_cursor->get_value(
      file_cursor, &hs_stop_durable_ts, &hs_durable_ts, &hs_upd_type, hs_value));
    WT_ASSERT(session, hs_stop_durable_ts == WT_TS_MAX);
    WT_ASSERT(session, (uint8_t)hs_upd_type == WT_UPDATE_STANDARD);

    /* Use set_value method to pack the new value. */
    file_cursor->set_value(
      file_cursor, hs_cursor->time_window.stop_ts, hs_durable_ts, hs_upd_type, hs_value);

    WT_ERR(__wt_upd_alloc(session, &file_cursor->value, WT_UPDATE_STANDARD, &hs_upd, NULL));
    hs_upd->start_ts = hs_cursor->time_window.start_ts;
    hs_upd->durable_ts = hs_cursor->time_window.durable_start_ts;
    hs_upd->txnid = hs_cursor->time_window.start_txn;

    /* Connect the tombstone to the update. */
    hs_tombstone->next = hs_upd;

    /* Insert the updates and if we fail, search and try again. */
    while ((ret = __wt_hs_modify(cbt, hs_tombstone)) == WT_RESTART) {
        WT_WITH_PAGE_INDEX(session, ret = __wt_hs_row_search(cbt, &file_cursor->key, true));
        WT_ERR(ret);
    }

    if (0) {
err:
        __wt_free(session, hs_tombstone);
        __wt_free(session, hs_upd);
        __wt_scr_free(session, &hs_value);
    }
    API_END_RET(session, ret);
}

/*
 * __wt_curhs_open --
 *     Initialize a history store cursor.
 */
int
__wt_curhs_open(WT_SESSION_IMPL *session, WT_CURSOR *owner, WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __curhs_get_key, /* get-key */
      __curhs_get_value,                          /* get-value */
      __curhs_set_key,                            /* set-key */
      __curhs_set_value,                          /* set-value */
      __wt_cursor_compare_notsup,                 /* compare */
      __wt_cursor_equals_notsup,                  /* equals */
      __curhs_next,                               /* next */
      __curhs_prev,                               /* prev */
      __curhs_reset,                              /* reset */
      __wt_cursor_notsup,                         /* search */
      __curhs_search_near,                        /* search-near */
      __curhs_insert,                             /* insert */
      __wt_cursor_modify_value_format_notsup,     /* modify */
      __curhs_update,                             /* update */
      __curhs_remove,                             /* remove */
      __wt_cursor_notsup,                         /* reserve */
      __wt_cursor_reconfigure_notsup,             /* reconfigure */
      __wt_cursor_notsup,                         /* cache */
      __wt_cursor_reopen_notsup,                  /* reopen */
      __curhs_close);                             /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_ITEM *datastore_key;

    WT_RET(__wt_calloc_one(session, &hs_cursor));
    cursor = (WT_CURSOR *)hs_cursor;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = WT_HS_KEY_FORMAT;
    cursor->value_format = WT_HS_VALUE_FORMAT;

    /* Open the file cursor for operations on the regular history store .*/
    WT_ERR(__hs_cursor_open_int(session, &hs_cursor->file_cursor));

    WT_ERR(__wt_cursor_init(cursor, WT_HS_URI, owner, NULL, cursorp));
    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);
    hs_cursor->btree_id = 0;
    datastore_key = &hs_cursor->datastore_key;
    WT_ERR(__wt_scr_alloc(session, 0, &datastore_key));
    hs_cursor->flags = 0;

    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);

    if (0) {
err:
        WT_TRET(__curhs_close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
