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
 * __stat_sources_free --
 *     Free the array of statistics sources.
 */
static void
__stat_sources_free(WT_SESSION_IMPL *session, char ***sources)
{
    char **p;

    if ((p = (*sources)) != NULL) {
        for (; *p != NULL; ++p)
            __wt_free(session, *p);
        __wt_free(session, *sources);
    }
}

/*
 * __stat_config_discard --
 *     Discard all statistics-log configuration.
 */
static int
__stat_config_discard(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /*
     * Discard all statistics-log configuration information, called when reconfiguring or destroying
     * the statistics logging setup,
     */
    __wt_free(session, conn->stat_format);
    ret = __wt_fclose(session, &conn->stat_fs);
    __wt_free(session, conn->stat_path);
    __stat_sources_free(session, &conn->stat_sources);
    conn->stat_stamp = NULL;
    conn->stat_usecs = 0;
    return (ret);
}

/*
 * __wt_conn_stat_init --
 *     Initialize the per-connection statistics.
 */
void
__wt_conn_stat_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_STATS **stats;

    conn = S2C(session);
    stats = conn->stats;

    __wt_cache_stats_update(session);
    __wt_txn_stats_update(session);

    WT_STAT_SET(session, stats, file_open, conn->open_file_count);
    WT_STAT_SET(session, stats, cursor_open_count, conn->open_cursor_count);
    WT_STAT_SET(session, stats, dh_conn_handle_count, conn->dhandle_count);
    WT_STAT_SET(session, stats, rec_split_stashed_objects, conn->stashed_objects);
    WT_STAT_SET(session, stats, rec_split_stashed_bytes, conn->stashed_bytes);
}

/*
 * __statlog_config --
 *     Parse and setup the statistics server options.
 */
static int
__statlog_config(WT_SESSION_IMPL *session, const char **cfg, bool *runp)
{
    WT_CONFIG objectconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    int cnt;
    char **sources;

    /*
     * A note on reconfiguration: the standard "is this configuration string allowed" checks should
     * fail if reconfiguration has invalid strings, for example, "log=(enabled)", or
     * "statistics_log=(path=XXX)", because the connection reconfiguration method doesn't allow
     * those strings. Additionally, the base configuration values during reconfiguration are the
     * currently configured values (so we don't revert to default values when repeatedly
     * reconfiguring), and configuration processing of a currently set value should not change the
     * currently set value.
     *
     * In this code path, a previous statistics log server reconfiguration may have stopped the
     * server (and we're about to restart it). Because stopping the server discarded the configured
     * information stored in the connection structure, we have to re-evaluate all configuration
     * values, reconfiguration can't skip any of them.
     */

    conn = S2C(session);
    sources = NULL;

    /* Only start the server if wait time is non-zero */
    WT_RET(__wt_config_gets(session, cfg, "statistics_log.wait", &cval));
    *runp = cval.val != 0;
    conn->stat_usecs = (uint64_t)cval.val * WT_MILLION;

    /*
     * Only set the JSON flag when stats are enabled, otherwise setting this flag can implicitly
     * enable statistics gathering.
     */
    WT_RET(__wt_config_gets(session, cfg, "statistics_log.json", &cval));
    if (cval.val != 0 && WT_STAT_ENABLED(session))
        FLD_SET(conn->stat_flags, WT_STAT_JSON);

    WT_RET(__wt_config_gets(session, cfg, "statistics_log.on_close", &cval));
    if (cval.val != 0)
        FLD_SET(conn->stat_flags, WT_STAT_ON_CLOSE);

    /*
     * We don't allow the log path to be reconfigured for security reasons. (Applications passing
     * input strings directly to reconfigure would expose themselves to a potential security
     * problem, the utility of reconfiguring a statistics log path isn't worth the security risk.)
     *
     * See above for the details, but during reconfiguration we're loading the path value from the
     * saved configuration information, and it's required during reconfiguration because we
     * potentially stopped and are restarting, the server.
     */
    WT_RET(__wt_config_gets(session, cfg, "statistics_log.path", &cval));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_buf_fmt(session, tmp, "%.*s/%s", (int)cval.len, cval.str, WT_STATLOG_FILENAME));
    WT_ERR(__wt_filename(session, tmp->data, &conn->stat_path));

    WT_ERR(__wt_config_gets(session, cfg, "statistics_log.sources", &cval));
    __wt_config_subinit(session, &objectconf, &cval);
    for (cnt = 0; (ret = __wt_config_next(&objectconf, &k, &v)) == 0; ++cnt)
        ;
    WT_ERR_NOTFOUND_OK(ret, false);
    if (cnt != 0) {
        WT_ERR(__wt_calloc_def(session, cnt + 1, &sources));
        __wt_config_subinit(session, &objectconf, &cval);
        for (cnt = 0; (ret = __wt_config_next(&objectconf, &k, &v)) == 0; ++cnt) {
            /*
             * XXX Only allow "file:" and "lsm:" for now: "file:" works because it's been converted
             * to data handles, "lsm:" works because we can easily walk the list of open LSM
             * objects, even though it hasn't been converted.
             */
            if (!WT_PREFIX_MATCH(k.str, "file:") && !WT_PREFIX_MATCH(k.str, "lsm:"))
                WT_ERR_MSG(session, EINVAL,
                  "statistics_log sources configuration only supports objects of type \"file\" or "
                  "\"lsm\"");
            WT_ERR(__wt_strndup(session, k.str, k.len, &sources[cnt]));
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        conn->stat_sources = sources;
        sources = NULL;
    }

/*
 * When using JSON format, use the same timestamp format as MongoDB by
 * default. This requires caution: the user might have set the timestamp
 * in a previous reconfigure call and we don't want to override that, so
 * compare the retrieved value with the default value to decide if we
 * should use the JSON default.
 *
 * (This still implies if the user explicitly sets the timestamp to the
 * default value, then sets the JSON flag in a separate reconfigure
 * call, or vice-versa, we will incorrectly switch to the JSON default
 * timestamp. But there's no way to detect that, and this is all a low
 * probability path.)
 *
 * !!!
 * Don't rewrite in the compressed "%FT%T.000Z" form, MSVC13 segfaults.
 */
#define WT_TIMESTAMP_DEFAULT "%b %d %H:%M:%S"
#define WT_TIMESTAMP_JSON_DEFAULT "%Y-%m-%dT%H:%M:%S.000Z"
    WT_ERR(__wt_config_gets(session, cfg, "statistics_log.timestamp", &cval));
    if (FLD_ISSET(conn->stat_flags, WT_STAT_JSON) &&
      WT_STRING_MATCH(WT_TIMESTAMP_DEFAULT, cval.str, cval.len))
        WT_ERR(__wt_strdup(session, WT_TIMESTAMP_JSON_DEFAULT, &conn->stat_format));
    else
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->stat_format));

