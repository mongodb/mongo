/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
      session, ret = __wt_schema_drop(session, "file:WiredTigerLAS.wt", drop_cfg, false));

    return (ret);
}

/*
 * __hs_get_btree --
 *     Get the history store btree by opening a history store cursor.
 */
static int
__hs_get_btree(WT_SESSION_IMPL *session, uint32_t hs_id, WT_BTREE **hs_btreep)
{
    WT_CURSOR *hs_cursor;

    *hs_btreep = NULL;

    WT_RET(__wt_curhs_open_ext(session, hs_id, 0, NULL, &hs_cursor));
    *hs_btreep = __wt_curhs_get_btree(hs_cursor);
    WT_ASSERT(session, *hs_btreep != NULL);

    WT_RET(hs_cursor->close(hs_cursor));

    return (0);
}

/*
 * __hs_config --
 *     Configure one history store table.
 */
static int
__hs_config(WT_SESSION_IMPL *session, uint32_t hs_id, const char **cfg)
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

    /* The history store is not available for in-memory configurations. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY))
        return (0);

    WT_ERR(__wt_open_internal_session(conn, "hs_access", true, 0, 0, &tmp_setup_session));

    /* Retrieve the btree from the history store cursor. */
    WT_ERR(__hs_get_btree(tmp_setup_session, hs_id, &btree));

    /* Track the history store file ID. */
    if (conn->cache->hs_fileid == 0)
        conn->cache->hs_fileid = btree->id;

    /* We need to set file_max on the btree associated with one of the history store sessions. */
    btree->file_max = (uint64_t)cval.val;
    WT_STAT_CONN_SET(session, cache_hs_ondisk_max, btree->file_max);

    /*
     * Now that we have the history store's handle, we may set the flag because we know the file is
     * open.
     */
    F_SET_ATOMIC_32(conn, WT_CONN_HS_OPEN);

err:
    if (tmp_setup_session != NULL)
        WT_TRET(__wt_session_close_internal(tmp_setup_session));
    return (ret);
}

/*
 * __wt_hs_config --
 *     Configure the all history store tables.
 */
int
__wt_hs_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_DECL_RET;
    uint32_t hs_id;

    hs_id = 0;
    for (;;) {
        WT_RET_NOTFOUND_OK(ret = __wt_curhs_next_hs_id(session, hs_id, &hs_id));
        if (ret == WT_NOTFOUND)
            return (0);
        WT_RET(__hs_config(session, hs_id, cfg));
    }
}

/*
 * __wt_hs_open --
 *     Initialize the database's history store.
 */
int
__wt_hs_open(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *hs_session;

    conn = S2C(session);
    hs_session = NULL;

    /* Read-only and in-memory configurations don't need the history store table. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /*
     * It is necessary to create a new session to initialize the HS file because the default session
     * can be used by other tasks concurrently during recovery.
     */
    WT_ERR(__wt_open_internal_session(conn, "hs-open", false, 0, 0, &hs_session));

    /* Drop the lookaside file if it still exists. */
    WT_ERR(__hs_cleanup_las(hs_session));

    /* Create the local table. */
    WT_ERR(__wt_session_create(hs_session, WT_HS_URI, WT_HS_CONFIG_LOCAL));

    /* Create the shared table. */
    if (__wt_conn_is_disagg(session))
        WT_ERR(__wt_session_create(hs_session, WT_HS_URI_SHARED, WT_HS_CONFIG_SHARED));

    /* Configure all history stores. */
    WT_ERR(__wt_hs_config(hs_session, cfg));

err:
    if (hs_session != NULL)
        WT_TRET(__wt_session_close_internal(hs_session));
    return (ret);
}

/*
 * __wt_hs_close --
 *     Clear the connection's flag to make the history store unavailable.
 */
void
__wt_hs_close(WT_SESSION_IMPL *session)
{
    F_CLR_ATOMIC_32(S2C(session), WT_CONN_HS_OPEN);
}
