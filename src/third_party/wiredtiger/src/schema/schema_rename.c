/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rename_file --
 *     WT_SESSION::rename for a file.
 */
static int
__rename_file(WT_SESSION_IMPL *session, const char *uri, const char *newuri)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    char *newvalue, *oldvalue;
    const char *filecfg[3] = {NULL, NULL, NULL};
    const char *filename, *newfile;
    bool exist;

    newvalue = oldvalue = NULL;

    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");
    newfile = newuri;
    WT_PREFIX_SKIP_REQUIRED(session, newfile, "file:");

    WT_RET(__wt_schema_backup_check(session, filename));
    WT_RET(__wt_schema_backup_check(session, newfile));
    /* Close any btree handles in the file. */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, false));
    WT_ERR(ret);
    WT_ERR(__wt_scr_alloc(session, 1024, &buf));

    /*
     * First, check if the file being renamed exists in the system. Doing this check first matches
     * the table rename behavior because we return WT_NOTFOUND when the renamed file doesn't exist
     * (subsequently mapped to ENOENT by the session layer).
     */
    WT_ERR(__wt_metadata_search(session, uri, &oldvalue));

    /*
     * Check to see if the proposed name is already in use, in either the metadata or the
     * filesystem.
     */
    switch (ret = __wt_metadata_search(session, newuri, &newvalue)) {
    case 0:
        WT_ERR_MSG(session, EEXIST, "%s", newuri);
    /* NOTREACHED */
    case WT_NOTFOUND:
        break;
    default:
        WT_ERR(ret);
    }
    __wt_free(session, newvalue);
    WT_ERR(__wt_fs_exist(session, newfile, &exist));
    if (exist)
        WT_ERR_MSG(session, EEXIST, "%s", newfile);

    WT_ERR(__wt_metadata_remove(session, uri));
    filecfg[0] = oldvalue;
    if (F_ISSET(S2C(session), WT_CONN_INCR_BACKUP)) {
        WT_ERR(__wt_reset_blkmod(session, oldvalue, buf));
        filecfg[1] = buf->mem;
    } else
        filecfg[1] = NULL;
    WT_ERR(__wt_config_collapse(session, filecfg, &newvalue));
    WT_ERR(__wt_metadata_insert(session, newuri, newvalue));

    /* Rename the underlying file. */
    WT_ERR(__wt_fs_rename(session, filename, newfile, false));
    if (WT_META_TRACKING(session))
        WT_ERR(__wt_meta_track_fileop(session, uri, newuri));

err:
    __wt_scr_free(session, &buf);
    __wt_free(session, newvalue);
    __wt_free(session, oldvalue);
    return (ret);
}

/*
 * __rename_tree --
 *     Rename an index or colgroup reference.
 */
static int
__rename_tree(WT_SESSION_IMPL *session, WT_TABLE *table, const char *newuri, const char *name,
  const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_ITEM(nn);
    WT_DECL_ITEM(ns);
    WT_DECL_ITEM(nv);
    WT_DECL_ITEM(os);
    WT_DECL_RET;
    char *value;
    const char *newname, *olduri, *suffix;
    bool is_colgroup;

    olduri = table->iface.name;
    value = NULL;

    newname = newuri;
    WT_PREFIX_SKIP_REQUIRED(session, newname, "table:");

    /*
     * Create the new data source URI and update the schema value.
     *
     * 'name' has the format (colgroup|index):<tablename>[:<suffix>]; we need the suffix.
     */
    is_colgroup = WT_PREFIX_MATCH(name, "colgroup:");
    if (!is_colgroup && !WT_PREFIX_MATCH(name, "index:"))
        WT_ERR_MSG(session, EINVAL, "expected a 'colgroup:' or 'index:' source: '%s'", name);

    suffix = strchr(name, ':');
    /* An existing table should have a well formed name. */
    WT_ASSERT(session, suffix != NULL);
    suffix = strchr(suffix + 1, ':');

    WT_ERR(__wt_scr_alloc(session, 0, &nn));
    WT_ERR(__wt_buf_fmt(session, nn, "%s%s%s", is_colgroup ? "colgroup:" : "index:", newname,
      (suffix == NULL) ? "" : suffix));

    /* Skip the colon, if any. */
    if (suffix != NULL)
        ++suffix;

    /* Read the old schema value. */
    WT_ERR(__wt_metadata_search(session, name, &value));

    /*
     * Calculate the new data source URI. Use the existing table structure and substitute the new
     * name temporarily.
     */
    WT_ERR(__wt_scr_alloc(session, 0, &ns));
    table->iface.name = newuri;
    if (is_colgroup)
        WT_ERR(__wt_schema_colgroup_source(session, table, suffix, value, ns));
    else
        WT_ERR(__wt_schema_index_source(session, table, suffix, value, ns));

    /* Convert not-found errors to EINVAL for the application. */
    if ((ret = __wt_config_getones(session, value, "source", &cval)) != 0)
        WT_ERR_MSG(session, ret == WT_NOTFOUND ? EINVAL : ret,
          "index or column group has no data source: %s", value);

    /* Take a copy of the old data source. */
    WT_ERR(__wt_scr_alloc(session, 0, &os));
    WT_ERR(__wt_buf_fmt(session, os, "%.*s", (int)cval.len, cval.str));

    /* Overwrite it with the new data source. */
    WT_ERR(__wt_scr_alloc(session, 0, &nv));
    WT_ERR(__wt_buf_fmt(session, nv, "%.*s%s%s", (int)WT_PTRDIFF(cval.str, value), value,
      (const char *)ns->data, cval.str + cval.len));

    /*
     * Do the rename before updating the metadata to avoid leaving the metadata inconsistent if the
     * rename fails.
     */
    WT_ERR(__wt_schema_rename(session, os->data, ns->data, cfg));

    /*
     * Remove the old metadata entry. Insert the new metadata entry.
     */
    WT_ERR(__wt_metadata_remove(session, name));
    WT_ERR(__wt_metadata_insert(session, nn->data, nv->data));

err:
    __wt_scr_free(session, &nn);
    __wt_scr_free(session, &ns);
    __wt_scr_free(session, &nv);
    __wt_scr_free(session, &os);
    __wt_free(session, value);
    table->iface.name = olduri;
    return (ret);
}

