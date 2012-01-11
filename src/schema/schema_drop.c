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
__wt_drop_file(WT_SESSION_IMPL *session, const char *name, int force)
{
	static const char *list[] = { "file", "root", "version", NULL };
	WT_BUF *buf;
	int exist, ret;
	const char **lp;

	buf = NULL;

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, name));

	/* Remove all of the schema table entries for this file. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	for (lp = list; *lp != NULL; ++lp) {
		WT_ERR(__wt_buf_fmt(session, buf, "%s:%s", *lp, name));

		/* Remove the schema table entry (ignore missing items). */
		WT_TRET(__wt_schema_table_remove(session, buf->data));
		if (force && ret == WT_NOTFOUND)
			ret = 0;
	}

	/* Remove the underlying physical file. */
	WT_ERR(__wt_exist(session, name, &exist));
	if (exist)
		WT_TRET(__wt_remove(session, name));

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __drop_tree --
 *	Drop an index or colgroup reference.
 */
static int
__drop_tree(WT_SESSION_IMPL *session, WT_BTREE *btree, int force)
{
	int ret;

	ret = 0;

	/* Remove the schema table entry (ignore missing items). */
	WT_TRET(__wt_schema_table_remove(session, btree->name));
	if (force && ret == WT_NOTFOUND)
		ret = 0;

	/* Remove the file. */
	WT_TRET(__wt_drop_file(session, btree->filename, force));

	return (ret);
}

/*
 * __drop_table --
 *	WT_SESSION::drop for a table.
 */
static int
__drop_table(WT_SESSION_IMPL *session, const char *uri, int force)
{
	WT_BTREE *btree;
	WT_TABLE *table;
	int i, ret;
	const char *name;

	ret = 0;

	name = uri;
	(void)WT_PREFIX_SKIP(name, "table:");

	WT_RET(__wt_schema_get_table(session, name, strlen(name), &table));

	/* Drop the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if ((btree = table->colgroup[i]) == NULL)
			continue;
		table->colgroup[i] = NULL;
		WT_TRET(__drop_tree(session, btree, force));
	}

	/* Drop the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		btree = table->index[i];
		table->index[i] = NULL;
		WT_TRET(__drop_tree(session, btree, force));
	}

	WT_TRET(__wt_schema_remove_table(session, table));

	/* Remove the schema table entry (ignore missing items). */
	WT_TRET(__wt_schema_table_remove(session, uri));
	if (force && ret == WT_NOTFOUND)
		ret = 0;

	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	int force, ret;

	cval.val = 0;
	ret = __wt_config_gets(session, cfg, "force", &cval);
	if (ret != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	force = cval.val == 0 ? 0 : 1;

	/* Disallow drops from the WiredTiger name space. */
	WT_RET(__wt_schema_name_check(session, uri));

	if (WT_PREFIX_SKIP(uri, "file:"))
		ret = __wt_drop_file(session, uri, force);
	else if (WT_PREFIX_MATCH(uri, "table:"))	/* NOT skip prefix */
		ret = __drop_table(session, uri, force);
	else
		return (__wt_unknown_object_type(session, uri));

	/* If we didn't find a schema file entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
