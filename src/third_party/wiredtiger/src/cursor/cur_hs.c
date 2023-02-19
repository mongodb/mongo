/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curhs_file_cursor_next(WT_SESSION_IMPL *, WT_CURSOR *);
static int __curhs_file_cursor_open(WT_SESSION_IMPL *, WT_CURSOR *, WT_CURSOR **);
static int __curhs_file_cursor_prev(WT_SESSION_IMPL *, WT_CURSOR *);
static int __curhs_file_cursor_search_near(WT_SESSION_IMPL *, WT_CURSOR *, int *);
static int __curhs_prev_visible(WT_SESSION_IMPL *, WT_CURSOR_HS *);
static int __curhs_next_visible(WT_SESSION_IMPL *, WT_CURSOR_HS *);
static int __curhs_search_near_helper(WT_SESSION_IMPL *, WT_CURSOR *, bool);
/*
 * __curhs_file_cursor_open --
 *     Open a new history store table cursor, internal function.
 */
static int
__curhs_file_cursor_open(WT_SESSION_IMPL *session, WT_CURSOR *owner, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    size_t len;
    char *tmp;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL};

    if (WT_READING_CHECKPOINT(session)) {
        /*
         * Propagate the checkpoint setting to the history cursor. Use the indicated history store
         * checkpoint. If that's null, it means there is no history store checkpoint to read and we
         * aren't supposed to come here.
         */
        WT_ASSERT(session, session->hs_checkpoint != NULL);
        len = strlen("checkpoint=") + strlen(session->hs_checkpoint) + 1;
        WT_RET(__wt_malloc(session, len, &tmp));
        WT_ERR(__wt_snprintf(tmp, len, "checkpoint=%s", session->hs_checkpoint));
        open_cursor_cfg[1] = tmp;
    } else
        tmp = NULL;

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_HS_URI, owner, open_cursor_cfg, &cursor));
    WT_ERR(ret);

    /* History store cursors should always ignore tombstones. */
    F_SET(cursor, WT_CURSTD_IGNORE_TOMBSTONE);

    *cursorp = cursor;

err:
    __wt_free(session, tmp);
    return (ret);
}

/*
 * __wt_curhs_cache --
 *     Cache a new history store table cursor. Open and then close a history store cursor without
 *     saving it in the session.
 */
int
__wt_curhs_cache(WT_SESSION_IMPL *session)
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
    WT_RET(__curhs_file_cursor_open(session, NULL, &cursor));
    WT_RET(cursor->close(cursor));
    return (0);
}

/*
 * __curhs_compare --
 *     WT_CURSOR->compare method for the history store cursor type.
 */
static int
__curhs_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL(a, session, compare, NULL);

    WT_ERR(__cursor_checkkey(a));
    WT_ERR(__cursor_checkkey(b));

    ret = __wt_compare(session, NULL, &a->key, &b->key, cmpp);
err:
    API_END_RET_STAT(session, ret, cursor_compare);
}

/*
 * __curhs_file_cursor_next --
 *     Execute a next operation on a history store cursor with the appropriate isolation level.
 */
static int
__curhs_file_cursor_next(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = cursor->next(cursor));
    return (ret);
}

/*
 * __curhs_file_cursor_prev --
 *     Execute a prev operation on a history store cursor with the appropriate isolation level.
 */
static int
__curhs_file_cursor_prev(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = cursor->prev(cursor));
    return (ret);
}

/*
 * __curhs_file_cursor_search_near --
 *     Execute a search near operation on a history store cursor with the appropriate isolation
 *     level.
 */
static int
__curhs_file_cursor_search_near(WT_SESSION_IMPL *session, WT_CURSOR *cursor, int *exactp)
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(
      session, WT_ISO_READ_UNCOMMITTED, ret = cursor->search_near(cursor, exactp));
    return (ret);
}

/*
 * __curhs_set_key_ptr --
 *     Copy the key buffer pointer from file cursor to the history store cursor.
 */
static inline void
__curhs_set_key_ptr(WT_CURSOR *hs_cursor, WT_CURSOR *file_cursor)
{
    hs_cursor->key.data = file_cursor->key.data;
    hs_cursor->key.size = file_cursor->key.size;
    WT_ASSERT(CUR2S(file_cursor), F_ISSET(file_cursor, WT_CURSTD_KEY_SET));
    F_SET(hs_cursor, F_MASK(file_cursor, WT_CURSTD_KEY_SET));
}

