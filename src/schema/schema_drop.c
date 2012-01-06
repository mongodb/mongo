/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_drop_file --
 *	Drop a file.
 */
int
__wt_drop_file(WT_SESSION_IMPL *session, const char *fileuri, const char *cfg[])
{
	static const char *list[] = { "file", "root", "version", NULL };
	WT_BUF *buf;
	int exist, ret;
	const char *filename, **lp;

	WT_UNUSED(cfg);
	buf = NULL;

	filename = fileuri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'file:' URI: %s", fileuri);

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, filename));

	/* Remove all of the schema table entries for this file. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	for (lp = list; *lp != NULL; ++lp) {
		WT_ERR(__wt_buf_fmt(session, buf, "%s:%s", *lp, filename));
		WT_TRET(__wt_schema_table_remove(session, buf->data));
	}

	/* Remove the underlying physical file. */
	WT_ERR(__wt_exist(session, filename, &exist));
	if (exist)
		WT_TRET(__wt_remove(session, filename));

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __drop_tree --
 *	Drop a btree handle (which may be an index or colgroup).
 */
static int
__drop_tree(WT_SESSION_IMPL *session, WT_BTREE *btree, const char *cfg[])
{
	WT_BUF uribuf;
	const char *fileuri;
	int ret;

	ret = 0;
	WT_CLEAR(uribuf);
	WT_RET(__wt_buf_fmt(session, &uribuf, "file:%s", btree->filename));
	fileuri = uribuf.data;

	/* Remove the schema table entry. */
	WT_TRET(__wt_schema_table_remove(session, btree->name));

	/* Remove the file. */
	WT_TRET(__wt_drop_file(session, fileuri, cfg));

	__wt_buf_free(session, &uribuf);

	return (ret);
}

/*
 * __drop_table --
 *	WT_SESSION::drop for a table.
 */
static int
__drop_table(
    WT_SESSION_IMPL *session, const char *tableuri, const char *cfg[])
{
	WT_BTREE *btree;
	WT_TABLE *table;
	const char *tablename;
	int i, ret;

	ret = 0;

	tablename = tableuri;
	if (!WT_PREFIX_SKIP(tablename, "table:"))
		WT_RET_MSG(
		    session, EINVAL, "Expected a 'table:' URI: %s", tableuri);

	WT_RET(__wt_schema_get_table(session,
	    tablename, strlen(tablename), &table));

	/* Drop the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if ((btree = table->colgroup[i]) == NULL)
			continue;
		table->colgroup[i] = NULL;
		WT_TRET(__drop_tree(session, btree, cfg));
	}

	/* Drop the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		btree = table->index[i];
		table->index[i] = NULL;
		WT_TRET(__drop_tree(session, btree, cfg));
	}

	WT_TRET(__wt_schema_remove_table(session, table));
	WT_TRET(__wt_schema_table_remove(session, tableuri));

	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	int ret;

	WT_UNUSED(cfg);

	if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __wt_drop_file(session, uri, cfg);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __drop_table(session, uri, cfg);
	else
		return (__wt_unknown_object_type(session, uri));

	/* If we didn't find a schema file entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
