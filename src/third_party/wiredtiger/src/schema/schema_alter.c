/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
static int __schema_alter(WT_SESSION_IMPL *, const char *, const char *[]);

/*
 * __alter_apply --
 *     Alter an object
 */
static int
__alter_apply(
  WT_SESSION_IMPL *session, const char *uri, const char *newcfg[], const char *base_config)
{
    WT_DECL_RET;
    char *config, *newconfig;
    const char *cfg[4];

    newconfig = NULL;

    /* Find the URI */
    WT_RET(__wt_metadata_search(session, uri, &config));

    WT_ASSERT(session, newcfg[0] != NULL);

    /*
     * Start with the base configuration because collapse is like a projection and if we are reading
     * older metadata, it may not have all the components.
     */
    cfg[0] = base_config;
    cfg[1] = config;
    cfg[2] = newcfg[0];
    cfg[3] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &newconfig));
    /*
     * Only rewrite if there are changes.
     */
    if (strcmp(config, newconfig) != 0)
        WT_ERR(__wt_metadata_update(session, uri, newconfig));
    else
        WT_STAT_CONN_INCR(session, session_table_alter_skip);

err:
    __wt_free(session, config);
    __wt_free(session, newconfig);
    /*
     * Map WT_NOTFOUND to ENOENT, based on the assumption WT_NOTFOUND means there was no metadata
     * entry.
     */
    if (ret == WT_NOTFOUND)
        ret = __wt_set_return(session, ENOENT);

    return (ret);
}

/*
 * __alter_file --
 *     Alter a file.
 */
static int
__alter_file(WT_SESSION_IMPL *session, const char *newcfg[])
{
    const char *uri;

    /*
     * We know that we have exclusive access to the file. So it will be closed after we're done with
     * it and the next open will see the updated metadata.
     */
    uri = session->dhandle->name;
    if (!WT_PREFIX_MATCH(uri, "file:"))
        return (__wt_unexpected_object_type(session, uri, "file:"));

    return (__alter_apply(session, uri, newcfg, WT_CONFIG_BASE(session, file_meta)));
}

/*
 * __alter_tier --
 *     Alter a tier.
 */
static int
__alter_tier(WT_SESSION_IMPL *session, const char *newcfg[])
{
    const char *uri;

    /*
     * We know that we have exclusive access to the tier. So it will be closed after we're done with
     * it and the next open will see the updated metadata.
     */
    uri = session->dhandle->name;
    if (!WT_PREFIX_MATCH(uri, "tier:"))
        return (__wt_unexpected_object_type(session, uri, "tier:"));

    return (__alter_apply(session, uri, newcfg, WT_CONFIG_BASE(session, tier_meta)));
}

/*
 * __alter_object --
 *     Alter a tiered object. There are no object dhandles.
 */
static int
__alter_object(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
{
    if (!WT_PREFIX_MATCH(uri, "object:"))
        return (__wt_unexpected_object_type(session, uri, "object:"));

    return (__alter_apply(session, uri, newcfg, WT_CONFIG_BASE(session, object_meta)));
}

/*
 * __alter_get_object_id_range --
 *     Get current and oldest object IDs for the tiered object.
 */
static int
__alter_get_object_id_range(WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *uri,
  uint32_t *current_idp, uint32_t *oldest_idp)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint32_t current_id, oldest_id;
    char *value;

    *current_idp = *oldest_idp = WT_TIERED_OBJECTID_NONE;
    value = NULL;
    /*
     * First try to get oldest and current object IDs from the tiered object. If it's not
     * initialized, get this information from the metadata.
     */
    current_id = tiered->current_id;
    oldest_id = tiered->oldest_id;
    if (current_id == WT_TIERED_OBJECTID_NONE) {
        WT_RET(__wt_metadata_search(session, uri, &value));

        WT_ERR(__wt_config_getones(session, value, "oldest", &cval));
        oldest_id = (uint32_t)cval.val;

        WT_ERR(__wt_config_getones(session, value, "last", &cval));
        current_id = (uint32_t)cval.val;
    }

    *current_idp = current_id;
    *oldest_idp = oldest_id;
err:
    __wt_free(session, value);

    return (ret);
}

/*
 * __alter_objects --
 *     Alter all objects in the oldest-current range.
 */
static int
__alter_objects(WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *newcfg[],
  uint32_t current_id, uint32_t oldest_id)
{
    WT_DECL_RET;
    uint32_t object_id;
    char *value;
    const char *name;

    value = NULL;
    name = NULL;

    /*
     * Iterate over all objects in the range oldest-current object IDs and alter metadata of each of
     * them.
     */
    for (object_id = oldest_id; object_id < current_id; object_id++) {
        WT_ERR(__wt_tiered_name(session, &tiered->iface, object_id, WT_TIERED_NAME_OBJECT, &name));

        /*
         * Check if this object present in the metadata. Skip it if not found. It is expected for
         * the range of objects to be not contiguous, because some objects may have been deleted.
         */
        ret = __wt_metadata_search(session, name, &value);
        __wt_free(session, value);
        WT_ERR_NOTFOUND_OK(ret, true);
        if (ret == 0) {
            WT_WITH_DHANDLE(session, NULL, ret = __schema_alter(session, name, newcfg));
            WT_ERR(ret);
        }
        __wt_free(session, name);
    }
err:
    __wt_free(session, name);

    return (ret);
}