/*
 * __curhs_set_value_ptr --
 *     Copy the value buffer pointer from file cursor to the history store cursor.
 */
static inline void
__curhs_set_value_ptr(WT_CURSOR *hs_cursor, WT_CURSOR *file_cursor)
{
    hs_cursor->value.data = file_cursor->value.data;
    hs_cursor->value.size = file_cursor->value.size;
    WT_ASSERT(CUR2S(file_cursor), F_ISSET(file_cursor, WT_CURSTD_VALUE_SET));
    F_SET(hs_cursor, F_MASK(file_cursor, WT_CURSTD_VALUE_SET));
}

/*
 * __curhs_search --
 *     Search the history store for a given key and position the cursor on it.
 */
static int
__curhs_search(WT_CURSOR_BTREE *hs_cbt, bool insert)
{
    WT_BTREE *hs_btree;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_btree = CUR2BT(hs_cbt);
    session = CUR2S(hs_cbt);

#ifdef HAVE_DIAGNOSTIC
    /*
     * Turn off cursor-order checks in all cases on search. The search/search-near functions turn
     * them back on after a successful search.
     */
    __wt_cursor_key_order_reset(hs_cbt);
#endif

    WT_ERR(__wt_cursor_localkey(&hs_cbt->iface));

    WT_ERR(__wt_cursor_func_init(hs_cbt, true));

    WT_WITH_BTREE(session, hs_btree,
      ret = __wt_row_search(hs_cbt, &hs_cbt->iface.key, insert, NULL, false, NULL));

#ifdef HAVE_DIAGNOSTIC
    if (ret == 0)
        WT_TRET(__wt_cursor_key_order_init(hs_cbt));
#endif

err:
    if (ret != 0)
        WT_TRET(__cursor_reset(hs_cbt));

    return (ret);
}

/*
 * __curhs_next --
 *     WT_CURSOR->next method for the history store cursor type.
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

    WT_ERR(__curhs_file_cursor_next(session, file_cursor));
    /*
     * We need to check if the history store record is visible to the current session. If not, the
     * __curhs_next_visible() will also keep iterating forward through the records until it finds a
     * visible record or bail out if records stop satisfying the fields set in cursor.
     */
    WT_ERR(__curhs_next_visible(session, hs_cursor));

    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
    __curhs_set_key_ptr(cursor, file_cursor);
    __curhs_set_value_ptr(cursor, file_cursor);

    if (0) {
err:
        WT_TRET(cursor->reset(cursor));
    }
    API_END_RET(session, ret);
}

/*
 * __curhs_prev --
 *     WT_CURSOR->prev method for the history store cursor type.
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

    WT_ERR(__curhs_file_cursor_prev(session, file_cursor));
    /*
     * We need to check if the history store record is visible to the current session. If not, the
     * __curhs_prev_visible() will also keep iterating backwards through the records until it finds
     * a visible record or bail out if records stop satisfying the fields set in cursor.
     */
    WT_ERR(__curhs_prev_visible(session, hs_cursor));

    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
    __curhs_set_key_ptr(cursor, file_cursor);
    __curhs_set_value_ptr(cursor, file_cursor);

    if (0) {
err:
        WT_TRET(cursor->reset(cursor));
    }
    API_END_RET(session, ret);
}

/*
 * __curhs_close --
 *     WT_CURSOR->close method for the history store cursor type.
 */
