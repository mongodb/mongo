
/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
 * __curhs_close --
 *     WT_CURSOR->close method for the hs cursor type.
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
    if (file_cursor != NULL)
        WT_TRET(file_cursor->close(file_cursor));
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
    va_list ap;

    hs_cursor = (WT_CURSOR_HS *)cursor;
    file_cursor = hs_cursor->file_cursor;

    va_start(ap, cursor);
    file_cursor->set_key(file_cursor, va_arg(ap, uint32_t), va_arg(ap, WT_ITEM *),
      va_arg(ap, wt_timestamp_t), va_arg(ap, uint64_t));
    va_end(ap);
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

    /* Search the page and insert the updates. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_hs_row_search(cbt, &file_cursor->key, true));
    WT_ERR(ret);
    WT_ERR(__wt_hs_modify(cbt, hs_upd));

    if (0) {
err:
        __wt_free(session, hs_tombstone);
        __wt_free(session, hs_upd);
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
      __wt_cursor_notsup,                         /* next */
      __wt_cursor_notsup,                         /* prev */
      __curhs_reset,                              /* reset */
      __wt_cursor_notsup,                         /* search */
      __wt_cursor_search_near_notsup,             /* search-near */
      __curhs_insert,                             /* insert */
      __wt_cursor_modify_value_format_notsup,     /* modify */
      __wt_cursor_notsup,                         /* update */
      __wt_cursor_notsup,                         /* remove */
      __wt_cursor_notsup,                         /* reserve */
      __wt_cursor_reconfigure_notsup,             /* reconfigure */
      __wt_cursor_notsup,                         /* cache */
      __wt_cursor_reopen_notsup,                  /* reopen */
      __curhs_close);                             /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_HS *hs_cursor;
    WT_DECL_RET;

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

    if (0) {
err:
        WT_TRET(__curhs_close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
