/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __truncate_table --
 *     WT_SESSION::truncate for a table.
 */
static int
__truncate_table(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DECL_RET;
    WT_TABLE *table;
    u_int i;

    WT_RET(__wt_schema_get_table(session, uri, strlen(uri), false, 0, &table));
    WT_STAT_DSRC_INCR(session, cursor_truncate);

    /* Truncate the column groups. */
    for (i = 0; i < WT_COLGROUPS(table); i++)
        WT_ERR(__wt_schema_truncate(session, table->cgroups[i]->source, cfg));

    /* Truncate the indices. */
    WT_ERR(__wt_schema_open_indices(session, table));
    for (i = 0; i < table->nindices; i++)
        WT_ERR(__wt_schema_truncate(session, table->indices[i]->source, cfg));

err:
    WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}

/*
 * __truncate_tiered --
 *     Truncate for a tiered data source.
 */
static int
__truncate_tiered(WT_SESSION_IMPL *session, const char *uri)
{
    WT_DECL_RET;

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));

    WT_STAT_DSRC_INCR(session, cursor_truncate);

    WT_WITHOUT_DHANDLE(session, ret = __wt_session_range_truncate(session, uri, NULL, NULL));
    WT_ERR(ret);

err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __truncate_layered --
 *     Truncate for a layered data source.
 */
static int
__truncate_layered(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *start;
    WT_DECL_RET;

    start = NULL;
    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));

    WT_STAT_DSRC_INCR(session, cursor_truncate);

    /* To bypass the truncate requiring a btree uri. */
    WT_ERR(__wt_open_cursor(session, uri, NULL, NULL, &start));
    WT_ERR_NOTFOUND_OK(start->next(start), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }
    WT_WITHOUT_DHANDLE(session, ret = __wt_session_range_truncate(session, NULL, start, NULL));
    WT_ERR(ret);

done:
err:
    if (start != NULL)
        WT_TRET(start->close(start));
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __truncate_dsrc --
 *     WT_SESSION::truncate for a data-source without a truncate operation.
 */
static int
__truncate_dsrc(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cfg[2];

    /* Open a cursor and traverse the object, removing every entry. */
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = NULL;
    WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
    while ((ret = cursor->next(cursor)) == 0)
        WT_ERR(cursor->remove(cursor));
    WT_ERR_NOTFOUND_OK(ret, false);
    WT_STAT_DSRC_INCR(session, cursor_truncate);

err:
    WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __wt_schema_truncate --
 *     WT_SESSION::truncate without a range.
 */
int
__wt_schema_truncate(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    const char *tablename;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    tablename = uri;

    if (WT_PREFIX_MATCH(uri, "file:"))
        /*
         * File truncate translates into a range truncate.
         */
        ret = __wt_session_range_truncate(session, uri, NULL, NULL);
    else if (WT_PREFIX_MATCH(uri, "layered:"))
        ret = __truncate_layered(session, uri);
    else if (WT_PREFIX_SKIP(tablename, "table:"))
        ret = __truncate_table(session, tablename, cfg);
    else if (WT_PREFIX_MATCH(uri, "tiered:"))
        ret = __truncate_tiered(session, uri);
    else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
        ret = dsrc->truncate == NULL ?
          __truncate_dsrc(session, uri) :
          dsrc->truncate(dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg);
    else
        ret = __wt_bad_object_type(session, uri);

    /* If we didn't find a metadata entry, map that error to ENOENT. */
    return (ret == WT_NOTFOUND ? ENOENT : ret);
}

/*
 * __wt_range_truncate --
 *     Truncate of a cursor range, default implementation. This truncate takes explicit cursors
 *     rather than a truncate information structure since it is used to implement truncate for
 *     column groups within a complex table, and those use different cursors than the API level
 *     truncate tracks.
 */
int
__wt_range_truncate(WT_CURSOR *start, WT_CURSOR *stop)
{
    WT_DECL_RET;
    int cmp;

    if (start == NULL) {
        do {
            WT_RET(stop->remove(stop));
        } while ((ret = stop->prev(stop)) == 0);
        WT_RET_NOTFOUND_OK(ret);
    } else {
        cmp = -1;
        do {
            if (stop != NULL)
                WT_RET(start->compare(start, stop, &cmp));
            WT_RET(start->remove(start));
        } while (cmp < 0 && (ret = start->next(start)) == 0);
        WT_RET_NOTFOUND_OK(ret);
    }
    return (0);
}

/*
 * __wt_schema_range_truncate --
 *     WT_SESSION::truncate with a range.
 */
int
__wt_schema_range_truncate(WT_TRUNCATE_INFO *trunc_info)
{
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    const char *uri;

    session = trunc_info->session;
    uri = trunc_info->uri;

    if (WT_IS_URI_HS(uri))
        ret = __wt_curhs_range_truncate(trunc_info);
    else if (WT_PREFIX_MATCH(uri, "file:")) {
        WT_ERR(__cursor_needkey(trunc_info->start));
        if (F_ISSET(trunc_info, WT_TRUNC_EXPLICIT_STOP))
            WT_ERR(__cursor_needkey(trunc_info->stop));
        WT_WITH_BTREE(
          session, CUR2BT(trunc_info->start), ret = __wt_btcur_range_truncate(trunc_info));
    } else if (WT_PREFIX_MATCH(uri, "table:"))
        ret = __wt_table_range_truncate(trunc_info);
    else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL && dsrc->range_truncate != NULL)
        ret = dsrc->range_truncate(dsrc, &session->iface, trunc_info->start, trunc_info->stop);
    else
        ret = __wt_range_truncate(trunc_info->start, trunc_info->stop);
err:
    return (ret);
}