static int
__curhs_close(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(
      cursor, session, close, file_cursor == NULL ? NULL : CUR2BT(file_cursor));
err:
    __wt_scr_free(session, &hs_cursor->datastore_key);
    if (file_cursor != NULL)
        WT_TRET(file_cursor->close(file_cursor));
    __wt_cursor_close(cursor);
    --session->hs_cursor_counter;

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
    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);
    hs_cursor->btree_id = 0;
    hs_cursor->datastore_key->data = NULL;
    hs_cursor->datastore_key->size = 0;
    hs_cursor->flags = 0;
    cursor->key.data = NULL;
    cursor->key.size = 0;
    cursor->value.data = NULL;
    cursor->value.size = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET);
    F_CLR(cursor, WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __curhs_set_key --
 *     WT_CURSOR->set_key method for the history store cursor type.
 */
static void
__curhs_set_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
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

    hs_cursor->flags = 0;
    va_start(ap, cursor);
    arg_count = va_arg(ap, uint32_t);

    WT_ASSERT(session, arg_count >= 1 && arg_count <= 4);

    hs_cursor->btree_id = va_arg(ap, uint32_t);
    F_SET(hs_cursor, WT_HS_CUR_BTREE_ID_SET);
    if (arg_count > 1) {
        datastore_key = va_arg(ap, WT_ITEM *);
        if ((ret = __wt_buf_set(
               session, hs_cursor->datastore_key, datastore_key->data, datastore_key->size)) != 0)
            WT_IGNORE_RET(__wt_panic(session, ret, "failed to set the contents of buffer"));
        F_SET(hs_cursor, WT_HS_CUR_KEY_SET);
    } else {
        hs_cursor->datastore_key->data = NULL;
        hs_cursor->datastore_key->size = 0;
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
      file_cursor, hs_cursor->btree_id, hs_cursor->datastore_key, start_ts, counter);

    __curhs_set_key_ptr(cursor, file_cursor);
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

    for (; ret == 0; ret = __curhs_file_cursor_prev(session, file_cursor)) {
        WT_ERR(file_cursor->get_key(file_cursor, &btree_id, datastore_key, &start_ts, &counter));

        /*
         * Stop before crossing over to the next btree except when the
         * WT_CURSTD_HS_READ_ACROSS_BTREE flag is set. This flag is needed when we try to place the
         * cursor at the end of the btree range. In that case, We first place the cursor at the
         * smallest record that has a larger btree and then move the cursor backwards to the end of
         * the target btree range.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET) &&
          !F_ISSET(std_cursor, WT_CURSTD_HS_READ_ACROSS_BTREE) && btree_id != hs_cursor->btree_id) {
            ret = WT_NOTFOUND;
            goto err;
        }

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
            WT_ERR(__wt_compare(session, NULL, datastore_key, hs_cursor->datastore_key, &cmp));
            if (cmp != 0) {
                ret = WT_NOTFOUND;
                goto err;
            }
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record,
         * even with a globally visible tombstone.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_ALL))
            break;

        /*
         * If the stop time pair on the tombstone in the history store is already globally visible,
         * it is outdated and we must skip it rather than returning NOTFOUND. Subsequent entries
         * might have later stop times and we might need to return one of them.
         */
        if (__wt_txn_tw_stop_visible_all(session, &cbt->upd_value->tw)) {
            WT_STAT_CONN_DATA_INCR(session, cursor_prev_hs_tombstone);
            continue;
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record that
         * is not obsolete.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_COMMITTED))
            break;

        /*
         * If we are using a history store cursor and haven't set the WT_CURSTD_HS_READ_COMMITTED
         * flag then we must have a snapshot, assert that we do.
         */
        WT_ASSERT(session, F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

        if (__wt_txn_tw_stop_visible(session, &cbt->upd_value->tw)) {
            /*
             * If the stop time point of a record is visible to us, we won't be able to see anything
             * for this entire key.
             */
            if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
                ret = WT_NOTFOUND;
                goto err;
            } else
                continue;
        }

        /* If the start time point is visible to us, let's return that record. */
        if (__wt_txn_tw_start_visible(session, &cbt->upd_value->tw))
            break;
    }

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

    for (; ret == 0; ret = __curhs_file_cursor_next(session, file_cursor)) {
        WT_ERR(file_cursor->get_key(file_cursor, &btree_id, datastore_key, &start_ts, &counter));

        /*
         * Stop before crossing over to the next btree except when the
         * WT_CURSTD_HS_READ_ACROSS_BTREE flag is set. This flag is needed when we try to place the
         * cursor at the end of the btree range. In that case, We first place the cursor at the
         * smallest record that has a larger btree and then move the cursor backwards to the end of
         * the target btree range.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET) &&
          !F_ISSET(std_cursor, WT_CURSTD_HS_READ_ACROSS_BTREE) && btree_id != hs_cursor->btree_id) {
            ret = WT_NOTFOUND;
            goto err;
        }

        /*
         * Keys are sorted in an order, skip the ones before the desired key, and bail out if we
         * have crossed over the desired key and not found the record we are looking for.
         */
        if (F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET)) {
            WT_ERR(__wt_compare(session, NULL, datastore_key, hs_cursor->datastore_key, &cmp));
            if (cmp != 0) {
                ret = WT_NOTFOUND;
                goto err;
            }
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record,
         * even with a globally visible tombstone.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_ALL))
            break;

        /*
         * If the stop time pair on the tombstone in the history store is already globally visible,
         * it is outdated and we must skip it rather than returning NOTFOUND. Subsequent entries
         * might have later stop times and we might need to return one of them.
         */
        if (__wt_txn_tw_stop_visible_all(session, &cbt->upd_value->tw)) {
            WT_STAT_CONN_DATA_INCR(session, cursor_next_hs_tombstone);
            continue;
        }

        /*
         * Don't check the visibility of the record if we want to read any history store record that
         * is not obsolete.
         */
        if (F_ISSET(std_cursor, WT_CURSTD_HS_READ_COMMITTED))
            break;

        /*
         * If we are using a history store cursor and haven't set the WT_CURSTD_HS_READ_COMMITTED
         * flag then we must have a snapshot, assert that we do.
         */
        WT_ASSERT(session, F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

        /*
         * If the stop time point of a record is visible to us, check the next one.
         */
        if (__wt_txn_tw_stop_visible(session, &cbt->upd_value->tw))
            continue;

        /* If the start time point is visible to us, let's return that record. */
        if (__wt_txn_tw_start_visible(session, &cbt->upd_value->tw))
            break;
    }

