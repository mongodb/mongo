/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Prefix of files the background compaction server deals with. */
#define WT_BACKGROUND_COMPACT_URI_PREFIX "file:"

/*
 * __background_compact_server_run_chk --
 *     Check to decide if the compact server should continue running.
 */
static bool
__background_compact_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_COMPACT));
}

/*
 * __background_compact_list_insert --
 *     Insert compaction statistics for a file into the background compact list.
 */
static void
__background_compact_list_insert(WT_SESSION_IMPL *session, WT_BACKGROUND_COMPACT_STAT *compact_stat)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;

    conn = S2C(session);

    hash = __wt_hash_city64(compact_stat->uri, strlen(compact_stat->uri));
    bucket = hash & (conn->hash_size - 1);

    TAILQ_INSERT_HEAD(&conn->background_compact.stat_hash[bucket], compact_stat, hashq);
    ++conn->background_compact.file_count;
    WT_STAT_CONN_INCR(session, background_compact_files_tracked);
}

/*
 * __background_compact_list_remove --
 *     Remove and free compaction statistics for a file from the background compact list.
 */
static void
__background_compact_list_remove(
  WT_SESSION_IMPL *session, WT_BACKGROUND_COMPACT_STAT *compact_stat, uint64_t bucket)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    TAILQ_REMOVE(&conn->background_compact.stat_hash[bucket], compact_stat, hashq);
    WT_ASSERT(session, conn->background_compact.file_count > 0);
    --conn->background_compact.file_count;
    WT_STAT_CONN_DECR(session, background_compact_files_tracked);

    __wt_free(session, compact_stat->uri);
    __wt_free(session, compact_stat);
}

/*
 * __background_compact_get_stat --
 *     Get the statistics for the given uri and id. The id ensures uniqueness in the event of
 *     dropping and recreating files of the same name.
 */
static WT_BACKGROUND_COMPACT_STAT *
__background_compact_get_stat(WT_SESSION_IMPL *session, const char *uri, int64_t id)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat, *temp_compact_stat;
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;

    conn = S2C(session);

    WT_ASSERT(session, uri != NULL);

    hash = __wt_hash_city64(uri, strlen(uri));
    bucket = hash & (conn->hash_size - 1);

    /* Find the uri in the files compacted list. */
    TAILQ_FOREACH_SAFE(
      compact_stat, &conn->background_compact.stat_hash[bucket], hashq, temp_compact_stat)
    {
        if (strcmp(uri, compact_stat->uri) == 0) {
            /*
             * If we've found an entry in the list with the same URI but different IDs we must've
             * dropped and recreated this table. Reset the entry in this case.
             */
            if (id != compact_stat->id) {
                __background_compact_list_remove(session, compact_stat, bucket);
                return (NULL);
            }

            return (compact_stat);
        }
    }

    return (NULL);
}

/*
 * __background_compact_should_run --
 *     Check whether we should proceed with calling compaction on the given file.
 */
static bool
__background_compact_should_run(WT_SESSION_IMPL *session, const char *uri, int64_t id)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat;
    WT_CONNECTION_IMPL *conn;
    uint64_t cur_time;

    conn = S2C(session);

    /* If we haven't seen this file before we should try and compact it. */
    compact_stat = __background_compact_get_stat(session, uri, id);
    if (compact_stat == NULL)
        return (true);

    /* Proceed with compaction when the file has not been compacted for some time. */
    cur_time = __wt_clock(session);
    if (WT_CLOCKDIFF_SEC(cur_time, compact_stat->prev_compact_time) >=
      conn->background_compact.max_file_skip_time)
        return (true);

    /*
     * If the last compaction pass was unsuccessful or less successful than the average, skip it for
     * some time.
     */
    if (!compact_stat->prev_compact_success ||
      compact_stat->bytes_rewritten < conn->background_compact.bytes_rewritten_ema) {
        compact_stat->skip_count++;
        conn->background_compact.files_skipped++;
        WT_STAT_CONN_INCR(session, background_compact_skipped);
        return (false);
    }

    return (true);
}

/*
 * __wt_background_compact_start --
 *     Pre-fill compact related statistics for the file being compacted by the current session.
 */
int
__wt_background_compact_start(WT_SESSION_IMPL *session)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat;
    WT_BM *bm;
    WT_DECL_RET;
    uint32_t id;
    const char *uri;

    bm = S2BT(session)->bm;
    id = S2BT(session)->id;
    uri = session->dhandle->name;

    compact_stat = __background_compact_get_stat(session, uri, id);

    /* If the table is not in the list, allocate a new entry and insert it. */
    if (compact_stat == NULL) {
        WT_ERR(__wt_calloc_one(session, &compact_stat));
        WT_ERR(__wt_strdup(session, uri, &compact_stat->uri));
        compact_stat->id = id;
        __background_compact_list_insert(session, compact_stat);
    }

    /* Fill starting information prior to running compaction. */
    WT_ERR(bm->size(bm, session, &compact_stat->start_size));
    compact_stat->prev_compact_time = __wt_clock(session);

    return (0);

