/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_curstat_colgroup_init --
 *	Initialize the statistics for a column group.
 */
int
__wt_curstat_colgroup_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	WT_COLGROUP *colgroup;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_RET(__wt_schema_get_colgroup(session, uri, 0, NULL, &colgroup));

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", colgroup->source));
	ret = __wt_curstat_init(session, buf->data, cfg, cst);

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_curstat_index_init --
 *	Initialize the statistics for an index.
 */
int
__wt_curstat_index_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_INDEX *idx;

	WT_RET(__wt_schema_get_index(session, uri, 0, NULL, &idx));

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", idx->source));
	ret = __wt_curstat_init(session, buf->data, cfg, cst);

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_curstat_table_init --
 *	Initialize the statistics for a table.
 */
int
__wt_curstat_table_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst)
{
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_DSRC_STATS *new, *stats;
	WT_TABLE *table;
	u_int i;
	const char *name;

	name = uri + strlen("table:");
	WT_RET(__wt_schema_get_table(session, name, strlen(name), 0, &table));

	WT_ERR(__wt_scr_alloc(session, 0, &buf));

	/*
	 * Process the column groups.
	 *
	 * Set the cursor to reference the data source statistics; we don't
	 * initialize it, instead we copy (rather than aggregate), the first
	 * column's statistics, which has the same effect.
	 */
	stats = &cst->u.dsrc_stats;
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		WT_ERR(__wt_buf_fmt(
		    session, buf, "statistics:%s", table->cgroups[i]->name));
		WT_ERR(__wt_curstat_open(
		    session, buf->data, cfg, &stat_cursor));
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
		WT_ERR(__wt_buf_fmt(
		    session, buf, "statistics:%s", table->indices[i]->name));
		WT_ERR(__wt_curstat_open(
		    session, buf->data, cfg, &stat_cursor));
		new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
		__wt_stat_dsrc_aggregate_single(new, stats);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

	__wt_curstat_dsrc_final(cst);

err:	__wt_schema_release_table(session, table);

	__wt_scr_free(session, &buf);
	return (ret);
}
