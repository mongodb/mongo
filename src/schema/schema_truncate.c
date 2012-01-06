/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __truncate_file --
 *	WT_SESSION::truncate for a file.
 */
static int
__truncate_file(WT_SESSION_IMPL *session, const char *name)
{
	WT_BUF *uribuf;
	int ret;

	uribuf = NULL;
	ret = 0;

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, name));

	/* Delete the root address and truncate the file. */
	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));
	WT_ERR(__wt_buf_fmt(session, uribuf, "root:%s", name));
	WT_ERR(__wt_schema_table_remove(session, uribuf->data));

	WT_ERR(__wt_btree_truncate(session, name));

err:	__wt_scr_free(&uribuf);
	return (ret);
}

/*
 * __truncate_table --
 *	WT_SESSION::truncate for a table.
 */
static int
__truncate_table(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE *btree;
	WT_TABLE *table;
	int i, ret;

	ret = 0;

	WT_RET(__wt_schema_get_table(session, name, strlen(name), &table));

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if ((btree = table->colgroup[i]) == NULL)
			continue;
		table->colgroup[i] = NULL;
		WT_TRET(__truncate_file(session, btree->filename));
	}

	/* Truncate the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		btree = table->index[i];
		table->index[i] = NULL;
		WT_TRET(__truncate_file(session, btree->filename));
	}

	return (ret);
}

/*
 * __wt_schema_truncate --
 *	WT_SESSION::truncate.
 */
int
__wt_schema_truncate(
    WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	int ret;

	WT_UNUSED(cfg);

	if (WT_PREFIX_SKIP(uri, "file:"))
		ret = __truncate_file(session, uri);
	else if (WT_PREFIX_SKIP(uri, "table:"))
		ret = __truncate_table(session, uri);
	else
		return (__wt_unknown_object_type(session, uri));

	/* If we didn't find a schema file entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