err:
    __wt_free(session, compact_stat);

    return (ret);
}

/*
 * __wt_background_compact_end --
 *     Fill resulting compact statistics in the background compact tracking list for the file being
 *     compacted by the current session.
 */
int
__wt_background_compact_end(WT_SESSION_IMPL *session)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat;
    WT_BM *bm;
    WT_CONNECTION_IMPL *conn;
    wt_off_t bytes_recovered;
    int64_t id;
    const char *uri;

    bm = S2BT(session)->bm;
    id = S2BT(session)->id;
    uri = session->dhandle->name;

    compact_stat = __background_compact_get_stat(session, uri, id);

    WT_ASSERT(session, compact_stat != NULL);

    conn = S2C(session);

    WT_RET(bm->size(bm, session, &compact_stat->end_size));
    compact_stat->bytes_rewritten = bm->block->compact_bytes_rewritten;
    bytes_recovered = compact_stat->start_size - compact_stat->end_size;

    /*
     * If the file failed to decrease in size, mark as an unsuccessful attempt. It's possible for
     * compaction to do work (rewriting bytes) while other operations cause the file to increase in
     * size.
     */
    if (bytes_recovered <= 0) {
        compact_stat->consecutive_unsuccessful_attempts++;
        compact_stat->prev_compact_success = false;
    } else {
        WT_STAT_CONN_INCRV(session, background_compact_bytes_recovered, bytes_recovered);
        compact_stat->consecutive_unsuccessful_attempts = 0;
        conn->background_compact.files_compacted++;
        compact_stat->prev_compact_success = true;

        /*
         * Update the moving average of bytes rewritten across each file compact attempt. A
         * weighting of 10% means that we are effectively considering the last 10 attempts in the
         * average.
         */
        conn->background_compact.bytes_rewritten_ema =
          (uint64_t)(0.1 * bm->block->compact_bytes_rewritten +
            0.9 * conn->background_compact.bytes_rewritten_ema);
        WT_STAT_CONN_SET(
          session, background_compact_ema, conn->background_compact.bytes_rewritten_ema);
    }

    return (0);
}

/*
 * __background_compact_list_cleanup --
 *     Free all entries as part of cleanup or any entry that has been idle for too long in the
 *     background compact tracking list.
 */
static void
__background_compact_list_cleanup(
  WT_SESSION_IMPL *session, WT_BACKGROUND_COMPACT_CLEANUP_STAT_TYPE cleanup_type)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat, *temp_compact_stat;
    WT_CONNECTION_IMPL *conn;
    uint64_t cur_time, i;

    conn = S2C(session);
    cur_time = __wt_clock(session);

    for (i = 0; i < conn->hash_size; i++)
        TAILQ_FOREACH_SAFE(
          compact_stat, &conn->background_compact.stat_hash[i], hashq, temp_compact_stat)
        {
            if (cleanup_type == BACKGROUND_CLEANUP_ALL_STAT ||
              WT_CLOCKDIFF_SEC(cur_time, compact_stat->prev_compact_time) >
                conn->background_compact.max_file_idle_time)
                __background_compact_list_remove(session, compact_stat, i);
        }

    if (cleanup_type == BACKGROUND_CLEANUP_ALL_STAT)
        __wt_free(session, conn->background_compact.stat_hash);
}

/*
 * __background_compact_find_next_uri --
 *     Given a URI, find the next one in the metadata file that is eligible for compaction.
 */
static int
__background_compact_find_next_uri(WT_SESSION_IMPL *session, WT_ITEM *uri, WT_ITEM *next_uri)
{
    WT_CONFIG_ITEM id;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    int exact;
    const char *key, *value;

    cursor = NULL;
    exact = 0;
    key = NULL;
    value = NULL;

    /* Use a metadata cursor to have access to the existing URIs. */
    WT_ERR(__wt_metadata_cursor(session, &cursor));

    /* Position the cursor on the given URI. */
    cursor->set_key(cursor, (const char *)uri->data);
    WT_ERR(cursor->search_near(cursor, &exact));

    /*
     * The given URI may not exist in the metadata file. Since we always want to return a URI that
     * is lexicographically larger the given one, make sure not to go backwards.
     */
    if (exact <= 0)
        WT_ERR(cursor->next(cursor));

    /* Loop through the eligible candidates. */
    do {
        WT_ERR(cursor->get_key(cursor, &key));
        /* Check we are still dealing with keys which have the right prefix. */
        if (!WT_PREFIX_MATCH(key, WT_BACKGROUND_COMPACT_URI_PREFIX)) {
            ret = WT_NOTFOUND;
            break;
        }

        /* Check the file is eligible for compaction. */
        if (__wt_compact_check_eligibility(session, key)) {
            /*
             * Check the list of files background compact has tracked statistics for. This avoids
             * having to open a dhandle for the file if compaction is unlikely to work efficiently
             * on this file.
             */
            WT_ERR(cursor->get_value(cursor, &value));
            WT_ERR(__wt_config_getones(session, value, "id", &id));
            if (__background_compact_should_run(session, key, id.val))
                break;
        }
    } while ((ret = cursor->next(cursor)) == 0);
    WT_ERR(ret);

    /* Save the selected uri. */
    WT_ERR(__wt_buf_set(session, next_uri, cursor->key.data, cursor->key.size));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));

    return (ret);
}

