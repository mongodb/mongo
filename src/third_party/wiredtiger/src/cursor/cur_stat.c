/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * The statistics identifier is an offset from a base to ensure the integer ID values don't overlap
 * (the idea is if they overlap it's easy for application writers to confuse them).
 */
#define WT_STAT_KEY_MAX(cst) (((cst)->stats_base + (cst)->stats_count) - 1)
#define WT_STAT_KEY_MIN(cst) ((cst)->stats_base)
#define WT_STAT_KEY_OFFSET(cst) ((cst)->key - (cst)->stats_base)

/*
 * __curstat_print_value --
 *     Convert statistics cursor value to printable format.
 */
static int
__curstat_print_value(WT_SESSION_IMPL *session, uint64_t v, WT_ITEM *buf)
{
    if (v >= WT_BILLION)
        WT_RET(__wt_buf_fmt(session, buf, "%" PRIu64 "B (%" PRIu64 ")", v / WT_BILLION, v));
    else if (v >= WT_MILLION)
        WT_RET(__wt_buf_fmt(session, buf, "%" PRIu64 "M (%" PRIu64 ")", v / WT_MILLION, v));
    else
        WT_RET(__wt_buf_fmt(session, buf, "%" PRIu64, v));

    return (0);
}

/*
 * __curstat_get_key --
 *     WT_CURSOR->get_key for statistics cursors.
 */
static int
__curstat_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_ITEM *item;
    WT_SESSION_IMPL *session;
    size_t size;
    va_list ap;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, get_key, NULL);

    WT_ERR(__cursor_needkey(cursor));

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        WT_ERR(__wt_struct_size(session, &size, cursor->key_format, cst->key));
        WT_ERR(__wt_buf_initsize(session, &cursor->key, size));
        WT_ERR(__wt_struct_pack(session, cursor->key.mem, size, cursor->key_format, cst->key));

        va_start(ap, cursor);
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->key.data;
        item->size = cursor->key.size;
        va_end(ap);
    } else {
        va_start(ap, cursor);
        *va_arg(ap, int *) = cst->key;
        va_end(ap);
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curstat_get_value --
 *     WT_CURSOR->get_value for statistics cursors.
 */
static int
__curstat_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_ITEM *item;
    WT_SESSION_IMPL *session;
    size_t size;
    uint64_t *v;
    const char *desc, **p;
    va_list ap;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, get_value, NULL);

    WT_ERR(__cursor_needvalue(cursor));

    WT_ERR(cst->stats_desc(cst, WT_STAT_KEY_OFFSET(cst), &desc));
    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        WT_ERR(__wt_struct_size(session, &size, cursor->value_format, desc, cst->pv.data, cst->v));
        WT_ERR(__wt_buf_initsize(session, &cursor->value, size));
        WT_ERR(__wt_struct_pack(
          session, cursor->value.mem, size, cursor->value_format, desc, cst->pv.data, cst->v));

        va_start(ap, cursor);
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->value.data;
        item->size = cursor->value.size;
        va_end(ap);
    } else {
        /*
         * Don't drop core if the statistics value isn't requested; NULL pointer support isn't
         * documented, but it's a cheap test.
         */
        va_start(ap, cursor);
        if ((p = va_arg(ap, const char **)) != NULL)
            *p = desc;
        if ((p = va_arg(ap, const char **)) != NULL)
            *p = cst->pv.data;
        if ((v = va_arg(ap, uint64_t *)) != NULL)
            *v = cst->v;
        va_end(ap);
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curstat_set_keyv --
 *     WT_CURSOR->set_key for statistics cursors.
 */
static int
__curstat_set_keyv(WT_CURSOR *cursor, va_list ap)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_ITEM *item;
    WT_SESSION_IMPL *session;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, set_key, NULL);
    F_CLR(cursor, WT_CURSTD_KEY_SET);

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        item = va_arg(ap, WT_ITEM *);
        ret = __wt_struct_unpack(session, item->data, item->size, cursor->key_format, &cst->key);
    } else
        cst->key = va_arg(ap, int);

    if ((cursor->saved_err = ret) == 0)
        F_SET(cursor, WT_CURSTD_KEY_EXT);

