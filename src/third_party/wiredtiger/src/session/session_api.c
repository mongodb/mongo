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
 * __wti_session_notsup --
 *     Unsupported session method.
 */
int
__wti_session_notsup(WT_SESSION_IMPL *session)
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
__wt_session_cursor_cache_sweep(WT_SESSION_IMPL *session, bool big_sweep)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor, *cursor_tmp;
    WT_CURSOR_LIST *cached_list;
    WT_DATA_HANDLE *saved_dhandle;
    WT_DECL_RET;
    uint64_t now, sweep_max, sweep_min;
    uint32_t i, nbuckets, nclosed, nexamined, position;
    int t_ret;
    bool productive;

    if (!F_ISSET(session, WT_SESSION_CACHE_CURSORS))
        return (0);

    conn = S2C(session);

    /*
     * Periodically sweep for dead cursors; if we've swept recently, don't do it again.
     *
     * Each call of this sweep function visits all the cursors in some number of buckets used by the
     * cursor cache. If any of the visited cursors reference dead or dying data handles those
     * cursors are fully closed and removed from the cache. Removing a cursor from the cursor cache
     * has the important effect of freeing a reference to the associated data handle. Data handles
     * can be closed and marked dead, but cannot be freed until all referencing sessions give up
     * their references. So sweeping the cursor cache (for all sessions!) is a prerequisite for the
     * connection data handle sweep to find handles that can be freed.
     *
     * We determine the number of buckets to visit based on how this function is called. When
     * big_sweep is true and enough time has passed, walk through at least a quarter of the buckets,
     * and as long as there is progress finding enough cursors to close, continue on, up to the
     * entire set of buckets. With the current settings, and assuming regular calls to the session
     * reset API, we'll visit every cached cursor about once every two minutes, or sooner.
     *
     * When big_sweep is false, we start with a small set of buckets to look at and quit when we
     * stop making progress or when we reach the maximum configured. This way, we amortize the work
     * of the sweep over many calls in a performance path.
     */
    __wt_seconds(session, &now);
    if (big_sweep && now - session->last_cursor_big_sweep >= 30) {
        session->last_cursor_big_sweep = session->last_cursor_sweep = now;
        sweep_min = conn->hash_size / 4;
        sweep_max = conn->hash_size;
    } else if (now - session->last_cursor_sweep >= 1) {
        session->last_cursor_sweep = now;
        sweep_min = WT_SESSION_CURSOR_SWEEP_MIN;
        sweep_max = WT_SESSION_CURSOR_SWEEP_MAX;
    } else
        return (0);

    position = session->cursor_sweep_position;
    productive = true;
    nbuckets = nclosed = nexamined = 0;
    saved_dhandle = session->dhandle;

    /* Turn off caching so that cursor close doesn't try to cache. */
    F_CLR(session, WT_SESSION_CACHE_CURSORS);
    for (i = 0; i < sweep_max && productive; i++) {
        ++nbuckets;
        cached_list = &session->cursor_cache[position];
        position = (position + 1) & (conn->hash_size - 1);
        TAILQ_FOREACH_SAFE(cursor, cached_list, q, cursor_tmp)
        {
            ++nexamined;

            /*
             * When a cursor is cached, we retain various memory buffers. Thus, frequently used
             * cursors will get reopened from cached state with those memory buffers intact, and
             * thereby benefit from memory reuse. However, we don't want to retain this memory for
             * too long, especially for infrequently used cached cursors. So regardless of whether
             * we sweep this cursor, release any memory held by it now.
             */
            __wt_cursor_free_cached_memory(cursor);

            /*
             * Check to see if the cursor could be reopened and should be swept.
             */
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
        productive = (nclosed + sweep_min > i);
    }

    session->cursor_sweep_position = position;
    F_SET(session, WT_SESSION_CACHE_CURSORS);

    WT_STAT_CONN_INCR(session, cursor_sweep);
    WT_STAT_CONN_INCRV(session, cursor_sweep_buckets, nbuckets);
    WT_STAT_CONN_INCRV(session, cursor_sweep_examined, nexamined);
    WT_STAT_CONN_INCRV(session, cursor_sweep_closed, nclosed);

    WT_ASSERT_ALWAYS(
      session, session->dhandle == saved_dhandle, "Session dhandle changed during cursor sweep");
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
              __wt_atomic_loadv64(&txn_shared->pinned_id) != WT_TXN_NONE ||
                (WT_BTREE_PREFIX(cursor->uri) &&
                  WT_DHANDLE_IS_CHECKPOINT(((WT_CURSOR_BTREE *)cursor)->dhandle)));
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

    __wt_atomic_store32(&session->hazards.inuse, 0);
    session->hazards.num_active = 0;
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
          !WT_IS_URI_HS(cursor->internal_uri))
            /*
             * Notify the user that we are closing the cursor handle via the registered close
             * callback.
             */
            WT_TRET(session->event_handler->handle_close(
              session->event_handler, &session->iface, cursor));

        if (WT_PREFIX_MATCH(cursor->internal_uri, "layered:"))
            F_SET(cursor, WT_CURSTD_CONSTITUENT_DEAD);
        WT_TRET(cursor->close(cursor));
    }
    WT_TAILQ_SAFE_REMOVE_END
#ifdef HAVE_DIAGNOSTIC
    WT_CONN_CLOSE_ABORT(session, ret);
#endif
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
#ifdef HAVE_DIAGNOSTIC
    WT_CONN_CLOSE_ABORT(session, ret);
#endif
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
#ifdef HAVE_CALL_LOG
    bool internal_session;
#endif

    conn = S2C(session);

#ifdef HAVE_CALL_LOG
    internal_session = F_ISSET(session, WT_SESSION_INTERNAL);
    if (!internal_session)
        /*
         * The call log entry for the session_close API is generated by two functions. The reason is
         * that the first function (called below) requires session's variables and the second
         * function (called at the end) requires the ret value.
         */
        WT_TRET(__wt_call_log_close_session(session));
