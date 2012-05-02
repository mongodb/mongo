/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
	const char *filename, *newfile, *value;

	value = NULL;

	filename = uri;
	newfile = newuri;
	if (!WT_PREFIX_SKIP(filename, "file:") ||
	    !WT_PREFIX_SKIP(newfile, "file:"))
		return (EINVAL);

	/* If open, close the btree handle. */
	WT_RET(__wt_session_close_any_open_btree(session, uri));

	/*
	 * Check to see if the proposed name is already in use, in either
	 * the metadata or the filesystem.
	 */
	switch (ret = __wt_metadata_read(session, newuri, &value)) {
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
	WT_ERR(__wt_metadata_read(session, uri, &value));
	WT_ERR(__wt_metadata_remove(session, uri));
	WT_ERR(__wt_metadata_insert(session, newuri, value));

	/* Rename the underlying file. */
	WT_ERR(__wt_rename(session, filename, newfile));
	if (WT_META_TRACKING(session))
		WT_ERR(__wt_meta_track_fileop(session, uri, newuri));

err:	__wt_free(session, value);

	return (ret);
}

/*
 * __rename_tree --
 *	Rename an index or colgroup reference.
 */
static int
__rename_tree(WT_SESSION_IMPL *session, const char *name, const char *newname)
{
	WT_DECL_RET;
	WT_ITEM *of, *nf, *nk, *nv;
	const char *newfile, *p, *t, *value;

	nf = nk = nv = of = NULL;

	/* Read the old schema value. */
	WT_ERR(__wt_metadata_read(session, name, &value));

	/*
	 * Create the new file name, new schema key, new schema value.
	 *
	 * Names are of the form "prefix.oldname:suffix", where suffix is
	 * optional; we need prefix and suffix.
	 */
	if ((p = strchr(name, ':')) == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "invalid index or column-group name: %s", name);
	t = strchr(p + 1, ':');

	WT_ERR(__wt_scr_alloc(session, 0, &nf));
	WT_ERR(__wt_buf_fmt(session, nf, "file:%s%s%s.wt", newname,
	    t == NULL ? "" : "_",  t == NULL ? "" : t + 1));
	newfile = (const char *)nf->data + strlen("file:");

	WT_ERR(__wt_scr_alloc(session, 0, &nk));
	WT_ERR(__wt_buf_fmt(session, nk, "%.*s:%s%s%s",
	    (int)WT_PTRDIFF(p, name), name, newname,
	    t == NULL ? "" : ":", t == NULL ? "" : t + 1));

	if ((p = strstr(value, "filename=")) == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "index or column-group value has no file name: %s", value);
	p += strlen("filename=");
	t = strchr(p, ',');

	/* Take a copy of the old filename. */
	WT_ERR(__wt_scr_alloc(session, 0, &of));
	WT_ERR(__wt_buf_fmt(session, of, "file:%.*s",
	    (int)((t == NULL) ? strlen(p) : WT_PTRDIFF(t, p)), p));

	/* Overwrite it with the new filename. */
	WT_ERR(__wt_scr_alloc(session, 0, &nv));
	WT_ERR(__wt_buf_fmt(session, nv, "%.*s%s%s",
	    (int)WT_PTRDIFF(p, value), value,
	    newfile, t == NULL ? "" : t));

	/*
	 * Remove the old metadata entry.
	 * Insert the new metadata entry.
	 */
	WT_ERR(__wt_metadata_remove(session, name));
	WT_ERR(__wt_metadata_insert(session, nk->data, nv->data));

	/* Rename the file. */
	WT_ERR(__rename_file(session, of->data, nf->data));

err:	__wt_scr_free(&nf);
	__wt_scr_free(&nk);
	__wt_scr_free(&nv);
	__wt_scr_free(&of);
	__wt_free(session, value);
	return (ret);
}

/*
 * __rename_table --
 *	WT_SESSION::rename for a table.
 */
static int
__rename_table(
    WT_SESSION_IMPL *session, const char *oldname, const char *newname)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	WT_TABLE *table;
	int i;
	const char *value;

	buf = NULL;

	WT_RET(
	    __wt_schema_get_table(session, oldname, strlen(oldname), &table));

	/* Rename the column groups. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if (table->colgroup[i] == NULL)
			continue;
		table->colgroup[i] = NULL;
		WT_RET(__rename_tree(session, table->cg_name[i], newname));
	}

	/* Rename the indices. */
	WT_RET(__wt_schema_open_index(session, table, NULL, 0));
	for (i = 0; i < table->nindices; i++) {
		table->index[i] = NULL;
		WT_RET(
		    __rename_tree(session, table->idx_name[i], newname));
	}

	WT_RET(__wt_schema_remove_table(session, table));

	/* Rename the table. */
	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "table:%s", oldname));
	WT_ERR(__wt_metadata_read(session, buf->data, &value));
	WT_ERR(__wt_metadata_remove(session, buf->data));
	WT_ERR(__wt_buf_fmt(session, buf, "table:%s", newname));
	WT_ERR(__wt_metadata_insert(session, buf->data, value));

err:	__wt_scr_free(&buf);
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

	WT_UNUSED(cfg);

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
		ret = __rename_table(session, oldname, newname);
	} else if ((ret = __wt_schema_get_source(session, oldname, &dsrc)) == 0)
		ret = dsrc->rename(dsrc,
		    &session->iface, oldname, newname, cfg[1]);

	WT_TRET(__wt_meta_track_off(session, ret != 0));

	/* If we didn't find a metadata entry, map that error to ENOENT. */
	return (ret == WT_NOTFOUND ? ENOENT : ret);
}
