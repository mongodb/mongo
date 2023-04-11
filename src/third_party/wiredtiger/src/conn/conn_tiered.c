/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 1)
/*
 * !!!
 * GCC with -Wformat-nonliteral complains about calls to strftime in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#endif

/*
 * __tiered_server_run_chk --
 *     Check to decide if the tiered storage server should continue running.
 */
static bool
__tiered_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_TIERED));
}

/*
 * __tier_storage_remove_local --
 *     Perform one iteration of tiered storage local object removal.
 */
static int
__tier_storage_remove_local(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;
    uint64_t now;
    const char *object;

    entry = NULL;
    for (;;) {
        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        __wt_seconds(session, &now);
        __wt_tiered_get_remove_local(session, now, &entry);
        if (entry == NULL)
            break;
        WT_ERR(__wt_tiered_name(
          session, &entry->tiered->iface, entry->id, WT_TIERED_NAME_OBJECT, &object));
        __wt_verbose_debug2(session, WT_VERB_TIERED, "REMOVE_LOCAL: %s at %" PRIu64, object, now);
        WT_PREFIX_SKIP_REQUIRED(session, object, "object:");
        /*
         * If the handle is still open, it could still be in use for reading. In that case put the
         * work unit back on the work queue and keep trying.
         */
        if (__wt_handle_is_open(session, object)) {
            __wt_verbose_debug2(
              session, WT_VERB_TIERED, "REMOVE_LOCAL: %s in USE, queue again", object);
            WT_STAT_CONN_INCR(session, local_objects_inuse);
            /*
             * Update the time on the entry before pushing it back on the queue so that we don't get
             * into an infinite loop trying to drop an open file that may be in use a while.
             */
            WT_ASSERT(session, entry->tiered != NULL && entry->tiered->bstorage != NULL);
            entry->op_val = now + entry->tiered->bstorage->retain_secs;
            __wt_tiered_requeue_work(session, entry);
        } else {
            __wt_verbose_debug2(
              session, WT_VERB_TIERED, "REMOVE_LOCAL: actually remove %s", object);
            WT_STAT_CONN_INCR(session, local_objects_removed);
            WT_ERR(__wt_fs_remove(session, object, false));
            /*
             * We are responsible for freeing the work unit when we're done with it.
             */
            __wt_tiered_work_free(session, entry);
        }
        entry = NULL;
    }
err:
    if (entry != NULL)
        __wt_tiered_work_free(session, entry);
    return (ret);
}

/*
 * __tier_flush_meta --
 *     Perform one iteration of altering the metadata after a flush. This is in its own function so
 *     that we can hold the schema lock while doing the metadata tracking.
 */
static int
__tier_flush_meta(
  WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *local_uri, const char *obj_uri)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    uint64_t now;
    char hex_timestamp[WT_TS_HEX_STRING_SIZE];
    char *newconfig, *obj_value;
    const char *cfg[3] = {NULL, NULL, NULL};
    bool release, tracking;

    conn = S2C(session);
    release = tracking = false;
    WT_RET(__wt_scr_alloc(session, 512, &buf));
    dhandle = &tiered->iface;

    newconfig = obj_value = NULL;
    WT_ERR(__wt_meta_track_on(session));
    tracking = true;

    WT_ERR(__wt_session_get_dhandle(session, dhandle->name, NULL, NULL, 0));
    release = true;
    /*
     * Once the flush call succeeds we want to first remove the file: entry from the metadata and
     * then update the object: metadata to indicate the flush is complete. Record the flush
     * timestamp from the flush call. We know that no new flush_tier call can begin until all work
     * from the last call completes, so the connection field is correct.
     */
    __wt_timestamp_to_hex_string(conn->flush_ts, hex_timestamp);
    WT_ERR(__wt_metadata_remove(session, local_uri));
    WT_ERR(__wt_metadata_search(session, obj_uri, &obj_value));
    __wt_seconds(session, &now);
    WT_ERR(__wt_buf_fmt(
      session, buf, "flush_time=%" PRIu64 ",flush_timestamp=\"%s\"", now, hex_timestamp));
    cfg[0] = obj_value;
    cfg[1] = buf->mem;
    WT_ERR(__wt_config_collapse(session, cfg, &newconfig));
    WT_ERR(__wt_metadata_update(session, obj_uri, newconfig));
    WT_ERR(__wt_meta_track_off(session, true, ret != 0));
    tracking = false;

