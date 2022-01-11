/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __session_rollback_transaction(WT_SESSION *, const char *);

/*
 * __wt_session_notsup --
 *     Unsupported session method.
 */
int
__wt_session_notsup(WT_SESSION_IMPL *session)
{
    WT_RET_MSG(session, ENOTSUP, "Unsupported session method");
}

/*
 * __wt_session_reset_cursors --
 *     Reset all open cursors.
 */
int
__wt_session_reset_cursors(WT_SESSION_IMPL *session, bool free_buffers)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    TAILQ_FOREACH (cursor, &session->cursors, q) {
        /* Stop when there are no positioned cursors. */
        if (session->ncursors == 0)
            break;
        if (!F_ISSET(cursor, WT_CURSTD_JOINED))
            WT_TRET(cursor->reset(cursor));
        /* Optionally, free the cursor buffers */
        if (free_buffers) {
            __wt_buf_free(session, &cursor->key);
            __wt_buf_free(session, &cursor->value);
        }
    }

    WT_ASSERT(session, session->ncursors == 0);
    return (ret);
}

/*
 * __wt_session_cursor_cache_sweep --
 *     Sweep the cursor cache.
 */
int
__wt_session_cursor_cache_sweep(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor, *cursor_tmp;
    WT_CURSOR_LIST *cached_list;
    WT_DECL_RET;
#ifdef HAVE_DIAGNOSTIC
    WT_DATA_HANDLE *saved_dhandle;
#endif
    uint64_t now;
    uint32_t position;
    int i, t_ret, nbuckets, nexamined, nclosed;
    bool productive;

    if (!F_ISSET(session, WT_SESSION_CACHE_CURSORS))
        return (0);

    /*
     * Periodically sweep for dead cursors; if we've swept recently, don't do it again.
     */
    __wt_seconds(session, &now);
    if (now - session->last_cursor_sweep < 1)
        return (0);
    session->last_cursor_sweep = now;

    position = session->cursor_sweep_position;
    productive = true;
    nbuckets = nexamined = nclosed = 0;
#ifdef HAVE_DIAGNOSTIC
    saved_dhandle = session->dhandle;
#endif

    /* Turn off caching so that cursor close doesn't try to cache. */
    F_CLR(session, WT_SESSION_CACHE_CURSORS);
    for (i = 0; i < WT_SESSION_CURSOR_SWEEP_MAX && productive; i++) {
        ++nbuckets;
        cached_list = &session->cursor_cache[position];
        position = (position + 1) & (S2C(session)->hash_size - 1);
        TAILQ_FOREACH_SAFE(cursor, cached_list, q, cursor_tmp)
        {
            /*
             * First check to see if the cursor could be reopened and should be swept.
             */
            ++nexamined;
            t_ret = cursor->reopen(cursor, true);
            if (t_ret != 0) {
                WT_TRET_NOTFOUND_OK(t_ret);
                WT_TRET_NOTFOUND_OK(cursor->reopen(cursor, false));
                WT_TRET(cursor->close(cursor));
                ++nclosed;
            }
        }

        /*
         * We continue sweeping as long as we have some good average productivity, or we are under
         * the minimum.
         */
        productive = (nclosed + WT_SESSION_CURSOR_SWEEP_MIN > i);
    }

    session->cursor_sweep_position = position;
    F_SET(session, WT_SESSION_CACHE_CURSORS);

    WT_STAT_CONN_INCR(session, cursor_sweep);
    WT_STAT_CONN_INCRV(session, cursor_sweep_buckets, nbuckets);
    WT_STAT_CONN_INCRV(session, cursor_sweep_examined, nexamined);
    WT_STAT_CONN_INCRV(session, cursor_sweep_closed, nclosed);

    WT_ASSERT(session, session->dhandle == saved_dhandle);
    return (ret);
}

/*
 * __wt_session_copy_values --
 *     Copy values into all positioned cursors, so that they don't keep transaction IDs pinned.
 */
int
__wt_session_copy_values(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;

    TAILQ_FOREACH (cursor, &session->cursors, q)
        if (F_ISSET(cursor, WT_CURSTD_VALUE_INT)) {
#ifdef HAVE_DIAGNOSTIC
            /*
             * We have to do this with a transaction ID pinned unless the cursor is reading from a
             * checkpoint.
             */
            WT_TXN_SHARED *txn_shared = WT_SESSION_TXN_SHARED(session);
            WT_ASSERT(session,
              txn_shared->pinned_id != WT_TXN_NONE ||
                (WT_BTREE_PREFIX(cursor->uri) &&
                  F_ISSET((WT_CURSOR_BTREE *)cursor, WT_CBT_NO_TXN)));
#endif
            WT_RET(__cursor_localvalue(cursor));
        }

    return (0);
}

/*
 * __wt_session_release_resources --
 *     Release common session resources.
 */
int
__wt_session_release_resources(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    bool done;

    /*
     * Called when sessions are reset and closed, and when heavy-weight session methods or functions
     * complete (for example, checkpoint and compact). If the session has no open cursors discard it
     * all; if there are cursors, discard what we can safely clean out.
     */
    done = TAILQ_FIRST(&session->cursors) == NULL;

    /* Transaction cleanup */
    if (done)
        __wt_txn_release_resources(session);

    /* Block manager cleanup */
    if (session->block_manager_cleanup != NULL)
        WT_TRET(session->block_manager_cleanup(session));

    /* Reconciliation cleanup */
    if (session->reconcile_cleanup != NULL)
        WT_TRET(session->reconcile_cleanup(session));

    /* Stashed memory. */
    __wt_stash_discard(session);

    /* Scratch buffers and error memory. */
    if (done) {
        __wt_scr_discard(session);
        __wt_buf_free(session, &session->err);
    }

    return (ret);
}

/*
 * __session_clear --
 *     Clear a session structure.
 */
static void
__session_clear(WT_SESSION_IMPL *session)
{
    /*
     * There's no serialization support around the review of the hazard array, which means threads
     * checking for hazard pointers first check the active field (which may be 0) and then use the
     * hazard pointer (which cannot be NULL).
     *
     * Additionally, the session structure can include information that persists past the session's
     * end-of-life, stored as part of page splits.
     *
     * For these reasons, be careful when clearing the session structure.
     */
    memset(session, 0, WT_SESSION_CLEAR_SIZE);

    session->hazard_inuse = 0;
    session->nhazard = 0;
}

/*
 * __session_close_cursors --
 *     Close all cursors in a list.
 */
static int
__session_close_cursors(WT_SESSION_IMPL *session, WT_CURSOR_LIST *cursors)
{
    WT_CURSOR *cursor, *cursor_tmp;
    WT_DECL_RET;

    /* Close all open cursors. */
    WT_TAILQ_SAFE_REMOVE_BEGIN(cursor, cursors, q, cursor_tmp)
    {
        if (F_ISSET(cursor, WT_CURSTD_CACHED))
            /*
             * Put the cached cursor in an open state that allows it to be closed.
             */
            WT_TRET_NOTFOUND_OK(cursor->reopen(cursor, false));
        else if (session->event_handler->handle_close != NULL &&
          strcmp(cursor->internal_uri, WT_HS_URI) != 0)
            /*
             * Notify the user that we are closing the cursor handle via the registered close
             * callback.
             */
            WT_TRET(session->event_handler->handle_close(
              session->event_handler, &session->iface, cursor));

        WT_TRET(cursor->close(cursor));
    }
    WT_TAILQ_SAFE_REMOVE_END

    return (ret);
}

/*
 * __session_close_cached_cursors --
 *     Fully close all cached cursors.
 */
static int
__session_close_cached_cursors(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    uint64_t i;

    for (i = 0; i < S2C(session)->hash_size; i++)
        WT_TRET(__session_close_cursors(session, &session->cursor_cache[i]));
    return (ret);
}

