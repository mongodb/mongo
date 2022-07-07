/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __drop_file --
 *     Drop a file.
 */
static int
__drop_file(WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    const char *filename;
    bool remove_files;

    WT_RET(__wt_config_gets(session, cfg, "remove_files", &cval));
    remove_files = cval.val != 0;

    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");

    WT_RET(__wt_schema_backup_check(session, filename));
    /* Close all btree handles associated with this file. */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, force));
    WT_RET(ret);

    /* Remove the metadata entry (ignore missing items). */
    WT_TRET(__wt_metadata_remove(session, uri));
    if (!remove_files)
        return (ret);

    /*
     * Schedule the remove of the underlying physical file when the drop completes.
     */
    WT_TRET(__wt_meta_track_drop(session, filename));

    return (ret);
}

/*
 * __drop_colgroup --
 *     WT_SESSION::drop for a colgroup.
 */
static int
__drop_colgroup(WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[])
{
    WT_COLGROUP *colgroup;
    WT_DECL_RET;
    WT_TABLE *table;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE));

    /* If we can get the colgroup, detach it from the table. */
    if ((ret = __wt_schema_get_colgroup(session, uri, force, &table, &colgroup)) == 0) {
        WT_TRET(__wt_schema_drop(session, colgroup->source, cfg));
        if (ret == 0)
            table->cg_complete = false;
    }

    WT_TRET(__wt_metadata_remove(session, uri));
    return (ret);
}

/*
 * __drop_index --
 *     WT_SESSION::drop for an index.
 */
static int
__drop_index(WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[])
{
    WT_DECL_RET;
    WT_INDEX *idx;

    /* If we can get the index, detach it from the table. */
    if ((ret = __wt_schema_get_index(session, uri, true, force, &idx)) == 0)
        WT_TRET(__wt_schema_drop(session, idx->source, cfg));

    WT_TRET(__wt_metadata_remove(session, uri));
    return (ret);
}

/*
 * __drop_table --
 *     WT_SESSION::drop for a table.
 */
static int
__drop_table(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_COLGROUP *colgroup;
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_TABLE *table;
    u_int i;
    const char *name;
    bool tracked;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE));

    name = uri;
    WT_PREFIX_SKIP_REQUIRED(session, name, "table:");

    table = NULL;
    tracked = false;

    /*
     * Open the table so we can drop its column groups and indexes.
     *
     * Ideally we would keep the table locked exclusive across the drop, but for now we rely on the
     * global table lock to prevent the table being reopened while it is being dropped. One issue is
     * that the WT_WITHOUT_LOCKS macro can drop and reacquire the global table lock, avoiding
     * deadlocks while waiting for LSM operation to quiesce.
     *
     * Temporarily getting the table exclusively serves the purpose of ensuring that cursors on the
     * table that are already open must at least be closed before this call proceeds.
     */
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table));
    WT_ERR(__wt_schema_release_table(session, &table));
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, 0, &table));

    /* Drop the column groups. */
    for (i = 0; i < WT_COLGROUPS(table); i++) {
        if ((colgroup = table->cgroups[i]) == NULL)
            continue;
        /*
         * Drop the column group before updating the metadata to avoid the metadata for the table
         * becoming inconsistent if we can't get exclusive access.
         */
        WT_ERR(__wt_schema_drop(session, colgroup->source, cfg));
        WT_ERR(__wt_metadata_remove(session, colgroup->name));
    }

    /* Drop the indices. */
    WT_ERR(__wt_schema_open_indices(session, table));
    for (i = 0; i < table->nindices; i++) {
        if ((idx = table->indices[i]) == NULL)
            continue;
        /*
         * Drop the index before updating the metadata to avoid the metadata for the table becoming
         * inconsistent if we can't get exclusive access.
         */
        WT_ERR(__wt_schema_drop(session, idx->source, cfg));
        WT_ERR(__wt_metadata_remove(session, idx->name));
    }

    /* Make sure the table data handle is closed. */
    WT_ERR(__wt_schema_release_table(session, &table));
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table));
    F_SET(&table->iface, WT_DHANDLE_DISCARD);
    if (WT_META_TRACKING(session)) {
        WT_WITH_DHANDLE(session, &table->iface, ret = __wt_meta_track_handle_lock(session, false));
        WT_ERR(ret);
        tracked = true;
    }

    /* Remove the metadata entry (ignore missing items). */
    WT_ERR(__wt_metadata_remove(session, uri));