err:
    __wt_free(session, newconfig);
    __wt_free(session, obj_value);
    if (release)
        WT_TRET(__wt_session_release_dhandle(session));
    __wt_scr_free(session, &buf);
    if (tracking)
        WT_TRET(__wt_meta_track_off(session, true, ret != 0));
    if (ret == ENOENT)
        ret = 0;
    return (ret);
}

/*
 * __tier_do_operation --
 *     Perform one iteration of copying newly flushed objects to shared storage or post-flush
 *     processing.
 */
static int
__tier_do_operation(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id, const char *local_uri,
  const char *obj_uri, uint32_t op)
{
    WT_CONFIG_ITEM pfx;
    WT_DECL_RET;
    WT_FILE_SYSTEM *bucket_fs;
    WT_STORAGE_SOURCE *storage_source;
    size_t len;
    char *tmp;
    const char *cfg[2], *local_name, *obj_name;

    WT_ASSERT(session, (op == WT_TIERED_WORK_FLUSH || op == WT_TIERED_WORK_FLUSH_FINISH));
    tmp = NULL;
    if (tiered->bstorage == NULL) {
        __wt_verbose(session, WT_VERB_TIERED, "DO_OP: tiered %p NULL bstorage.", (void *)tiered);
        WT_ASSERT(session, tiered->bstorage != NULL);
    }
    storage_source = tiered->bstorage->storage_source;
    bucket_fs = tiered->bstorage->file_system;

    local_name = local_uri;
    WT_PREFIX_SKIP_REQUIRED(session, local_name, "file:");
    obj_name = obj_uri;
    WT_PREFIX_SKIP_REQUIRED(session, obj_name, "object:");
    cfg[0] = tiered->obj_config;
    cfg[1] = NULL;
    WT_RET(__wt_config_gets(session, cfg, "tiered_storage.bucket_prefix", &pfx));
    WT_ASSERT(session, pfx.len != 0);
    len = strlen(obj_name) + pfx.len + 1;
    WT_RET(__wt_calloc_def(session, len, &tmp));
    WT_ERR(__wt_snprintf(tmp, len, "%.*s%s", (int)pfx.len, pfx.str, obj_name));

    if (op == WT_TIERED_WORK_FLUSH_FINISH)
        WT_ERR(storage_source->ss_flush_finish(
          storage_source, &session->iface, bucket_fs, local_name, tmp, NULL));
    else {
        /* WT_TIERED_WORK_FLUSH */
        /* This call make take a while, and may fail due to network timeout. */
        ret = storage_source->ss_flush(
          storage_source, &session->iface, bucket_fs, local_name, tmp, NULL);
        if (ret == 0)
            WT_WITH_CHECKPOINT_LOCK(session,
              WT_WITH_SCHEMA_LOCK(
                session, ret = __tier_flush_meta(session, tiered, local_uri, obj_uri)));
        /*
         * If a user did a flush_tier with sync off, it is possible that a drop happened before the
         * flush work unit was processed. Ignore non-existent errors from either previous call.
         */
        if (ret == ENOENT)
            ret = 0;
        else {
            WT_ERR(ret);

            /*
             * After successful flushing, push a work unit to perform whatever post-processing the
             * shared storage wants to do for this object. Note that this work unit is unrelated to
             * the remove local work unit below. They do not need to be in any order and do not
             * interfere with each other.
             */
            WT_ERR(__wt_tiered_put_flush_finish(session, tiered, id));
            /*
             * After successful flushing, push a work unit to remove the local object in the future.
             * The object will be removed locally after the local retention period expires.
             */
            WT_ERR(__wt_tiered_put_remove_local(session, tiered, id));
        }
    }

err:
    __wt_free(session, tmp);
    return (ret);
}