/*
 * __alter_tiered --
 *     Alter a tiered metadata.
 */
static int
__alter_tiered(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[], uint32_t flags)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_TIERED *tiered;
    uint32_t current_id, name_flag, oldest_id;
    u_int i;
    char *value;
    const char *name;

    dhandle = NULL;
    value = NULL;
    name = NULL;

    if (!WT_PREFIX_MATCH(uri, "tiered:"))
        return (__wt_unexpected_object_type(session, uri, "tiered:"));

    /*
     * If the operation requires exclusive access, close any open handles, including checkpoints.
     */
    if (FLD_ISSET(flags, WT_DHANDLE_EXCLUSIVE)) {
        WT_WITH_HANDLE_LIST_WRITE_LOCK(
          session, ret = __wt_conn_dhandle_close_all(session, uri, false, false, false));
        WT_RET(ret);
    }

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, flags));
    tiered = (WT_TIERED *)session->dhandle;

    WT_ERR(__alter_get_object_id_range(session, tiered, uri, &current_id, &oldest_id));

    /* Alter each tier. */
    for (i = 0; i < WT_TIERED_MAX_TIERS; i++) {
        dhandle = tiered->tiers[i].tier;
        if (dhandle == NULL) {
            /*
             * Tiers may not be initialized because we open tiered handle with lock only flag. In
             * this case we need to find the names of the tiers manually.
             */
            if (i == WT_TIERED_INDEX_LOCAL)
                name_flag = WT_TIERED_NAME_LOCAL;
            else if (i == WT_TIERED_INDEX_SHARED)
                name_flag = WT_TIERED_NAME_SHARED;
            else
                continue;

            WT_ERR(__wt_tiered_name(session, &tiered->iface, current_id, name_flag, &name));

            /* Check if metadata has entry for this tier. */
            ret = __wt_metadata_search(session, name, &value);
            __wt_free(session, value);
            WT_ERR_NOTFOUND_OK(ret, true);
            if (ret != 0) {
                /*
                 * Not found in the metadata. Skip it. This is expected, for instance, in the
                 * scenario when tier hasn't been flushed and there's no shared tier yet.
                 */
                __wt_free(session, name);
                continue;
            }
        }

        WT_WITH_DHANDLE(session, NULL,
          ret = __schema_alter(session, name == NULL ? dhandle->name : name, newcfg));
        __wt_free(session, name);
        WT_ERR(ret);
    }

    /* Alter all objects. */
    WT_ERR(__alter_objects(session, tiered, newcfg, current_id, oldest_id));

    /* Apply change to the tiered metadata. */
    WT_ERR(__alter_apply(session, uri, newcfg, WT_CONFIG_BASE(session, tiered_meta)));

err:
    WT_TRET(__wt_session_release_dhandle(session));
    __wt_free(session, name);

    return (ret);
}

/*
 * __alter_tree --
 *     Alter an index or colgroup reference.
 */
static int
__alter_tree(WT_SESSION_IMPL *session, const char *name, const char *newcfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_ITEM(data_source);
    WT_DECL_RET;
    char *value;
    bool is_colgroup;

    value = NULL;

    is_colgroup = WT_PREFIX_MATCH(name, "colgroup:");
    if (!is_colgroup && !WT_PREFIX_MATCH(name, "index:"))
        return (__wt_unexpected_object_type(session, name, "'colgroup:' or 'index:'"));

    /* Read the schema value. */
    WT_ERR(__wt_metadata_search(session, name, &value));

    /* Get the data source URI, converting not-found errors to EINVAL for the application. */
    if ((ret = __wt_config_getones(session, value, "source", &cval)) != 0)
        WT_ERR_MSG(session, ret == WT_NOTFOUND ? EINVAL : ret,
          "index or column group has no data source: %s", value);

    WT_ERR(__wt_scr_alloc(session, 0, &data_source));
    WT_ERR(__wt_buf_fmt(session, data_source, "%.*s", (int)cval.len, cval.str));

    /* Alter the data source */
    WT_ERR(__schema_alter(session, data_source->data, newcfg));

    /* Alter the index or colgroup */
    if (is_colgroup)
        WT_ERR(__alter_apply(session, name, newcfg, WT_CONFIG_BASE(session, colgroup_meta)));
    else
        WT_ERR(__alter_apply(session, name, newcfg, WT_CONFIG_BASE(session, index_meta)));

err:
    __wt_scr_free(session, &data_source);
    __wt_free(session, value);
    return (ret);
}

