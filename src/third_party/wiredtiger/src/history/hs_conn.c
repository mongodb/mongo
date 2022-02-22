/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __hs_start_internal_session --
 *     Create a temporary internal session to retrieve history store.
 */
static int
__hs_start_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
{
    return (__wt_open_internal_session(S2C(session), "hs_access", true, 0, 0, int_sessionp));
}

/*
 * __hs_release_internal_session --
 *     Release the temporary internal session started to retrieve history store.
 */
static int
__hs_release_internal_session(WT_SESSION_IMPL *int_session)
{
    return (__wt_session_close_internal(int_session));
}

/*
 * __hs_cleanup_las --
 *     Drop the lookaside file if it exists.
 */
static int
__hs_cleanup_las(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL};

    conn = S2C(session);

    /* Read-only and in-memory configurations won't drop the lookaside. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* The LAS table may exist on upgrade. Discard it. */
    WT_WITH_SCHEMA_LOCK(
      session, ret = __wt_schema_drop(session, "file:WiredTigerLAS.wt", drop_cfg));

    return (ret);
}

/*
 * __wt_hs_get_btree --
 *     Get the history store btree by opening a history store cursor.
 */
int
__wt_hs_get_btree(WT_SESSION_IMPL *session, WT_BTREE **hs_btreep)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_RET;

    *hs_btreep = NULL;

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    *hs_btreep = __wt_curhs_get_btree(hs_cursor);
    WT_ASSERT(session, *hs_btreep != NULL);
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __wt_hs_config --
 *     Configure the history store table.
 */
int
__wt_hs_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *tmp_setup_session;

    conn = S2C(session);
    tmp_setup_session = NULL;

    WT_ERR(__wt_config_gets(session, cfg, "history_store.file_max", &cval));
    if (cval.val != 0 && cval.val < WT_HS_FILE_MIN)
        WT_ERR_MSG(session, EINVAL, "max history store size %" PRId64 " below minimum %d", cval.val,
          WT_HS_FILE_MIN);

    /* in-memory or readonly configurations do not have a history store. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    WT_ERR(__hs_start_internal_session(session, &tmp_setup_session));

    /*
     * Retrieve the btree from the history store cursor.
     */
    WT_ERR(__wt_hs_get_btree(tmp_setup_session, &btree));

    /* Track the history store file ID. */
    if (conn->cache->hs_fileid == 0)
        conn->cache->hs_fileid = btree->id;

    /*
     * We need to set file_max on the btree associated with one of the history store sessions.
     */
    btree->file_max = (uint64_t)cval.val;
    WT_STAT_CONN_SET(session, cache_hs_ondisk_max, btree->file_max);

err:
    if (tmp_setup_session != NULL)
        WT_TRET(__hs_release_internal_session(tmp_setup_session));
    return (ret);
}

/*
 * __wt_hs_open --
 *     Initialize the database's history store.
 */
int
__wt_hs_open(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* Read-only and in-memory configurations don't need the history store table. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* Drop the lookaside file if it still exists. */
    WT_RET(__hs_cleanup_las(session));

    /* Create the table. */
    WT_RET(__wt_session_create(session, WT_HS_URI, WT_HS_CONFIG));

    WT_RET(__wt_hs_config(session, cfg));

    /* The statistics server is already running, make sure we don't race. */
    WT_WRITE_BARRIER();
    F_SET(conn, WT_CONN_HS_OPEN);

    return (0);
}

/*
 * __wt_hs_close --
 *     Destroy the database's history store.
 */
void
__wt_hs_close(WT_SESSION_IMPL *session)
{
    F_CLR(S2C(session), WT_CONN_HS_OPEN);
}
