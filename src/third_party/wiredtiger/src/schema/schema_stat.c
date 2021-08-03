/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_curstat_colgroup_init --
 *     Initialize the statistics for a column group.
 */
int
__wt_curstat_colgroup_init(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
    WT_COLGROUP *colgroup;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;

    WT_RET(__wt_schema_get_colgroup(session, uri, false, NULL, &colgroup));

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", colgroup->source));
    ret = __wt_curstat_init(session, buf->data, NULL, cfg, cst);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_curstat_index_init --
 *     Initialize the statistics for an index.
 */
int
__wt_curstat_index_init(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_INDEX *idx;

    WT_RET(__wt_schema_get_index(session, uri, false, false, &idx));

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", idx->source));
    ret = __wt_curstat_init(session, buf->data, NULL, cfg, cst);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __curstat_size_only --
 *     For very simple tables we can avoid getting table handles if configured to only retrieve the
 *     size. It's worthwhile because workloads that create and drop a lot of tables can put a lot of
 *     pressure on the table list lock.
 */
static int
__curstat_size_only(WT_SESSION_IMPL *session, const char *uri, bool *was_fast, WT_CURSOR_STAT *cst)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, colconf, cval;
    WT_DECL_RET;
    WT_ITEM namebuf;
    wt_off_t filesize;
    char *tableconf;
    bool exist;

    WT_CLEAR(namebuf);
    *was_fast = false;

    /* Retrieve the metadata for this table. */
    WT_RET(__wt_metadata_search(session, uri, &tableconf));

    /*
     * The fast path only works if the table consists of a single file and does not have any
     * indexes. The absence of named columns is how we determine that neither of those conditions
     * can be satisfied.
     */
    WT_ERR(__wt_config_getones(session, tableconf, "columns", &colconf));
    __wt_config_subinit(session, &cparser, &colconf);
    if ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
        goto err;

    /* Build up the file name from the table URI. */
    WT_ERR(__wt_buf_fmt(session, &namebuf, "%s.wt", uri + strlen("table:")));

    /*
     * Get the size of the underlying file. This will fail for anything other than simple tables
     * (LSM for example) and will fail if there are concurrent schema level operations (for example
     * drop). That is fine - failing here results in falling back to the slow path of opening the
     * handle.
     */
    WT_ERR(__wt_fs_exist(session, namebuf.data, &exist));
    if (exist) {
        WT_ERR(__wt_fs_size(session, namebuf.data, &filesize));

        /* Setup and populate the statistics structure */
        __wt_stat_dsrc_init_single(&cst->u.dsrc_stats);
        cst->u.dsrc_stats.block_size = filesize;
        __wt_curstat_dsrc_final(cst);

        *was_fast = true;
    }

err:
    __wt_free(session, tableconf);
    __wt_buf_free(session, &namebuf);

    return (ret);
}

/*
 * __wt_curstat_table_init --
 *     Initialize the statistics for a table.
 */
int
__wt_curstat_table_init(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
    WT_CURSOR *stat_cursor;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_DSRC_STATS *new, *stats;
    WT_TABLE *table;
    u_int i;
    const char *name;
    bool was_fast;

    /*
     * If only gathering table size statistics, try a fast path that avoids the schema and table
     * list locks.
     */
    if (F_ISSET(cst, WT_STAT_TYPE_SIZE)) {
        WT_RET(__curstat_size_only(session, uri, &was_fast, cst));
        if (was_fast)
            return (0);
    }

    name = uri + strlen("table:");
    WT_RET(__wt_schema_get_table(session, name, strlen(name), false, 0, &table));

    WT_ERR(__wt_scr_alloc(session, 0, &buf));

    /*
     * Process the column groups.
     *
     * Set the cursor to reference the data source statistics; we don't initialize it, instead we
     * copy (rather than aggregate), the first column's statistics, which has the same effect.
     */
    stats = &cst->u.dsrc_stats;
    for (i = 0; i < WT_COLGROUPS(table); i++) {
        WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", table->cgroups[i]->name));
        WT_ERR(__wt_curstat_open(session, buf->data, NULL, cfg, &stat_cursor));
        new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
        if (i == 0)
            *stats = *new;
        else
            __wt_stat_dsrc_aggregate_single(new, stats);
        WT_ERR(stat_cursor->close(stat_cursor));
    }

    /* Process the indices. */
    WT_ERR(__wt_schema_open_indices(session, table));
    for (i = 0; i < table->nindices; i++) {
        WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", table->indices[i]->name));
        WT_ERR(__wt_curstat_open(session, buf->data, NULL, cfg, &stat_cursor));
        new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
        __wt_stat_dsrc_aggregate_single(new, stats);
        WT_ERR(stat_cursor->close(stat_cursor));
    }

    __wt_curstat_dsrc_final(cst);

err:
    WT_TRET(__wt_schema_release_table(session, &table));

    __wt_scr_free(session, &buf);
    return (ret);
}