err:
    __wt_scr_free(session, &datastore_key);
    return (ret);
}

/*
 * __wt_curhs_search_near_before --
 *     Set the cursor position at the requested position or before it.
 */
int
__wt_curhs_search_near_before(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    /*
     * If the btree id is set alone, it is not possible to position the cursor at a place that is
     * smaller than set search key, therefore assert that the key must be set to use this function.
     */
    WT_ASSERT(session, F_ISSET((WT_CURSOR_HS *)cursor, WT_HS_CUR_KEY_SET));
    return (__curhs_search_near_helper(session, cursor, true));
}

/*
 * __wt_curhs_search_near_after --
 *     Set the cursor position at the requested position or after it.
 */
int
__wt_curhs_search_near_after(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    return (__curhs_search_near_helper(session, cursor, false));
}

/*
 * __curhs_search_near_helper --
 *     Helper function to set the cursor position based on search criteria.
 */
static int
__curhs_search_near_helper(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool before)
{
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    int cmp;

    WT_RET(__wt_scr_alloc(session, 0, &srch_key));
    WT_ERR(__wt_buf_set(session, srch_key, cursor->key.data, cursor->key.size));
    WT_ERR(cursor->search_near(cursor, &cmp));
    if (before) {
        /*
         * If we want to land on a key that is smaller or equal to the specified key, keep walking
         * backwards as there may be content inserted concurrently.
         */
        if (cmp > 0) {
            while ((ret = cursor->prev(cursor)) == 0) {
                WT_STAT_CONN_INCR(session, cursor_skip_hs_cur_position);
                WT_STAT_DATA_INCR(session, cursor_skip_hs_cur_position);
                WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
                /*
                 * Exit if we have found a key that is smaller than or equal to the specified key.
                 */
                if (cmp <= 0)
                    break;
            }
        }
    } else {
        /*
         * If we want to land on a key that is larger or equal to the specified key, keep walking
         * forwards as there may be content inserted concurrently.
         */
        if (cmp < 0) {
            while ((ret = cursor->next(cursor)) == 0) {
                WT_STAT_CONN_INCR(session, cursor_skip_hs_cur_position);
                WT_STAT_DATA_INCR(session, cursor_skip_hs_cur_position);
                WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
                /* Exit if we have found a key that is larger than or equal to the specified key. */
                if (cmp >= 0)
                    break;
            }
        }
    }

err:
    __wt_scr_free(session, &srch_key);
    return (ret);
}

/*
 * __curhs_search_near --
 *     WT_CURSOR->search_near method for the history store cursor type.
 */
