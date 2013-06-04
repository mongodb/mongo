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
	uint32_t allocsize;

	filename = name;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (EINVAL);

	/* Get the allocation size. */
	allocsize = S2BT(session)->allocsize;

	/* Close any btree handles in the file. */
	WT_RET(__wt_conn_dhandle_close_all(session, name));

	/* Delete the root address and truncate the file. */
	WT_RET(__wt_meta_checkpoint_clear(session, name));
	WT_RET(__wt_block_manager_truncate(session, filename, allocsize));

	return (0);
}

/*
 * __truncate_table --
 *	WT_SESSION::truncate for a table.
 */
static int
__truncate_table(WT_SESSION_IMPL *session, const char *name, const char *cfg[])
{
	WT_DECL_RET;
	WT_TABLE *table;
	u_int i;

	WT_RET(__wt_schema_get_table(session, name, strlen(name), 0, &table));

	/* Truncate the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++)
		WT_ERR(__wt_schema_truncate(
		    session, table->cgroups[i]->source, cfg));

	/* Truncate the indices. */
	WT_ERR(__wt_schema_open_indices(session, table));
	for (i = 0; i < table->nindices; i++)
		WT_ERR(__wt_schema_truncate(
		    session, table->indices[i]->source, cfg));

err:	__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __truncate_dsrc --
 *	WT_SESSION::truncate for a data-source without a truncate operation.
 */
static int
__truncate_dsrc(WT_SESSION_IMPL *session, const char *uri)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *cfg[2];

	/* Open a cursor and traverse the object, removing every entry. */
	cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	cfg[1] = NULL;
	WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
	while ((ret = cursor->next(cursor)) == 0)
		WT_ERR(cursor->remove(cursor));
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_TRET(cursor->close(cursor));
	return (0);
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

	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_RET(__wt_session_get_btree(
		    session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
		ret = __truncate_file(session, uri);
	} else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_lsm_tree_truncate(session, uri, cfg);
	else if (WT_PREFIX_SKIP(tablename, "table:"))
		ret = __truncate_table(session, tablename, cfg);
	else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
		ret = dsrc->truncate == NULL ?
		    __truncate_dsrc(session, uri) :
		    dsrc->truncate(
		    dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg);
	else
		ret = __wt_bad_object_type(session, uri);

	/* If we didn't find a metadata entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
