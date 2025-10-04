/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CONFLICT_BACKUP_MSG "the table is currently performing backup and cannot be dropped"
#define WT_CONFLICT_DHANDLE_MSG "another thread is currently holding the data handle of the table"

/*
 * __drop_file --
 *     Drop a file.
 */
static int
__drop_file(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    const char *filename;
    char *metadata_cfg = NULL;
    bool id_found, remove_files;
    uint32_t id = 0;

    WT_RET(__wt_config_gets(session, cfg, "remove_files", &cval));
    remove_files = cval.val != 0;

    filename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");

    if ((ret = __wti_schema_backup_check(session, filename)) == EBUSY)
        WT_RET_SUB(session, ret, WT_CONFLICT_BACKUP, WT_CONFLICT_BACKUP_MSG);
    WT_RET(ret);

    /* Close all btree handles associated with this file. */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, force, check_visibility));
    if (ret == EBUSY)
        WT_RET_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
    WT_RET(ret);

    /* Get file id that will be used to truncate history store for the file. */
    id_found = __wt_metadata_search(session, uri, &metadata_cfg) == 0 &&
      __wt_config_getones(session, metadata_cfg, "id", &cval) == 0;
    if (id_found)
        id = (uint32_t)cval.val;

    /* Remove the metadata entry (ignore missing items). */
    WT_TRET(__wt_metadata_remove(session, uri));
    if (remove_files)
        /*
         * Schedule the remove of the underlying physical file when the drop completes.
         */
        WT_TRET(__wt_meta_track_drop(session, filename));

    /*
     * Truncate history store for the dropped file if we can find its id from the metadata, this is
     * a best-effort operation, as we don't fail drop if truncate returns an error. There is no
     * history store to truncate for in-memory database, and we should not call truncate if
     * connection is not ready for history store operations.
     */
    WT_ERR(ret);
    if (id_found && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY) &&
      F_ISSET_ATOMIC_32(S2C(session), WT_CONN_READY))
        if (__wt_hs_btree_truncate(session, id) != 0)
            __wt_verbose_warning(
              session, WT_VERB_HS, "Failed to truncate history store for the file: %s", uri);
err:
    __wt_free(session, metadata_cfg);
    return (ret);
}

/*
 * __drop_colgroup --
 *     WT_SESSION::drop for a colgroup.
 */
static int
__drop_colgroup(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
{
    WT_COLGROUP *colgroup;
    WT_DECL_RET;
    WT_TABLE *table;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE));

    /* If we can get the colgroup, detach it from the table. */
    if ((ret = __wt_schema_get_colgroup(session, uri, force, &table, &colgroup)) == 0) {
        WT_TRET(__wt_schema_drop(session, colgroup->source, cfg, check_visibility));
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
__drop_index(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
{
    WT_DECL_RET;
    WT_INDEX *idx;

    /* If we can get the index, detach it from the table. */
    if ((ret = __wti_schema_get_index(session, uri, true, force, &idx)) == 0)
        WT_TRET(__wt_schema_drop(session, idx->source, cfg, check_visibility));

    WT_TRET(__wt_metadata_remove(session, uri));
    return (ret);
}

/*
 * __drop_layered --
 *     WT_SESSION::drop for a layered table.
 */
static int
__drop_layered(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
{
    WT_DECL_ITEM(ingest_uri_buf);
    WT_DECL_ITEM(stable_uri_buf);
    WT_DECL_RET;
    const char *ingest_uri, *stable_uri, *tablename;
    WT_UNUSED(force);

    WT_ASSERT(session, WT_PREFIX_MATCH(uri, "layered:"));

    WT_RET(__wt_scr_alloc(session, 0, &ingest_uri_buf));
    WT_ERR(__wt_scr_alloc(session, 0, &stable_uri_buf));

    tablename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "layered:");
    WT_ERR(__wt_buf_fmt(session, ingest_uri_buf, "file:%s.wt_ingest", tablename));
    ingest_uri = ingest_uri_buf->data;
    WT_ERR(__wt_buf_fmt(session, stable_uri_buf, "file:%s.wt_stable", tablename));
    stable_uri = stable_uri_buf->data;

    WT_ERR(__wt_schema_drop(session, ingest_uri, cfg, check_visibility));

    /*
     * FIXME-WT-14503: as part of the bigger garbage-collection picture, we should eventually find a
     * way to tell PALI that this was dropped.
     */
    WT_ERR(__wt_schema_drop(session, stable_uri, cfg, check_visibility));

    /* Now drop the top-level table. */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, true, check_visibility));
    WT_ERR(ret);
    WT_ERR(__wt_metadata_remove(session, uri));

    /* No need for a meta track drop, since the top-level table has no underlying files to remove.
     */

err:
    __wt_scr_free(session, &ingest_uri_buf);
    __wt_scr_free(session, &stable_uri_buf);
    return (ret);
}

