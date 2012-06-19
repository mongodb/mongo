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
__truncate_file(WT_SESSION_IMPL *session, const char *name)
{
	const char *filename;

	filename = name;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (EINVAL);

	/* Close any btree handles in the file. */
	WT_RET(__wt_conn_btree_close_all(session, name));

	/* Delete the root address and truncate the file. */
	WT_RET(__wt_meta_checkpoint_clear(session, name));
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
	int i, tret;

	WT_RET(__wt_schema_get_table(session, name, strlen(name), &table));
	WT_RET(__wt_scr_alloc(session, 0, &namebuf));

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		/*
		 * Get an exclusive lock on the handle: it will be released by
		 * __wt_conn_btree_close_all.
		 */
		if ((tret = __wt_schema_get_btree(session,
		    table->cg_name[i], strlen(table->cg_name[i]),
		    NULL, WT_BTREE_EXCLUSIVE)) != 0) {
			WT_TRET(tret);
			continue;
		}
		btree = session->btree;
		WT_ERR(__wt_buf_set(
		    session, namebuf, btree->name, strlen(btree->name) + 1));
		WT_TRET(__truncate_file(session, namebuf->data));
	}

	/* Truncate the indices. */
	WT_TRET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		/*
		 * Get an exclusive lock on the handle: it will be released by
		 * __wt_conn_btree_close_all.
		 */
		if ((tret = __wt_schema_get_btree(session,
		    table->idx_name[i], strlen(table->idx_name[i]),
		    NULL, WT_BTREE_EXCLUSIVE)) != 0) {
			WT_TRET(tret);
			continue;
		}
		btree = session->btree;
		WT_ERR(__wt_buf_set(
		    session, namebuf, btree->name, strlen(btree->name) + 1));
		WT_TRET(__truncate_file(session, namebuf->data));
	}

	table->idx_complete = 0;

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
