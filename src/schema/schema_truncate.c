/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
	WT_RET(__wt_conn_dhandle_close_all(session, name));

	/* Delete the root address and truncate the file. */
	WT_RET(__wt_meta_checkpoint_clear(session, name));
	WT_RET(__wt_block_manager_truncate(session, filename));

	return (0);
}

/*
 * __truncate_table --
 *	WT_SESSION::truncate for a table.
 */
static int
__truncate_table(WT_SESSION_IMPL *session, const char *name)
{
	WT_DECL_ITEM(namebuf);
	WT_DECL_RET;
	WT_TABLE *table;
	const char *hname;
	u_int i;

	WT_RET(__wt_scr_alloc(session, 0, &namebuf));
	WT_ERR(__wt_schema_get_table(session, name, strlen(name), 0, &table));

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		/*
		 * Get an exclusive lock on the handle: it will be released by
		 * __wt_conn_btree_close_all.
		 */
		WT_ERR(__wt_session_get_btree(session,
		    table->cgroups[i]->source, NULL, NULL,
		    WT_DHANDLE_EXCLUSIVE));
		hname = session->dhandle->name;
		WT_ERR(
		    __wt_buf_set(session, namebuf, hname, strlen(hname) + 1));
		WT_ERR(__truncate_file(session, namebuf->data));
	}

	/* Truncate the indices. */
	WT_ERR(__wt_schema_open_indices(session, table));
	for (i = 0; i < table->nindices; i++) {
		/*
		 * Get an exclusive lock on the handle: it will be released by
		 * __wt_conn_btree_close_all.
		 */
		WT_ERR(__wt_session_get_btree(session,
		    table->indices[i]->source, NULL, NULL,
		    WT_DHANDLE_EXCLUSIVE));
		hname = session->dhandle->name;
		WT_ERR(__wt_buf_set(
		    session, namebuf, hname, strlen(hname) + 1));
		WT_ERR(__truncate_file(session, namebuf->data));
	}

err:	__wt_scr_free(&namebuf);
	__wt_schema_release_table(session, table);
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
	else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_lsm_tree_truncate(session, uri, cfg);
	else if (WT_PREFIX_SKIP(tablename, "table:"))
		ret = __truncate_table(session, tablename);
	else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0)
		ret = dsrc->truncate(dsrc, &session->iface, uri, cfg);

	/* If we didn't find a metadata entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
