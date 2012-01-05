/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __truncate_file --
 *	Truncate a file.
 */
static int
__truncate_file(
    WT_SESSION_IMPL *session, const char *fileuri, const char *cfg[])
{
	WT_BUF *uribuf;
	int ret;
	const char *filename;

	WT_UNUSED(cfg);
	uribuf = NULL;
	ret = 0;

	filename = fileuri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'file:' URI: %s", fileuri);

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, filename));

	/* Delete the root address and truncate the file. */
	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));
	WT_ERR(__wt_buf_fmt(session, uribuf, "root:%s", filename));
	WT_ERR(__wt_schema_table_remove(session, uribuf->data));

	WT_ERR(__wt_btree_truncate(session, filename));

err:	__wt_scr_free(&uribuf);
	return (ret);
}

/*
 * __truncate_table --
 *	WT_SESSION::drop for a table.
 */
static int
__truncate_table(
    WT_SESSION_IMPL *session, const char *tableuri, const char *cfg[])
{
	WT_BUF *uribuf;
	WT_BTREE *btree;
	WT_TABLE *table;
	const char *tablename;
	int i, ret;

	WT_UNUSED(cfg);
	uribuf = NULL;
	ret = 0;

	tablename = tableuri;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'table:' URI: %s", tableuri);

	WT_RET(__wt_schema_get_table(
	    session, tablename, strlen(tablename), &table));

	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));

	/* Drop the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if ((btree = table->colgroup[i]) == NULL)
			continue;
		table->colgroup[i] = NULL;

		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "file:%s", btree->filename));
		WT_TRET(__truncate_file(session, uribuf->data, cfg));
	}

	/* Drop the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		btree = table->index[i];
		table->index[i] = NULL;

		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "file:%s", btree->filename));
		WT_TRET(__truncate_file(session, uribuf->data, cfg));
	}

err:	__wt_scr_free(&uribuf);
	return (ret);
}

int
__wt_schema_truncate(
    WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_UNUSED(cfg);

	if (WT_PREFIX_MATCH(uri, "file:"))
		return (__truncate_file(session, uri, cfg));
	if (WT_PREFIX_MATCH(uri, "table:"))
		return (__truncate_table(session, uri, cfg));

	return (__wt_unknown_object_type(session, uri));
}