/*
 * __session_close --
 *     WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    SESSION_API_CALL_PREPARE_ALLOWED(session, close, config, cfg);
    WT_UNUSED(cfg);

    WT_TRET(__wt_session_close_internal(session));
    session = NULL;

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_close_internal --
 *     Internal function of WT_SESSION->close method.
 */
int
__wt_session_close_internal(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Close all open cursors while the cursor cache is disabled. */
    F_CLR(session, WT_SESSION_CACHE_CURSORS);

    /* Rollback any active transaction. */
    if (F_ISSET(session->txn, WT_TXN_RUNNING))
        WT_TRET(__session_rollback_transaction((WT_SESSION *)session, NULL));

    /*
     * Also release any pinned transaction ID from a non-transactional operation.
     */
    if (conn->txn_global.txn_shared_list != NULL)
        __wt_txn_release_snapshot(session);

    /*
     * Close all open cursors. We don't need to explicitly close the session's pointer to the
     * history store cursor since it will also be included in session's cursor table.
     */
    WT_TRET(__session_close_cursors(session, &session->cursors));
    WT_TRET(__session_close_cached_cursors(session));

    WT_ASSERT(session, session->ncursors == 0);

    /* Discard cached handles. */
    __wt_session_close_cache(session);

    /* Confirm we're not holding any hazard pointers. */
    __wt_hazard_close(session);

    /* Discard metadata tracking. */
    __wt_meta_track_discard(session);

    /*
     * Close the file where we tracked long operations. Do this before releasing resources, as we do
     * scratch buffer management when we flush optrack buffers to disk.
     */
    if (F_ISSET(conn, WT_CONN_OPTRACK)) {
        if (session->optrackbuf_ptr > 0) {
            __wt_optrack_flush_buffer(session);
            WT_TRET(__wt_close(session, &session->optrack_fh));
        }

        /* Free the operation tracking buffer */
        __wt_free(session, session->optrack_buf);
    }

    /* Release common session resources. */
    WT_TRET(__wt_session_release_resources(session));

    /* The API lock protects opening and closing of sessions. */
    __wt_spin_lock(session, &conn->api_lock);

    /*
     * Free transaction information: inside the lock because we're freeing the WT_TXN structure and
     * RTS looks at it.
     */
    __wt_txn_destroy(session);

    /* Decrement the count of open sessions. */
    WT_STAT_CONN_DECR(session, session_open);

    /*
     * Sessions are re-used, clear the structure: the clear sets the active field to 0, which will
     * exclude the hazard array from review by the eviction thread. Because some session fields are
     * accessed by other threads, the structure must be cleared carefully.
     *
     * We don't need to publish here, because regardless of the active field being non-zero, the
     * hazard pointer is always valid.
     */
    __session_clear(session);
    session = conn->default_session;

    /*
     * Decrement the count of active sessions if that's possible: a session being closed may or may
     * not be at the end of the array, step toward the beginning of the array until we reach an
     * active session.
     */
    while (conn->sessions[conn->session_cnt - 1].active == 0)
        if (--conn->session_cnt == 0)
            break;

    __wt_spin_unlock(session, &conn->api_lock);

    return (ret);
}

/*
 * __session_reconfigure --
 *     WT_SESSION->reconfigure method.
 */
static int
__session_reconfigure(WT_SESSION *wt_session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_NOT_ALLOWED(session, reconfigure, config, cfg);
    WT_UNUSED(cfg);

    WT_ERR(__wt_txn_context_check(session, false));

    WT_ERR(__wt_session_reset_cursors(session, false));

    /*
     * Note that this method only checks keys that are passed in by the application: we don't want
     * to reset other session settings to their default values.
     */
    WT_ERR(__wt_txn_reconfigure(session, config));

    ret = __wt_config_getones(session, config, "ignore_cache_size", &cval);
    if (ret == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_IGNORE_CACHE_SIZE);
        else
            F_CLR(session, WT_SESSION_IGNORE_CACHE_SIZE);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    ret = __wt_config_getones(session, config, "cache_cursors", &cval);
    if (ret == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_CACHE_CURSORS);
        else {
            F_CLR(session, WT_SESSION_CACHE_CURSORS);
            WT_ERR(__session_close_cached_cursors(session));
        }
    }

    /*
     * There is a session debug configuration which can be set to evict pages as they are released
     * and no longer needed.
     */
    if ((ret = __wt_config_getones(session, config, "debug.release_evict_page", &cval)) == 0) {
        if (cval.val) {
            F_SET(session, WT_SESSION_DEBUG_RELEASE_EVICT);
        } else {
            F_CLR(session, WT_SESSION_DEBUG_RELEASE_EVICT);
        }
    }

    WT_ERR_NOTFOUND_OK(ret, false);

    ret = __wt_config_getones(session, config, "cache_max_wait_ms", &cval);
    if (ret == 0 && cval.val)
        session->cache_max_wait_us = (uint64_t)(cval.val * WT_THOUSAND);
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_open_cursor_int --
 *     Internal version of WT_SESSION::open_cursor, with second cursor arg.
 */
static int
__session_open_cursor_int(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  WT_CURSOR *other, const char *cfg[], uint64_t hash_value, WT_CURSOR **cursorp)
{
    WT_COLGROUP *colgroup;
    WT_DATA_SOURCE *dsrc;
    WT_DECL_RET;

    *cursorp = NULL;

    /*
     * Open specific cursor types we know about, or call the generic data source open function.
     *
     * Unwind a set of string comparisons into a switch statement hoping the compiler can make it
     * fast, but list the common choices first instead of sorting so if/else patterns are still
     * fast.
     */
    switch (uri[0]) {
    /*
     * Common cursor types.
     */
    case 't':
        if (WT_PREFIX_MATCH(uri, "table:"))
            WT_RET(__wt_curtable_open(session, uri, owner, cfg, cursorp));
        if (WT_PREFIX_MATCH(uri, "tiered:"))
            WT_RET(__wt_curfile_open(session, uri, owner, cfg, cursorp));
        break;
    case 'c':
        if (WT_PREFIX_MATCH(uri, "colgroup:")) {
            /*
             * Column groups are a special case: open a cursor on the underlying data source.
             */
            WT_RET(__wt_schema_get_colgroup(session, uri, false, NULL, &colgroup));
            WT_RET(__wt_open_cursor(session, colgroup->source, owner, cfg, cursorp));
        } else if (WT_PREFIX_MATCH(uri, "config:"))
            WT_RET(__wt_curconfig_open(session, uri, cfg, cursorp));
        break;
    case 'i':
        if (WT_PREFIX_MATCH(uri, "index:"))
            WT_RET(__wt_curindex_open(session, uri, owner, cfg, cursorp));
        break;
    case 'j':
        if (WT_PREFIX_MATCH(uri, "join:"))
            WT_RET(__wt_curjoin_open(session, uri, owner, cfg, cursorp));
        break;
    case 'l':
        if (WT_PREFIX_MATCH(uri, "lsm:"))
            WT_RET(__wt_clsm_open(session, uri, owner, cfg, cursorp));
        else if (WT_PREFIX_MATCH(uri, "log:"))
            WT_RET(__wt_curlog_open(session, uri, cfg, cursorp));
        break;

    /*
     * Less common cursor types.
     */
    case 'f':
        if (WT_PREFIX_MATCH(uri, "file:"))
            WT_RET(__wt_curfile_open(session, uri, owner, cfg, cursorp));
        break;
    case 'm':
        if (WT_PREFIX_MATCH(uri, WT_METADATA_URI))
            WT_RET(__wt_curmetadata_open(session, uri, owner, cfg, cursorp));
        break;
    case 'b':
        if (WT_PREFIX_MATCH(uri, "backup:"))
            WT_RET(__wt_curbackup_open(session, uri, other, cfg, cursorp));
        break;
    case 's':
        if (WT_PREFIX_MATCH(uri, "statistics:"))
            WT_RET(__wt_curstat_open(session, uri, other, cfg, cursorp));
        break;
    default:
        break;
    }

    if (*cursorp == NULL && (dsrc = __wt_schema_get_source(session, uri)) != NULL)
        WT_RET(dsrc->open_cursor == NULL ?
            __wt_object_unsupported(session, uri) :
            __wt_curds_open(session, uri, owner, cfg, dsrc, cursorp));

    if (*cursorp == NULL)
        return (__wt_bad_object_type(session, uri));

    if (owner != NULL) {
        /*
         * We support caching simple cursors that have no children. If this cursor is a child, we're
         * not going to cache this child or its parent.
         */
        F_CLR(owner, WT_CURSTD_CACHEABLE);
        F_CLR(*cursorp, WT_CURSTD_CACHEABLE);
    }

    /*
     * When opening simple tables, the table code calls this function on the underlying data source,
     * in which case the application's URI has been copied.
     */
    if ((*cursorp)->uri == NULL && (ret = __wt_strdup(session, uri, &(*cursorp)->uri)) != 0) {
        WT_TRET((*cursorp)->close(*cursorp));
        *cursorp = NULL;
    }

    if (*cursorp != NULL)
        (*cursorp)->uri_hash = hash_value;

    return (ret);
}