static int
__curhs_search_near(WT_CURSOR *cursor, int *exactp)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_ITEM(datastore_key);
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    wt_timestamp_t start_ts;
    uint64_t counter;
    uint32_t btree_id;
    int exact, cmp;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    *exactp = 0;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, search_near, CUR2BT(file_cursor));

    WT_ERR(__wt_scr_alloc(session, 0, &datastore_key));
    WT_ERR(__wt_scr_alloc(session, 0, &srch_key));
    /* At least we have the btree id set. */
    WT_ASSERT(session, F_ISSET(hs_cursor, WT_HS_CUR_BTREE_ID_SET));
    WT_ERR(__wt_buf_set(session, srch_key, file_cursor->key.data, file_cursor->key.size));
    /* Reset cursor if we get WT_NOTFOUND. */
    WT_ERR(__curhs_file_cursor_search_near(session, file_cursor, &exact));

    if (exact >= 0) {
        /*
         * We placed the file cursor after or exactly at the search key. Try first to walk forwards
         * to see if we can find a visible record. If nothing is visible, try to walk backwards.
         */
        WT_ERR_NOTFOUND_OK(__curhs_next_visible(session, hs_cursor), true);
        if (ret == WT_NOTFOUND) {
            /*
             * When walking backwards, first ensure we walk back to the specified btree or key space
             * as we may have crossed the boundary. Do that in a loop as there may be content
             * inserted concurrently.
             */
            while ((ret = __curhs_file_cursor_prev(session, file_cursor)) == 0) {
                WT_ERR(
                  file_cursor->get_key(file_cursor, &btree_id, datastore_key, &start_ts, &counter));

                /* We are back in the specified btree range. */
                if (btree_id == hs_cursor->btree_id) {
                    if (!F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET))
                        break;

                    WT_ERR(
                      __wt_compare(session, NULL, datastore_key, hs_cursor->datastore_key, &cmp));

                    /* We are back in the specified key range. */
                    if (cmp == 0)
                        break;

                    /*
                     * We're comparing the entire history store key (as opposed to just the data
                     * store component) because ordering can be different between the data store and
                     * history store due to packing. Since we know we're NOT in the specified key
                     * range due to the check above, checking whether we're before or after the full
                     * history store key that we're running a `search near` on will tell us whether
                     * we're before or after the specified key range.
                     *
                     * If we're before the specified key range, that means nothing is visible to us
                     * in the specified key range and we should terminate the search.
                     */
                    WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
                    if (cmp < 0) {
                        ret = WT_NOTFOUND;
                        goto err;
                    }
                }

                /*
                 * We are now smaller than the btree range, which indicates nothing is visible to us
                 * in the specified btree range.
                 */
                if (btree_id < hs_cursor->btree_id) {
                    ret = WT_NOTFOUND;
                    goto err;
                }
            }
            WT_ERR(ret);
            /*
             * Keep looking for the first visible update in the specified range when walking
             * backwards.
             */
            WT_ERR(__curhs_prev_visible(session, hs_cursor));
            /*
             * We can't find anything visible when first walking forwards so we must have found an
             * update that is smaller than the specified key.
             */
            *exactp = -1;
        } else {
            WT_ERR(ret);
            /*
             * We find an update when walking forwards. If initially we landed on the same key as
             * the specified key, we need to compare the keys to see where we are now. Otherwise, we
             * must have found a key that is larger than the specified key.
             */
            if (exact == 0) {
                WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
                *exactp = cmp;
            } else
                *exactp = exact;
        }
    } else {
        /*
         * We placed the file cursor before the search key. Try first to walk backwards to see if we
         * can find a visible record. If nothing is visible, try to walk forwards.
         */
        WT_ERR_NOTFOUND_OK(__curhs_prev_visible(session, hs_cursor), true);
        if (ret == WT_NOTFOUND) {
            /*
             * When walking forwards, first ensure we walk back to the specified btree or key space
             * as we may have crossed the boundary. Do that in a loop as there may be content
             * inserted concurrently.
             */
            while ((ret = __curhs_file_cursor_next(session, file_cursor)) == 0) {
                WT_ERR(
                  file_cursor->get_key(file_cursor, &btree_id, datastore_key, &start_ts, &counter));

                /* We are back in the specified btree range. */
                if (btree_id == hs_cursor->btree_id) {
                    if (!F_ISSET(hs_cursor, WT_HS_CUR_KEY_SET))
                        break;

                    WT_ERR(
                      __wt_compare(session, NULL, datastore_key, hs_cursor->datastore_key, &cmp));

                    /* We are back in the specified key range. */
                    if (cmp == 0)
                        break;

                    /*
                     * We're comparing the entire history store key (as opposed to just the data
                     * store component) because ordering can be different between the data store and
                     * history store due to packing. Since we know we're NOT in the specified key
                     * range due to the check above, checking whether we're before or after the full
                     * history store key that we're running a `search near` on will tell us whether
                     * we're before or after the specified key range.
                     *
                     * If we're after the specified key range, that means nothing is visible to us
                     * in the specified key range and we should terminate the search.
                     */
                    WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
                    if (cmp > 0) {
                        ret = WT_NOTFOUND;
                        goto err;
                    }
                }

                /*
                 * We are now larger than the btree range, which indicates nothing is visible to us
                 * in the specified btree range.
                 */
                if (btree_id > hs_cursor->btree_id) {
                    ret = WT_NOTFOUND;
                    goto err;
                }
            }
            WT_ERR(ret);
            /*
             * Keep looking for the first visible update in the specified range when walking
             * forwards.
             */
            WT_ERR(__curhs_next_visible(session, hs_cursor));
            /*
             * We can't find anything visible when first walking backwards so we must have found an
             * update that is larger than the specified key.
             */
            *exactp = 1;
        } else {
            WT_ERR(ret);
            *exactp = exact;
        }
    }

