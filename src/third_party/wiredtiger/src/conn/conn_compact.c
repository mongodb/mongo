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
 * __background_compact_exclude_list_add --
 *     Add the entry to the exclude hash table.
 */
static int
__background_compact_exclude_list_add(WT_SESSION_IMPL *session, const char *name, size_t len)
{
    WT_BACKGROUND_COMPACT_EXCLUDE *new_entry;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;

    conn = S2C(session);

    /* Early exit if this allocation fails. */
    WT_RET(__wt_calloc_one(session, &new_entry));
    WT_ERR(__wt_strndup(session, name, len, &new_entry->name));

    hash = __wt_hash_city64(name, len);
    bucket = hash & (conn->hash_size - 1);
    /* Insert entry into hash table. */
    TAILQ_INSERT_HEAD(&conn->background_compact.exclude_list_hash[bucket], new_entry, hashq);

    if (ret != 0) {
err:
        __wt_free(session, new_entry->name);
        __wt_free(session, new_entry);
    }

    return (ret);
}

/*
 * __background_compact_exclude_list_clear --
 *     Clear the list of entries excluded from compaction.
 */
static void
__background_compact_exclude_list_clear(WT_SESSION_IMPL *session, bool closing)
{
    WT_BACKGROUND_COMPACT_EXCLUDE *entry;
    WT_CONNECTION_IMPL *conn;
    uint64_t i;

    conn = S2C(session);

    for (i = 0; i < conn->hash_size; ++i) {
        while (!TAILQ_EMPTY(&conn->background_compact.exclude_list_hash[i])) {
            entry = TAILQ_FIRST(&conn->background_compact.exclude_list_hash[i]);
            /* Remove entry from the hash table. */
            TAILQ_REMOVE(&conn->background_compact.exclude_list_hash[i], entry, hashq);
            __wt_free(session, entry->name);
            __wt_free(session, entry);
        }
    }

    if (closing)
        __wt_free(session, conn->background_compact.exclude_list_hash);
}

/*
 * __background_compact_exclude_list_process --
 *     Process the exclude list present in a configuration.
 */
static int
__background_compact_exclude_list_process(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG exclude_config;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_RET;
    const char *cfg[3] = {NULL, NULL, NULL};

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_compact);
    cfg[1] = config;
    cfg[2] = NULL;

    __background_compact_exclude_list_clear(session, false);
    WT_RET_NOTFOUND_OK(__wt_config_gets(session, cfg, "exclude", &cval));
    if (cval.len != 0) {
        /*
         * Check that the configuration string only has table schema formats in the target list and
         * construct the target hash table.
         */
        __wt_config_subinit(session, &exclude_config, &cval);
        while ((ret = __wt_config_next(&exclude_config, &k, &v)) == 0) {
            if (!WT_PREFIX_MATCH(k.str, "table:"))
                WT_RET_MSG(session, EINVAL,
                  "Background compaction can only exclude objects of type \"table\" formats in "
                  "the exclude uri list, found %.*s instead.",
                  (int)k.len, k.str);

            WT_PREFIX_SKIP_REQUIRED(session, k.str, "table:");
            WT_RET(__background_compact_exclude_list_add(
              session, (char *)k.str, k.len - strlen("table:")));
        }
        WT_RET_NOTFOUND_OK(ret);
        ret = 0;
    }
    return (ret);
}

/*
 * __background_compact_exclude --
 *     Search if the given URI is part of the excluded entries.
 */