/*
 * __wt_open_cursor --
 *     Internal version of WT_SESSION::open_cursor.
 */
int
__wt_open_cursor(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_DECL_RET;
    uint64_t hash_value;

    hash_value = 0;

    /* We should not open other cursors when there are open history store cursors in the session. */
    WT_ASSERT(session, strcmp(uri, WT_HS_URI) == 0 || session->hs_cursor_counter == 0);

    /* We do not cache any subordinate tables/files cursors. */
    if (owner == NULL) {
        __wt_cursor_get_hash(session, uri, NULL, &hash_value);
        if ((ret = __wt_cursor_cache_get(session, uri, hash_value, NULL, cfg, cursorp)) == 0)
            return (0);
        WT_RET_NOTFOUND_OK(ret);
    }

    return (__session_open_cursor_int(session, uri, owner, NULL, cfg, hash_value, cursorp));
}

/*
 * __session_open_cursor --
 *     WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session, const char *uri, WT_CURSOR *to_dup,
  const char *config, WT_CURSOR **cursorp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t hash_value;
    bool dup_backup, statjoin;

    cursor = *cursorp = NULL;
    hash_value = 0;
    dup_backup = false;
    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, open_cursor, config, cfg);

    statjoin = (to_dup != NULL && uri != NULL && strcmp(uri, "statistics:join") == 0);
    if (!statjoin) {
        if ((to_dup == NULL && uri == NULL) || (to_dup != NULL && uri != NULL))
            WT_ERR_MSG(session, EINVAL,
              "should be passed either a URI or a cursor to duplicate, but not both");

        __wt_cursor_get_hash(session, uri, to_dup, &hash_value);
        if ((ret = __wt_cursor_cache_get(session, uri, hash_value, to_dup, cfg, &cursor)) == 0)
            goto done;

        /*
         * Detect if we're duplicating a backup cursor specifically. That needs special handling.
         */
        if (to_dup != NULL && strcmp(to_dup->uri, "backup:") == 0)
            dup_backup = true;
        WT_ERR_NOTFOUND_OK(ret, false);

        if (to_dup != NULL) {
            uri = to_dup->uri;
            if (!WT_PREFIX_MATCH(uri, "backup:") && !WT_PREFIX_MATCH(uri, "colgroup:") &&
              !WT_PREFIX_MATCH(uri, "index:") && !WT_PREFIX_MATCH(uri, "file:") &&
              !WT_PREFIX_MATCH(uri, "lsm:") && !WT_PREFIX_MATCH(uri, WT_METADATA_URI) &&
              !WT_PREFIX_MATCH(uri, "table:") && !WT_PREFIX_MATCH(uri, "tiered:") &&
              __wt_schema_get_source(session, uri) == NULL)
                WT_ERR(__wt_bad_object_type(session, uri));
        }
    }

    if (config != NULL && (WT_PREFIX_MATCH(uri, "backup:") || to_dup != NULL))
        __wt_verbose(session, WT_VERB_BACKUP, "Backup cursor config \"%s\"", config);

    WT_ERR(__session_open_cursor_int(
      session, uri, NULL, statjoin || dup_backup ? to_dup : NULL, cfg, hash_value, &cursor));

done:
    if (to_dup != NULL && !statjoin && !dup_backup)
        WT_ERR(__wt_cursor_dup_position(to_dup, cursor));

    *cursorp = cursor;

    if (0) {
err:
        if (cursor != NULL)
            WT_TRET(cursor->close(cursor));
    }
    /*
     * Opening a cursor on a non-existent data source will set ret to either of ENOENT or
     * WT_NOTFOUND at this point. However, applications may reasonably do this inside a transaction
     * to check for the existence of a table or index.
     *
     * Failure in opening a cursor should not set an error on the transaction and WT_NOTFOUND will
     * be mapped to ENOENT.
     */

    API_END_RET_NO_TXN_ERROR(session, ret);
}

/*
 * __session_alter_internal --
 *     Internal implementation of the WT_SESSION.alter method.
 */
static int
__session_alter_internal(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
    WT_DECL_RET;

    SESSION_API_CALL(session, alter, config, cfg);

    /* In-memory ignores alter operations. */
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        goto err;

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));

    /*
     * We replace the default configuration listing with the current configuration. Otherwise the
     * defaults for values that can be altered would override settings used by the user in create.
     */
    cfg[0] = cfg[1];
    cfg[1] = NULL;
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_alter(session, uri, cfg)));

err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_alter_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_alter_success);
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_blocking_checkpoint --
 *     Perform a checkpoint or wait if it is already running to resolve an EBUSY error.
 */
static int
__session_blocking_checkpoint(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    uint64_t txn_gen;
    const char *checkpoint_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_checkpoint), NULL};

    if ((ret = __wt_txn_checkpoint(session, checkpoint_cfg, false)) == 0)
        return (0);
    WT_RET_BUSY_OK(ret);

    /*
     * If there's a checkpoint running, wait for it to complete. If there's no checkpoint running or
     * the checkpoint generation number changes, the checkpoint blocking us has completed.
     */
#define WT_CKPT_WAIT 2
    txn_global = &S2C(session)->txn_global;
    for (txn_gen = __wt_gen(session, WT_GEN_CHECKPOINT);; __wt_sleep(WT_CKPT_WAIT, 0)) {
        /*
         * This loop only checks objects that are declared volatile, therefore no barriers are
         * needed.
         */
        if (!txn_global->checkpoint_running || txn_gen != __wt_gen(session, WT_GEN_CHECKPOINT))
            break;
    }

    return (0);
}

/*
 * __session_alter --
 *     Alter a table setting.
 */
static int
__session_alter(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    /*
     * Alter table can return EBUSY error when the table is modified in parallel by eviction. Retry
     * the command after performing a system wide checkpoint. Only retry once to avoid potentially
     * waiting forever.
     */
    ret = __session_alter_internal(session, uri, config);
    if (ret == EBUSY) {
        WT_RET(__session_blocking_checkpoint(session));
        WT_STAT_CONN_INCR(session, session_table_alter_trigger_checkpoint);
        ret = __session_alter_internal(session, uri, config);
    }

    return (ret);
}

/*
 * __session_alter_readonly --
 *     WT_SESSION->alter method; readonly version.
 */
static int
__session_alter_readonly(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, alter);

    WT_STAT_CONN_INCR(session, session_table_alter_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __wt_session_create --
 *     Internal version of WT_SESSION::create.
 */
int
__wt_session_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
    WT_DECL_RET;

    WT_WITH_SCHEMA_LOCK(
      session, WT_WITH_TABLE_WRITE_LOCK(session, ret = __wt_schema_create(session, uri, config)));
    return (ret);
}