err:
    if (!tracked)
        WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}

/*
 * __drop_tiered --
 *     Drop a tiered store.
 */
static int
__drop_tiered(WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *tier;
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;
    const char *filename, *name;
    bool exist, locked, remove_files, remove_shared;

    conn = S2C(session);
    locked = false;
    WT_RET(__wt_config_gets(session, cfg, "remove_files", &cval));
    remove_files = cval.val != 0;
    WT_RET(__wt_config_gets(session, cfg, "remove_shared", &cval));
    remove_shared = cval.val != 0;

    if (!remove_files && remove_shared)
        WT_RET_MSG(session, EINVAL,
          "drop for tiered storage object must configure removal of underlying files "
          "if forced removal of shared objects is enabled");

    name = NULL;
    /* Get the tiered data handle. */
    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
    tiered = (WT_TIERED *)session->dhandle;

    /*
     * We cannot remove the objects on shared storage as other systems may be accessing them too.
     * Remove the current local file object, the tiered entry and all bucket objects from the
     * metadata only.
     */
    tier = tiered->tiers[WT_TIERED_INDEX_LOCAL].tier;
    if (tier != NULL) {
        __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: drop local object %s", tier->name);
        WT_WITHOUT_DHANDLE(session,
          WT_WITH_HANDLE_LIST_WRITE_LOCK(
            session, ret = __wt_conn_dhandle_close_all(session, tier->name, true, force)));
        WT_ERR(ret);
        WT_ERR(__wt_metadata_remove(session, tier->name));
        if (remove_files) {
            filename = tier->name;
            WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");
            WT_ERR(__wt_meta_track_drop(session, filename));
        }
        tiered->tiers[WT_TIERED_INDEX_LOCAL].tier = NULL;
    }

    /* Close any dhandle and remove any tier: entry from metadata. */
    tier = tiered->tiers[WT_TIERED_INDEX_SHARED].tier;
    if (tier != NULL) {
        __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: drop shared object %s", tier->name);
        WT_WITHOUT_DHANDLE(session,
          WT_WITH_HANDLE_LIST_WRITE_LOCK(
            session, ret = __wt_conn_dhandle_close_all(session, tier->name, true, force)));
        WT_ERR(ret);
        WT_ERR(__wt_metadata_remove(session, tier->name));
        tiered->tiers[WT_TIERED_INDEX_SHARED].tier = NULL;
    }

    /*
     * We remove all metadata entries for both the file and object versions of an object. The local
     * retention means we can have both versions in the metadata. Ignore WT_NOTFOUND.
     */
    for (i = tiered->oldest_id; i < tiered->current_id; ++i) {
        WT_ERR(__wt_tiered_name(session, &tiered->iface, i, WT_TIERED_NAME_LOCAL, &name));
        __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: remove object %s from metadata", name);
        WT_ERR_NOTFOUND_OK(__wt_metadata_remove(session, name), false);
        __wt_free(session, name);
        WT_ERR(__wt_tiered_name(session, &tiered->iface, i, WT_TIERED_NAME_OBJECT, &name));
        __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: remove object %s from metadata", name);
        WT_ERR_NOTFOUND_OK(__wt_metadata_remove(session, name), false);
        if (remove_files && tier != NULL) {
            filename = name;
            WT_PREFIX_SKIP_REQUIRED(session, filename, "object:");
            WT_ERR(__wt_fs_exist(session, filename, &exist));
            if (exist)
                WT_ERR(__wt_meta_track_drop(session, filename));

            /*
             * If a drop operation on tiered storage is configured to force removal of shared
             * objects, we want to remove these files after the drop operation is successful.
             */
            if (remove_shared)
                WT_ERR(__wt_meta_track_drop_object(session, tiered->bstorage, filename));
        }
        __wt_free(session, name);
    }

    /*
     * We are about to close the dhandle. If that is successful we need to remove any tiered work
     * from the queue relating to that dhandle. But if closing the dhandle has an error we don't
     * remove the work. So hold the tiered lock for the duration so that the worker thread cannot
     * race and process work for this handle.
     */
    __wt_spin_lock(session, &conn->tiered_lock);
    locked = true;
    /*
     * Close all btree handles associated with this table. This must be done after we're done using
     * the tiered structure because that is from the dhandle.
     */
    WT_ERR(__wt_session_release_dhandle(session));
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, force));
    WT_ERR(ret);

    /* If everything is successful, remove any tiered work associated with this tiered handle. */
    __wt_tiered_remove_work(session, tiered, locked);
    __wt_spin_unlock(session, &conn->tiered_lock);
    locked = false;

    __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: remove tiered table %s from metadata", uri);
    ret = __wt_metadata_remove(session, uri);