#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__wt_compare(session, NULL, &file_cursor->key, srch_key, &cmp));
    WT_ASSERT(
      session, (cmp == 0 && *exactp == 0) || (cmp < 0 && *exactp < 0) || (cmp > 0 && *exactp > 0));
#endif

    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
    __curhs_set_key_ptr(cursor, file_cursor);
    __curhs_set_value_ptr(cursor, file_cursor);

    if (0) {
err:
        WT_TRET(cursor->reset(cursor));
    }

    __wt_scr_free(session, &datastore_key);
    __wt_scr_free(session, &srch_key);
    API_END_RET(session, ret);
}

/*
 * __curhs_set_value --
 *     WT_CURSOR->set_value method for the history store cursor type.
 */
static void
__curhs_set_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_ITEM *hs_val;
    wt_timestamp_t start_ts;
    wt_timestamp_t stop_ts;
    uint64_t type;
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    va_start(ap, cursor);
    hs_cursor->time_window = *va_arg(ap, WT_TIME_WINDOW *);

    stop_ts = va_arg(ap, wt_timestamp_t);
    start_ts = va_arg(ap, wt_timestamp_t);
    type = va_arg(ap, uint64_t);
    hs_val = va_arg(ap, WT_ITEM *);

    file_cursor->set_value(file_cursor, stop_ts, start_ts, type, hs_val);
    va_end(ap);

    __curhs_set_value_ptr(cursor, file_cursor);
}

/*
 * __curhs_insert --
 *     WT_CURSOR->insert method for the history store cursor type.
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
    int exact;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    hs_tombstone = hs_upd = NULL;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, insert, CUR2BT(file_cursor));

    /*
     * Disable bulk loads into history store. This would normally occur when updating a record with
     * a cursor however the history store doesn't use cursor update, so we do it here.
     */
    __wt_btree_disable_bulk(session);

    /*
     * The actual record to be inserted into the history store. Set the current update start time
     * point as the commit time point to the history store record.
     */
    WT_ERR(__wt_upd_alloc(session, &file_cursor->value, WT_UPDATE_STANDARD, &hs_upd, NULL));
    hs_upd->start_ts = hs_cursor->time_window.start_ts;
    hs_upd->durable_ts = hs_cursor->time_window.durable_start_ts;
    hs_upd->txnid = hs_cursor->time_window.start_txn;

    /*
     * Allocate a tombstone only when there is a valid stop time point, and insert the standard
     * update as the update after the tombstone.
     */
    if (WT_TIME_WINDOW_HAS_STOP(&hs_cursor->time_window)) {
        /* We should not see a tombstone with max transaction id. */
        WT_ASSERT(session, hs_cursor->time_window.stop_txn != WT_TXN_MAX);
        /*
         * Insert a delete record to represent stop time point for the actual record to be inserted.
         * Set the stop time point as the commit time point of the history store delete record.
         */
        WT_ERR(__wt_upd_alloc_tombstone(session, &hs_tombstone, NULL));
        hs_tombstone->start_ts = hs_cursor->time_window.stop_ts;
        hs_tombstone->durable_ts = hs_cursor->time_window.durable_stop_ts;
        hs_tombstone->txnid = hs_cursor->time_window.stop_txn;

        WT_ASSERT(session,
          hs_tombstone->start_ts >= hs_upd->start_ts &&
            hs_tombstone->durable_ts >= hs_upd->durable_ts);

        hs_tombstone->next = hs_upd;
        hs_upd = hs_tombstone;
    }

    do {
        WT_WITH_PAGE_INDEX(session, ret = __curhs_search(cbt, true));
        WT_ERR(ret);
    } while ((ret = __wt_hs_modify(cbt, hs_upd)) == WT_RESTART);
    WT_ERR(ret);

    /* We no longer own the update memory, the page does; don't free it under any circumstances. */
    hs_tombstone = hs_upd = NULL;

    if (EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_HS_VALIDATE)) {
        /* Do a search again and call next to check the key order. */
        ret = __curhs_file_cursor_search_near(session, file_cursor, &exact);
        /* We can get not found if the inserted history store record is obsolete. */
        WT_ASSERT_ALWAYS(session, ret == 0 || ret == WT_NOTFOUND,
          "Key order verification failed for history store insertion. ret: %d", ret);

        /*
         * If a globally visible tombstone is inserted and the page is evicted during search_near
         * then the key would be removed. Hence, a search_near would return a non-zero exact value.
         * Therefore, check that exact is zero before calling next.
         */
        if (ret == 0 && exact == 0)
            WT_ERR_NOTFOUND_OK(__curhs_file_cursor_next(session, file_cursor), false);
        else if (ret == WT_NOTFOUND)
            ret = 0;
    }

    /* Insert doesn't maintain a position across calls, clear resources. */
