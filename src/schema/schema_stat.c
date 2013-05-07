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
	WT_COLGROUP *colgroup;
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_DSRC_STATS *stats;
	WT_INDEX *idx;
	WT_TABLE *table;
	const char *desc, *name, *pvalue;
	uint64_t value;
	u_int i;
	int stat_key;

	WT_UNUSED(flags);
	name = uri + strlen("table:");

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_schema_get_table(session, name, strlen(name), 0, &table));

	/* Clear the statistics we are about to recalculate. */
	if (cst->stats != NULL) {
		__wt_stat_clear_dsrc_stats(cst->stats);
		stats = (WT_DSRC_STATS *)cst->stats;
	} else {
		WT_ERR(__wt_calloc_def(session, 1, &stats));
		__wt_stat_init_dsrc_stats(stats);
		cst->stats_first = cst->stats = (WT_STATS *)stats;
		cst->stats_count = sizeof(*stats) / sizeof(WT_STATS);
	}

	/* Process the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		colgroup = table->cgroups[i];

		WT_ERR(__wt_buf_fmt(
		    session, buf, "statistics:%s", colgroup->name));
		WT_ERR(__wt_curstat_open(
		    session, buf->data, cfg, &stat_cursor));

		while ((ret = stat_cursor->next(stat_cursor)) == 0) {
			WT_ERR(stat_cursor->get_key(stat_cursor, &stat_key));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRKV(session, stats, stat_key, value);
		}
		WT_ERR_NOTFOUND_OK(ret);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

	/* Process the indices. */
	WT_ERR(__wt_schema_open_indices(session, table));
	for (i = 0; i < table->nindices; i++) {
		idx = table->indices[i];

		WT_ERR(__wt_buf_fmt(
		    session, buf, "statistics:%s", idx->name));
		WT_ERR(__wt_curstat_open(
		    session, buf->data, cfg, &stat_cursor));

		while ((ret = stat_cursor->next(stat_cursor)) == 0) {
			WT_ERR(stat_cursor->get_key(stat_cursor, &stat_key));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRKV(session, stats, stat_key, value);
		}
		WT_ERR_NOTFOUND_OK(ret);
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