err:
    __stat_sources_free(session, &sources);
    __wt_scr_free(session, &tmp);

    return (ret);
}

/*
 * __statlog_print_header --
 *     Write the header for statistics when running in JSON mode.
 */
static int
__statlog_print_header(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->stat_flags, WT_STAT_JSON))
        return (0);

    /*
     * This flag is required in order to generate correct JSON when printing out stats for
     * individual tables. When we are about to print the first table's stats we must print out the
     * wiredTigerTables header once only and add a correct closing brace when we finish the tables
     * section. To do this we maintain a flag variable to note when we have printed the first table.
     * Unfortunately, the mechanism which we use to print stats for each table does not allow
     * passing of variables by reference, this necessitates the use of a variable on the connection.
     * The variable is safe as the JSON printing logic is only performed by the single threaded stat
     * server.
     */
    conn->stat_json_tables = false;
    WT_RET(__wt_fprintf(session, conn->stat_fs, "{\"version\":\"%s\",\"localTime\":\"%s\"",
      WIREDTIGER_VERSION_STRING, conn->stat_stamp));
    return (0);
}

/*
 * __statlog_print_table_name --
 *     Write the header for the wiredTigerTables section of statistics if running in JSON mode and
 *     the header has not been written this round, then print the name of the table.
 */
static int
__statlog_print_table_name(WT_SESSION_IMPL *session, const char *name, bool conn_stats)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->stat_flags, WT_STAT_JSON))
        return (0);

    /*
     * If printing the connection stats, write that header and we are done.
     */
    if (conn_stats) {
        WT_RET(__wt_fprintf(session, conn->stat_fs, ",\"wiredTiger\":{"));
        return (0);
    }

    /*
     * If this is the first table we are printing stats for print the header for the
     * wiredTigerTables section. Otherwise print a comma as this is a subsequent table.
     */
    if (conn->stat_json_tables)
        WT_RET(__wt_fprintf(session, conn->stat_fs, ","));
    else {
        conn->stat_json_tables = true;
        WT_RET(__wt_fprintf(session, conn->stat_fs, ",\"wiredTigerTables\":{"));
    }
    WT_RET(__wt_fprintf(session, conn->stat_fs, "\"%s\":{", name));
    return (0);
}