err:
    API_END_RET(session, ret);
}

/*
 * __curstat_set_key --
 *     WT_CURSOR->set_key for statistics cursors.
 */
static void
__curstat_set_key(WT_CURSOR *cursor, ...)
{
    va_list ap;

    va_start(ap, cursor);
    WT_IGNORE_RET(__curstat_set_keyv(cursor, ap));
    va_end(ap);
}

/*
 * __curstat_set_value --
 *     WT_CURSOR->set_value for statistics cursors.
 */
static void
__curstat_set_value(WT_CURSOR *cursor, ...)
{
    WT_UNUSED(cursor);
}

/*
 * __curstat_next --
 *     WT_CURSOR->next method for the statistics cursor type.
 */
static int
__curstat_next(WT_CURSOR *cursor)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, next, NULL);

    /* Initialize on demand. */
    if (cst->notinitialized) {
        WT_ERR(__wt_curstat_init(session, cursor->internal_uri, NULL, cst->cfg, cst));
        cst->notinitialized = false;
    }

    /* Move to the next item. */
    if (cst->notpositioned) {
        cst->notpositioned = false;
        cst->key = WT_STAT_KEY_MIN(cst);
        if (cst->next_set != NULL)
            WT_ERR((*cst->next_set)(session, cst, true, true));
    } else if (cst->key < WT_STAT_KEY_MAX(cst))
        ++cst->key;
    else if (cst->next_set != NULL)
        WT_ERR((*cst->next_set)(session, cst, true, false));
    else
        WT_ERR(WT_NOTFOUND);

    cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
    WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

    if (0) {
err:
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    }
    API_END_RET(session, ret);
}

/*
 * __curstat_prev --
 *     WT_CURSOR->prev method for the statistics cursor type.
 */
static int
__curstat_prev(WT_CURSOR *cursor)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, prev, NULL);

    /* Initialize on demand. */
    if (cst->notinitialized) {
        WT_ERR(__wt_curstat_init(session, cursor->internal_uri, NULL, cst->cfg, cst));
        cst->notinitialized = false;
    }

    /* Move to the previous item. */
    if (cst->notpositioned) {
        cst->notpositioned = false;
        cst->key = WT_STAT_KEY_MAX(cst);
        if (cst->next_set != NULL)
            WT_ERR((*cst->next_set)(session, cst, false, true));
    } else if (cst->key > WT_STAT_KEY_MIN(cst))
        --cst->key;
    else if (cst->next_set != NULL)
        WT_ERR((*cst->next_set)(session, cst, false, false));
    else
        WT_ERR(WT_NOTFOUND);

    cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
    WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

    if (0) {
err:
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    }
    API_END_RET(session, ret);
}

/*
 * __curstat_reset --
 *     WT_CURSOR->reset method for the statistics cursor type.
 */
static int
__curstat_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    cst->notinitialized = cst->notpositioned = true;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    /* Reset the session statistics to zero. */
    if (strcmp(cursor->uri, "statistics:session") == 0)
        __wt_stat_session_clear_single(&session->stats);

err:
    API_END_RET(session, ret);
}

/*
 * __curstat_search --
 *     WT_CURSOR->search method for the statistics cursor type.
 */
static int
__curstat_search(WT_CURSOR *cursor)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL(cursor, session, search, NULL);

    WT_ERR(__cursor_needkey(cursor));
    F_CLR(cursor, WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_SET);

    /* Initialize on demand. */
    if (cst->notinitialized) {
        WT_ERR(__wt_curstat_init(session, cursor->internal_uri, NULL, cst->cfg, cst));
        cst->notinitialized = false;
    }

    if (cst->key < WT_STAT_KEY_MIN(cst) || cst->key > WT_STAT_KEY_MAX(cst))
        WT_ERR(WT_NOTFOUND);

    cst->v = (uint64_t)cst->stats[WT_STAT_KEY_OFFSET(cst)];
    WT_ERR(__curstat_print_value(session, cst->v, &cst->pv));
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __curstat_close --
 *     WT_CURSOR->close method for the statistics cursor type.
 */