static bool
__background_compact_exclude(WT_SESSION_IMPL *session, const char *uri)
{
    WT_BACKGROUND_COMPACT_EXCLUDE *entry;
    WT_CONNECTION_IMPL *conn;
    uint64_t bucket, hash;
    bool found;

    conn = S2C(session);
    found = false;

    WT_PREFIX_SKIP_REQUIRED(session, uri, "file:");
    hash = __wt_hash_city64(uri, strlen(uri));
    bucket = hash & (conn->hash_size - 1);

    TAILQ_FOREACH (entry, &conn->background_compact.exclude_list_hash[bucket], hashq) {
        if (strcmp(uri, entry->name) == 0) {
            found = true;
            break;
        }
    }
    return (found);
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
 * __background_compact_should_skip --
 *     Check whether we should proceed with calling compaction on the given file.
 */
static int
__background_compact_should_skip(WT_SESSION_IMPL *session, const char *uri, int64_t id, bool *skipp)
{
    WT_BACKGROUND_COMPACT_STAT *compact_stat;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    wt_off_t file_size;
    uint64_t cur_time;
    const char *filename;

    conn = S2C(session);

    /* Check if the file is excluded. */
    if (__background_compact_exclude(session, uri)) {
        WT_STAT_CONN_INCR(session, background_compact_exclude);
        *skipp = true;
        return (0);
    }

    /* Fast path to check the file size, ignore small files. */
    filename = uri;
    WT_PREFIX_SKIP(filename, "file:");
    ret = __wt_block_manager_named_size(session, filename, &file_size);

    /* Ignore the error if the file no longer exists or in case of permission issues. */
    if (ret == ENOENT || ret == EACCES) {
        *skipp = true;
        return (0);
    }

    WT_RET(ret);

    if (file_size <= WT_MEGABYTE) {
        WT_STAT_CONN_INCR(session, background_compact_skipped);
        *skipp = true;
        return (0);
    }

    /* If we haven't seen this file before we should try and compact it. */
    compact_stat = __background_compact_get_stat(session, uri, id);
    if (compact_stat == NULL) {
        *skipp = false;
        return (0);
    }

    /* If we are running once, force compaction on the file. */
    if (conn->background_compact.run_once) {
        *skipp = false;
        return (0);
    }

    /* Proceed with compaction when the file has not been compacted for some time. */
    cur_time = __wt_clock(session);
    if (WT_CLOCKDIFF_SEC(cur_time, compact_stat->prev_compact_time) >=
      conn->background_compact.max_file_skip_time) {
        *skipp = false;
        return (0);
    }

    /*
     * If the last compaction pass was unsuccessful or less successful than the average, skip it for
     * some time.
     */
    if (!compact_stat->prev_compact_success ||
      compact_stat->bytes_rewritten < conn->background_compact.bytes_rewritten_ema) {
        compact_stat->skip_count++;
        conn->background_compact.files_skipped++;
        WT_STAT_CONN_INCR(session, background_compact_skipped);
        *skipp = true;
        return (0);
    }

    *skipp = false;
    return (0);
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
    if (bytes_recovered <= 0)
        compact_stat->prev_compact_success = false;
    else {
        WT_STAT_CONN_INCRV(session, background_compact_bytes_recovered, bytes_recovered);
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

    __wt_verbose_info(session, WT_VERB_COMPACT_PROGRESS,
      "%s: background compaction finished (status: %s) - reclaimed %" PRIu64 " bytes", uri,
      (compact_stat->prev_compact_success ? "success" : "failure"), (uint64_t)bytes_recovered);

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
    bool cleanup_stat;

    cleanup_stat = false;
    conn = S2C(session);
    cur_time = __wt_clock(session);

    if (cleanup_type == BACKGROUND_COMPACT_CLEANUP_EXIT ||
      cleanup_type == BACKGROUND_COMPACT_CLEANUP_OFF)
        cleanup_stat = true;

    for (i = 0; i < conn->hash_size; i++) {
        TAILQ_FOREACH_SAFE(
          compact_stat, &conn->background_compact.stat_hash[i], hashq, temp_compact_stat)
        {
            if (cleanup_stat ||
              WT_CLOCKDIFF_SEC(cur_time, compact_stat->prev_compact_time) >
                conn->background_compact.max_file_idle_time)
                __background_compact_list_remove(session, compact_stat, i);
        }
    }

    if (cleanup_type == BACKGROUND_COMPACT_CLEANUP_EXIT)
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
    bool skip;

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
            WT_ERR(__background_compact_should_skip(session, key, id.val, &skip));
            if (!skip)
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
    bool cache_pressure, full_iteration, running;

    session = arg;
    conn = S2C(session);
    wt_session = (WT_SESSION *)session;
    cache_pressure = full_iteration = running = false;

    WT_ERR(__wt_scr_alloc(session, 1024, &config));
    WT_ERR(__wt_scr_alloc(session, 1024, &next_uri));
    WT_ERR(__wt_scr_alloc(session, 1024, &uri));

    WT_STAT_CONN_SET(session, background_compact_running, 0);

    for (;;) {

        /* If the server is configured to run once, stop it after a full iteration. */
        if (full_iteration && conn->background_compact.run_once) {
            __wt_spin_lock(session, &conn->background_compact.lock);
            __wt_atomic_storebool(&conn->background_compact.running, false);
            running = false;
            WT_STAT_CONN_SET(session, background_compact_running, running);
            __wt_spin_unlock(session, &conn->background_compact.lock);
        }

        /*
         * Take a break or wait until signalled in any of the following conditions:
         * - Background compaction is not enabled.
         * - The entire metadata has been parsed.
         * - There is cache pressure and we don't want compaction to potentially add more.
         */
        if (!running || full_iteration || cache_pressure) {
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
                __background_compact_list_cleanup(session,
                  conn->background_compact.run_once ? BACKGROUND_COMPACT_CLEANUP_OFF :
                                                      BACKGROUND_COMPACT_CLEANUP_STALE_STAT);
            }

            if (cache_pressure) {
                WT_STAT_CONN_INCR(session, background_compact_sleep_cache_pressure);
                cache_pressure = false;
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
        running = __wt_atomic_loadbool(&conn->background_compact.running);

        /* The server has been signalled to change state. */
        if (conn->background_compact.signalled) {

            /* If configured to run once, start from the beginning. */
            if (running && conn->background_compact.run_once)
                WT_ERR(__wt_buf_set(session, uri, WT_BACKGROUND_COMPACT_URI_PREFIX,
                  strlen(WT_BACKGROUND_COMPACT_URI_PREFIX) + 1));

            /* If disabled, clean up the stats. */
            if (!running)
                __background_compact_list_cleanup(session, BACKGROUND_COMPACT_CLEANUP_OFF);

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

        /*
         * Throttle background compaction if any of the following conditions is met:
         * - The dirty trigger threshold has been reached as compaction may generate more dirty
         * content. Note that updates are not considered as compaction only marks pages dirty and
         * does not generate additional updates.
         * - The cache content is almost at the eviction_trigger threshold.
         */
        cache_pressure =
          __wt_evict_dirty_needed(session, NULL) || __wt_evict_clean_needed(session, NULL);
        if (cache_pressure)
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
                if (ret == EBUSY && __wt_evict_cache_stuck(session))
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
                running = __wt_atomic_loadbool(&conn->background_compact.running);
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
    __background_compact_exclude_list_clear(session, true);
    __background_compact_list_cleanup(session, BACKGROUND_COMPACT_CLEANUP_EXIT);

    __wt_free(session, conn->background_compact.config);
    __wt_scr_free(session, &config);
    __wt_scr_free(session, &next_uri);
    __wt_scr_free(session, &uri);

    if (ret != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "compact server error"));
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wti_background_compact_server_create --
 *     Start the compact thread.
 */
int
__wti_background_compact_server_create(WT_SESSION_IMPL *session)
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
    WT_RET(__wt_calloc_def(session, conn->hash_size, &conn->background_compact.exclude_list_hash));
    for (i = 0; i < conn->hash_size; i++) {
        TAILQ_INIT(&conn->background_compact.stat_hash[i]);
        TAILQ_INIT(&conn->background_compact.exclude_list_hash[i]);
    }

    /*
     * Compaction does enough I/O it may be called upon to perform slow operations for the block
     * manager. Don't let the background compaction thread be pulled into eviction to limit
     * performance impacts.
     */
    session_flags = WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE;
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
 * __wti_background_compact_server_destroy --
 *     Destroy the background compaction server thread.
 */
int
__wti_background_compact_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_COMPACT);
    if (conn->background_compact.tid_set) {
        __wt_atomic_storebool(&conn->background_compact.running, false);
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
    bool enable, running;

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
    if (conn->background_compact.signalled)
        WT_ERR_MSG(session, EBUSY, "Background compact is busy processing a previous command");

    running = __wt_atomic_loadbool(&conn->background_compact.running);

    WT_ERR(__wt_config_getones(session, config, "background", &cval));
    enable = cval.val;

    /* Strip the toggle field from the configuration to check if the configuration has changed. */
    WT_ERR(__wt_config_merge(session, cfg, "background=", &stripped_config));

    /* The background compact configuration cannot be changed while it's already running. */
    if (enable && running && strcmp(stripped_config, conn->background_compact.config) != 0)
        WT_ERR_SUB(session, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING,
          "Cannot reconfigure background compaction while it's already running.");

    /* If we haven't changed states, we're done. */
    if (enable == running)
        goto err;

    /* Update the background compaction settings when the server is enabled. */
    if (enable) {
        /* The background compaction server can be configured to run once. */
        WT_ERR(__wt_config_getones(session, stripped_config, "run_once", &cval));
        conn->background_compact.run_once = cval.val;

        /* Process excluded tables. */
        WT_ERR(__background_compact_exclude_list_process(session, config));
    }

    /* The background compaction has been signalled successfully, update its state. */
    __wt_atomic_storebool(&conn->background_compact.running, enable);
    __wt_free(session, conn->background_compact.config);
    conn->background_compact.config = stripped_config;
    stripped_config = NULL;
    conn->background_compact.signalled = true;
    __wt_cond_signal(session, conn->background_compact.cond);

err:
    __wt_free(session, stripped_config);
    __wt_spin_unlock(session, &conn->background_compact.lock);
    return (ret);
}