err:
    __wt_free(session, name);
    if (locked)
        __wt_spin_unlock(session, &conn->tiered_lock);
    return (ret);
}

/*
 * __schema_drop --
 *     Process a WT_SESSION::drop operation for all supported types.
 */
static int
__schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    bool force;

    WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
    force = cval.val != 0;

    WT_RET(__wt_meta_track_on(session));

    /* Paranoia: clear any handle from our caller. */
    session->dhandle = NULL;

    if (WT_PREFIX_MATCH(uri, "colgroup:"))
        ret = __drop_colgroup(session, uri, force, cfg);
    else if (WT_PREFIX_MATCH(uri, "file:"))
        ret = __drop_file(session, uri, force, cfg);
    else if (WT_PREFIX_MATCH(uri, "index:"))
        ret = __drop_index(session, uri, force, cfg);
    else if (WT_PREFIX_MATCH(uri, "lsm:"))
        ret = __wt_lsm_tree_drop(session, uri, cfg);
    else if (WT_PREFIX_MATCH(uri, "table:"))
        ret = __drop_table(session, uri, cfg);
    else if (WT_PREFIX_MATCH(uri, "tiered:"))
        ret = __drop_tiered(session, uri, force, cfg);
    else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
        ret = dsrc->drop == NULL ? __wt_object_unsupported(session, uri) :
                                   dsrc->drop(dsrc, &session->iface, uri, (WT_CONFIG_ARG *)cfg);
    else
        ret = __wt_bad_object_type(session, uri);

    /*
     * Map WT_NOTFOUND to ENOENT, based on the assumption WT_NOTFOUND means there was no metadata
     * entry. Map ENOENT to zero if force is set.
     */
    if (ret == WT_NOTFOUND || ret == ENOENT)
        ret = force ? 0 : ENOENT;

    if (F_ISSET(S2C(session), WT_CONN_BACKUP_PARTIAL_RESTORE))
        WT_TRET(__wt_meta_track_off(session, false, ret != 0));
    else
        WT_TRET(__wt_meta_track_off(session, true, ret != 0));

    return (ret);
}

/*
 * __wt_schema_drop --
 *     Process a WT_SESSION::drop operation for all supported types.
 */
int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DECL_RET;
    WT_SESSION_IMPL *int_session;

    WT_RET(__wt_schema_internal_session(session, &int_session));
    ret = __schema_drop(int_session, uri, cfg);
    WT_TRET(__wt_schema_session_release(session, int_session));
    return (ret);
}