static int
__curstat_close(WT_CURSOR *cursor)
{
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t i;

    cst = (WT_CURSOR_STAT *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    if (cst->cfg != NULL) {
        for (i = 0; cst->cfg[i] != NULL; ++i)
            __wt_free(session, cst->cfg[i]);
        __wt_free(session, cst->cfg);
    }

    __wt_buf_free(session, &cst->pv);
    __wt_free(session, cst->desc_buf);

    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __curstat_conn_init --
 *     Initialize the statistics for a connection.
 */
static void
__curstat_conn_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /*
     * Fill in the connection statistics, and copy them to the cursor. Optionally clear the
     * connection statistics.
     */
    __wt_conn_stat_init(session);
    __wt_stat_connection_init_single(&cst->u.conn_stats);
    __wt_stat_connection_aggregate(conn->stats, &cst->u.conn_stats);
    if (F_ISSET(cst, WT_STAT_CLEAR))
        __wt_stat_connection_clear_all(conn->stats);

    cst->stats = (int64_t *)&cst->u.conn_stats;
    cst->stats_base = WT_CONNECTION_STATS_BASE;
    cst->stats_count = sizeof(WT_CONNECTION_STATS) / sizeof(int64_t);
    cst->stats_desc = __wt_stat_connection_desc;
}

/*
 * __curstat_file_init --
 *     Initialize the statistics for a file.
 */
static int
__curstat_file_init(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    wt_off_t size;
    const char *filename;

    /*
     * If we are only getting the size of the file, we don't need to open the tree.
     */
    if (F_ISSET(cst, WT_STAT_TYPE_SIZE)) {
        filename = uri;
        if (!WT_PREFIX_SKIP(filename, "file:"))
            return (__wt_unexpected_object_type(session, uri, "file:"));
        __wt_stat_dsrc_init_single(&cst->u.dsrc_stats);
        WT_RET(__wt_block_manager_named_size(session, filename, &size));
        cst->u.dsrc_stats.block_size = size;
        __wt_curstat_dsrc_final(cst);
        return (0);
    }

    WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, 0, NULL, NULL));
    dhandle = session->dhandle;

    /*
     * Fill in the data source statistics, and copy them to the cursor. Optionally clear the data
     * source statistics.
     */
    if ((ret = __wt_btree_stat_init(session, cst)) == 0) {
        __wt_stat_dsrc_init_single(&cst->u.dsrc_stats);
        __wt_stat_dsrc_aggregate(dhandle->stats, &cst->u.dsrc_stats);
        if (F_ISSET(cst, WT_STAT_CLEAR))
            __wt_stat_dsrc_clear_all(dhandle->stats);
        __wt_curstat_dsrc_final(cst);
    }

    /* Release the handle, we're done with it. */
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_curstat_dsrc_final --
 *     Finalize a data-source statistics cursor.
 */
void
__wt_curstat_dsrc_final(WT_CURSOR_STAT *cst)
{
    cst->stats = (int64_t *)&cst->u.dsrc_stats;
    cst->stats_base = WT_DSRC_STATS_BASE;
    cst->stats_count = sizeof(WT_DSRC_STATS) / sizeof(int64_t);
    cst->stats_desc = __wt_stat_dsrc_desc;
}

/*
 * __curstat_join_next_set --
 *     Advance to another index used in a join to give another set of statistics.
 */
static int
__curstat_join_next_set(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst, bool forw, bool init)
{
    WT_CURSOR_JOIN *cjoin;
    WT_JOIN_STATS_GROUP *join_group;
    ssize_t pos;

    join_group = &cst->u.join_stats_group;
    cjoin = join_group->join_cursor;
    if (init)
        pos = forw ? 0 : (ssize_t)cjoin->entries_next - 1;
    else
        pos = join_group->join_cursor_entry + (forw ? 1 : -1);
    if (pos < 0 || (size_t)pos >= cjoin->entries_next)
        return (WT_NOTFOUND);

    join_group->join_cursor_entry = pos;
    if (cjoin->entries[pos].index == NULL) {
        WT_ASSERT(session, WT_PREFIX_MATCH(cjoin->iface.uri, "join:"));
        join_group->desc_prefix = cjoin->iface.uri + 5;
    } else
        join_group->desc_prefix = cjoin->entries[pos].index->name;
    join_group->join_stats = cjoin->entries[pos].stats;
    if (!init)
        cst->key = forw ? WT_STAT_KEY_MIN(cst) : WT_STAT_KEY_MAX(cst);
    return (0);
}