/*
 * __statlog_print_footer --
 *     Write the footer for statistics when running in JSON mode.
 */
static int
__statlog_print_footer(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->stat_flags, WT_STAT_JSON))
        return (0);

    /* If we have printed a tables stats, then close that section. */
    if (conn->stat_json_tables) {
        WT_RET(__wt_fprintf(session, conn->stat_fs, "}"));
        conn->stat_json_tables = false;
    }
    WT_RET(__wt_fprintf(session, conn->stat_fs, "}\n"));
    return (0);
}

/*
 * __statlog_dump --
 *     Dump out handle/connection statistics.
 */
static int
__statlog_dump(WT_SESSION_IMPL *session, const char *name, bool conn_stats)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    size_t prefixlen;
    int64_t val;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};
    const char *desc, *endprefix, *valstr, *uri;
    bool first, groupfirst;

    conn = S2C(session);
    cursor = NULL;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    first = groupfirst = true;

    /* Build URI and configuration string. */
    if (conn_stats)
        uri = "statistics:";
    else {
        WT_ERR(__wt_buf_fmt(session, tmp, "statistics:%s", name));
        uri = tmp->data;
    }

    /*
     * Open the statistics cursor and dump the statistics.
     *
     * If we don't find an underlying object, silently ignore it, the object may exist only
     * intermittently.
     */
    if ((ret = __wt_curstat_open(session, uri, NULL, cfg, &cursor)) != 0) {
        if (ret == EBUSY || ret == ENOENT || ret == WT_NOTFOUND)
            ret = 0;
        goto err;
    }

    WT_ERR(__statlog_print_table_name(session, name, conn_stats));
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_value(cursor, &desc, &valstr, &val));
        if (FLD_ISSET(conn->stat_flags, WT_STAT_JSON)) {
            /* Check if we are starting a new section. */
            endprefix = strchr(desc, ':');
            prefixlen = WT_PTRDIFF(endprefix, desc);
            WT_ASSERT(session, endprefix != NULL);
            if (first || tmp->size != prefixlen || strncmp(desc, tmp->data, tmp->size) != 0) {
                WT_ERR(__wt_buf_set(session, tmp, desc, prefixlen));
                WT_ERR(__wt_fprintf(
                  session, conn->stat_fs, "%s\"%.*s\":{", first ? "" : "},", (int)prefixlen, desc));
                first = false;
                groupfirst = true;
            }
            WT_ERR(__wt_fprintf(session, conn->stat_fs, "%s\"%s\":%" PRId64, groupfirst ? "" : ",",
              endprefix + 2, val));
            groupfirst = false;
        } else
            WT_ERR(__wt_fprintf(
              session, conn->stat_fs, "%s %" PRId64 " %s %s\n", conn->stat_stamp, val, name, desc));
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    if (FLD_ISSET(conn->stat_flags, WT_STAT_JSON))
        WT_ERR(__wt_fprintf(session, conn->stat_fs, "}}"));

err:
    __wt_scr_free(session, &tmp);
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __statlog_apply --
 *     Review a single open handle and dump statistics on demand.
 */
static int
__statlog_apply(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    char **p;

    WT_UNUSED(cfg);

    dhandle = session->dhandle;

    /* Check for a match on the set of sources. */
    for (p = S2C(session)->stat_sources; *p != NULL; ++p)
        if (WT_PREFIX_MATCH(dhandle->name, *p)) {
            WT_WITHOUT_DHANDLE(session, ret = __statlog_dump(session, dhandle->name, false));
            return (ret);
        }
    return (0);
}

/*
 * __statlog_lsm_apply --
 *     Review the list open LSM trees, and dump statistics on demand. XXX This code should be
 *     removed when LSM objects are converted to data handles.
 */