/*
 * __session_create --
 *     WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, create, config, cfg);
    WT_UNUSED(cfg);

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));

    /*
     * Type configuration only applies to tables, column groups and indexes. We don't want
     * applications to attempt to layer LSM on top of their extended data-sources, and the fact we
     * allow LSM as a valid URI is an invitation to that mistake: nip it in the bud.
     */
    if (!WT_PREFIX_MATCH(uri, "colgroup:") && !WT_PREFIX_MATCH(uri, "index:") &&
      !WT_PREFIX_MATCH(uri, "table:")) {
        /*
         * We can't disallow type entirely, a configuration string might innocently include it, for
         * example, a dump/load pair. If the underlying type is "file", it's OK ("file" is the
         * underlying type for every type); if the URI type prefix and the type are the same, let it
         * go.
         */
        if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
          !WT_STRING_MATCH("file", cval.str, cval.len) &&
          (strncmp(uri, cval.str, cval.len) != 0 || uri[cval.len] != ':'))
            WT_ERR_MSG(session, EINVAL, "%s: unsupported type configuration", uri);
        WT_ERR_NOTFOUND_OK(ret, false);
    }

    ret = __wt_session_create(session, uri, config);

err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_create_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_create_success);
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_create_readonly --
 *     WT_SESSION->create method; readonly version.
 */
static int
__session_create_readonly(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, create);

    WT_STAT_CONN_INCR(session, session_table_create_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_log_flush --
 *     WT_SESSION->log_flush method.
 */
static int
__session_log_flush(WT_SESSION *wt_session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint32_t flags;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, log_flush, config, cfg);
    WT_STAT_CONN_INCR(session, log_flush);

    conn = S2C(session);
    flags = 0;
    /*
     * If logging is not enabled there is nothing to do.
     */
    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        WT_ERR_MSG(session, EINVAL, "logging not enabled");

    WT_ERR(__wt_config_gets_def(session, cfg, "sync", 0, &cval));
    if (WT_STRING_MATCH("off", cval.str, cval.len))
        flags = WT_LOG_FLUSH;
    else if (WT_STRING_MATCH("on", cval.str, cval.len))
        flags = WT_LOG_FSYNC;
    ret = __wt_log_flush(session, flags);

err:
    API_END_RET(session, ret);
}

/*
 * __session_log_flush_readonly --
 *     WT_SESSION->log_flush method; readonly version.
 */
static int
__session_log_flush_readonly(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, log_flush);

    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_log_printf --
 *     WT_SESSION->log_printf method.
 */
static int
__session_log_printf(WT_SESSION *wt_session, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 2, 3)))
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED_NOCONF(session, log_printf);

    va_start(ap, fmt);
    ret = __wt_log_vprintf(session, fmt, ap);
    va_end(ap);

err:
    API_END_RET(session, ret);
}

/*
 * __session_log_printf_readonly --
 *     WT_SESSION->log_printf method; readonly version.
 */
static int
__session_log_printf_readonly(WT_SESSION *wt_session, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 2, 3)))
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(fmt);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, log_printf);

    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_rename --
 *     WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session, const char *uri, const char *newuri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, rename, config, cfg);

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));
    WT_ERR(__wt_str_name_check(session, newuri));

    WT_WITH_CHECKPOINT_LOCK(session,
      WT_WITH_SCHEMA_LOCK(session,
        WT_WITH_TABLE_WRITE_LOCK(session, ret = __wt_schema_rename(session, uri, newuri, cfg))));
err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_rename_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_rename_success);
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_rename_readonly --
 *     WT_SESSION->rename method; readonly version.
 */
static int
__session_rename_readonly(
  WT_SESSION *wt_session, const char *uri, const char *newuri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(newuri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, rename);

    WT_STAT_CONN_INCR(session, session_table_rename_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_reset --
 *     WT_SESSION->reset method.
 */
static int
__session_reset(WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(session, reset);

    WT_ERR(__wt_txn_context_check(session, false));

    WT_TRET(__wt_session_reset_cursors(session, true));

    if (--session->cursor_sweep_countdown == 0) {
        session->cursor_sweep_countdown = WT_SESSION_CURSOR_SWEEP_COUNTDOWN;
        WT_TRET(__wt_session_cursor_cache_sweep(session));
    }

    /* Release common session resources. */
    WT_TRET(__wt_session_release_resources(session));

    /* Reset the session statistics. */
    if (WT_STAT_ENABLED(session))
        __wt_stat_session_clear_single(&session->stats);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_drop --
 *     WT_SESSION->drop method.
 */
static int
__session_drop(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool checkpoint_wait, lock_wait;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, drop, config, cfg);

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));

    WT_ERR(__wt_config_gets_def(session, cfg, "checkpoint_wait", 1, &cval));
    checkpoint_wait = cval.val != 0;
    WT_ERR(__wt_config_gets_def(session, cfg, "lock_wait", 1, &cval));
    lock_wait = cval.val != 0;

    /*
     * Take the checkpoint lock if there is a need to prevent the drop operation from failing with
     * EBUSY due to an ongoing checkpoint.
     */
    if (checkpoint_wait) {
        if (lock_wait)
            WT_WITH_CHECKPOINT_LOCK(session,
              WT_WITH_SCHEMA_LOCK(session,
                WT_WITH_TABLE_WRITE_LOCK(session, ret = __wt_schema_drop(session, uri, cfg))));
        else
            WT_WITH_CHECKPOINT_LOCK_NOWAIT(session, ret,
              WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret,
                WT_WITH_TABLE_WRITE_LOCK_NOWAIT(
                  session, ret, ret = __wt_schema_drop(session, uri, cfg))));
    } else {
        if (lock_wait)
            WT_WITH_SCHEMA_LOCK(session,
              WT_WITH_TABLE_WRITE_LOCK(session, ret = __wt_schema_drop(session, uri, cfg)));
        else
            WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret,
              WT_WITH_TABLE_WRITE_LOCK_NOWAIT(
                session, ret, ret = __wt_schema_drop(session, uri, cfg)));
    }

err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_drop_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_drop_success);

    /* Note: drop operations cannot be unrolled (yet?). */
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_drop_readonly --
 *     WT_SESSION->drop method; readonly version.
 */