/*
 * __background_compact_server --
 *     The compact server thread.
 */
static WT_THREAD_RET
__background_compact_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(config);
    WT_DECL_ITEM(next_uri);
    WT_DECL_ITEM(uri);
    WT_DECL_RET;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    bool full_iteration, running;

    session = arg;
    conn = S2C(session);
    wt_session = (WT_SESSION *)session;
    full_iteration = running = false;

    WT_ERR(__wt_scr_alloc(session, 1024, &config));
    WT_ERR(__wt_scr_alloc(session, 1024, &next_uri));
    WT_ERR(__wt_scr_alloc(session, 1024, &uri));

    WT_STAT_CONN_SET(session, background_compact_running, 0);

    for (;;) {

        /* When the entire metadata file has been parsed, take a break or wait until signalled. */
        if (full_iteration || !running) {

            /*
             * In order to always try to parse all the candidates present in the metadata file even
             * though the compaction server may be stopped at random times, only set the URI to the
             * prefix for the very first iteration and when all the candidates in the metadata file
             * have been parsed.
             */
            if (uri->size == 0 || full_iteration) {
                full_iteration = false;
                WT_ERR(__wt_buf_set(session, uri, WT_BACKGROUND_COMPACT_URI_PREFIX,
                  strlen(WT_BACKGROUND_COMPACT_URI_PREFIX) + 1));
                __background_compact_list_cleanup(session, BACKGROUND_CLEANUP_STALE_STAT);
            }

            /* Check periodically in case the signal was missed. */
            __wt_cond_wait(session, conn->background_compact.cond,
              conn->background_compact.full_iteration_wait_time * WT_MILLION,
              __background_compact_server_run_chk);
        }

        /* Check if we're quitting or being reconfigured. */
        if (!__background_compact_server_run_chk(session))
            break;

        __wt_spin_lock(session, &conn->background_compact.lock);
        running = conn->background_compact.running;
        if (conn->background_compact.signalled) {
            conn->background_compact.signalled = false;
            WT_STAT_CONN_SET(session, background_compact_running, running);
        }
        __wt_spin_unlock(session, &conn->background_compact.lock);

        /*
         * This check is necessary as we may have timed out while waiting on the mutex to be
         * signalled and compaction is not supposed to be executed.
         */
        if (!running)
            continue;

        /* Find the next URI to compact. */
        WT_ERR_NOTFOUND_OK(__background_compact_find_next_uri(session, uri, next_uri), true);

        /* All the keys with the specified prefix have been parsed. */
        if (ret == WT_NOTFOUND) {
            full_iteration = true;
            continue;
        }

        /* Use the retrieved URI. */
        WT_ERR(__wt_buf_set(session, uri, next_uri->data, next_uri->size));

        /* Compact the file with the latest configuration. */
        __wt_spin_lock(session, &conn->background_compact.lock);
        if (config->size == 0 ||
          !WT_STREQ((const char *)config->data, conn->background_compact.config))
            ret = __wt_buf_set(session, config, conn->background_compact.config,
              strlen(conn->background_compact.config) + 1);
        __wt_spin_unlock(session, &conn->background_compact.lock);

        WT_ERR(ret);

        ret = wt_session->compact(wt_session, (const char *)uri->data, (const char *)config->data);

        /*
         * Compact may return:
         * - EBUSY or WT_ROLLBACK for various reasons.
         * - ENOENT if the underlying file does not exist.
         * - ETIMEDOUT if the configured timer has elapsed.
         * - WT_ERROR if the background compaction has been interrupted.
         */
        if (ret != 0) {
            WT_STAT_CONN_INCR(session, background_compact_fail);
            /* The following errors are always silenced. */
            if (ret == EBUSY || ret == ENOENT || ret == ETIMEDOUT || ret == WT_ROLLBACK) {
                if (ret == EBUSY && __wt_cache_stuck(session))
                    WT_STAT_CONN_INCR(session, background_compact_fail_cache_pressure);
                else if (ret == ETIMEDOUT)
                    WT_STAT_CONN_INCR(session, background_compact_timeout);
                ret = 0;
            }

            /*
             * Verify WT_ERROR comes from an interruption by checking the server is no longer
             * running.
             */
            else if (ret == WT_ERROR) {
                __wt_spin_lock(session, &conn->background_compact.lock);
                running = conn->background_compact.running;
                __wt_spin_unlock(session, &conn->background_compact.lock);
                if (!running) {
                    WT_STAT_CONN_INCR(session, background_compact_interrupted);
                    ret = 0;
                }
            }
            WT_ERR(ret);
        } else
            WT_STAT_CONN_INCR(session, background_compact_success);
    }

    WT_STAT_CONN_SET(session, background_compact_running, 0);