/*
 * __drop_table --
 *     WT_SESSION::drop for a table.
 */
static int
__drop_table(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
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
     * FIXME-WT-13812: Investigate table locking during session->drop Ideally we would keep the
     * table locked exclusive across the drop, but for now we rely on the global table lock to
     * prevent the table being reopened while it is being dropped.
     *
     * Temporarily getting the table exclusively serves the purpose of ensuring that cursors on the
     * table that are already open must at least be closed before this call proceeds.
     */
    ret = __wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table);
    if (ret == EBUSY)
        WT_ERR_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
    WT_ERR(ret);
    WT_ERR(__wti_schema_release_table_gen(session, &table, true));
    WT_ERR(__wt_schema_get_table_uri(session, uri, true, 0, &table));

    if (force && !table->is_simple) {
        __wt_verbose_warning(session, WT_VERB_HANDLEOPS,
          "ENOTSUP: drop table with force=true is not supported for complex tables. uri=%s", uri);
        WT_ERR(ENOTSUP);
    }

    /* Drop the column groups. */
    for (i = 0; i < WT_COLGROUPS(table); i++) {
        if ((colgroup = table->cgroups[i]) == NULL)
            continue;
        /*
         * Drop the column group before updating the metadata to avoid the metadata for the table
         * becoming inconsistent if we can't get exclusive access.
         */
        WT_ERR(__wt_schema_drop(session, colgroup->source, cfg, check_visibility));
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
        WT_ERR(__wt_schema_drop(session, idx->source, cfg, check_visibility));
        WT_ERR(__wt_metadata_remove(session, idx->name));
    }

    /* Make sure the table data handle is closed. */
    WT_ERR(__wti_schema_release_table_gen(session, &table, true));
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
__drop_tiered(
  WT_SESSION_IMPL *session, const char *uri, bool force, const char *cfg[], bool check_visibility)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *tier;
    WT_DECL_RET;
    WT_TIERED *tiered, tiered_tmp;
    u_int i, localid;
    const char *filename, *name;
    bool exist, got_dhandle, remove_files, remove_shared;

    conn = S2C(session);
    WT_NOT_READ(got_dhandle, false);

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
    ret = __wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE);
    if (ret == EBUSY)
        WT_RET_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
    WT_RET(ret);
    got_dhandle = true;
    tiered = (WT_TIERED *)session->dhandle;
    /*
     * Save a copy because we cannot release the tiered resources until after the dhandle is
     * released and closed. We have to know if the table is busy or if the close is successful
     * before cleaning up the tiered information.
     */
    tiered_tmp = *tiered;

    /*
     * We are about to close the dhandle. If that is successful we need to remove any tiered work
     * from the queue relating to that dhandle. But if closing the dhandle has an error we don't
     * remove the work. So hold the tiered lock for the duration so that the worker thread cannot
     * race and process work for this handle.
     */
    __wt_spin_lock(session, &conn->tiered_lock);
    /*
     * Close all btree handles associated with this table. This must be done after we're done using
     * the tiered structure because that is from the dhandle.
     */
    WT_ERR(__wt_session_release_dhandle(session));
    got_dhandle = false;
    WT_WITH_HANDLE_LIST_WRITE_LOCK(
      session, ret = __wt_conn_dhandle_close_all(session, uri, true, force, check_visibility));
    if (ret == EBUSY)
        WT_ERR_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
    WT_ERR(ret);

    /*
     * If closing the URI succeeded then we can remove tiered information using the saved tiered
     * structure from above. We need the copy because the dhandle has been released.
     */

    /*
     * We cannot remove the objects on shared storage as other systems may be accessing them too.
     * Remove the current local file object, the tiered entry and all bucket objects from the
     * metadata only.
     */
    tier = tiered_tmp.tiers[WT_TIERED_INDEX_LOCAL].tier;
    localid = tiered_tmp.current_id;
    if (tier != NULL) {
        __wt_verbose_debug2(
          session, WT_VERB_TIERED, "DROP_TIERED: drop %u local object %s", localid, tier->name);
        WT_WITHOUT_DHANDLE(session,
          WT_WITH_HANDLE_LIST_WRITE_LOCK(
            session, ret = __wt_conn_dhandle_close_all(session, tier->name, true, force, false)));
        if (ret == EBUSY)
            WT_ERR_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
        WT_ERR(ret);
        WT_ERR(__wt_metadata_remove(session, tier->name));
        if (remove_files) {
            filename = tier->name;
            WT_PREFIX_SKIP_REQUIRED(session, filename, "file:");
            WT_ERR(__wt_meta_track_drop(session, filename));
        }
    }

    /* Close any dhandle and remove any tier: entry from metadata. */
    tier = tiered_tmp.tiers[WT_TIERED_INDEX_SHARED].tier;
    if (tier != NULL) {
        __wt_verbose_debug2(
          session, WT_VERB_TIERED, "DROP_TIERED: drop shared object %s", tier->name);
        WT_WITHOUT_DHANDLE(session,
          WT_WITH_HANDLE_LIST_WRITE_LOCK(
            session, ret = __wt_conn_dhandle_close_all(session, tier->name, true, force, false)));
        if (ret == EBUSY)
            WT_ERR_SUB(session, ret, WT_CONFLICT_DHANDLE, WT_CONFLICT_DHANDLE_MSG);
        WT_ERR(ret);
        WT_ERR(__wt_metadata_remove(session, tier->name));
    } else
        /* If we don't have a shared tier we better be on the first object. */
        WT_ASSERT(session, localid == 1);

    /*
     * We remove all metadata entries for both the file and object versions of an object. The local
     * retention means we can have both versions in the metadata. Ignore WT_NOTFOUND.
     */
    for (i = tiered_tmp.oldest_id; i < tiered_tmp.current_id; ++i) {
        WT_ERR(__wt_tiered_name(session, &tiered_tmp.iface, i, WT_TIERED_NAME_LOCAL, &name));
        __wt_verbose_debug2(
          session, WT_VERB_TIERED, "DROP_TIERED: remove local object %s from metadata", name);
        WT_ERR_NOTFOUND_OK(__wt_metadata_remove(session, name), false);
        __wt_free(session, name);
        WT_ERR(__wt_tiered_name(session, &tiered_tmp.iface, i, WT_TIERED_NAME_OBJECT, &name));
        __wt_verbose_debug2(
          session, WT_VERB_TIERED, "DROP_TIERED: remove object %s from metadata", name);
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
                WT_ERR(__wt_meta_track_drop_object(session, tiered_tmp.bstorage, filename));
        }
        __wt_free(session, name);
    }

    /*
     * If everything is successful, remove any tiered work associated with this tiered handle. The
     * dhandle has been released here but queued work may still refer to it. The queued work unit
     * has its own reference to it and we're holding the lock so it isn't yet stale.
     */
    __wt_verbose(session, WT_VERB_TIERED, "DROP_TIERED: remove work for %p", (void *)tiered);
    __wt_tiered_remove_work(session, tiered, true);
    __wt_spin_unlock(session, &conn->tiered_lock);

    ret = __wt_metadata_remove(session, uri);