/*
 * __curstat_join_desc --
 *     Assemble the description field based on current index and statistic.
 */
static int
__curstat_join_desc(WT_CURSOR_STAT *cst, int slot, const char **resultp)
{
    WT_JOIN_STATS_GROUP *sgrp;
    WT_SESSION_IMPL *session;
    size_t len;
    const char *static_desc;

    sgrp = &cst->u.join_stats_group;
    session = CUR2S(sgrp->join_cursor);
    WT_RET(__wt_stat_join_desc(cst, slot, &static_desc));

    /*
     * We conceptually want to insert the index name between the "join: " and the following
     * description. Skip past the first part.
     */
    WT_PREFIX_SKIP_REQUIRED(session, static_desc, "join: ");
    len = strlen("join: ") + strlen(sgrp->desc_prefix) + strlen(": ") + strlen(static_desc) + 1;
    WT_RET(__wt_realloc_noclear(session, NULL, len, &cst->desc_buf));
    WT_RET(__wt_snprintf(cst->desc_buf, len, "join: %s: %s", sgrp->desc_prefix, static_desc));
    *resultp = cst->desc_buf;
    return (0);
}

/*
 * __curstat_join_init --
 *     Initialize the statistics for a joined cursor.
 */
static int
__curstat_join_init(
  WT_SESSION_IMPL *session, WT_CURSOR *curjoin, const char *cfg[], WT_CURSOR_STAT *cst)
{
    WT_CURSOR_JOIN *cjoin;

    WT_UNUSED(cfg);

    if (curjoin == NULL && cst->u.join_stats_group.join_cursor != NULL)
        curjoin = &cst->u.join_stats_group.join_cursor->iface;
    if (curjoin == NULL || !WT_PREFIX_MATCH(curjoin->uri, "join:"))
        WT_RET_MSG(session, EINVAL, "join cursor must be used with statistics:join");
    cjoin = (WT_CURSOR_JOIN *)curjoin;
    memset(&cst->u.join_stats_group, 0, sizeof(WT_JOIN_STATS_GROUP));
    cst->u.join_stats_group.join_cursor = cjoin;

    cst->stats = (int64_t *)&cst->u.join_stats_group.join_stats;
    cst->stats_base = WT_JOIN_STATS_BASE;
    cst->stats_count = sizeof(WT_JOIN_STATS) / sizeof(int64_t);
    cst->stats_desc = __curstat_join_desc;
    cst->next_set = __curstat_join_next_set;
    return (0);
}

/*
 * __curstat_session_init --
 *     Initialize the statistics for a session.
 */
static void
__curstat_session_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
    /*
     * Copy stats from the session to the cursor. Optionally clear the session's statistics.
     */
    memcpy(&cst->u.session_stats, &session->stats, sizeof(WT_SESSION_STATS));
    if (F_ISSET(cst, WT_STAT_CLEAR))
        __wt_stat_session_clear_single(&session->stats);

    cst->stats = (int64_t *)&cst->u.session_stats;
    cst->stats_base = WT_SESSION_STATS_BASE;
    cst->stats_count = sizeof(WT_SESSION_STATS) / sizeof(int64_t);
    cst->stats_desc = __wt_stat_session_desc;
}

/*
 * __wt_curstat_init --
 *     Initialize a statistics cursor.
 */