err:
    __wt_free_update_list(session, &hs_upd);
    WT_TRET(cursor->reset(cursor));
    API_END_RET(session, ret);
}

/*
 * __curhs_remove_int --
 *     Remove a record from the history store.
 */
static inline int
__curhs_remove_int(WT_CURSOR_BTREE *cbt, const WT_ITEM *value, u_int modify_type)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *hs_tombstone;

    WT_UNUSED(value);
    WT_UNUSED(modify_type);
    session = CUR2S(cbt);
    WT_ASSERT(session, modify_type == WT_UPDATE_TOMBSTONE);

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
        WT_WITH_PAGE_INDEX(session, ret = __curhs_search(cbt, false));
        WT_ERR(ret);
    }

    if (0) {
err:
        __wt_free(session, hs_tombstone);
    }

    return (ret);
}

/*
 * __curhs_remove --
 *     WT_CURSOR->remove method for the history store cursor type.
 */
static int
__curhs_remove(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, remove, CUR2BT(file_cursor));

    /* Remove must be called with cursor positioned. */
    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));

    WT_ERR(__curhs_remove_int(cbt, NULL, WT_UPDATE_TOMBSTONE));

    /* We must still hold the cursor position after the remove call. */
    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));

    /* Invalidate the previous value but we will hold on to the position of the key. */
    F_CLR(file_cursor, WT_CURSTD_VALUE_SET);
    F_CLR(cursor, WT_CURSTD_VALUE_SET);

    if (0) {
err:
        WT_TRET(cursor->reset(cursor));
    }

    API_END_RET(session, ret);
}

/*
 * __curhs_update --
 *     WT_CURSOR->update method for the history store cursor type.
 */
static int
__curhs_update(WT_CURSOR *cursor)
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

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, update, CUR2BT(file_cursor));

    /* Update must be called with cursor positioned. */
    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
    WT_ASSERT(session, F_ISSET(hs_cursor, WT_HS_CUR_COUNTER_SET | WT_HS_CUR_TS_SET));

    /*
     * Only valid scenario to update the history store is to add the stop timestamp. Any other case
     * should fail.
     */
    WT_ASSERT(session, !WT_TIME_WINDOW_IS_EMPTY(&hs_cursor->time_window));
    WT_ASSERT(session, WT_TIME_WINDOW_HAS_STOP(&hs_cursor->time_window));

    /* The tombstone to represent the stop time window. */
    WT_ERR(__wt_upd_alloc_tombstone(session, &hs_tombstone, NULL));
    hs_tombstone->start_ts = hs_cursor->time_window.stop_ts;
    hs_tombstone->durable_ts = hs_cursor->time_window.durable_stop_ts;
    hs_tombstone->txnid = hs_cursor->time_window.stop_txn;

    WT_ERR(__wt_upd_alloc(session, &file_cursor->value, WT_UPDATE_STANDARD, &hs_upd, NULL));
    hs_upd->start_ts = hs_cursor->time_window.start_ts;
    hs_upd->durable_ts = hs_cursor->time_window.durable_start_ts;
    hs_upd->txnid = hs_cursor->time_window.start_txn;

    WT_ASSERT(session,
      hs_tombstone->start_ts >= hs_upd->start_ts && hs_tombstone->durable_ts >= hs_upd->durable_ts);

    /* Connect the tombstone to the update. */
    hs_tombstone->next = hs_upd;

    /*
     * Since we're using internal functions to modify the row structure, we need to manually set the
     * comparison to an exact match.
     */
    cbt->compare = 0;
    /* Make the updates and if we fail, search and try again. */
    while ((ret = __wt_hs_modify(cbt, hs_tombstone)) == WT_RESTART) {
        WT_WITH_PAGE_INDEX(session, ret = __curhs_search(cbt, false));
        WT_ERR(ret);
    }

    __curhs_set_key_ptr(cursor, file_cursor);
    __curhs_set_value_ptr(cursor, file_cursor);

    if (0) {
err:
        __wt_free(session, hs_tombstone);
        __wt_free(session, hs_upd);
        WT_TRET(cursor->reset(cursor));
    }
    API_END_RET(session, ret);
}

