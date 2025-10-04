/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_execute_handle_operation --
 *     Apply a function to a handle, getting exclusive access if requested.
 */
int
__wti_execute_handle_operation(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[], uint32_t open_flags)
{
    WT_DECL_RET;

    /*
     * If the operation requires exclusive access, close any open file handles, including
     * checkpoints.
     */
    if (FLD_ISSET(open_flags, WT_DHANDLE_EXCLUSIVE)) {
        WT_WITH_HANDLE_LIST_WRITE_LOCK(
          session, ret = __wt_conn_dhandle_close_all(session, uri, false, false, false));
        WT_RET(ret);
    }

    WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, open_flags, NULL, NULL));
    WT_SAVE_DHANDLE(session, ret = file_func(session, cfg));
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __schema_tiered_worker --
 *     Run a schema worker operation on each tier of a tiered data source.
 */
static int
__schema_tiered_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, open_flags));
    tiered = (WT_TIERED *)session->dhandle;

    for (i = 0; i < WT_TIERED_MAX_TIERS; i++) {
        dhandle = tiered->tiers[i].tier;
        if (dhandle == NULL)
            continue;
        WT_WITHOUT_DHANDLE(session,
          ret = __wt_schema_worker(session, dhandle->name, file_func, name_func, cfg, open_flags));
        WT_ERR(ret);
    }

err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __schema_layered_worker_verify --
 *     Run a schema worker operation (which is verification) on the layered table.
 *
 * FIXME-WT-15047: Implement ingest table verification on followers.
 */
static int
__schema_layered_worker_verify(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    int ingest_ret, stable_ret;

    conn = S2C(session);
    ingest_ret = 0;

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, open_flags));
    WT_LAYERED_TABLE *layered = (WT_LAYERED_TABLE *)session->dhandle;

    const char *ingest_uri = layered->ingest_uri;
    const char *stable_uri = layered->stable_uri;

    /*
     * FIXME-WT-15413 - Verify assumes the stable table always exists. However, on followers that
     * have not yet picked up their first checkpoint, the stable constituent will be missing. We
     * should handle this transient state by skipping stable verification instead of failing with
     * ENOENT.
     */
    WT_ASSERT(session, stable_uri != NULL);
    WT_ASSERT(session, ingest_uri != NULL);
    WT_ASSERT(session, file_func == __wt_verify);

    /*
     * Verifying stable tables of layered tables uses the existing verify logic.
     * Ingest tables, however, require special handling:
     * - On leader: ingest must always be empty/no-op.
     * - On followers: FIXME-WT-15047 ingest tables are not checkpointed.
     */

    /* Verify the stable table of the layered table. */
    WT_WITHOUT_DHANDLE(session,
      stable_ret = __wt_schema_worker(session, stable_uri, file_func, name_func, cfg, open_flags));

    if (stable_ret != 0 && stable_ret != EBUSY)
        WT_ERR_MSG(session, stable_ret, "Verify (layered): %s stable table verification failed. ",
          stable_uri);

    /*
     * Verify the ingest table of the layered table. FIXME-WT-15047: Implement ingest table
     * verification on followers.
     */
    if (conn->layered_table_manager.leader) {
        /*
         * On leader, if verifying ingest returns EBUSY, it means ingest is not empty (dirty
         * content, or open cursors), which is an invalid state.
         */
        WT_WITHOUT_DHANDLE(session,
          ingest_ret =
            __wt_schema_worker(session, ingest_uri, file_func, name_func, cfg, open_flags));

        WT_ASSERT_ALWAYS(session, ingest_ret != EBUSY,
          "Verify: %s ingest table on leader cannot be verified. "
          "Ingest contains dirty content or open cursors, which is an invalid "
          "state.",
          ingest_uri);

        WT_ERR_MSG_CHK(session, ingest_ret,
          "Verify (layered): %s ingest table verification failed. Ingest on leader must be empty.",
          ingest_uri);
    }

err:
    __wt_verbose_level(session, WT_VERB_VERIFY, WT_VERBOSE_DEBUG_2,
      "Verify (layered): stable table %s returned %s, ingest table %s returned %s", stable_uri,
      __wt_wiredtiger_error(stable_ret), ingest_uri, __wt_wiredtiger_error(ingest_ret));

    WT_TRET(__wt_session_release_dhandle(session));

    /* Ingest is expected to never return EBUSY so it's enough to check it for 0 only */
    ret = ingest_ret != 0 ? ingest_ret : stable_ret;

    return (ret);
}

/*
 * __wt_schema_worker --
 *     Get Btree handles for the object and cycle through calls to an underlying worker function
 *     with each handle.
 */