int
__wt_curstat_init(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *curjoin, const char *cfg[],
  WT_CURSOR_STAT *cst)
{
    const char *dsrc_uri;

    if (strcmp(uri, "statistics:") == 0) {
        __curstat_conn_init(session, cst);
        return (0);
    }

    /* Data source statistics are only available after recovery completes. */
    WT_ASSERT(session, F_ISSET(S2C(session), WT_CONN_RECOVERY_COMPLETE));
    dsrc_uri = uri + strlen("statistics:");

    if (strcmp(dsrc_uri, "join") == 0)
        WT_RET(__curstat_join_init(session, curjoin, cfg, cst));
    else if (strcmp(dsrc_uri, "session") == 0) {
        __curstat_session_init(session, cst);
        return (0);
    } else if (WT_PREFIX_MATCH(dsrc_uri, "colgroup:"))
        WT_RET(__wt_curstat_colgroup_init(session, dsrc_uri, cfg, cst));
    else if (WT_PREFIX_MATCH(dsrc_uri, "file:"))
        WT_RET(__curstat_file_init(session, dsrc_uri, cfg, cst));
    else if (WT_PREFIX_MATCH(dsrc_uri, "index:"))
        WT_RET(__wt_curstat_index_init(session, dsrc_uri, cfg, cst));
    else if (WT_PREFIX_MATCH(dsrc_uri, "lsm:"))
        WT_RET(__wt_curstat_lsm_init(session, dsrc_uri, cst));
    else if (WT_PREFIX_MATCH(dsrc_uri, "table:"))
        WT_RET(__wt_curstat_table_init(session, dsrc_uri, cfg, cst));
    else
        return (__wt_bad_object_type(session, uri));

    return (0);
}

/*
 * __wt_curstat_open --
 *     WT_SESSION->open_cursor method for the statistics cursor type.
 */