/*
 * __curhs_range_truncate --
 *     Discard a cursor range from the history store tree.
 */
static int
__curhs_range_truncate(WT_CURSOR *start, WT_CURSOR *stop)
{
    WT_CURSOR *start_file_cursor, *stop_file_cursor;
    WT_SESSION_IMPL *session;

    session = CUR2S(start);
    start_file_cursor = ((WT_CURSOR_HS *)start)->file_cursor;
    stop_file_cursor = stop != NULL ? ((WT_CURSOR_HS *)stop)->file_cursor : NULL;

    WT_STAT_DATA_INCR(session, cursor_truncate);

    WT_ASSERT(session, F_ISSET(start_file_cursor, WT_CURSTD_KEY_INT));
    WT_RET(__wt_cursor_localkey(start_file_cursor));
    if (stop != NULL) {
        WT_ASSERT(session, F_ISSET(stop_file_cursor, WT_CURSTD_KEY_INT));
        WT_RET(__wt_cursor_localkey(stop_file_cursor));
    }

    WT_RET(__wt_cursor_truncate((WT_CURSOR_BTREE *)start_file_cursor,
      (WT_CURSOR_BTREE *)stop_file_cursor, __curhs_remove_int));

    return (0);
}

/*
 * __wt_curhs_range_truncate --
 *     Discard a cursor range from the history store tree.
 */
int
__wt_curhs_range_truncate(WT_CURSOR *start, WT_CURSOR *stop)
{
    WT_CURSOR *start_file_cursor;
    WT_DECL_RET;

    start_file_cursor = ((WT_CURSOR_HS *)start)->file_cursor;

    WT_WITH_BTREE(
      CUR2S(start), CUR2BT(start_file_cursor), ret = __curhs_range_truncate(start, stop));

    return (ret);
}

/*
 * __wt_curhs_open --
 *     Initialize a history store cursor.
 */
int
__wt_curhs_open(WT_SESSION_IMPL *session, WT_CURSOR *owner, WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value_notsup,           /* get-raw-key-value */
      __curhs_set_key,                                /* set-key */
      __curhs_set_value,                              /* set-value */
      __curhs_compare,                                /* compare */
      __wt_cursor_equals_notsup,                      /* equals */
      __curhs_next,                                   /* next */
      __curhs_prev,                                   /* prev */
      __curhs_reset,                                  /* reset */
      __wt_cursor_notsup,                             /* search */
      __curhs_search_near,                            /* search-near */
      __curhs_insert,                                 /* insert */
      __wt_cursor_modify_value_format_notsup,         /* modify */
      __curhs_update,                                 /* update */
      __curhs_remove,                                 /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curhs_close);                                 /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;

    *cursorp = NULL;
    WT_RET(__wt_calloc_one(session, &hs_cursor));
    ++session->hs_cursor_counter;
    cursor = (WT_CURSOR *)hs_cursor;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = WT_HS_KEY_FORMAT;
    cursor->value_format = WT_HS_VALUE_FORMAT;
    WT_ERR(__wt_strdup(session, WT_HS_URI, &cursor->uri));

    /* Open the file cursor for operations on the regular history store .*/
    WT_ERR(__curhs_file_cursor_open(session, owner, &hs_cursor->file_cursor));

    WT_WITH_BTREE(session, CUR2BT(hs_cursor->file_cursor),
      ret = __wt_cursor_init(cursor, WT_HS_URI, owner, NULL, cursorp));
    WT_ERR(ret);
    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);
    hs_cursor->btree_id = 0;
    WT_ERR(__wt_scr_alloc(session, 0, &hs_cursor->datastore_key));
    hs_cursor->flags = 0;

    WT_TIME_WINDOW_INIT(&hs_cursor->time_window);

    if (0) {
err:
        WT_TRET(cursor->close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