/*
 * __alter_table --
 *     Alter a table.
 */
static int
__alter_table(
  WT_SESSION_IMPL *session, const char *uri, const char *newcfg[], bool exclusive_refreshed)
{
    WT_COLGROUP *colgroup;
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_TABLE *table;
    u_int i;
    const char *name;

    colgroup = NULL;
    table = NULL;
    name = uri;
    WT_PREFIX_SKIP_REQUIRED(session, name, "table:");

    /*
     * If we have exclusive access update all objects in the schema for this table and reopen the
     * handle to update the in-memory state.
     */
    if (exclusive_refreshed) {
        /*
         * Open the table so we can alter its column groups and indexes, keeping the table locked
         * exclusive across the alter.
         */
        WT_RET(__wt_schema_get_table_uri(session, uri, true, WT_DHANDLE_EXCLUSIVE, &table));
        /*
         * Meta tracking needs to be used because alter needs to be atomic.
         */
        WT_ASSERT(session, WT_META_TRACKING(session));
        WT_WITH_DHANDLE(session, &table->iface, ret = __wt_meta_track_handle_lock(session, false));
        WT_RET(ret);

        /* Alter the column groups. */
        for (i = 0; i < WT_COLGROUPS(table); i++) {
            if ((colgroup = table->cgroups[i]) == NULL)
                continue;
            WT_RET(__alter_tree(session, colgroup->name, newcfg));
        }

        /* Alter the indices. */
        WT_RET(__wt_schema_open_indices(session, table));
        for (i = 0; i < table->nindices; i++) {
            if ((idx = table->indices[i]) == NULL)
                continue;
            WT_RET(__alter_tree(session, idx->name, newcfg));
        }
    }

    /* Alter the table */
    WT_RET(__alter_apply(session, uri, newcfg, WT_CONFIG_BASE(session, table_meta)));

    return (ret);
}

/*
 * __schema_alter --
 *     Alter an object.
 */
static int
__schema_alter(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
{
    WT_CONFIG_ITEM cv;
    uint32_t flags;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_alter), newcfg[0], NULL};
    bool exclusive_refreshed;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    /*
     * Determine what configuration says about exclusive access. A non exclusive alter that doesn't
     * refresh in-memory configuration is only valid for the table objects.
     */
    WT_RET(__wt_config_gets(session, cfg, "exclusive_refreshed", &cv));
    exclusive_refreshed = (bool)cv.val;

    if (!exclusive_refreshed && !WT_PREFIX_MATCH(uri, "table:"))
        WT_RET_MSG(session, EINVAL,
          "option \"exclusive_refreshed\" "
          "is applicable only on simple tables");

    /*
     * The exclusive flag avoids conflicts with other operations and the lock only flag is required
     * because we don't need to have a handle to update the metadata and opening the handle causes
     * problems when meta tracking is enabled.
     */
    flags = WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY;
    if (WT_PREFIX_MATCH(uri, "file:"))
        return (__wti_execute_handle_operation(session, uri, __alter_file, newcfg, flags));
    if (WT_PREFIX_MATCH(uri, "colgroup:") || WT_PREFIX_MATCH(uri, "index:"))
        return (__alter_tree(session, uri, newcfg));
    if (WT_PREFIX_MATCH(uri, "object:"))
        return (__alter_object(session, uri, newcfg));
    if (WT_PREFIX_MATCH(uri, "table:"))
        return (__alter_table(session, uri, newcfg, exclusive_refreshed));
    if (WT_PREFIX_MATCH(uri, "tier:"))
        return (__wti_execute_handle_operation(session, uri, __alter_tier, newcfg, flags));
    if (WT_PREFIX_MATCH(uri, "tiered:"))
        return (__alter_tiered(session, uri, newcfg, flags));
    return (__wt_bad_object_type(session, uri));
}

/*
 * __wt_schema_alter --
 *     Alter an object.
 */
int
__wt_schema_alter(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
{
    struct timespec tsp;
    WT_DECL_RET;
    WT_SESSION_IMPL *int_session;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    /* Add a 2 second wait to simulate session alter slowness. */
    tsp.tv_sec = 2;
    tsp.tv_nsec = 0;
    __wt_timing_stress(session, WT_TIMING_STRESS_SESSION_ALTER_SLOW, &tsp);

    WT_RET(__wti_schema_internal_session(session, &int_session));
    WT_ERR(__wt_meta_track_on(int_session));
    ret = __schema_alter(int_session, uri, newcfg);
    WT_TRET(__wt_meta_track_off(int_session, true, ret != 0));
err:
    WT_TRET(__wti_schema_session_release(session, int_session));
    return (ret);
}