/*
 * __tier_operation --
 *     Given an ID generate the URI names and call the operation code to flush or finish.
 */
static int
__tier_operation(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id, uint32_t op)
{
    WT_DECL_RET;
    const char *local_uri, *obj_uri;

    local_uri = obj_uri = NULL;
    WT_ERR(__wt_tiered_name(session, &tiered->iface, id, WT_TIERED_NAME_LOCAL, &local_uri));
    WT_ERR(__wt_tiered_name(session, &tiered->iface, id, WT_TIERED_NAME_OBJECT, &obj_uri));
    WT_ERR(__tier_do_operation(session, tiered, id, local_uri, obj_uri, op));

err:
    __wt_free(session, local_uri);
    __wt_free(session, obj_uri);
    return (ret);
}

/*
 * __tier_storage_finish --
 *     Perform one iteration of shared storage post-flush work. This is separated from copying the
 *     objects to shared storage to allow the flush_tier call to return after only the necessary
 *     work has completed.
 */
static int
__tier_storage_finish(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;

    entry = NULL;
    /*
     * Sleep a known period of time so that tests using the timing stress flag can have an idea when
     * to check for the cache operation to complete. Sleep one second before processing the work
     * queue of cache work units.
     */
    if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_TIERED_FLUSH_FINISH))
        __wt_sleep(1, 0);
    for (;;) {
        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        __wt_tiered_get_flush_finish(session, &entry);
        if (entry == NULL)
            break;
        WT_ERR(__tier_operation(session, entry->tiered, entry->id, WT_TIERED_WORK_FLUSH_FINISH));
        /*
         * We are responsible for freeing the work unit when we're done with it.
         */
        __wt_tiered_work_free(session, entry);
        entry = NULL;
    }

err:
    if (entry != NULL)
        __wt_tiered_work_free(session, entry);
    return (ret);
}

/*
 * __tier_storage_copy --
 *     Perform one iteration of copying newly flushed objects to the shared storage.
 */
static int
__tier_storage_copy(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;

    /* There is nothing to do until the checkpoint after the flush completes. */
    if (!S2C(session)->flush_ckpt_complete)
        return (0);
    entry = NULL;
    for (;;) {
        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        /*
         * We probably need some kind of flush generation so that we don't process flush items for
         * tables that are added during an in-progress flush_tier. This thread could run due to a
         * condition timeout rather than a signal. Checking that generation number would be part of
         * calling __wt_tiered_get_flush so that we don't pull it off the queue until we're sure we
         * want to process it.
         */
        __wt_tiered_get_flush(session, &entry);
        if (entry == NULL)
            break;
        WT_ERR(__tier_operation(session, entry->tiered, entry->id, WT_TIERED_WORK_FLUSH));
        /*
         * We are responsible for freeing the work unit when we're done with it.
         */
        __wt_tiered_work_free(session, entry);
        entry = NULL;
    }

err:
    if (entry != NULL)
        __wt_tiered_work_free(session, entry);
    return (ret);
}

/*
 * __tier_storage_remove --
 *     Perform one iteration of tiered storage local tier removal.
 */
static int
__tier_storage_remove(WT_SESSION_IMPL *session, bool force)
{
    WT_UNUSED(session);
    WT_UNUSED(force);

    /*
     * We want to walk the metadata perhaps and for each tiered URI, call remove on its file:URI
     * version.
     */
    WT_RET(__tier_storage_remove_local(session));
    return (0);
}

/*
 * __tiered_server --
 *     The tiered storage server thread.
 */