#endif

    /* Free the memory allocated to the error message buffer. */
    __wt_free(session, session->err_info.err_msg_buf.mem);

    /* Make sure no new error messages are saved during the close call. */
    memset(&(session->err_info), 0, sizeof(WT_ERROR_INFO));
    F_CLR(session, WT_SESSION_SAVE_ERRORS);

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
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_OPTRACK)) {
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

    __wt_spin_unlock_if_owned(session, &session->scratch_lock);
    __wt_spin_destroy(session, &session->scratch_lock);

#ifdef HAVE_DIAGNOSTIC
    /*
     * Unlock the thread_check mutex if we own it, this a bit of a cheeky workaround as there's one
     * scenario where we enter this path and the mutex itself isn't locked anyway.
     *
     * Essentially if the caller enters through __session_close then they lock this mutex in the API
     * enter macro. This code then destroys it prior to the associated unlock call in the API exit
     * code. By placing the unlock here we avoid this destroy happening early.
     *
     * The connection close path goes through here too but doesn't go via __session_close, so we
     * check if the caller owns the mutex before deciding to unlock it.
     */
    __wt_spin_unlock_if_owned(session, &session->thread_check.lock);
    __wt_spin_destroy(session, &session->thread_check.lock);
#endif

    /*
     * Sessions are re-used, clear the structure: the clear sets the active field to 0, which will
     * exclude the hazard array from review by the eviction thread. Because some session fields are
     * accessed by other threads, the structure must be cleared carefully.
     *
     * We don't need to release write here, because regardless of the active field being non-zero,
     * the hazard pointer is always valid.
     */
    __session_clear(session);
    session = conn->default_session;

    /*
     * Decrement the count of active sessions if that's possible: a session being closed may or may
     * not be at the end of the array, step toward the beginning of the array until we reach an
     * active session.
     */
    while (WT_CONN_SESSIONS_GET(conn)[__wt_atomic_load32(&conn->session_array.cnt) - 1].active == 0)
        if (__wt_atomic_sub32(&conn->session_array.cnt, 1) == 0)
            break;

    __wt_spin_unlock(session, &conn->api_lock);

#ifdef HAVE_CALL_LOG
    if (!internal_session)
        WT_TRET(__wt_call_log_print_return(conn, session, ret, ""));
#endif
#ifdef HAVE_DIAGNOSTIC
    WT_CONN_CLOSE_ABORT(&conn->dummy_session, ret);
#endif
    return (ret);
}

/*
 * __session_config_prefetch --
 *     Configure pre-fetch flags on the session.
 */
static int
__session_config_prefetch(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;

    if (S2C(session)->prefetch_auto_on)
        F_SET(session, WT_SESSION_PREFETCH_ENABLED);
    else
        F_CLR(session, WT_SESSION_PREFETCH_ENABLED);

    /*
     * Override any connection-level pre-fetch settings if a specific session-level setting was
     * provided.
     */
    if (__wt_config_gets(session, cfg + 1, "prefetch.enabled", &cval) == 0) {
        if (cval.val) {
            if (!S2C(session)->prefetch_available) {
                F_CLR(session, WT_SESSION_PREFETCH_ENABLED);
                WT_RET_MSG(session, EINVAL,
                  "pre-fetching cannot be enabled for the session if pre-fetching is configured as "
                  "unavailable");
            } else
                F_SET(session, WT_SESSION_PREFETCH_ENABLED);
        } else
            F_CLR(session, WT_SESSION_PREFETCH_ENABLED);
    }

    return (0);
}

/*
 * __session_config_int --
 *     Configure basic flags and values on the session. Tested via a unit test.
 */
static int
__session_config_int(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;

    if ((ret = __wt_config_getones(session, config, "ignore_cache_size", &cval)) == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_IGNORE_CACHE_SIZE);
        else
            F_CLR(session, WT_SESSION_IGNORE_CACHE_SIZE);
    }
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_getones(session, config, "cache_cursors", &cval)) == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_CACHE_CURSORS);
        else {
            F_CLR(session, WT_SESSION_CACHE_CURSORS);
            WT_RET(__session_close_cached_cursors(session));
        }
    }
    WT_RET_NOTFOUND_OK(ret);

    /*
     * FIXME-WT-12021 Replace this debug option with the corresponding failpoint once this project
     * is completed.
     */
    if ((ret = __wt_config_getones(
           session, config, "debug.checkpoint_fail_before_turtle_update", &cval)) == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_DEBUG_CHECKPOINT_FAIL_BEFORE_TURTLE_UPDATE);
        else
            F_CLR(session, WT_SESSION_DEBUG_CHECKPOINT_FAIL_BEFORE_TURTLE_UPDATE);
    }
    WT_RET_NOTFOUND_OK(ret);

    /*
     * There is a session debug configuration which can be set to evict pages as they are released
     * and no longer needed.
     */
    if ((ret = __wt_config_getones(session, config, "debug.release_evict_page", &cval)) == 0) {
        if (cval.val)
            F_SET(session, WT_SESSION_DEBUG_RELEASE_EVICT);
        else
            F_CLR(session, WT_SESSION_DEBUG_RELEASE_EVICT);
    }
    WT_RET_NOTFOUND_OK(ret);

    if ((ret = __wt_config_getones(session, config, "cache_max_wait_ms", &cval)) == 0) {
        if (cval.val > 1)
            session->cache_max_wait_us = (uint64_t)(cval.val * WT_THOUSAND);
        else if (cval.val == 1)
            session->cache_max_wait_us = 1;
        else
            session->cache_max_wait_us = 0;
    }
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __session_reconfigure --
 *     WT_SESSION->reconfigure method.
 */