static int
__session_drop_readonly(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, drop);

    WT_STAT_CONN_INCR(session, session_table_drop_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_join --
 *     WT_SESSION->join method.
 */
static int
__session_join(
  WT_SESSION *wt_session, WT_CURSOR *join_cursor, WT_CURSOR *ref_cursor, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *firstcg;
    WT_CURSOR_INDEX *cindex;
    WT_CURSOR_JOIN *cjoin;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_SESSION_IMPL *session;
    WT_TABLE *table;
    uint64_t count;
    uint32_t bloom_bit_count, bloom_hash_count;
    uint8_t flags, range;
    bool nested;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, join, config, cfg);

    firstcg = NULL;
    table = NULL;
    nested = false;
    count = 0;

    if (!WT_PREFIX_MATCH(join_cursor->uri, "join:"))
        WT_ERR_MSG(session, EINVAL, "not a join cursor");

    if (WT_PREFIX_MATCH(ref_cursor->uri, "index:")) {
        cindex = (WT_CURSOR_INDEX *)ref_cursor;
        idx = cindex->index;
        table = cindex->table;
        firstcg = cindex->cg_cursors[0];
    } else if (WT_PREFIX_MATCH(ref_cursor->uri, "table:")) {
        idx = NULL;
        ctable = (WT_CURSOR_TABLE *)ref_cursor;
        table = ctable->table;
        firstcg = ctable->cg_cursors[0];
    } else if (WT_PREFIX_MATCH(ref_cursor->uri, "join:")) {
        idx = NULL;
        table = ((WT_CURSOR_JOIN *)ref_cursor)->table;
        nested = true;
    } else
        WT_ERR_MSG(session, EINVAL, "ref_cursor must be an index, table or join cursor");

    if (firstcg != NULL && !F_ISSET(firstcg, WT_CURSTD_KEY_SET))
        WT_ERR_MSG(session, EINVAL, "requires reference cursor be positioned");
    cjoin = (WT_CURSOR_JOIN *)join_cursor;
    if (cjoin->table != table)
        WT_ERR_MSG(session, EINVAL, "table for join cursor does not match table for ref_cursor");
    if (F_ISSET(ref_cursor, WT_CURSTD_JOINED))
        WT_ERR_MSG(session, EINVAL, "cursor already used in a join");

    /* "ge" is the default */
    range = WT_CURJOIN_END_GT | WT_CURJOIN_END_EQ;
    flags = 0;
    WT_ERR(__wt_config_gets(session, cfg, "compare", &cval));
    if (cval.len != 0) {
        if (WT_STRING_MATCH("gt", cval.str, cval.len))
            range = WT_CURJOIN_END_GT;
        else if (WT_STRING_MATCH("lt", cval.str, cval.len))
            range = WT_CURJOIN_END_LT;
        else if (WT_STRING_MATCH("le", cval.str, cval.len))
            range = WT_CURJOIN_END_LE;
        else if (WT_STRING_MATCH("eq", cval.str, cval.len))
            range = WT_CURJOIN_END_EQ;
        else if (!WT_STRING_MATCH("ge", cval.str, cval.len))
            WT_ERR_MSG(session, EINVAL, "compare=%.*s not supported", (int)cval.len, cval.str);
    }
    WT_ERR(__wt_config_gets(session, cfg, "count", &cval));
    if (cval.len != 0)
        count = (uint64_t)cval.val;

    WT_ERR(__wt_config_gets(session, cfg, "strategy", &cval));
    if (cval.len != 0) {
        if (WT_STRING_MATCH("bloom", cval.str, cval.len))
            LF_SET(WT_CURJOIN_ENTRY_BLOOM);
        else if (!WT_STRING_MATCH("default", cval.str, cval.len))
            WT_ERR_MSG(session, EINVAL, "strategy=%.*s not supported", (int)cval.len, cval.str);
    }
    WT_ERR(__wt_config_gets(session, cfg, "bloom_bit_count", &cval));
    if ((uint64_t)cval.val > UINT32_MAX)
        WT_ERR_MSG(session, EINVAL, "bloom_bit_count: value too large");
    bloom_bit_count = (uint32_t)cval.val;
    WT_ERR(__wt_config_gets(session, cfg, "bloom_hash_count", &cval));
    if ((uint64_t)cval.val > UINT32_MAX)
        WT_ERR_MSG(session, EINVAL, "bloom_hash_count: value too large");
    bloom_hash_count = (uint32_t)cval.val;
    if (LF_ISSET(WT_CURJOIN_ENTRY_BLOOM) && count == 0)
        WT_ERR_MSG(session, EINVAL, "count must be nonzero when strategy=bloom");
    WT_ERR(__wt_config_gets_def(session, cfg, "bloom_false_positives", 0, &cval));
    if (cval.val != 0)
        LF_SET(WT_CURJOIN_ENTRY_FALSE_POSITIVES);

    WT_ERR(__wt_config_gets(session, cfg, "operation", &cval));
    if (cval.len != 0 && WT_STRING_MATCH("or", cval.str, cval.len))
        LF_SET(WT_CURJOIN_ENTRY_DISJUNCTION);

    if (nested && (count != 0 || range != WT_CURJOIN_END_EQ || LF_ISSET(WT_CURJOIN_ENTRY_BLOOM)))
        WT_ERR_MSG(session, EINVAL,
          "joining a nested join cursor is incompatible with setting \"strategy\", \"compare\" or "
          "\"count\"");

    WT_ERR(__wt_curjoin_join(
      session, cjoin, idx, ref_cursor, flags, range, count, bloom_bit_count, bloom_hash_count));
    /*
     * There's an implied ownership ordering that isn't known when the cursors are created: the join
     * cursor must be closed before any of the indices. Enforce that here by reordering.
     */
    if (TAILQ_FIRST(&session->cursors) != join_cursor) {
        TAILQ_REMOVE(&session->cursors, join_cursor, q);
        TAILQ_INSERT_HEAD(&session->cursors, join_cursor, q);
    }
    /* Disable the reference cursor for regular operations */
    F_SET(ref_cursor, WT_CURSTD_JOINED);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_salvage_worker --
 *     Wrapper function for salvage processing.
 */
static int
__session_salvage_worker(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_RET(__wt_schema_worker(
      session, uri, __wt_salvage, NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_SALVAGE));
    WT_RET(__wt_schema_worker(session, uri, NULL, __wt_rollback_to_stable_one, cfg, 0));
    return (0);
}

/*
 * __session_salvage --
 *     WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    SESSION_API_CALL(session, salvage, config, cfg);

    WT_ERR(__wt_inmem_unsupported_op(session, NULL));

    /*
     * Run salvage and then rollback-to-stable (to bring the object into compliance with database
     * timestamps).
     *
     * Block out checkpoints to avoid spurious EBUSY errors.
     *
     * Hold the schema lock across both salvage and rollback-to-stable to avoid races where another
     * thread opens the handle before rollback-to-stable completes.
     */
    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __session_salvage_worker(session, uri, cfg)));

err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_salvage_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_salvage_success);
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_salvage_readonly --
 *     WT_SESSION->salvage method; readonly version.
 */
