/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __truncate_file --
 *	WT_SESSION::truncate for a file.
 */
static int
__truncate_file(WT_SESSION_IMPL *session, const char *fileuri)
{
	const char *filename;

	filename = fileuri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (EINVAL);

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, fileuri));

	/* Delete the root address and truncate the file. */
	WT_RET(__wt_snapshot_clear(session, fileuri));
	WT_RET(__wt_btree_truncate(session, filename));

	return (0);
}

/*
 * __truncate_table --
 *	WT_SESSION::truncate for a table.
 */
static int
__truncate_table(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *namebuf;
	WT_TABLE *table;
	int i;

	WT_RET(__wt_schema_get_table(session, name, strlen(name), &table));
	WT_RET(__wt_scr_alloc(session, 0, &namebuf));
	/*
	 * We are closing the column groups, they must be reopened for future
	 * accesses to the table.
	 */
	table->cg_complete = 0;

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if ((btree = table->colgroup[i]) == NULL)
			continue;
		table->colgroup[i] = NULL;
		WT_ERR(__wt_buf_set(
		    session, namebuf, btree->name, strlen(btree->name) + 1));
		WT_TRET(__truncate_file(session, namebuf->data));
	}

	/* Truncate the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		if ((btree = table->index[i]) == NULL)
			continue;
		table->index[i] = NULL;
		WT_ERR(__wt_buf_set(
		    session, namebuf, btree->name, strlen(btree->name) + 1));
		WT_TRET(__truncate_file(session, namebuf->data));
	}

	/* Reopen the column groups. */
	if (ret == 0)
		ret = __wt_schema_open_colgroups(session, table);

err:	__wt_scr_free(&namebuf);
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
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	const char *tablename;

	WT_UNUSED(cfg);
	tablename = uri;

	if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __truncate_file(session, uri);
	else if (WT_PREFIX_SKIP(tablename, "table:"))
		ret = __truncate_table(session, tablename);
	else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0)
		ret = dsrc->truncate(dsrc, &session->iface, uri, cfg[1]);

	/* If we didn't find a metadata entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