static int
__session_reconfigure(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED(session, reconfigure, config, cfg);

    WT_ERR(__wt_session_reset_cursors(session, false));

    /*
     * Note that this method only checks keys that are passed in by the application: we don't want
     * to reset other session settings to their default values.
     */
    ret = __wt_txn_reconfigure(session, config);
    if (ret == EINVAL) {
        /*
         * EINVAL is returned iff there is an active transaction and txn is being reconfigured. In
         * this case, don't want to fail the transaction - same as in SESSION_API_PREPARE_CHECK.
         */
        __set_err = false;
        goto err;
    }

    WT_ERR(__session_config_int(session, config));

    WT_ERR(__session_config_prefetch(session, cfg));
err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_reconfigure_notsup --
 *     WT_SESSION->reconfigure method; not supported version.
 */
static int
__session_reconfigure_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, reconfigure);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
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
    WT_CONFIG_ITEM cval;
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
    case 'l':
        if (WT_PREFIX_MATCH(uri, "log:"))
            WT_RET(__wt_curlog_open(session, uri, cfg, cursorp));
        else if (WT_PREFIX_MATCH(uri, "layered:"))
            WT_RET(__wt_clayered_open(session, uri, owner, cfg, cursorp));
        break;

    /*
     * Less common cursor types.
     */
    case 'f':
        if (WT_PREFIX_MATCH(uri, "file:")) {
            /*
             * Open a version cursor instead of a table cursor if we are using the special debug
             * configuration.
             */
            if ((ret = __wt_config_gets_def(
                   session, cfg, "debug.dump_version.enabled", 0, &cval)) == 0 &&
              cval.val) {
                if (WT_IS_URI_HS(uri))
                    WT_RET_MSG(session, EINVAL, "cannot open version cursor on the history store");
                WT_RET(__wt_curversion_open(session, uri, owner, cfg, cursorp));
            } else
                WT_RET(__wt_curfile_open(session, uri, owner, cfg, cursorp));
        }
        break;
    case 'm':
        if (WT_PREFIX_MATCH(uri, WT_METADATA_URI))
            WT_RET(__wt_curmetadata_open(session, uri, owner, cfg, cursorp));
        break;
    case 'b':
        if (WT_PREFIX_MATCH(uri, "backup:")) {
            if (__wt_live_restore_migration_in_progress(session))
                WT_RET_SUB(session, EINVAL, WT_CONFLICT_LIVE_RESTORE,
                  "backup cannot be taken when live restore is enabled");
            WT_RET(__wt_curbackup_open(session, uri, other, cfg, cursorp));
        }
        break;
    case 'p':
        if (WT_PREFIX_MATCH(uri, "prepared_discover:"))
            WT_RET(__wt_cursor_prepared_discover_open(session, uri, other, cfg, cursorp));
        break;
    case 's':
        if (WT_PREFIX_MATCH(uri, "statistics:")) {
            WT_ASSERT(session, other == NULL);
            WT_RET(__wt_curstat_open(session, uri, cfg, cursorp));
        }
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

    /* Support caching simple cursors that have no children or if the owner is a layered cursor. */
    if (owner != NULL && !WT_PREFIX_MATCH(owner->internal_uri, "layered:")) {
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
    WT_TXN_GLOBAL *txn_global;
    uint64_t hash_value;

    hash_value = 0;
    /* Don't require a NULL input cursor */
    *cursorp = NULL;

#ifdef HAVE_DIAGNOSTIC
    struct timespec end_time, start_time;
    bool cursor_timing = false;
    if (session->cursor_open_timer_running == false) {
        session->cursor_open_timer_running = true;
        cursor_timing = true;
        __wt_epoch(session, &start_time);
    }
#endif
    WT_NOT_READ(txn_global, &S2C(session)->txn_global);

    /*
     * We should not open other cursors when there are open history store cursors in the session.
     * There are some exceptions to this rule:
     *  - Verifying the metadata through an internal session.
     *  - The btree is being verified.
     *  - Opening the meta files while performing a checkpoint.
     */
    WT_ASSERT(session,
      WT_IS_URI_HS(uri) ||
        (WT_IS_URI_METADATA(uri) && __wt_atomic_loadvbool(&txn_global->checkpoint_running)) ||
        session->hs_cursor_counter == 0 || F_ISSET(session, WT_SESSION_INTERNAL) ||
        (S2BT_SAFE(session) != NULL && F_ISSET(S2BT(session), WT_BTREE_VERIFY)));

    if (owner != NULL && !WT_PREFIX_MATCH(owner->internal_uri, "layered:")) {
        WT_ERR(__session_open_cursor_int(session, uri, owner, NULL, cfg, hash_value, cursorp));
    } else {
        /* Try to find the cursor in the cache. */
        __wt_cursor_get_hash(session, uri, NULL, &hash_value);
        WT_ERR_NOTFOUND_OK(
          __wt_cursor_cache_get(session, uri, hash_value, NULL, cfg, cursorp), false);

        /* Open a new cursor if no cached cursor was found. */
        if (*cursorp == NULL)
            WT_ERR(__session_open_cursor_int(session, uri, owner, NULL, cfg, hash_value, cursorp));
    }

err:
    /* Always close out the timing information regardless of success. */
#ifdef HAVE_DIAGNOSTIC
    if (cursor_timing) {
        session->cursor_open_timer_running = false;
        __wt_epoch(session, &end_time);
        uint64_t time_diff_usec = WT_TIMEDIFF_US(end_time, start_time);
        /*
         * We may not have a valid dhandle when EBUSY is returned. In that case only increment the
         * connection statistics
         */
        if (ret != EBUSY)
            WT_STAT_CONN_DSRC_INCRV(session, cursor_open_time_internal_usecs, time_diff_usec);
        else
            WT_STAT_CONN_INCRV(session, cursor_open_time_internal_usecs, time_diff_usec);
    }
#endif
    return (ret);
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
    bool dup_backup;
#ifdef HAVE_DIAGNOSTIC
    struct timespec end_time, start_time;
    bool cursor_timing;

    cursor_timing = false;
#endif
    cursor = *cursorp = NULL;

    hash_value = 0;
    dup_backup = false;
    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL(session, ret, open_cursor, config, cfg);

#ifdef HAVE_DIAGNOSTIC
    if (session->cursor_open_timer_running == false) {
        session->cursor_open_timer_running = true;
        cursor_timing = true;
        __wt_epoch(session, &start_time);
    }
#endif

    /*
     * Check for early usage of a user session to collect statistics. If the connection is not fully
     * ready but can be used, then only allow a cursor uri of "statistics:" only. The conditional is
     * complicated. Allow the cursor to open if any of these conditions are met:
     * - The connection is ready
     * - The session is an internal session
     * - The connection is minimally ready and the URI is "statistics:"
     */
    if (!F_ISSET_ATOMIC_32(S2C(session), WT_CONN_READY) && !F_ISSET(session, WT_SESSION_INTERNAL) &&
      (!F_ISSET_ATOMIC_32(S2C(session), WT_CONN_MINIMAL) || strcmp(uri, "statistics:") != 0))
        WT_ERR_MSG(
          session, EINVAL, "cannot open a non-statistics cursor before connection is opened");

    if ((to_dup == NULL && uri == NULL) || (to_dup != NULL && uri != NULL))
        WT_ERR_MSG(
          session, EINVAL, "should be passed either a URI or a cursor to duplicate, but not both");

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
          !WT_PREFIX_MATCH(uri, WT_METADATA_URI) && !WT_PREFIX_MATCH(uri, "table:") &&
          !WT_PREFIX_MATCH(uri, "tiered:") && __wt_schema_get_source(session, uri) == NULL)
            WT_ERR(__wt_bad_object_type(session, uri));
    }

    WT_ERR(__session_open_cursor_int(
      session, uri, NULL, dup_backup ? to_dup : NULL, cfg, hash_value, &cursor));