static int
__session_salvage_readonly(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, salvage);

    WT_STAT_CONN_INCR(session, session_table_salvage_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __wt_session_range_truncate --
 *     Session handling of a range truncate.
 */
int
__wt_session_range_truncate(
  WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *start, WT_CURSOR *stop)
{
    WT_DECL_RET;
    int cmp;
    bool local_start;

    local_start = false;
    if (uri != NULL) {
        WT_ASSERT(session, WT_BTREE_PREFIX(uri));
        /*
         * A URI file truncate becomes a range truncate where we set a start cursor at the
         * beginning. We already know the NULL stop goes to the end of the range.
         */
        WT_ERR(__session_open_cursor((WT_SESSION *)session, uri, NULL, NULL, &start));
        local_start = true;
        WT_ERR_NOTFOUND_OK(start->next(start), true);
        if (ret == WT_NOTFOUND) {
            /*
             * If there are no elements, there is nothing to do.
             */
            ret = 0;
            goto done;
        }
    }

    /*
     * Cursor truncate is only supported for some objects, check for a supporting compare method.
     */
    if (start != NULL && start->compare == NULL)
        WT_ERR(__wt_bad_object_type(session, start->uri));
    if (stop != NULL && stop->compare == NULL)
        WT_ERR(__wt_bad_object_type(session, stop->uri));

    /*
     * If both cursors set, check they're correctly ordered with respect to each other. We have to
     * test this before any search, the search can change the initial cursor position.
     *
     * Rather happily, the compare routine will also confirm the cursors reference the same object
     * and the keys are set.
     *
     * The test for a NULL start comparison function isn't necessary (we checked it above), but it
     * quiets clang static analysis complaints.
     */
    if (start != NULL && stop != NULL && start->compare != NULL) {
        WT_ERR(start->compare(start, stop, &cmp));
        if (cmp > 0)
            WT_ERR_MSG(
              session, EINVAL, "the start cursor position is after the stop cursor position");
    }

    /*
     * Truncate does not require keys actually exist so that applications can discard parts of the
     * object's name space without knowing exactly what records currently appear in the object. For
     * this reason, do a search-near, rather than a search. Additionally, we have to correct after
     * calling search-near, to position the start/stop cursors on the next record greater than/less
     * than the original key. If we fail to find a key in a search-near, there are no keys in the
     * table. If we fail to move forward or backward in a range, there are no keys in the range. In
     * either of those cases, we're done.
     */
    if (start != NULL)
        if ((ret = start->search_near(start, &cmp)) != 0 ||
          (cmp < 0 && (ret = start->next(start)) != 0)) {
            WT_ERR_NOTFOUND_OK(ret, false);
            goto done;
        }
    if (stop != NULL)
        if ((ret = stop->search_near(stop, &cmp)) != 0 ||
          (cmp > 0 && (ret = stop->prev(stop)) != 0)) {
            WT_ERR_NOTFOUND_OK(ret, false);
            goto done;
        }

    /*
     * We always truncate in the forward direction because the underlying data structures can move
     * through pages faster forward than backward. If we don't have a start cursor, create one and
     * position it at the first record.
     *
     * If start is NULL, stop must not be NULL, but static analyzers have a hard time with that,
     * test explicitly.
     */
    if (start == NULL && stop != NULL) {
        WT_ERR(__session_open_cursor((WT_SESSION *)session, stop->uri, NULL, NULL, &start));
        local_start = true;
        WT_ERR(start->next(start));
    }

    /*
     * If the start/stop keys cross, we're done, the range must be empty.
     */
    if (stop != NULL) {
        WT_ERR(start->compare(start, stop, &cmp));
        if (cmp > 0)
            goto done;
    }

    WT_ERR(__wt_schema_range_truncate(session, start, stop));

done:
err:
    /*
     * Close any locally-opened start cursor.
     *
     * Reset application cursors, they've possibly moved and the application cannot use them. Note
     * that we can make it here with a NULL start cursor (e.g., if the truncate range is empty).
     */
    if (local_start)
        WT_TRET(start->close(start));
    else if (start != NULL)
        WT_TRET(start->reset(start));
    if (stop != NULL)
        WT_TRET(stop->reset(stop));
    return (ret);
}

/*
 * __session_truncate --
 *     WT_SESSION->truncate method.
 */
static int
__session_truncate(
  WT_SESSION *wt_session, const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_TXN_API_CALL(session, truncate, config, cfg);
    WT_STAT_CONN_INCR(session, cursor_truncate);

    /*
     * If the URI is specified, we don't need a start/stop, if start/stop is specified, we don't
     * need a URI. One exception is the log URI which may truncate (archive) log files for a backup
     * cursor.
     *
     * If no URI is specified, and both cursors are specified, start/stop must reference the same
     * object.
     *
     * Any specified cursor must have been initialized.
     */
    if ((uri == NULL && start == NULL && stop == NULL) ||
      (uri != NULL && !WT_PREFIX_MATCH(uri, "log:") && (start != NULL || stop != NULL)))
        WT_ERR_MSG(session, EINVAL,
          "the truncate method should be passed either a URI or start/stop cursors, but not both");

    if (uri != NULL) {
        /* Disallow objects in the WiredTiger name space. */
        WT_ERR(__wt_str_name_check(session, uri));

        if (WT_PREFIX_MATCH(uri, "log:")) {
            /*
             * Verify the user only gave the URI prefix and not a specific target name after that.
             */
            if (strcmp(uri, "log:") != 0)
                WT_ERR_MSG(session, EINVAL,
                  "the truncate method should not specify any target after the log: URI prefix");
            WT_ERR(__wt_log_truncate_files(session, start, false));
        } else if (WT_BTREE_PREFIX(uri))
            WT_ERR(__wt_session_range_truncate(session, uri, start, stop));
        else
            /* Wait for checkpoints to avoid EBUSY errors. */
            WT_WITH_CHECKPOINT_LOCK(
              session, WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_truncate(session, uri, cfg)));
    } else
        WT_ERR(__wt_session_range_truncate(session, uri, start, stop));

err:
    TXN_API_END(session, ret, false);

    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_truncate_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_truncate_success);
    /*
     * Only map WT_NOTFOUND to ENOENT if a URI was specified.
     */
    return (ret == WT_NOTFOUND && uri != NULL ? ENOENT : ret);
}

/*
 * __session_truncate_readonly --
 *     WT_SESSION->truncate method; readonly version.
 */
static int
__session_truncate_readonly(
  WT_SESSION *wt_session, const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(start);
    WT_UNUSED(stop);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, truncate);

    WT_STAT_CONN_INCR(session, session_table_truncate_fail);
    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_upgrade --
 *     WT_SESSION->upgrade method.
 */
static int
__session_upgrade(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    SESSION_API_CALL(session, upgrade, config, cfg);

    WT_ERR(__wt_inmem_unsupported_op(session, NULL));

    /* Block out checkpoints to avoid spurious EBUSY errors. */
    WT_WITH_CHECKPOINT_LOCK(session,
      WT_WITH_SCHEMA_LOCK(session,
        ret = __wt_schema_worker(
          session, uri, __wt_upgrade, NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_UPGRADE)));

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_upgrade_readonly --
 *     WT_SESSION->upgrade method; readonly version.
 */
static int
__session_upgrade_readonly(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, upgrade);

    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_verify --
 *     WT_SESSION->verify method.
 */
static int
__session_verify(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, verify, config, cfg);
    WT_ERR(__wt_inmem_unsupported_op(session, NULL));

    /* Block out checkpoints to avoid spurious EBUSY errors. */
    WT_WITH_CHECKPOINT_LOCK(session,
      WT_WITH_SCHEMA_LOCK(session,
        ret = __wt_schema_worker(
          session, uri, __wt_verify, NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_VERIFY)));
    WT_ERR(ret);
err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_verify_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_verify_success);
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_begin_transaction --
 *     WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_NOT_ALLOWED(session, begin_transaction, config, cfg);
    WT_STAT_CONN_INCR(session, txn_begin);

    WT_ERR(__wt_txn_context_check(session, false));

    ret = __wt_txn_begin(session, cfg);

err:
    API_END_RET(session, ret);
}

/*
 * __session_commit_transaction --
 *     WT_SESSION->commit_transaction method.
 */
static int
__session_commit_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_TXN *txn;

    session = (WT_SESSION_IMPL *)wt_session;
    txn = session->txn;
    SESSION_API_CALL_PREPARE_ALLOWED(session, commit_transaction, config, cfg);
    WT_STAT_CONN_INCR(session, txn_commit);

    if (F_ISSET(txn, WT_TXN_PREPARE)) {
        WT_STAT_CONN_INCR(session, txn_prepare_commit);
        WT_STAT_CONN_DECR(session, txn_prepare_active);
    }

    WT_ERR(__wt_txn_context_check(session, true));

    /* Permit the commit if the transaction failed, but was read-only. */
    if (F_ISSET(txn, WT_TXN_ERROR) && txn->mod_count != 0) {
        __wt_err(session, EINVAL,
          "failed %s"
          "transaction requires rollback%s%s",
          F_ISSET(txn, WT_TXN_PREPARE) ? "prepared " : "", txn->rollback_reason == NULL ? "" : ": ",
          txn->rollback_reason == NULL ? "" : txn->rollback_reason);
        ret = EINVAL;
    }

err:
    /*
     * We might have failed because an illegal configuration was specified or because there wasn't a
     * transaction running, and we check the former as part of the api macros before we check the
     * latter. Deal with it here: if there's an error and a transaction is running, roll it back.
     */
    if (ret == 0) {
        F_SET(session, WT_SESSION_RESOLVING_TXN);
        ret = __wt_txn_commit(session, cfg);
        F_CLR(session, WT_SESSION_RESOLVING_TXN);
    } else if (F_ISSET(txn, WT_TXN_RUNNING)) {
        if (F_ISSET(txn, WT_TXN_PREPARE))
            WT_RET_PANIC(session, ret, "failed to commit prepared transaction, failing the system");

        WT_TRET(__wt_session_reset_cursors(session, false));
        F_SET(session, WT_SESSION_RESOLVING_TXN);
        WT_TRET(__wt_txn_rollback(session, cfg));
        F_CLR(session, WT_SESSION_RESOLVING_TXN);
    }

    API_END_RET(session, ret);
}