static int
__statlog_lsm_apply(WT_SESSION_IMPL *session)
{
#define WT_LSM_TREE_LIST_SLOTS 100
    WT_LSM_TREE *lsm_tree, *list[WT_LSM_TREE_LIST_SLOTS];
    WT_DECL_RET;
    int cnt;
    bool locked;
    char **p;

    cnt = 0;

    /*
     * Walk the list of LSM trees, checking for a match on the set of sources.
     *
     * XXX We can't hold the schema lock for the traversal because the LSM statistics code acquires
     * the tree lock, and the LSM cursor code acquires the tree lock and then acquires the schema
     * lock, it's a classic deadlock. This is temporary code so I'm not going to do anything fancy.
     * It is OK to not keep holding the schema lock after populating the list of matching LSM trees,
     * since the __wt_lsm_tree_get call will bump a reference count, so the tree won't go away.
     */
    __wt_spin_lock(session, &S2C(session)->schema_lock);
    locked = true;
    TAILQ_FOREACH (lsm_tree, &S2C(session)->lsmqh, q) {
        if (cnt == WT_LSM_TREE_LIST_SLOTS)
            break;
        for (p = S2C(session)->stat_sources; *p != NULL; ++p)
            if (WT_PREFIX_MATCH(lsm_tree->name, *p)) {
                WT_ERR(__wt_lsm_tree_get(session, lsm_tree->name, false, &list[cnt++]));
                break;
            }
    }
    __wt_spin_unlock(session, &S2C(session)->schema_lock);
    locked = false;

    while (cnt > 0) {
        --cnt;
        WT_TRET(__statlog_dump(session, list[cnt]->name, false));
        __wt_lsm_tree_release(session, list[cnt]);
    }

err:
    if (locked)
        __wt_spin_unlock(session, &S2C(session)->schema_lock);
    /* Release any LSM trees on error. */
    while (cnt > 0) {
        --cnt;
        __wt_lsm_tree_release(session, list[cnt]);
    }
    return (ret);
}

/*
 * __statlog_log_one --
 *     Output a set of statistics into the current log file.
 */
static int
__statlog_log_one(WT_SESSION_IMPL *session, WT_ITEM *path, WT_ITEM *tmp)
{
    struct timespec ts;
    struct tm localt;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* Get the current local time of day. */
    __wt_epoch(session, &ts);
    WT_RET(__wt_localtime(session, &ts.tv_sec, &localt));

    /* Create the logging path name for this time of day. */
    if (strftime(tmp->mem, tmp->memsize, conn->stat_path, &localt) == 0)
        WT_RET_MSG(session, ENOMEM, "strftime path conversion");

    /* If the path has changed, cycle the log file. */
    if (conn->stat_fs == NULL || path == NULL || strcmp(tmp->mem, path->mem) != 0) {
        WT_RET(__wt_fclose(session, &conn->stat_fs));
        WT_RET(__wt_fopen(session, tmp->mem, WT_FS_OPEN_CREATE | WT_FS_OPEN_FIXED, WT_STREAM_APPEND,
          &conn->stat_fs));

        if (path != NULL)
            WT_RET(__wt_buf_setstr(session, path, tmp->mem));
    }

    /* Create the entry prefix for this time of day. */
    if (strftime(tmp->mem, tmp->memsize, conn->stat_format, &localt) == 0)
        WT_RET_MSG(session, ENOMEM, "strftime timestamp conversion");
    conn->stat_stamp = tmp->mem;
    WT_RET(__statlog_print_header(session));

    /* Dump the connection statistics. */
    WT_RET(__statlog_dump(session, conn->home, true));

    /*
     * Lock the schema and walk the list of open handles, dumping any that match the list of object
     * sources.
     */
    if (conn->stat_sources != NULL)
        WT_RET(__wt_conn_btree_apply(session, NULL, __statlog_apply, NULL, NULL));

    /*
     * Walk the list of open LSM trees, dumping any that match the list of object sources.
     *
     * XXX This code should be removed when LSM objects are converted to data handles.
     */
    if (conn->stat_sources != NULL)
        WT_RET(__statlog_lsm_apply(session));
    WT_RET(__statlog_print_footer(session));

    /* Flush. */
    return (__wt_fflush(session, conn->stat_fs));
}

/*
 * __statlog_on_close --
 *     Log a set of statistics at close. Requires the server is not currently running.
 */
static int
__statlog_on_close(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    conn = S2C(session);

    if (!FLD_ISSET(conn->stat_flags, WT_STAT_ON_CLOSE))
        return (0);

    if (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_STATISTICS))
        WT_RET_MSG(session, EINVAL, "Attempt to log statistics while a server is running");

    WT_RET(__wt_scr_alloc(session, strlen(conn->stat_path) + 128, &tmp));
    WT_ERR(__wt_buf_setstr(session, tmp, ""));
    WT_ERR(__statlog_log_one(session, NULL, tmp));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __statlog_server_run_chk --
 *     Check to decide if the statistics log server should continue running.
 */