done:
    if (to_dup != NULL && !dup_backup)
        WT_ERR(__wt_cursor_dup_position(to_dup, cursor));

    *cursorp = cursor;

    if (0) {
err:
        if (cursor != NULL)
            WT_TRET(cursor->close(cursor));
    }
    /* Always close out the timing information regardless of success. */
#ifdef HAVE_DIAGNOSTIC
    if (cursor_timing) {
        session->cursor_open_timer_running = false;
        __wt_epoch(session, &end_time);
        uint64_t time_diff_usec = WT_TIMEDIFF_US(end_time, start_time);
        /*
         * It's considered a user open if it comes via a top-level API call. This could
         * alternatively decide the statistic based on whether it's a user or internal session.
         *
         * We may not have a valid dhandle when EBUSY is returned. In that case only increment the
         * connection statistics
         */
        if (ret != EBUSY)
            if (API_USER_ENTRY(session))
                WT_STAT_CONN_DSRC_INCRV(session, cursor_open_time_user_usecs, time_diff_usec);
            else
                WT_STAT_CONN_DSRC_INCRV(session, cursor_open_time_internal_usecs, time_diff_usec);
        else if (API_USER_ENTRY(session))
            WT_STAT_CONN_INCRV(session, cursor_open_time_user_usecs, time_diff_usec);
        else
            WT_STAT_CONN_INCRV(session, cursor_open_time_internal_usecs, time_diff_usec);
    }
#endif
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
__session_alter_internal(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DECL_RET;

    /* In-memory ignores alter operations. */
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        goto err;

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));

    WT_WITH_CHECKPOINT_LOCK(
      session, WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_alter(session, uri, cfg)));

err:
    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_alter_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_alter_success);
    return (ret);
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

    if ((ret = __wt_checkpoint_db(session, checkpoint_cfg, false)) == 0)
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
        if (!__wt_atomic_loadvbool(&txn_global->checkpoint_running) ||
          txn_gen != __wt_gen(session, WT_GEN_CHECKPOINT))
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
    SESSION_API_CALL(session, ret, alter, config, cfg);
    /*
     * We replace the default configuration listing with the current configuration. Otherwise the
     * defaults for values that can be altered would override settings used by the user in create.
     */
    cfg[0] = cfg[1];
    cfg[1] = NULL;

    /*
     * Alter table can return EBUSY error when the table is modified in parallel by eviction. Retry
     * the command after performing a system wide checkpoint. Only retry once to avoid potentially
     * waiting forever.
     */
    WT_ERR_ERROR_OK(__session_alter_internal(session, uri, cfg), EBUSY, true);
    if (ret == EBUSY) {
        WT_ERR(__session_blocking_checkpoint(session));
        WT_STAT_CONN_INCR(session, session_table_alter_trigger_checkpoint);
        ret = __session_alter_internal(session, uri, cfg);
    }

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
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
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_bind_configuration --
 *     Bind values to a compiled configuration string.
 */
static int
__session_bind_configuration(WT_SESSION *wt_session, const char *compiled, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    WT_UNUSED(compiled);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, alter);

    va_start(ap, compiled);
    ret = __wt_conf_bind(session, compiled, ap);
    va_end(ap);
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
    bool is_import;

    session = (WT_SESSION_IMPL *)wt_session;
    is_import = session->import_list != NULL ||
      (__wt_config_getones(session, config, "import.enabled", &cval) == 0 && cval.val != 0);
    SESSION_API_CALL(session, ret, create, config, cfg);
    WT_UNUSED(cfg);

    /* Disallow objects in the WiredTiger name space. */
    WT_ERR(__wt_str_name_check(session, uri));

    /* Type configuration only applies to tables, column groups and indexes. */
    if (!WT_PREFIX_MATCH(uri, "colgroup:") && !WT_PREFIX_MATCH(uri, "index:") &&
      !WT_PREFIX_MATCH(uri, "table:")) {
        /*
         * We can't disallow type entirely, a configuration string might innocently include it, for
         * example, a dump/load pair. If the underlying type is "file", it's OK ("file" is the
         * underlying type for every type); if the URI type prefix and the type are the same, let it
         * go.
         */
        if ((ret = __wt_config_getones(session, config, "type", &cval)) == 0 &&
          !WT_CONFIG_LIT_MATCH("file", cval) &&
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

    if (is_import) {
        if (ret != 0)
            WT_STAT_CONN_INCR(session, session_table_create_import_fail);
        else
            WT_STAT_CONN_INCR(session, session_table_create_import_success);
    }
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
    ret = __wti_session_notsup(session);
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
    SESSION_API_CALL(session, ret, log_flush, config, cfg);
    WT_STAT_CONN_INCR(session, log_flush);

    conn = S2C(session);
    flags = 0;
    /*
     * If logging is not enabled there is nothing to do.
     */
    if (!F_ISSET(&conn->log_mgr, WT_LOG_ENABLED))
        WT_ERR_MSG(session, EINVAL, "logging not enabled");

    WT_ERR(__wt_config_gets_def(session, cfg, "sync", 0, &cval));
    if (WT_CONFIG_LIT_MATCH("off", cval))
        flags = WT_LOG_FLUSH;
    else if (WT_CONFIG_LIT_MATCH("on", cval))
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

    ret = __wti_session_notsup(session);
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

    ret = __wti_session_notsup(session);
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
    SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(session, ret, reset);

    WT_ERR(__wt_txn_context_check(session, false));

    WT_TRET(__wt_session_reset_cursors(session, true));

    /*
     * Run the session sweeps. Run the cursor cache sweep with "big" option to sweep more, as we're
     * not in a performance path.
     */
    session->cursor_sweep_countdown = WT_SESSION_CURSOR_SWEEP_COUNTDOWN;
    WT_TRET(__wt_session_cursor_cache_sweep(session, true));
    __wt_session_dhandle_sweep(session);

    /* Release common session resources. */
    WT_TRET(__wt_session_release_resources(session));

    /* Reset the session statistics. */
    if (WT_STAT_ENABLED(session))
        __wt_stat_session_clear_single(&session->stats);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __session_reset_notsup --
 *     WT_SESSION->reset method; not supported version.
 */
static int
__session_reset_notsup(WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, reset);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
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
    SESSION_API_CALL(session, ret, drop, config, cfg);

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
                WT_WITH_TABLE_WRITE_LOCK(
                  session, ret = __wt_schema_drop(session, uri, cfg, true))));
        else
            WT_WITH_CHECKPOINT_LOCK_NOWAIT(session, ret,
              WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret,
                WT_WITH_TABLE_WRITE_LOCK_NOWAIT(
                  session, ret, ret = __wt_schema_drop(session, uri, cfg, true))));
    } else {
        if (lock_wait)
            WT_WITH_SCHEMA_LOCK(session,
              WT_WITH_TABLE_WRITE_LOCK(session, ret = __wt_schema_drop(session, uri, cfg, true)));
        else
            WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret,
              WT_WITH_TABLE_WRITE_LOCK_NOWAIT(
                session, ret, ret = __wt_schema_drop(session, uri, cfg, true)));
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
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_salvage_worker --
 *     Wrapper function for salvage processing.
 */
