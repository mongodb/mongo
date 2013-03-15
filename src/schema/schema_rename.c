/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rename_file --
 *	WT_SESSION::rename for a file.
 */
static int
__rename_file(
    WT_SESSION_IMPL *session, const char *uri, const char *newuri)
{
	WT_DECL_RET;
	int exist;
	const char *filename, *newfile, *newvalue, *oldvalue;

	newvalue = oldvalue = NULL;

	filename = uri;
	newfile = newuri;
	if (!WT_PREFIX_SKIP(filename, "file:") ||
	    !WT_PREFIX_SKIP(newfile, "file:"))
		return (EINVAL);

	/* Close any btree handles in the file. */
	WT_ERR(__wt_conn_btree_close_all(session, uri));

	/*
	 * First, check if the file being renamed exists in the system.  Doing
	 * this check first matches the table rename behavior because we return
	 * WT_NOTFOUND when the renamed file doesn't exist (subsequently mapped
	 * to ENOENT by the session layer).
	 */
	WT_ERR(__wt_metadata_read(session, uri, &oldvalue));

	/*
	 * Check to see if the proposed name is already in use, in either the
	 * metadata or the filesystem.
	 */
	switch (ret = __wt_metadata_read(session, newuri, &newvalue)) {
	case 0:
		WT_ERR_MSG(session, EEXIST, "%s", newuri);
	case WT_NOTFOUND:
		ret = 0;
		break;
	default:
		WT_ERR(ret);
	}
	WT_ERR(__wt_exist(session, newfile, &exist));
	if (exist)
		WT_ERR_MSG(session, EEXIST, "%s", newfile);

	/* Replace the old file entries with new file entries. */
	WT_ERR(__wt_metadata_remove(session, uri));
	WT_ERR(__wt_metadata_insert(session, newuri, oldvalue));

	/* Rename the underlying file. */
	WT_ERR(__wt_rename(session, filename, newfile));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_fileop(session, uri, newuri));

err:	__wt_free(session, newvalue);
	__wt_free(session, oldvalue);
	return (ret);
}

/*
 * __rename_tree --
 *	Rename an index or colgroup reference.
 */
static int
__rename_tree(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *newuri, const char *name, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_ITEM(nn);
	WT_DECL_ITEM(ns);
	WT_DECL_ITEM(nv);
	WT_DECL_ITEM(os);
	WT_DECL_RET;
	const char *newname, *olduri, *suffix, *value;
	int is_colgroup;

	olduri = table->name;
	newname = newuri;
	(void)WT_PREFIX_SKIP(newname, "table:");

	/*
	 * Create the new data source URI and update the schema value.
	 *
	 * 'name' has the format (colgroup|index):<tablename>[:<suffix>];
	 * we need the suffix.
	 */
	is_colgroup = WT_PREFIX_MATCH(name, "colgroup:");
	if (!is_colgroup && !WT_PREFIX_MATCH(name, "index:"))
		WT_ERR_MSG(session, EINVAL,
		    "expected a 'colgroup:' or 'index:' source: '%s'", name);

	suffix = strchr(name, ':');
	/* An existing table should have a well formed name. */
	WT_ASSERT(session, suffix != NULL);
	suffix = strchr(suffix + 1, ':');

	WT_ERR(__wt_scr_alloc(session, 0, &nn));
	WT_ERR(__wt_buf_fmt(session, nn, "%s%s%s",
	    is_colgroup ? "colgroup:" : "index:",
	    newname,
	    (suffix == NULL) ? "" : suffix));

	/* Skip the colon, if any. */
	if (suffix != NULL)
		++suffix;

	/* Read the old schema value. */
	WT_ERR(__wt_metadata_read(session, name, &value));

	/*
	 * Calculate the new data source URI.  Use the existing table structure
	 * and substitute the new name temporarily.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &ns));
	table->name = newuri;
	if (is_colgroup)
		WT_ERR(__wt_schema_colgroup_source(
		    session, table, suffix, value, ns));
	else
		WT_ERR(__wt_schema_index_source(
		    session, table, suffix, value, ns));

	if ((ret = __wt_config_getones(session, value, "source", &cval)) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "index or column group has no data source: %s", value);

	/* Take a copy of the old data source. */
	WT_ERR(__wt_scr_alloc(session, 0, &os));
	WT_ERR(__wt_buf_fmt(session, os, "%.*s", (int)cval.len, cval.str));

	/* Overwrite it with the new data source. */
	WT_ERR(__wt_scr_alloc(session, 0, &nv));
	WT_ERR(__wt_buf_fmt(session, nv, "%.*s%s%s",
	    (int)WT_PTRDIFF(cval.str, value), value,
	    (const char *)ns->data,
	    cval.str + cval.len));

	/*
	 * Remove the old metadata entry.
	 * Insert the new metadata entry.
	 */
	WT_ERR(__wt_metadata_remove(session, name));
	WT_ERR(__wt_metadata_insert(session, nn->data, nv->data));

	/* Rename the file. */
	WT_ERR(__wt_schema_rename(session, os->data, ns->data, cfg));

err:	__wt_scr_free(&nn);
	__wt_scr_free(&ns);
	__wt_scr_free(&nv);
	__wt_scr_free(&os);
	__wt_free(session, value);
	table->name = olduri;
	return (ret);
}

