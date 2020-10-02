
/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_hs_cursor_open --
 *     Open a new history store table cursor.
 */
int
__wt_hs_cursor_open(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    /* Not allowed to open a cursor if you already have one */
    WT_ASSERT(session, session->hs_cursor == NULL);

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_HS_URI, NULL, open_cursor_cfg, &cursor));
    WT_RET(ret);

    /* History store cursors should always ignore tombstones. */
    F_SET(cursor, WT_CURSTD_IGNORE_TOMBSTONE);

    session->hs_cursor = cursor;
    return (0);
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