static int
__session_salvage_worker(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    WT_RET(__wt_schema_worker(
      session, uri, __wt_salvage, NULL, cfg, WT_DHANDLE_EXCLUSIVE | WT_BTREE_SALVAGE));
    WT_RET(
      __wt_schema_worker(session, uri, NULL, S2C(session)->rts->rollback_to_stable_one, cfg, 0));
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

    SESSION_API_CALL(session, ret, salvage, config, cfg);

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
    ret = __wti_session_notsup(session);
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
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(orig_start_key);
    WT_DECL_ITEM(orig_stop_key);
    WT_DECL_RET;
    WT_ITEM start_key, stop_key;
    WT_TRUNCATE_INFO *trunc_info, _trunc_info;
    int cmp;
    const char *actual_uri;
    bool local_start, local_stop, log_op, log_trunc, needs_next_prev;

    actual_uri = NULL;
    local_start = local_stop = log_trunc = false;
    orig_start_key = orig_stop_key = NULL;

    /* Setup the truncate information structure */
    trunc_info = &_trunc_info;
    memset(trunc_info, 0, sizeof(*trunc_info));
    if (uri == NULL && start != NULL)
        F_SET(trunc_info, WT_TRUNC_EXPLICIT_START);
    if (uri == NULL && stop != NULL)
        F_SET(trunc_info, WT_TRUNC_EXPLICIT_STOP);

    /* Find the actual URI, as the "uri" argument could be NULL. */
    if (uri != NULL) {
        WT_ASSERT(session, WT_BTREE_PREFIX(uri));
        actual_uri = uri;
    } else if (start != NULL)
        actual_uri = start->internal_uri;
    else if (stop != NULL)
        actual_uri = stop->internal_uri;

    /*
     * Cursor truncate is only supported for some objects, check for a supporting compare method.
     */
    if (start != NULL && start->compare == NULL)
        WT_ERR(__wt_bad_object_type(session, start->uri));
    if (stop != NULL && stop->compare == NULL)
        WT_ERR(__wt_bad_object_type(session, stop->uri));

    /*
     * Use temporary buffers to store the original start and stop keys. We track the original keys
     * for writing the truncate operation in the write-ahead log.
     */
    if (start != NULL) {
        WT_ERR(__wt_cursor_get_raw_key(start, &start_key));
        WT_ERR(__wt_scr_alloc(session, 0, &orig_start_key));
        WT_ERR(__wt_buf_set(session, orig_start_key, start_key.data, start_key.size));
    }
    if (stop != NULL) {
        WT_ERR(__wt_cursor_get_raw_key(stop, &stop_key));
        WT_ERR(__wt_scr_alloc(session, 0, &orig_stop_key));
        WT_ERR(__wt_buf_set(session, orig_stop_key, stop_key.data, stop_key.size));
    }

    /*
     * If both cursors are set, check they're correctly ordered with respect to each other. We have
     * to test this before any search, the search can change the initial cursor position.
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
     * If the start cursor does not exist, create a new one.
     */
    if (start == NULL) {
        WT_ERR(__session_open_cursor((WT_SESSION *)session, actual_uri, NULL, NULL, &start));
        local_start = true;
    }

    /*
     * Now that the truncate is setup and ready regardless of how the API was called, populate our
     * truncate information cookie.
     */
    trunc_info->session = session;
    trunc_info->start = start;
    trunc_info->stop = stop;
    trunc_info->orig_start_key = orig_start_key;
    trunc_info->orig_stop_key = orig_stop_key;
    trunc_info->uri = actual_uri;

    /*
     * Truncate does not require keys actually exist so that applications can discard parts of the
     * object's name space without knowing exactly what records currently appear in the object.
     * Search-near is suboptimal, because it may return prepare conflicts outside of the truncate
     * key range, as it will walk beyond the end key.
     *
     * No need to search the record again if it is already pointing to the btree.
     */
    if (!F_ISSET(start, WT_CURSTD_KEY_INT)) {
        needs_next_prev = true;
        if (orig_start_key != NULL) {
            WT_ERR_NOTFOUND_OK(start->search_near(start, &cmp), true);
            if (ret == WT_NOTFOUND) {
                ret = 0;
                log_trunc = true;
                goto done;
            }
            needs_next_prev = (cmp < 0);
        }
        if (needs_next_prev) {
            WT_ERR_NOTFOUND_OK(start->next(start), true);
            if (ret == WT_NOTFOUND) {
                /* If there are no elements, there is nothing to do. */
                ret = 0;
                log_trunc = true;
                goto done;
            }
        }
    }
    if (stop != NULL && !F_ISSET(stop, WT_CURSTD_KEY_INT)) {
        WT_ERR_NOTFOUND_OK(stop->search_near(stop, &cmp), true);
        if (ret == WT_NOTFOUND) {
            ret = 0;
            log_trunc = true;
            goto done;
        }
        needs_next_prev = (cmp > 0);
        if (needs_next_prev) {
            WT_ERR_NOTFOUND_OK(stop->prev(stop), true);
            if (ret == WT_NOTFOUND) {
                ret = 0;
                log_trunc = true;
                goto done;
            }
        }
    }

    /*
     * If the start/stop keys cross, we're done, the range must be empty.
     */
    if (stop != NULL) {
        WT_ERR(start->compare(start, stop, &cmp));
        if (cmp > 0) {
            log_trunc = true;
            goto done;
        }
    }

    WT_ERR(__wt_schema_range_truncate(trunc_info));

done:
    /*
     * In the cases where truncate doesn't have work to do, we still need to generate a log record
     * for the operation. That way we can be consistent with other competing inserts or truncates on
     * other tables in this transaction.
     */
    if (log_trunc) {
        /*
         * If we have cursors and know there is no work to do, there may not be a dhandle in the
         * session. Grab it from the start or stop cursor as needed.
         */
        dhandle = session->dhandle;
        if (dhandle == NULL) {
            if (start != NULL)
                dhandle = ((WT_CURSOR_BTREE *)start)->dhandle;
            else if (stop != NULL)
                dhandle = ((WT_CURSOR_BTREE *)stop)->dhandle;
        }
        /* We have to have a dhandle from somewhere. */
        WT_ASSERT(session, dhandle != NULL);
        if (WT_DHANDLE_BTREE(dhandle)) {
            WT_WITH_DHANDLE(session, dhandle, log_op = __wt_txn_log_op_check(session));
            if (log_op) {
                WT_WITH_DHANDLE(session, dhandle, ret = __wt_txn_truncate_log(trunc_info));
                WT_ERR(ret);
                __wt_txn_truncate_end(session);
            }
        }
    }
err:
    /* Clear temporary buffer that were storing the original start and stop keys. */
    if (orig_start_key != NULL)
        __wt_scr_free(session, &orig_start_key);
    if (orig_stop_key != NULL)
        __wt_scr_free(session, &orig_stop_key);

    /*
     * Close any locally-opened start and stop cursors.
     *
     * Reset application cursors, they've possibly moved and the application cannot use them. Note
     * that we can make it here with a NULL start cursor (e.g., if the truncate range is empty).
     */
    if (local_start)
        WT_TRET(start->close(start));
    else if (start != NULL)
        WT_TRET(start->reset(start));
    if (local_stop)
        WT_TRET(stop->close(stop));
    else if (stop != NULL)
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
    SESSION_TXN_API_CALL(session, ret, truncate, config, cfg);
    WT_STAT_CONN_INCR(session, cursor_truncate);

    if ((start != NULL && start->session != wt_session) ||
      (stop != NULL && stop->session != wt_session))
        WT_ERR_MSG(session, EINVAL, "bounding cursors must be owned by the truncating session");

    /*
     * If the URI is specified, we don't need a start/stop, if start/stop is specified, we don't
     * need a URI. One exception is the log URI which may remove log files for a backup cursor.
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
    /* Map prepare-conflict to rollback. */
    if (ret == WT_PREPARE_CONFLICT)
        ret = WT_ROLLBACK;

    TXN_API_END(session, ret, false);

    if (ret != 0)
        WT_STAT_CONN_INCR(session, session_table_truncate_fail);
    else
        WT_STAT_CONN_INCR(session, session_table_truncate_success);

    /* Map WT_NOTFOUND to ENOENT if a URI was specified. */
    if (ret == WT_NOTFOUND && uri != NULL)
        ret = ENOENT;
    return (ret);
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
    ret = __wti_session_notsup(session);
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
    SESSION_API_CALL(session, ret, verify, config, cfg);
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
 * __session_verify_notsup --
 *     WT_SESSION->verify method; not supported version.
 */
static int
__session_verify_notsup(WT_SESSION *wt_session, const char *uri, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(uri);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, verify);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_begin_transaction --
 *     WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_CONF(WT_SESSION, begin_transaction, conf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(session, ret, begin_transaction);
    SESSION_API_CONF(session, begin_transaction, config, conf);
    WT_STAT_CONN_INCR(session, txn_begin);

    WT_ERR(__wt_txn_context_check(session, false));

    ret = __wt_txn_begin(session, conf);

err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_begin_transaction(session, config, ret));
#endif
    API_CONF_END(session, conf);
    API_END_RET(session, ret);
}