/*
 * __rename_table --
 *	WT_SESSION::rename for a table.
 */
static int
__rename_table(WT_SESSION_IMPL *session,
    const char *uri, const char *newuri, const char *cfg[])
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_TABLE *table;
	u_int i;
	const char *oldname, *value;

	oldname = uri;
	(void)WT_PREFIX_SKIP(oldname, "table:");

	WT_RET(__wt_schema_get_table(
	    session, oldname, strlen(oldname), 0, &table));

	/* Rename the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++)
		WT_ERR(__rename_tree(session, table, newuri,
		    table->cgroups[i]->name, cfg));

	/* Rename the indices. */
	WT_ERR(__wt_schema_open_indices(session, table));
	for (i = 0; i < table->nindices; i++)
		WT_ERR(__rename_tree(session, table, newuri,
		    table->indices[i]->name, cfg));

	__wt_schema_remove_table(session, table);
	table = NULL;

	/* Rename the table. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_metadata_read(session, uri, &value));
	WT_ERR(__wt_metadata_remove(session, uri));
	WT_ERR(__wt_metadata_insert(session, newuri, value));

err:	__wt_scr_free(&buf);
	if (table != NULL)
		__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __wt_schema_rename --
 *	WT_SESSION::rename.
 */
int
__wt_schema_rename(WT_SESSION_IMPL *session,
    const char *uri, const char *newuri, const char *cfg[])
{
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	const char *oldname, *newname;

	/* Disallow renames to/from the WiredTiger name space. */
	WT_RET(__wt_schema_name_check(session, uri));
	WT_RET(__wt_schema_name_check(session, newuri));

	/*
	 * We track rename operations, if we fail in the middle, we want to
	 * back it all out.
	 */
	WT_RET(__wt_meta_track_on(session));

	oldname = uri;
	newname = newuri;
	if (WT_PREFIX_SKIP(oldname, "file:")) {
		if (!WT_PREFIX_SKIP(newname, "file:"))
			WT_RET_MSG(session, EINVAL,
			    "rename target type must match URI: %s to %s",
			    uri, newuri);
		ret = __rename_file(session, uri, newuri);
	} else if (WT_PREFIX_SKIP(oldname, "table:")) {
		if (!WT_PREFIX_SKIP(newname, "table:"))
			WT_RET_MSG(session, EINVAL,
			    "rename target type must match URI: %s to %s",
			    uri, newuri);
		ret = __rename_table(session, uri, newuri, cfg);
	} else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0)
		ret = dsrc->rename(dsrc, &session->iface, uri, newuri, cfg);

	/* Bump the schema generation so that stale data is ignored. */
	++S2C(session)->schema_gen;

	WT_TRET(__wt_meta_track_off(session, ret != 0));

	/* If we didn't find a metadata entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