int
__wt_curstat_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR_STATIC_INIT(iface, __curstat_get_key, /* get-key */
      __curstat_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value_notsup,         /* get-raw-key-value */
      __curstat_set_key,                            /* set-key */
      __curstat_set_value,                          /* set-value */
      __wt_cursor_compare_notsup,                   /* compare */
      __wt_cursor_equals_notsup,                    /* equals */
      __curstat_next,                               /* next */
      __curstat_prev,                               /* prev */
      __curstat_reset,                              /* reset */
      __curstat_search,                             /* search */
      __wt_cursor_search_near_notsup,               /* search-near */
      __wt_cursor_notsup,                           /* insert */
      __wt_cursor_modify_notsup,                    /* modify */
      __wt_cursor_notsup,                           /* update */
      __wt_cursor_notsup,                           /* remove */
      __wt_cursor_notsup,                           /* reserve */
      __wt_cursor_config_notsup,                    /* reconfigure */
      __wt_cursor_notsup,                           /* largest_key */
      __wt_cursor_config_notsup,                    /* bound */
      __wt_cursor_notsup,                           /* cache */
      __wt_cursor_reopen_notsup,                    /* reopen */
      __wt_cursor_checkpoint_id,                    /* checkpoint ID */
      __curstat_close);                             /* close */
    WT_CONFIG_ITEM cval, sval;
    WT_CURSOR *cursor;
    WT_CURSOR_STAT *cst;
    WT_DECL_RET;
    size_t i;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_STAT, iface) == 0);

    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &cst));
    cursor = (WT_CURSOR *)cst;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;

    /*
     * Statistics cursor configuration: must match (and defaults to), the database configuration.
     */
    if (!WT_STAT_ENABLED(session))
        goto config_err;
    ret = __wt_config_gets(session, cfg, "statistics", &cval);
    WT_ERR_NOTFOUND_OK(ret, true);
    if (ret == 0) {
        if ((ret = __wt_config_subgets(session, &cval, "all", &sval)) == 0 && sval.val != 0) {
            if (!FLD_ISSET(conn->stat_flags, WT_STAT_TYPE_ALL))
                goto config_err;
            F_SET(cst,
              WT_STAT_TYPE_ALL | WT_STAT_TYPE_CACHE_WALK | WT_STAT_TYPE_FAST |
                WT_STAT_TYPE_TREE_WALK);
        }
        WT_ERR_NOTFOUND_OK(ret, false);
        if ((ret = __wt_config_subgets(session, &cval, "fast", &sval)) == 0 && sval.val != 0) {
            if (F_ISSET(cst, WT_STAT_TYPE_ALL))
                WT_ERR_MSG(session, EINVAL,
                  "Only one of all, fast, none configuration values should be specified");
            F_SET(cst, WT_STAT_TYPE_FAST);
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        if ((ret = __wt_config_subgets(session, &cval, "cache_walk", &sval)) == 0 &&
          sval.val != 0) {
            /*
             * Configuring cache walk statistics implies fast statistics. Keep that knowledge
             * internal for now - it may change in the future.
             */
            F_SET(cst, WT_STAT_TYPE_CACHE_WALK | WT_STAT_TYPE_FAST);
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        if ((ret = __wt_config_subgets(session, &cval, "tree_walk", &sval)) == 0 && sval.val != 0) {
            /*
             * Configuring tree walk statistics implies fast statistics. Keep that knowledge
             * internal for now - it may change in the future.
             */
            F_SET(cst, WT_STAT_TYPE_FAST | WT_STAT_TYPE_TREE_WALK);
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        if ((ret = __wt_config_subgets(session, &cval, "size", &sval)) == 0 && sval.val != 0) {
            if (F_ISSET(cst, WT_STAT_TYPE_FAST | WT_STAT_TYPE_ALL))
                WT_ERR_MSG(session, EINVAL,
                  "Only one of all, fast, none configuration values should be specified");
            F_SET(cst, WT_STAT_TYPE_SIZE);
        }
        WT_ERR_NOTFOUND_OK(ret, false);
        if ((ret = __wt_config_subgets(session, &cval, "clear", &sval)) == 0 && sval.val != 0) {
            if (F_ISSET(cst, WT_STAT_TYPE_SIZE))
                WT_ERR_MSG(session, EINVAL, "clear is incompatible with size statistics");
            F_SET(cst, WT_STAT_CLEAR);
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        /* If no configuration, use the connection's configuration. */
        if (cst->flags == 0) {
            if (FLD_ISSET(conn->stat_flags, WT_STAT_TYPE_ALL))
                F_SET(cst, WT_STAT_TYPE_ALL);
            if (FLD_ISSET(conn->stat_flags, WT_STAT_TYPE_CACHE_WALK))
                F_SET(cst, WT_STAT_TYPE_CACHE_WALK);
            if (FLD_ISSET(conn->stat_flags, WT_STAT_TYPE_FAST))
                F_SET(cst, WT_STAT_TYPE_FAST);
            if (FLD_ISSET(conn->stat_flags, WT_STAT_TYPE_TREE_WALK))
                F_SET(cst, WT_STAT_TYPE_TREE_WALK);
        }

        /* If the connection configures clear, so do we. */
        if (FLD_ISSET(conn->stat_flags, WT_STAT_CLEAR))
            F_SET(cst, WT_STAT_CLEAR);
    }

    /*
     * We return the statistics field's offset as the key, and a string description, a string value,
     * and a int64_t value as the value columns.
     */
    cursor->key_format = "i";
    cursor->value_format = "SSq";

    /*
     * WT_CURSOR.reset on a statistics cursor refreshes the cursor, save the cursor's configuration
     * for that.
     */
    for (i = 0; cfg[i] != NULL; ++i)
        ;
    WT_ERR(__wt_calloc_def(session, i + 1, &cst->cfg));
    for (i = 0; cfg[i] != NULL; ++i)
        WT_ERR(__wt_strdup(session, cfg[i], &cst->cfg[i]));

    /*
     * Do the initial statistics snapshot: there won't be cursor operations to trigger
     * initialization with aggregating statistics for upper-level objects like tables so we need a
     * valid set of statistics before the open returns.
     */
    WT_ERR(__wt_curstat_init(session, uri, other, cst->cfg, cst));
    cst->notinitialized = false;

    /* The cursor isn't yet positioned. */
    cst->notpositioned = true;

    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
config_err:
        WT_ERR_MSG(session, EINVAL,
          "cursor's statistics configuration doesn't match the database statistics configuration");
    }

    if (0) {
err:
        WT_TRET(__curstat_close(cursor));
        *cursorp = NULL;
    }

    return (ret);
}