/*
 * __session_begin_transaction_notsup --
 *     WT_SESSION->begin_transaction method; not supported version.
 */
static int
__session_begin_transaction_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, begin_transaction);
    ret = __wti_session_notsup(session);
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
    if (F_ISSET(txn, WT_TXN_ERROR) && txn->mod_count != 0)
        WT_ERR_MSG(session, EINVAL, "failed %s transaction requires rollback",
          F_ISSET(txn, WT_TXN_PREPARE) ? "prepared " : "");

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
        WT_TRET(__wt_txn_rollback(session, cfg, false));
        F_CLR(session, WT_SESSION_RESOLVING_TXN);
    }
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_commit_transaction(session, config, ret));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_commit_transaction_notsup --
 *     WT_SESSION->commit_transaction method; not supported version.
 */
static int
__session_commit_transaction_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, commit_transaction);
    ret = __wti_session_notsup(session);
err:
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
    SESSION_API_CALL(session, ret, prepare_transaction, config, cfg);
    WT_STAT_CONN_INCR(session, txn_prepare);
    WT_STAT_CONN_INCR(session, txn_prepare_active);

    WT_ERR(__wt_txn_context_check(session, true));

    F_SET(session, WT_SESSION_RESOLVING_TXN);
    WT_ERR(__wt_txn_prepare(session, cfg));
    F_CLR(session, WT_SESSION_RESOLVING_TXN);

err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_prepare_transaction(session, config, ret));
#endif
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

    ret = __wti_session_notsup(session);
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
    WT_TRET(__wt_txn_rollback(session, cfg, true));
    F_CLR(session, WT_SESSION_RESOLVING_TXN);

err:
    /*
     * Check for a prepared transaction, and quit: we can't ignore the error and we can't roll back
     * a prepared transaction.
     */
    if (ret != 0 && session->txn && F_ISSET(session->txn, WT_TXN_PREPARE))
        WT_IGNORE_RET(__wt_panic(session, ret,
          "transactional error logged after transaction was prepared, failing the system"));

#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_rollback_transaction(session, config, ret));
#endif
#ifdef HAVE_DIAGNOSTIC
    WT_CONN_CLOSE_ABORT(session, ret);
#endif
    API_END_RET(session, ret);
}

/*
 * __session_rollback_transaction_notsup --
 *     WT_SESSION->rollback_transaction method; not supported version.
 */
static int
__session_rollback_transaction_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, rollback_transaction);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_timestamp_transaction --
 *     WT_SESSION->timestamp_transaction method. Also see __session_timestamp_transaction_uint if
 *     config parsing is a performance issue.
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

    ret = __wt_txn_set_timestamp(session, cfg, false);
err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_timestamp_transaction(session, config, ret));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_timestamp_transaction_notsup --
 *     WT_SESSION->timestamp_transaction method; not supported version.
 */
static int
__session_timestamp_transaction_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, timestamp_transaction);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_timestamp_transaction_uint --
 *     WT_SESSION->timestamp_transaction_uint method.
 */
static int
__session_timestamp_transaction_uint(WT_SESSION *wt_session, WT_TS_TXN_TYPE which, uint64_t ts)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED_NOCONF(session, timestamp_transaction_uint);

    ret = __wt_txn_set_timestamp_uint(session, which, ts);