/*
 * __metadata_rename --
 *     Rename an entry in the metadata table.
 */
static int
__metadata_rename(WT_SESSION_IMPL *session, const char *uri, const char *newuri)
{
    WT_DECL_RET;
    char *value;

    WT_RET(__wt_metadata_search(session, uri, &value));
    WT_ERR(__wt_metadata_remove(session, uri));
    WT_ERR(__wt_metadata_insert(session, newuri, value));

err:
    __wt_free(session, value);
    return (ret);
}

/*
 * __rename_table --
 *     WT_SESSION::rename for a table.
 */
static int
__rename_table(WT_SESSION_IMPL *session, const char *uri, const char *newuri, const char *cfg[])
{
    WT_DECL_RET;
    WT_TABLE *table;
    u_int i;
    const char *oldname;
    bool tracked;

    oldname = uri;
    (void)WT_PREFIX_SKIP(oldname, "table:");
    tracked = false;

    /*
     * Open the table so we can rename its column groups and indexes.
     *
     * Ideally we would keep the table locked exclusive across the rename, but for now we rely on
     * the global table lock to prevent the table being reopened while it is being renamed. One
     * issue is that the WT_WITHOUT_LOCKS macro can drop and reacquire the global table lock,
     * avoiding deadlocks while waiting for LSM operation to quiesce.
     */
    WT_RET(__wt_schema_get_table(session, oldname, strlen(oldname), false, 0, &table));

    /* Rename the column groups. */
    for (i = 0; i < WT_COLGROUPS(table); i++)
        WT_ERR(__rename_tree(session, table, newuri, table->cgroups[i]->name, cfg));

    /* Rename the indices. */
    WT_ERR(__wt_schema_open_indices(session, table));
    for (i = 0; i < table->nindices; i++)
        WT_ERR(__rename_tree(session, table, newuri, table->indices[i]->name, cfg));

    /* Make sure the table data handle is closed. */
    WT_ERR(__wt_schema_release_table(session, &table));
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table));
    F_SET(&table->iface, WT_DHANDLE_DISCARD);
    if (WT_META_TRACKING(session)) {
        WT_WITH_DHANDLE(session, &table->iface, ret = __wt_meta_track_handle_lock(session, false));
        WT_ERR(ret);
        tracked = true;
    }

    /* Rename the table. */
    ret = __metadata_rename(session, uri, newuri);

err:
    if (!tracked)
        WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}

/*
 * __rename_tiered --
 *     Rename a tiered data source.
 */
static int
__rename_tiered(WT_SESSION_IMPL *session, const char *olduri, const char *newuri, const char *cfg[])
{
    WT_UNUSED(olduri);
    WT_UNUSED(newuri);
    WT_UNUSED(cfg);
    /* We do not allow renaming a tiered table. */
    WT_RET_MSG(session, EINVAL, "rename of tiered table is not supported");
}

/*
 * __schema_rename --
 *     WT_SESSION::rename.
 */
static int
__schema_rename(WT_SESSION_IMPL *session, const char *uri, const char *newuri, const char *cfg[])
{
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    const char *p, *t;

    /* The target type must match the source type. */
    for (p = uri, t = newuri; *p == *t && *p != ':'; ++p, ++t)
        ;
    if (*p != ':' || *t != ':')
        WT_RET_MSG(session, EINVAL, "rename target type must match URI: %s to %s", uri, newuri);

    /*
     * We track rename operations, if we fail in the middle, we want to back it all out.
     */
    WT_RET(__wt_meta_track_on(session));

    if (WT_PREFIX_MATCH(uri, "file:"))
        ret = __rename_file(session, uri, newuri);
    else if (WT_PREFIX_MATCH(uri, "lsm:"))
        ret = __wt_lsm_tree_rename(session, uri, newuri, cfg);
    else if (WT_PREFIX_MATCH(uri, "table:"))
        ret = __rename_table(session, uri, newuri, cfg);
    else if (WT_PREFIX_MATCH(uri, "tiered:"))
        ret = __rename_tiered(session, uri, newuri, cfg);
    else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
        ret = dsrc->rename == NULL ?
          __wt_object_unsupported(session, uri) :
          dsrc->rename(dsrc, &session->iface, uri, newuri, (WT_CONFIG_ARG *)cfg);
    else
        ret = __wt_bad_object_type(session, uri);

    WT_TRET(__wt_meta_track_off(session, true, ret != 0));

    /* If we didn't find a metadata entry, map that error to ENOENT. */
    return (ret == WT_NOTFOUND ? ENOENT : ret);
}

/*
 * __wt_schema_rename --
 *     WT_SESSION::rename.
 */
int
__wt_schema_rename(WT_SESSION_IMPL *session, const char *uri, const char *newuri, const char *cfg[])
{
    WT_DECL_RET;
    WT_SESSION_IMPL *int_session;

    WT_RET(__wt_schema_internal_session(session, &int_session));
    ret = __schema_rename(int_session, uri, newuri, cfg);
    WT_TRET(__wt_schema_session_release(session, int_session));
    return (ret);
}