int
__wt_schema_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
{
    WT_COLGROUP *colgroup;
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_SESSION *wt_session;
    WT_TABLE *table;
    u_int i;
    bool is_tiered, skip;

    table = NULL;
    skip = false;
    WT_NOT_READ(is_tiered, false);

    if (name_func != NULL)
        WT_ERR(name_func(session, uri, &skip));

    /* If the callback said to skip this object, we're done. */
    if (skip)
        return (0);

    /* Tiered tables do not support verify or salvage operations. */
    is_tiered = WT_PREFIX_MATCH(uri, "object:") || WT_PREFIX_MATCH(uri, "tier:") ||
      WT_PREFIX_MATCH(uri, "tiered:");
    if (is_tiered && (file_func == __wt_salvage || file_func == __wt_verify))
        WT_ERR(ENOTSUP);

    /* Get the btree handle(s) and call the underlying function. */
    if (WT_PREFIX_MATCH(uri, "file:")) {
        if (file_func != NULL)
            WT_ERR(__wti_execute_handle_operation(session, uri, file_func, cfg, open_flags));
    } else if (WT_PREFIX_MATCH(uri, "colgroup:")) {
        WT_ERR(__wt_schema_get_colgroup(session, uri, false, NULL, &colgroup));
        WT_ERR(
          __wt_schema_worker(session, colgroup->source, file_func, name_func, cfg, open_flags));
    } else if (WT_PREFIX_MATCH(uri, "index:")) {
        idx = NULL;
        WT_ERR(__wti_schema_get_index(session, uri, false, false, &idx));
        WT_ERR(__wt_schema_worker(session, idx->source, file_func, name_func, cfg, open_flags));
    } else if (WT_PREFIX_MATCH(uri, "table:")) {
        /*
         * Note: we would like to use open_flags here (e.g., to lock the table exclusive during
         * schema-changing operations), but that is currently problematic because we get the table
         * again in order to discover column groups and indexes.
         */
        WT_ERR(__wt_schema_get_table_uri(session, uri, false, 0, &table));

        /*
         * We could make a recursive call for each colgroup or index URI, but since we have already
         * opened the table, we can take a short cut and skip straight to the sources. If we have a
         * name function, it needs to know about the intermediate URIs.
         */
        for (i = 0; i < WT_COLGROUPS(table); i++) {
            colgroup = table->cgroups[i];

            /* Verify is not implemented for tiered tables. */
            if ((file_func == __wt_salvage || file_func == __wt_verify) &&
              WT_PREFIX_MATCH(colgroup->source, "tiered:"))
                WT_ERR(ENOTSUP);

            skip = false;
            if (name_func != NULL)
                WT_ERR(name_func(session, colgroup->name, &skip));
            if (!skip)
                WT_ERR(__wt_schema_worker(
                  session, colgroup->source, file_func, name_func, cfg, open_flags));
        }

        /*
         * Some operations that walk handles, such as backup, need to open indexes. Others, such as
         * checkpoints, do not. Opening indexes requires the handle write lock, so check whether
         * that lock is held when deciding what to do.
         */
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE))
            WT_ERR(__wt_schema_open_indices(session, table));

        for (i = 0; i < table->nindices; i++) {
            idx = table->indices[i];
            skip = false;
            if (name_func != NULL)
                WT_ERR(name_func(session, idx->name, &skip));
            if (!skip)
                WT_ERR(
                  __wt_schema_worker(session, idx->source, file_func, name_func, cfg, open_flags));
        }
    } else if (WT_PREFIX_MATCH(uri, "layered:") && file_func == __wt_verify) {
        WT_ERR(__schema_layered_worker_verify(session, uri, file_func, name_func, cfg, open_flags));
    } else if (WT_PREFIX_MATCH(uri, "tiered:")) {
        WT_ERR(__schema_tiered_worker(session, uri, file_func, name_func, cfg, open_flags));
    } else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL) {
        wt_session = (WT_SESSION *)session;
        if (file_func == __wt_salvage && dsrc->salvage != NULL)
            WT_ERR(dsrc->salvage(dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
        else if (file_func == __wt_verify && dsrc->verify != NULL)
            WT_ERR(dsrc->verify(dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
        else if (file_func == __wt_checkpoint_file)
            ;
        else if (file_func == __wt_checkpoint_get_handles)
            ;
        else if (file_func == __wt_checkpoint_sync)
            ;
        else
            WT_ERR(__wt_object_unsupported(session, uri));
    } else
        WT_ERR(__wt_bad_object_type(session, uri));

err:
    WT_TRET(__wt_schema_release_table(session, &table));
    return (ret);
}