err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_timestamp_transaction_uint(session, which, ts, ret));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_timestamp_transaction_uint_notsup --
 *     WT_SESSION->timestamp_transaction_uint method; not supported version.
 */
static int
__session_timestamp_transaction_uint_notsup(
  WT_SESSION *wt_session, WT_TS_TXN_TYPE which, uint64_t ts)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(which);
    WT_UNUSED(ts);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, timestamp_transaction_uint);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_prepared_id_transaction --
 *     WT_SESSION->prepared_id_transaction method. Also see __session_prepared_id_transaction_uint
 *     if config parsing is a performance issue.
 */
static int
__session_prepared_id_transaction(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
#ifdef HAVE_DIAGNOSTIC
    SESSION_API_CALL_PREPARE_ALLOWED(session, prepared_id_transaction, config, cfg);
#else
    SESSION_API_CALL_PREPARE_ALLOWED(session, prepared_id_transaction, NULL, cfg);
    cfg[1] = config;
#endif

    ret = __wt_txn_set_prepared_id(session, cfg);
err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_prepared_id_transaction(session, config, ret));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_prepared_id_transaction_notsup --
 *     WT_SESSION->prepared_id_transaction method; not supported version.
 */
static int
__session_prepared_id_transaction_notsup(WT_SESSION *wt_session, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, prepared_id_transaction);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_prepared_id_transaction_uint --
 *     WT_SESSION->prepared_id_transaction_uint method.
 */
static int
__session_prepared_id_transaction_uint(WT_SESSION *wt_session, uint64_t prepared_id)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_PREPARE_ALLOWED_NOCONF(session, prepared_id_uint);

    ret = __wt_txn_set_prepared_id_uint(session, prepared_id);
err:
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_prepared_id_transaction_uint(session, prepared_id, ret));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_prepared_id_transaction_uint_notsup --
 *     WT_SESSION->prepared_id_transaction_uint method; not supported version.
 */
static int
__session_prepared_id_transaction_uint_notsup(WT_SESSION *wt_session, uint64_t prepared_id)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(prepared_id);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, prepared_id_uint);
    ret = __wti_session_notsup(session);
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
#ifdef HAVE_CALL_LOG
    WT_TRET(__wt_call_log_query_timestamp(session, config, hex_timestamp, ret, false));
#endif
    API_END_RET(session, ret);
}

/*
 * __session_query_timestamp_notsup --
 *     WT_SESSION->query_timestamp method; not supported version.
 */
static int
__session_query_timestamp_notsup(WT_SESSION *wt_session, char *hex_timestamp, const char *config)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(hex_timestamp);
    WT_UNUSED(config);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, query_timestamp);
    ret = __wti_session_notsup(session);
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
    WT_TXN *txn;

    session = (WT_SESSION_IMPL *)wt_session;
    txn = session->txn;

    /* Return error if the isolation mode is not snapshot. */
    if (txn->isolation != WT_ISO_SNAPSHOT)
        WT_RET_MSG(
          session, ENOTSUP, "not supported in read-committed or read-uncommitted transactions");

    /* Return error if the session has performed any write operations. */
    if (txn->mod_count != 0)
        WT_RET_MSG(session, ENOTSUP, "only supported before a transaction makes modifications");

    __wt_txn_release_snapshot(session);
    __wt_txn_get_snapshot(session);

    return (0);
}

/*
 * __session_reset_snapshot_notsup --
 *     WT_SESSION->reset_snapshot method; not supported version.
 */
static int
__session_reset_snapshot_notsup(WT_SESSION *wt_session)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, reset_snapshot);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
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
    SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(session, ret, transaction_pinned_range);

    txn_shared = WT_SESSION_TXN_SHARED(session);

    /* Assign pinned to the lesser of id or snap_min */
    if (__wt_atomic_loadv64(&txn_shared->id) != WT_TXN_NONE &&
      __wt_atomic_loadv64(&txn_shared->id) < __wt_atomic_loadv64(&txn_shared->pinned_id))
        pinned = __wt_atomic_loadv64(&txn_shared->id);
    else
        pinned = __wt_atomic_loadv64(&txn_shared->pinned_id);

    if (pinned == WT_TXN_NONE)
        *prange = 0;
    else
        *prange = __wt_atomic_loadv64(&S2C(session)->txn_global.current) - pinned;

err:
    API_END_RET(session, ret);
}

/*
 * __session_transaction_pinned_range_notsup --
 *     WT_SESSION->transaction_pinned_range method; not supported version.
 */
static int
__session_transaction_pinned_range_notsup(WT_SESSION *wt_session, uint64_t *prange)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(prange);

    session = (WT_SESSION_IMPL *)wt_session;
    SESSION_API_CALL_NOCONF(session, transaction_pinned_range);
    ret = __wti_session_notsup(session);
err:
    API_END_RET(session, ret);
}

/*
 * __session_get_last_error --
 *     WT_SESSION->get_last_error method.
 */