static WT_THREAD_RET
__tiered_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;
    uint64_t cond_time, time_start, time_stop, timediff;
    const char *msg;
    bool signalled;

    session = arg;
    conn = S2C(session);

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    /* Condition timeout is in microseconds. */
    cond_time = conn->tiered_interval * WT_MILLION;
    time_start = __wt_clock(session);
    signalled = false;
    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait_signal(
          session, conn->tiered_cond, cond_time, __tiered_server_run_chk, &signalled);

        /* Check if we're quitting or being reconfigured. */
        if (!__tiered_server_run_chk(session))
            break;

        time_stop = __wt_clock(session);
        timediff = WT_CLOCKDIFF_SEC(time_stop, time_start);
        /*
         * Here is where we do work. Work we expect to do:
         *  - Copy any files that need moving from a flush tier call.
         *  - Perform any shared storage processing after flushing.
         *  - Remove any cached objects that are aged out.
         */
        if (timediff >= conn->tiered_interval || signalled) {
            msg = "tier_storage_copy";
            WT_ERR(__tier_storage_copy(session));
            msg = "tier_storage_finish";
            WT_ERR(__tier_storage_finish(session));
            msg = "tier_storage_remove";
            WT_ERR(__tier_storage_remove(session, false));
            time_start = time_stop;
        }
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "storage server error from %s", msg));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_tiered_storage_create --
 *     Start the tiered storage subsystem.
 */
int
__wt_tiered_storage_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Start the internal thread. */
    WT_ERR(__wt_cond_alloc(session, "flush tier", &conn->flush_cond));
    WT_ERR(__wt_cond_alloc(session, "storage server", &conn->tiered_cond));
    FLD_SET(conn->server_flags, WT_CONN_SERVER_TIERED);

    WT_ERR(__wt_open_internal_session(conn, "tiered-server", true, 0, 0, &conn->tiered_session));
    session = conn->tiered_session;

    /*
     * Check for objects that are not flushed on the first flush_tier call. We cannot do that work
     * right now because it would entail opening and getting the dhandle for every table and that
     * work is already done in the flush_tier. So do it there and keep that code together.
     */
    F_SET(conn, WT_CONN_TIERED_FIRST_FLUSH);
    /* Start the thread. */
    WT_ERR(__wt_thread_create(session, &conn->tiered_tid, __tiered_server, session));
    conn->tiered_tid_set = true;

    if (0) {
err:
        FLD_CLR(conn->server_flags, WT_CONN_SERVER_TIERED);
        WT_TRET(__wt_tiered_storage_destroy(session, false));
    }
    return (ret);
}

/*
 * __wt_tiered_storage_destroy --
 *     Destroy the tiered storage server thread.
 */
int
__wt_tiered_storage_destroy(WT_SESSION_IMPL *session, bool final_flush)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TIERED_WORK_UNIT *entry;

    conn = S2C(session);

    /*
     * Stop the internal server thread. If there is unfinished work, we will recover it on startup
     * just as if there had been a system failure.
     */
    if (conn->flush_cond != NULL)
        __wt_cond_signal(session, conn->flush_cond);
    if (final_flush && conn->tiered_cond != NULL) {
        __wt_cond_signal(session, conn->tiered_cond);
        __wt_tiered_flush_work_wait(session, 30);
    }
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_TIERED);
    if (conn->tiered_tid_set) {
        WT_ASSERT(session, conn->tiered_cond != NULL);
        __wt_cond_signal(session, conn->tiered_cond);
        WT_TRET(__wt_thread_join(session, &conn->tiered_tid));
        conn->tiered_tid_set = false;
        while ((entry = TAILQ_FIRST(&conn->tieredqh)) != NULL) {
            TAILQ_REMOVE(&conn->tieredqh, entry, q);
            __wt_tiered_work_free(session, entry);
        }
    }
    if (conn->tiered_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->tiered_session));
        conn->tiered_session = NULL;
    }

    /* Destroy all condition variables after threads have stopped. */
    __wt_cond_destroy(session, &conn->tiered_cond);
    /* The flush condition variable must be last because any internal thread could be using it. */
    __wt_cond_destroy(session, &conn->flush_cond);

    return (ret);
}