err:
    if (got_dhandle)
        WT_TRET(__wt_session_release_dhandle(session));
    __wt_free(session, name);
    __wt_spin_unlock_if_owned(session, &conn->tiered_lock);
    return (ret);
}

/*
 * __schema_drop --
 *     Process a WT_SESSION::drop operation for all supported types.
 */
static int
__schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[], bool check_visibility)
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
        ret = __drop_colgroup(session, uri, force, cfg, check_visibility);
    else if (WT_PREFIX_MATCH(uri, "file:"))
        ret = __drop_file(session, uri, force, cfg, check_visibility);
    else if (WT_PREFIX_MATCH(uri, "index:"))
        ret = __drop_index(session, uri, force, cfg, check_visibility);
    else if (WT_PREFIX_MATCH(uri, "layered:"))
        ret = __drop_layered(session, uri, force, cfg, check_visibility);
    else if (WT_PREFIX_MATCH(uri, "table:"))
        ret = __drop_table(session, uri, force, cfg, check_visibility);
    else if (WT_PREFIX_MATCH(uri, "tiered:"))
        ret = __drop_tiered(session, uri, force, cfg, check_visibility);
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
__wt_schema_drop(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], bool check_visibility)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *int_session;

    /*
     * We should be calling this function with the schema lock, but we cannot verify it here because
     * we can re-enter this function with the internal session. If we get here using the internal
     * session, we cannot check whether we own the lock, as it would be locked by the outer session.
     * We can thus only check whether the lock is acquired, as opposed to, whether the lock is
     * acquired by us.
     */
    WT_ASSERT(session, __wt_spin_locked(session, &S2C(session)->schema_lock));

    WT_RET(__wti_schema_internal_session(session, &int_session));
    ret = __schema_drop(int_session, uri, cfg, check_visibility);
    WT_TRET(__wti_schema_session_release(session, int_session));
    return (ret);
}