static void
__session_get_last_error(WT_SESSION *wt_session, int *err, int *sub_level_err, const char **err_msg)
{
    WT_SESSION_IMPL *session = (WT_SESSION_IMPL *)wt_session;

    *err = session->err_info.err;
    *sub_level_err = session->err_info.sub_level_err;
    *err_msg = session->err_info.err_msg;
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
    WT_STAT_CONN_INCR(session, checkpoints_api);
    SESSION_API_CALL_PREPARE_NOT_ALLOWED(session, ret, checkpoint, config, cfg);

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

    ret = __wt_checkpoint_db(session, cfg, true);

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

    ret = __wti_session_notsup(session);
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
      stds = {NULL, NULL, __session_close, __session_reconfigure, __wt_session_strerror,
        __session_open_cursor, __session_alter, __session_bind_configuration, __session_create,
        __wti_session_compact, __session_drop, __session_log_flush, __session_log_printf,
        __session_reset, __session_salvage, __session_truncate, __session_verify,
        __session_begin_transaction, __session_commit_transaction, __session_prepare_transaction,
        __session_rollback_transaction, __session_query_timestamp, __session_timestamp_transaction,
        __session_timestamp_transaction_uint, __session_prepared_id_transaction,
        __session_prepared_id_transaction_uint, __session_checkpoint, __session_reset_snapshot,
        __session_transaction_pinned_range, __session_get_last_error, __wt_session_breakpoint},
      stds_min = {NULL, NULL, __session_close, __session_reconfigure_notsup, __wt_session_strerror,
        __session_open_cursor, __session_alter_readonly, __session_bind_configuration,
        __session_create_readonly, __wti_session_compact_readonly, __session_drop_readonly,
        __session_log_flush_readonly, __session_log_printf_readonly, __session_reset_notsup,
        __session_salvage_readonly, __session_truncate_readonly, __session_verify_notsup,
        __session_begin_transaction_notsup, __session_commit_transaction_notsup,
        __session_prepare_transaction_readonly, __session_rollback_transaction_notsup,
        __session_query_timestamp_notsup, __session_timestamp_transaction_notsup,
        __session_timestamp_transaction_uint_notsup, __session_prepared_id_transaction_notsup,
        __session_prepared_id_transaction_uint_notsup, __session_checkpoint_readonly,
        __session_reset_snapshot_notsup, __session_transaction_pinned_range_notsup,
        __session_get_last_error, __wt_session_breakpoint},
      stds_readonly = {NULL, NULL, __session_close, __session_reconfigure, __wt_session_strerror,
        __session_open_cursor, __session_alter_readonly, __session_bind_configuration,
        __session_create_readonly, __wti_session_compact_readonly, __session_drop_readonly,
        __session_log_flush_readonly, __session_log_printf_readonly, __session_reset,
        __session_salvage_readonly, __session_truncate_readonly, __session_verify,
        __session_begin_transaction, __session_commit_transaction,
        __session_prepare_transaction_readonly, __session_rollback_transaction,
        __session_query_timestamp, __session_timestamp_transaction,
        __session_timestamp_transaction_uint, __session_prepared_id_transaction,
        __session_prepared_id_transaction_uint, __session_checkpoint_readonly,
        __session_reset_snapshot, __session_transaction_pinned_range, __session_get_last_error,
        __wt_session_breakpoint};
    WT_DECL_RET;
    WT_SESSION_IMPL *session, *session_ret;
    uint32_t i;

    WT_VERIFY_OPAQUE_POINTER(WT_SESSION_IMPL);

    *sessionp = NULL;

    session = conn->default_session;
    session_ret = NULL;

    __wt_spin_lock(session, &conn->api_lock);

    /*
     * Make sure we don't try to open a new session after the application closes the connection.
     * This is particularly intended to catch cases where server threads open sessions.
     */
    WT_ASSERT(session, !F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING));

    /* Find the first inactive session slot. */
    for (session_ret = WT_CONN_SESSIONS_GET(conn), i = 0; i < conn->session_array.size;
         ++session_ret, ++i)
        if (!session_ret->active)
            break;
    if (i == conn->session_array.size)
        WT_ERR_MSG(session, WT_ERROR,
          "out of sessions, configured for %" PRIu32 " (including internal sessions)",
          conn->session_array.size);

    /*
     * If the active session count is increasing, update it. We don't worry about correcting the
     * session count on error, as long as we don't mark this session as active, we'll clean it up on
     * close.
     */
    if (i >= __wt_atomic_load32(&conn->session_array.cnt)) /* Defend against off-by-one errors. */
        __wt_atomic_store32(&conn->session_array.cnt, i + 1);

    /* Find the set of methods appropriate to this session. */
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_MINIMAL) && !F_ISSET(session, WT_SESSION_INTERNAL))
        session_ret->iface = stds_min;
    else
        session_ret->iface = F_ISSET(conn, WT_CONN_READONLY) ? stds_readonly : stds;
    session_ret->iface.connection = &conn->iface;

    session_ret->name = NULL;
    session_ret->id = i;

#ifdef HAVE_UNITTEST_ASSERTS
    session_ret->unittest_assert_hit = false;
    memset(session->unittest_assert_msg, 0, WT_SESSION_UNITTEST_BUF_LEN);
#endif

#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__wt_spin_init(session, &session_ret->thread_check.lock, "thread check lock"));
#endif

    WT_ERR(__wt_spin_init(session, &session_ret->scratch_lock, "scratch buffer lock"));

    /*
     * Initialize the pseudo random number generator. We're not seeding it, so all of the sessions
     * initialize to the same value and proceed in lock step for the session's life. That's not a
     * problem because sessions are long-lived and will diverge into different parts of the value
     * space, and what we care about are small values, that is, the low-order bits.
     */
    __wt_session_rng_init_once(session_ret);

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
        WT_ERR(
          __wt_calloc_def(session, WT_SESSION_INITIAL_HAZARD_SLOTS, &session_ret->hazards.arr));
        session_ret->hazards.size = WT_SESSION_INITIAL_HAZARD_SLOTS;
        __wt_atomic_store32(&session_ret->hazards.inuse, 0);
        session_ret->hazards.num_active = 0;
    }

    /*
     * Cache the offset of this session's statistics bucket. It's important we pass the correct
     * session to the hash define here or we'll calculate the stat bucket with the wrong session id.
     */
    session_ret->stat_conn_bucket = WT_STATS_CONN_SLOT_ID(session_ret);
    session_ret->stat_dsrc_bucket = WT_STATS_DSRC_SLOT_ID(session_ret);

    /* Safety check to make sure we're doing the right thing. */
    WT_ASSERT(
      session, session_ret->stat_conn_bucket == session_ret->id % WT_STAT_CONN_COUNTER_SLOTS);
    WT_ASSERT(
      session, session_ret->stat_dsrc_bucket == session_ret->id % WT_STAT_DSRC_COUNTER_SLOTS);

    /* Allocate the buffer for operation tracking */
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_OPTRACK)) {
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

    /* Initialize the default error info, including a buffer for the error message. */
    F_SET(session_ret, WT_SESSION_SAVE_ERRORS);
    session_ret->err_info.err_msg = NULL;
    WT_ERR(__wt_buf_initsize(session, &(session_ret->err_info.err_msg_buf), 128));
    __wt_session_reset_last_error(session_ret);

    /*
     * Release write to ensure structure fields are set before any other thread will consider the
     * session.
     */
    WT_RELEASE_WRITE_WITH_BARRIER(session_ret->active, 1);

    *sessionp = session_ret;

    WT_STAT_CONN_INCR(session, session_open);

err:
#ifdef HAVE_DIAGNOSTIC
    __wt_spin_destroy(session, &session->thread_check.lock);
#endif
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
     * Internal sessions should not save error info unless they are spawned by an external session,
     * in which case they will inherit the WT_SESSION_SAVE_ERRORS flag from session_flags.
     */
    F_CLR(session, WT_SESSION_SAVE_ERRORS);

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

#ifdef HAVE_UNITTEST
int
__ut_session_config_int(WT_SESSION_IMPL *session, const char *config)
{
    return (__session_config_int(session, config));
}
#endif