/*
 * __session_prepare_transaction --
 *     WT_SESSION->prepare_transaction method.
 */
static int
__session_prepare_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, prepare_transaction, config, cfg);
    WT_STAT_CONN_INCR(session, txn_prepare);
    WT_STAT_CONN_INCR(session, txn_prepare_active);

    WT_ERR(__wt_txn_context_check(session, true));

    F_SET(session, WT_SESSION_RESOLVING_TXN);
    WT_ERR(__wt_txn_prepare(session, cfg));
    F_CLR(session, WT_SESSION_RESOLVING_TXN);

err:
    API_END_RET(session, ret);
}

/*
 * __session_prepare_transaction_readonly --
 *     WT_SESSION->prepare_transaction method; readonly version.
 */
static int
__session_prepare_transaction_readonly(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, prepare_transaction);

    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_rollback_transaction --
 *     WT_SESSION->rollback_transaction method.
 */
static int
__session_rollback_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_TXN *txn;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED(session, rollback_transaction, config, cfg);
    WT_STAT_CONN_INCR(session, txn_rollback);

    txn = session->txn;
    if (F_ISSET(txn, WT_TXN_PREPARE)) {
        WT_STAT_CONN_INCR(session, txn_prepare_rollback);
        WT_STAT_CONN_DECR(session, txn_prepare_active);
    }

    WT_ERR(__wt_txn_context_check(session, true));

    WT_TRET(__wt_session_reset_cursors(session, false));

    F_SET(session, WT_SESSION_RESOLVING_TXN);
    WT_TRET(__wt_txn_rollback(session, cfg));
    F_CLR(session, WT_SESSION_RESOLVING_TXN);

err:
    API_END_RET(session, ret);
}

/*
 * __session_timestamp_transaction --
 *     WT_SESSION->timestamp_transaction method.
 */
static int
__session_timestamp_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
#ifdef HAVE_DIAGNOSTIC
    SESSION_API_CALL_PREPARE_ALLOWED(session, timestamp_transaction, config, cfg);
#else
    SESSION_API_CALL_PREPARE_ALLOWED(session, timestamp_transaction, NULL, cfg);
    cfg[1] = config;
#endif

    ret = __wt_txn_set_timestamp(session, cfg);
err:
    API_END_RET(session, ret);
}

/*
 * __session_query_timestamp --
 *     WT_SESSION->query_timestamp method.
 */
static int
__session_query_timestamp(WT_SESSION *wt_session, char *hex_timestamp, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED(session, query_timestamp, config, cfg);

    ret = __wt_txn_query_timestamp(session, hex_timestamp, cfg, false);
err:
    API_END_RET(session, ret);
}

/*
 * __session_reset_snapshot --
 *     WT_SESSION->reset_snapshot method.
 */
static int
__session_reset_snapshot(WT_SESSION *wt_session)
{
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    /* Return error if the isolation mode is read committed. */
    if (session->txn->isolation != WT_ISO_SNAPSHOT)
        WT_RET_MSG(
          session, ENOTSUP, "not supported in read-committed or read-uncommitted transactions.");

    /* Return error if the session has performed any write operations. */
    if (F_ISSET(session->txn, WT_TXN_HAS_ID))
        WT_RET_MSG(session, ENOTSUP, "not supported in write transactions.");

    __wt_txn_release_snapshot(session);
    __wt_txn_get_snapshot(session);

    return (0);
}

/*
 * __session_transaction_pinned_range --
 *     WT_SESSION->transaction_pinned_range method.
 */
static int
__session_transaction_pinned_range(WT_SESSION *wt_session, uint64_t *prange)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_TXN_SHARED *txn_shared;
    uint64_t pinned;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(session, transaction_pinned_range);

    txn_shared = WT_SESSION_TXN_SHARED(session);

    /* Assign pinned to the lesser of id or snap_min */
    if (txn_shared->id != WT_TXN_NONE && WT_TXNID_LT(txn_shared->id, txn_shared->pinned_id))
        pinned = txn_shared->id;
    else
        pinned = txn_shared->pinned_id;

    if (pinned == WT_TXN_NONE)
        *prange = 0;
    else
        *prange = S2C(session)->txn_global.current - pinned;

err:
    API_END_RET(session, ret);
}

/*
 * __session_get_rollback_reason --
 *     WT_SESSION->get_rollback_reason method.
 */
static const char *
__session_get_rollback_reason(WT_SESSION *wt_session)
{
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    return (session->txn->rollback_reason);
}

/*
 * __session_checkpoint --
 *     WT_SESSION->checkpoint method.
 */
static int
__session_checkpoint(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    WT_STAT_CONN_INCR(session, txn_checkpoint);
    SESSION_API_CALL_PREPARE_NOT_ALLOWED(session, checkpoint, config, cfg);

    WT_ERR(__wt_inmem_unsupported_op(session, NULL));

    /*
     * Checkpoints require a snapshot to write a transactionally consistent snapshot of the data.
     *
     * We can't use an application's transaction: if it has uncommitted changes, they will be
     * written in the checkpoint and may appear after a crash.
     *
     * Use a real snapshot transaction: we don't want any chance of the snapshot being updated
     * during the checkpoint. Eviction is prevented from evicting anything newer than this because
     * we track the oldest transaction ID in the system that is not visible to all readers.
     */
    WT_ERR(__wt_txn_context_check(session, false));

    ret = __wt_txn_checkpoint(session, cfg, true);

    /*
     * Release common session resources (for example, checkpoint may acquire significant
     * reconciliation structures/memory).
     */
    WT_TRET(__wt_session_release_resources(session));

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_checkpoint_readonly --
 *     WT_SESSION->checkpoint method; readonly version.
 */
static int
__session_checkpoint_readonly(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, checkpoint);

    ret = __wt_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __wt_session_strerror --
 *     WT_SESSION->strerror method.
 */
const char *
__wt_session_strerror(WT_SESSION *wt_session, int error)
{
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    return (__wt_strerror(session, error, NULL, 0));
}

/*
 * __session_flush_tier --
 *     Wrapper for the flush_tier method.
 */
static int
__session_flush_tier(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, flush_tier);
    ret = __wt_flush_tier(session, config);
err:
    API_END_RET(session, ret);
}

/*
 * __wt_session_breakpoint --
 *     A place to put a breakpoint, if you need one, or call some check code.
 */
int
__wt_session_breakpoint(WT_SESSION *wt_session)
{
    WT_UNUSED(wt_session);

    return (0);
}

/*
 * __open_session --
 *     Allocate a session handle.
 */
