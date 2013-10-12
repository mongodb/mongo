/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curstat_colgroup_init --
 *	Initialize the statistics for a column group.
 */
static int
__curstat_colgroup_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_COLGROUP *colgroup;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_RET(__wt_schema_get_colgroup(session, uri, NULL, &colgroup));

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", colgroup->source));
	ret = __wt_curstat_init(session, buf->data, cfg, cst, flags);

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __curstat_index_init --
 *	Initialize the statistics for an index.
 */
static int
__curstat_index_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_INDEX *idx;

	WT_RET(__wt_schema_get_index(session, uri, NULL, &idx));

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "statistics:%s", idx->source));
	ret = __wt_curstat_init(session, buf->data, cfg, cst, flags);

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __curstat_table_init --
 *	Initialize the statistics for a table.
 */
static int
__curstat_table_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_DSRC_STATS *new, *stats;
	WT_TABLE *table;
	u_int i;
	const char *name;

	WT_UNUSED(flags);
	name = uri + strlen("table:");

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_schema_get_table(session, name, strlen(name), 0, &table));

	/*
	 * Allocate an aggregated statistics structure as necessary.  Don't
	 * initialize it, instead we copy (rather than aggregate), the first
	 * column's statistics, which has the same effect.
	 */
	if ((stats = (WT_DSRC_STATS *)cst->stats) == NULL) {
		WT_ERR(__wt_calloc_def(session, 1, &stats));
		cst->stats_first = cst->stats = (WT_STATS *)stats;
		cst->stats_count = sizeof(WT_DSRC_STATS) / sizeof(WT_STATS);
	}

	/* Process the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		WT_ERR(__wt_buf_fmt(
		    session, buf, "statistics:%s", table->cgroups[i]->name));
		WT_ERR(__wt_curstat_open(
		    session, buf->data, cfg, &stat_cursor));
		new = (WT_DSRC_STATS *)WT_CURSOR_STATS(stat_cursor);
		if (i == 0)
			*stats = *new;
		else
			__wt_stat_aggregate_dsrc_stats(new, stats);
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
		__wt_stat_aggregate_dsrc_stats(new, stats);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

err:	__wt_scr_free(&buf);
	__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __wt_schema_stat_init --
 *	Configure a statistics cursor for a schema-level object.
 */
int
__wt_schema_stat_init(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR_STAT *cst, uint32_t flags)
{
	const char *dsrc_uri;

	dsrc_uri = uri + strlen("statistics:");

	if (WT_PREFIX_MATCH(dsrc_uri, "colgroup:"))
		return (__curstat_colgroup_init(
		    session, dsrc_uri, cfg, cst, flags));
	else if (WT_PREFIX_MATCH(dsrc_uri, "index:"))
		return (__curstat_index_init(
		    session, dsrc_uri, cfg, cst, flags));
	else if (WT_PREFIX_MATCH(dsrc_uri, "table:"))
		return (__curstat_table_init(
		    session, dsrc_uri, cfg, cst, flags));

	return (__wt_bad_object_type(session, uri));
}