err:
    __background_compact_list_cleanup(session, BACKGROUND_CLEANUP_ALL_STAT);

    __wt_free(session, conn->background_compact.config);
    __wt_scr_free(session, &config);
    __wt_scr_free(session, &next_uri);
    __wt_scr_free(session, &uri);

    if (ret != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "compact server error"));
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_background_compact_server_create --
 *     Start the compact thread.
 */
int
__wt_background_compact_server_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint64_t i;
    uint32_t session_flags;

    conn = S2C(session);

    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_COMPACT);

    WT_RET(__wt_calloc_def(session, conn->hash_size, &conn->background_compact.stat_hash));
    for (i = 0; i < conn->hash_size; i++)
        TAILQ_INIT(&conn->background_compact.stat_hash[i]);

    /*
     * Compaction does enough I/O it may be called upon to perform slow operations for the block
     * manager.
     */
    session_flags = WT_SESSION_CAN_WAIT;
    WT_RET(__wt_open_internal_session(
      conn, "compact-server", true, session_flags, 0, &conn->background_compact.session));
    session = conn->background_compact.session;

    WT_RET(__wt_cond_alloc(session, "compact server", &conn->background_compact.cond));

    WT_RET(__wt_thread_create(
      session, &conn->background_compact.tid, __background_compact_server, session));
    conn->background_compact.tid_set = true;

    return (0);
}

/*
 * __wt_background_compact_server_destroy --
 *     Destroy the background compaction server thread.
 */
int
__wt_background_compact_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_COMPACT);
    if (conn->background_compact.tid_set) {
        conn->background_compact.running = false;
        __wt_cond_signal(session, conn->background_compact.cond);
        WT_TRET(__wt_thread_join(session, &conn->background_compact.tid));
        conn->background_compact.tid_set = false;
    }
    __wt_cond_destroy(session, &conn->background_compact.cond);

    /* Close the server thread's session. */
    if (conn->background_compact.session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->background_compact.session));
        conn->background_compact.session = NULL;
    }

    return (ret);
}

/*
 * __wt_background_compact_signal --
 *     Signal the compact thread. Return an error if the background compaction server has not
 *     processed a previous signal yet or because of an invalid configuration.
 */
int
__wt_background_compact_signal(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *cfg[3] = {NULL, NULL, NULL}, *stripped_config;
    bool running;

    conn = S2C(session);
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_compact);
    cfg[1] = config;
    cfg[2] = NULL;
    stripped_config = NULL;

    /* The background compaction server is not compatible with in-memory or readonly databases. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY)) {
        __wt_verbose_warning(session, WT_VERB_COMPACT, "%s",
          "Background compact cannot be configured for in-memory or readonly databases.");
        return (ENOTSUP);
    }

    /* Wait for any previous signal to be processed first. */
    __wt_spin_lock(session, &conn->background_compact.lock);
    if (conn->background_compact.signalled) {
        ret = EBUSY;
        goto err;
    }

    running = conn->background_compact.running;

    WT_ERR(__wt_config_getones(session, config, "background", &cval));
    if (cval.val == running)
        /*
         * This is an error as we are already in the same state and reconfiguration is not allowed.
         */
        WT_ERR_MSG(
          session, EINVAL, "Background compaction is already %s", running ? "enabled" : "disabled");
    conn->background_compact.running = !running;

    /* Strip the background field from the configuration now it has been parsed. */
    WT_ERR(__wt_config_merge(session, cfg, "background=", &stripped_config));
    __wt_free(session, conn->background_compact.config);
    conn->background_compact.config = stripped_config;

    conn->background_compact.signalled = true;

err:
    __wt_spin_unlock(session, &conn->background_compact.lock);
    if (ret == 0)
        __wt_cond_signal(session, conn->background_compact.cond);
    return (ret);
}
