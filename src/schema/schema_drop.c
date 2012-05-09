/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __drop_file --
 *	Drop a file.
 */
static int
__drop_file(WT_SESSION_IMPL *session, const char *uri, int force)
{
	WT_DECL_RET;
	int exist;
	const char *filename;

	filename = uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (EINVAL);

	/* Close all btree handles associated with this file. */
	WT_RET(__wt_conn_btree_close_all(session, uri));

	/* Remove the metadata entry (ignore missing items). */
	WT_TRET(__wt_metadata_remove(session, uri));
	if (force && ret == WT_NOTFOUND)
		ret = 0;

	/* Remove the underlying physical file. */
	exist = 0;
	WT_TRET(__wt_exist(session, filename, &exist));
	if (exist)
		WT_TRET(__wt_remove(session, filename));

	return (ret);
}

/*
 * __drop_tree --
 *	Drop an index or colgroup reference.
 */
static int
__drop_tree(
    WT_SESSION_IMPL *session, const char *uri, int force)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *buf;

	btree = session->btree;
	buf = NULL;

	/* Remove the metadata entry (ignore missing items). */
	WT_TRET(__wt_metadata_remove(session, uri));
	if (force && ret == WT_NOTFOUND)
		ret = 0;

	/*
	 * Drop the file.
	 * __drop_file closes the WT_BTREE handle, so we copy the
	 * WT_BTREE->name field to save the URI.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_set(
	    session, buf, btree->name, strlen(btree->name) + 1));
	WT_ERR(__drop_file(session, buf->data, force));

err:	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __drop_colgroup --
 *	WT_SESSION::drop for a colgroup.
 */
static int
__drop_colgroup(
    WT_SESSION_IMPL *session, const char *uri, int force, const char *cfg[])
{
	WT_DECL_RET;
	WT_TABLE *table;
	const char *cgname, *tablename;
	size_t tlen;

	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "colgroup:"))
		return (EINVAL);
	cgname = strchr(tablename, ':');
	if (cgname != NULL) {
		tlen = (size_t)(cgname - tablename);
		++cgname;
	} else
		tlen = strlen(tablename);

	/*
	 * Try to get the btree handle.  Ideally, we would use an exclusive
	 * lock here to prevent access to the table while we are dropping it,
	 * but that conflicts with the exclusive lock taken by
	 * __wt_conn_btree_close_all.  If two threads race dropping
	 * the same object, it will be caught there.
	 *
	 * If we can't get a tree, try to remove it from the metadata.
	 */
	if ((ret = __wt_schema_get_btree(
	    session, uri, strlen(uri), cfg, WT_BTREE_NO_LOCK)) != 0) {
		if (ret == WT_NOTFOUND || ret == ENOENT)
			ret = 0;
		(void)__wt_metadata_remove(session, uri);
		return (ret);
	}

	/* If we can get the table, detach the colgroup from it. */
	if ((ret = __wt_schema_get_table(
	    session, tablename, tlen, &table)) == 0)
		table->cg_complete = 0;
	else if (ret == WT_NOTFOUND)
		ret = 0;

	WT_TRET(__drop_tree(session, uri, force));

	return (ret);
}

/*
 * __drop_index --
 *	WT_SESSION::drop for a colgroup.
 */
static int
__drop_index(
    WT_SESSION_IMPL *session, const char *uri, int force, const char *cfg[])
{
	WT_DECL_RET;
	WT_TABLE *table;
	const char *idxname, *tablename;
	size_t tlen;

	tablename = uri;
	if (!WT_PREFIX_SKIP(tablename, "index:") ||
	    (idxname = strchr(tablename, ':')) == NULL)
		return (EINVAL);
	tlen = (size_t)(idxname - tablename);
	++idxname;

	/*
	 * Try to get the btree handle.  Ideally, we would use an exclusive
	 * lock here to prevent access to the table while we are dropping it,
	 * but that conflicts with the exclusive lock taken by
	 * __wt_conn_btree_close_all.  If two threads race dropping
	 * the same object, it will be caught there.
	 *
	 * If we can't get a tree, try to remove it from the metadata.
	 */
	if ((ret = __wt_schema_get_btree(
	    session, uri, strlen(uri), cfg, WT_BTREE_NO_LOCK)) != 0) {
		if (ret == WT_NOTFOUND || ret == ENOENT)
			ret = 0;
		(void)__wt_metadata_remove(session, uri);
		return (ret);
	}

	/* If we can get the table, detach the index from it. */
	if ((ret = __wt_schema_get_table(
	    session, tablename, tlen, &table)) == 0)
		table->idx_complete = 0;
	else if (ret == WT_NOTFOUND)
		ret = 0;

	WT_TRET(__drop_tree(session, uri, force));

	return (ret);
}

/*
 * __drop_table --
 *	WT_SESSION::drop for a table.
 */
static int
__drop_table(
    WT_SESSION_IMPL *session, const char *uri, int force, const char *cfg[])
{
	WT_DECL_RET;
	WT_TABLE *table;
	int i;
	const char *name;

	name = uri;
	(void)WT_PREFIX_SKIP(name, "table:");

	WT_ERR(__wt_schema_get_table(session, name, strlen(name), &table));

	/* Drop the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if (table->cg_name[i] == NULL)
			continue;
		WT_TRET(__drop_colgroup(
		    session, table->cg_name[i], force, cfg));
	}

	/* Drop the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		if (table->idx_name[i] == NULL)
			continue;
		WT_TRET(__drop_index(session, table->idx_name[i], force, cfg));
	}

	WT_TRET(__wt_schema_remove_table(session, table));

	/* Remove the metadata entry (ignore missing items). */
	WT_TRET(__wt_metadata_remove(session, uri));

err:	if (force && ret == WT_NOTFOUND)
		ret = 0;
	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	int force;

	cval.val = 0;
	ret = __wt_config_gets(session, cfg, "force", &cval);
	if (ret != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	force = cval.val == 0 ? 0 : 1;

	/* Disallow drops from the WiredTiger name space. */
	WT_RET(__wt_schema_name_check(session, uri));

	if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __drop_colgroup(session, uri, force, cfg);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __drop_file(session, uri, force);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __drop_index(session, uri, force, cfg);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __drop_table(session, uri, force, cfg);
	else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0)
		ret = dsrc->drop(dsrc, &session->iface, uri, cfg[1]);

	/*
	 * Map WT_NOTFOUND to ENOENT (or to 0 if "force" is set), based on the
	 * assumption WT_NOTFOUND means there was no metadata entry.  The
	 * underlying drop functions should handle this case (we passed them
	 * the "force" value), but better safe than sorry.
	 */
	if (ret == WT_NOTFOUND)
		ret = force ? 0 : ENOENT;
	return (ret);
}