static int
__open_session(WT_CONNECTION_IMPL *conn, WT_EVENT_HANDLER *event_handler, const char *config,
  WT_SESSION_IMPL **sessionp)
{
    static const WT_SESSION
      stds = {NULL, NULL, __session_close, __session_reconfigure, __session_flush_tier,
        __wt_session_strerror, __session_open_cursor, __session_alter, __session_create,
        __wt_session_compact, __session_drop, __session_join, __session_log_flush,
        __session_log_printf, __session_rename, __session_reset, __session_salvage,
        __session_truncate, __session_upgrade, __session_verify, __session_begin_transaction,
        __session_commit_transaction, __session_prepare_transaction, __session_reset_snapshot,
        __session_rollback_transaction, __session_timestamp_transaction, __session_query_timestamp,
        __session_checkpoint, __session_transaction_pinned_range, __session_get_rollback_reason,
        __wt_session_breakpoint},
      stds_readonly = {NULL, NULL, __session_close, __session_reconfigure, __session_flush_tier,
        __wt_session_strerror, __session_open_cursor, __session_alter_readonly,
        __session_create_readonly, __wt_session_compact_readonly, __session_drop_readonly,
        __session_join, __session_log_flush_readonly, __session_log_printf_readonly,
        __session_rename_readonly, __session_reset, __session_salvage_readonly,
        __session_truncate_readonly, __session_upgrade_readonly, __session_verify,
        __session_begin_transaction, __session_commit_transaction,
        __session_prepare_transaction_readonly, __session_reset_snapshot,
        __session_rollback_transaction, __session_timestamp_transaction, __session_query_timestamp,
        __session_checkpoint_readonly, __session_transaction_pinned_range,
        __session_get_rollback_reason, __wt_session_breakpoint};
    WT_DECL_RET;
    WT_SESSION_IMPL *session, *session_ret;
    uint32_t i;

    *sessionp = NULL;

    session = conn->default_session;
    session_ret = NULL;

    __wt_spin_lock(session, &conn->api_lock);

    /*
     * Make sure we don't try to open a new session after the application closes the connection.
     * This is particularly intended to catch cases where server threads open sessions.
     */
    WT_ASSERT(session, !F_ISSET(conn, WT_CONN_CLOSING));

    /* Find the first inactive session slot. */
    for (session_ret = conn->sessions, i = 0; i < conn->session_size; ++session_ret, ++i)
        if (!session_ret->active)
            break;
    if (i == conn->session_size)
        WT_ERR_MSG(session, WT_ERROR,
          "out of sessions, configured for %" PRIu32 " (including internal sessions)",
          conn->session_size);

    /*
     * If the active session count is increasing, update it. We don't worry about correcting the
     * session count on error, as long as we don't mark this session as active, we'll clean it up on
     * close.
     */
    if (i >= conn->session_cnt) /* Defend against off-by-one errors. */
        conn->session_cnt = i + 1;

    session_ret->iface = F_ISSET(conn, WT_CONN_READONLY) ? stds_readonly : stds;
    session_ret->iface.connection = &conn->iface;

    session_ret->name = NULL;
    session_ret->id = i;

    if (WT_SESSION_FIRST_USE(session_ret))
        __wt_random_init(&session_ret->rnd);

    __wt_event_handler_set(
      session_ret, event_handler == NULL ? session->event_handler : event_handler);

    TAILQ_INIT(&session_ret->cursors);
    TAILQ_INIT(&session_ret->dhandles);

    /*
     * If we don't have them, allocate the cursor and dhandle hash arrays. Allocate the table hash
     * array as well.
     */
    if (session_ret->cursor_cache == NULL)
        WT_ERR(__wt_calloc_def(session, conn->hash_size, &session_ret->cursor_cache));
    if (session_ret->dhhash == NULL)
        WT_ERR(__wt_calloc_def(session, conn->dh_hash_size, &session_ret->dhhash));

    /* Initialize the dhandle hash array. */
    for (i = 0; i < (uint32_t)conn->dh_hash_size; i++)
        TAILQ_INIT(&session_ret->dhhash[i]);

    /* Initialize the cursor cache hash buckets and sweep trigger. */
    for (i = 0; i < (uint32_t)conn->hash_size; i++)
        TAILQ_INIT(&session_ret->cursor_cache[i]);
    session_ret->cursor_sweep_countdown = WT_SESSION_CURSOR_SWEEP_COUNTDOWN;

    /* Initialize transaction support: default to snapshot. */
    session_ret->isolation = WT_ISO_SNAPSHOT;
    WT_ERR(__wt_txn_init(session, session_ret));

    /*
     * The session's hazard pointer memory isn't discarded during normal session close because
     * access to it isn't serialized. Allocate the first time we open this session.
     */
    if (WT_SESSION_FIRST_USE(session_ret)) {
        WT_ERR(__wt_calloc_def(session, WT_SESSION_INITIAL_HAZARD_SLOTS, &session_ret->hazard));
        session_ret->hazard_size = WT_SESSION_INITIAL_HAZARD_SLOTS;
        session_ret->hazard_inuse = 0;
        session_ret->nhazard = 0;
    }

    /*
     * Cache the offset of this session's statistics bucket. It's important we pass the correct
     * session to the hash define here or we'll calculate the stat bucket with the wrong session id.
     */
    session_ret->stat_bucket = WT_STATS_SLOT_ID(session_ret);

    /* Safety check to make sure we're doing the right thing. */
    WT_ASSERT(session, session_ret->stat_bucket == session_ret->id % WT_COUNTER_SLOTS);

    /* Allocate the buffer for operation tracking */
    if (F_ISSET(conn, WT_CONN_OPTRACK)) {
        WT_ERR(__wt_malloc(session, WT_OPTRACK_BUFSIZE, &session_ret->optrack_buf));
        session_ret->optrackbuf_ptr = 0;
    }

    __wt_stat_session_init_single(&session_ret->stats);

    /* Set the default value for session flags. */
    if (F_ISSET(conn, WT_CONN_CACHE_CURSORS))
        F_SET(session_ret, WT_SESSION_CACHE_CURSORS);

    /*
     * Configuration: currently, the configuration for open_session is the same as
     * session.reconfigure, so use that function.
     */
    if (config != NULL)
        WT_ERR(__session_reconfigure((WT_SESSION *)session_ret, config));

    /*
     * Publish: make the entry visible to server threads. There must be a barrier for two reasons,
     * to ensure structure fields are set before any other thread will consider the session, and to
     * push the session count to ensure the eviction thread can't review too few slots.
     */
    WT_PUBLISH(session_ret->active, 1);

    WT_STATIC_ASSERT(offsetof(WT_SESSION_IMPL, iface) == 0);
    *sessionp = session_ret;

    WT_STAT_CONN_INCR(session, session_open);

err:
    __wt_spin_unlock(session, &conn->api_lock);
    return (ret);
}

/*
 * __wt_open_session --
 *     Allocate a session handle.
 */
int
__wt_open_session(WT_CONNECTION_IMPL *conn, WT_EVENT_HANDLER *event_handler, const char *config,
  bool open_metadata, WT_SESSION_IMPL **sessionp)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    *sessionp = NULL;

    /* Acquire a session. */
    WT_RET(__open_session(conn, event_handler, config, &session));

    /*
     * Acquiring the metadata handle requires the schema lock; we've seen problems in the past where
     * a session has acquired the schema lock unexpectedly, relatively late in the run, and
     * deadlocked. Be defensive, get it now. The metadata file may not exist when the connection
     * first creates its default session or the shared cache pool creates its sessions, let our
     * caller decline this work.
     */
    if (open_metadata) {
        WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));
        if ((ret = __wt_metadata_cursor(session, NULL)) != 0) {
            WT_TRET(__wt_session_close_internal(session));
            return (ret);
        }
    }

    *sessionp = session;
    return (0);
}

/*
 * __wt_open_internal_session --
 *     Allocate a session for WiredTiger's use.
 */
int
__wt_open_internal_session(WT_CONNECTION_IMPL *conn, const char *name, bool open_metadata,
  uint32_t session_flags, uint32_t session_lock_flags, WT_SESSION_IMPL **sessionp)
{
    WT_SESSION_IMPL *session;

    *sessionp = NULL;

    /* Acquire a session. */
    WT_RET(__wt_open_session(conn, NULL, NULL, open_metadata, &session));
    session->name = name;

    /*
     * Public sessions are automatically closed during WT_CONNECTION->close. If the session handles
     * for internal threads were to go on the public list, there would be complex ordering issues
     * during close. Set a flag to avoid this: internal sessions are not closed automatically.
     */
    F_SET(session, session_flags | WT_SESSION_INTERNAL);
    FLD_SET(session->lock_flags, session_lock_flags);

    *sessionp = session;
    return (0);
}