static bool
__statlog_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_STATISTICS));
}

/*
 * __statlog_server --
 *     The statistics server thread.
 */
static WT_THREAD_RET
__statlog_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM path, tmp;
    WT_SESSION_IMPL *session;

    session = arg;
    conn = S2C(session);

    WT_CLEAR(path);
    WT_CLEAR(tmp);

    /*
     * We need a temporary place to build a path and an entry prefix. The length of the path plus
     * 128 should be more than enough.
     *
     * We also need a place to store the current path, because that's how we know when to
     * close/re-open the file.
     */
    WT_ERR(__wt_buf_init(session, &path, strlen(conn->stat_path) + 128));
    WT_ERR(__wt_buf_setstr(session, &path, ""));
    WT_ERR(__wt_buf_init(session, &tmp, strlen(conn->stat_path) + 128));
    WT_ERR(__wt_buf_setstr(session, &tmp, ""));

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait(session, conn->stat_cond, conn->stat_usecs, __statlog_server_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__statlog_server_run_chk(session))
            break;

        if (WT_STAT_ENABLED(session))
            WT_ERR(__statlog_log_one(session, &path, &tmp));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "statistics log server error"));
    }
    __wt_buf_free(session, &path);
    __wt_buf_free(session, &tmp);
    return (WT_THREAD_RET_VALUE);
}

/*
 * __statlog_start --
 *     Start the statistics server thread.
 */
static int
__statlog_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    /* Nothing to do if the server is already running. */
    if (conn->stat_session != NULL)
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_STATISTICS);

    /* The statistics log server gets its own session. */
    WT_RET(__wt_open_internal_session(conn, "statlog-server", true, 0, 0, &conn->stat_session));
    session = conn->stat_session;

    WT_RET(__wt_cond_alloc(session, "statistics log server", &conn->stat_cond));

    /*
     * Start the thread.
     *
     * Statistics logging creates a thread per database, rather than using a single thread to do
     * logging for all of the databases. If we ever see lots of databases at a time, doing
     * statistics logging, and we want to reduce the number of threads, there's no reason we have to
     * have more than one thread, I just didn't feel like writing the code to figure out the
     * scheduling.
     */
    WT_RET(__wt_thread_create(session, &conn->stat_tid, __statlog_server, session));
    conn->stat_tid_set = true;

    return (0);
}

/*
 * __wt_statlog_create --
 *     Start the statistics server thread.
 */
int
__wt_statlog_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    bool start;

    conn = S2C(session);

    /* Readonly systems don't configure statistics logging. */
    if (F_ISSET(conn, WT_CONN_READONLY))
        return (0);

    /*
     * Stop any server that is already running. This means that each time reconfigure is called
     * we'll bounce the server even if there are no configuration changes. This makes our life
     * easier as the underlying configuration routine doesn't have to worry about freeing objects in
     * the connection structure (it's guaranteed to always start with a blank slate), and we don't
     * have to worry about races where a running server is reading configuration information that
     * we're updating, and it's not expected that reconfiguration will happen a lot.
     *
     * If there's no server running, discard any configuration information so we don't leak memory
     * during reconfiguration.
     */
    if (conn->stat_session == NULL)
        WT_RET(__stat_config_discard(session));
    else
        WT_RET(__wt_statlog_destroy(session, false));

    WT_RET(__statlog_config(session, cfg, &start));
    if (start)
        WT_RET(__statlog_start(conn));

    return (0);
}

/*
 * __wt_statlog_destroy --
 *     Destroy the statistics server thread.
 */
int
__wt_statlog_destroy(WT_SESSION_IMPL *session, bool is_close)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Readonly systems don't configure statistics logging. */
    if (F_ISSET(conn, WT_CONN_READONLY))
        return (0);

    /* Stop the server thread. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_STATISTICS);
    if (conn->stat_tid_set) {
        __wt_cond_signal(session, conn->stat_cond);
        WT_TRET(__wt_thread_join(session, &conn->stat_tid));
        conn->stat_tid_set = false;
    }
    __wt_cond_destroy(session, &conn->stat_cond);

    /* Log a set of statistics on shutdown if configured. */
    if (is_close)
        WT_TRET(__statlog_on_close(session));

    /* Discard all configuration information. */
    WT_TRET(__stat_config_discard(session));

    /* Close the server thread's session. */
    if (conn->stat_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->stat_session));
        conn->stat_session = NULL;
    }

    return (ret);
}
