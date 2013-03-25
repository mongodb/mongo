/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __GNUC__
/*
 * !!!
 * GCC with -Wformat-nonliteral complains about calls to strftime in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

/*
 * __wt_conn_stat_init --
 *	Initialize the per-connection statistics.
 */
void
__wt_conn_stat_init(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_UNUSED(flags);

	__wt_cache_stats_update(session);
}

/*
 * __wt_statlog_config --
 *	Parse and setup the statistics server options.
 */
static int
__statlog_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG objectconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int cnt;

	conn = S2C(session);

	/*
	 * The statistics logging configuration requires a wait time -- if it's
	 * not set, we're not running at all.
	 */
	WT_RET(__wt_config_gets(session, cfg, "statistics_log.wait", &cval));
	if (cval.val == 0) {
		*runp = 0;
		return (0);
	}
	conn->stat_usecs = (long)cval.val * 1000000;

	/* Statistics logging implies statistics. */
	conn->statistics = *runp = 1;

	WT_RET(__wt_config_gets(session, cfg, "statistics_log.clear", &cval));
	conn->stat_clear = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "statistics_log.sources", &cval));
	WT_RET(__wt_config_subinit(session, &objectconf, &cval));
	for (cnt = 0; (ret = __wt_config_next(&objectconf, &k, &v)) == 0; ++cnt)
		;
	WT_RET_NOTFOUND_OK(ret);
	if (cnt != 0) {
		WT_RET(
		    __wt_calloc_def(session, cnt * 2 + 1, &conn->stat_sources));
		WT_RET(__wt_config_subinit(session, &objectconf, &cval));
		for (cnt = 0;
		    (ret = __wt_config_next(&objectconf, &k, &v)) == 0;) {
			/*
			 * We close and re-open each statistics cursor each time
			 * we dump statistics (the object may or may not exist
			 * underneath at any point, and I don't want this code
			 * to break if/when the lifetime of an underlying object
			 * changes).  Create pairs of strings: the first is the
			 * object uri, written into the output, the second is
			 * the enhanced uri used to open the statistics cursor.
			 */
			WT_RET(__wt_strndup(session,
			    k.str, k.len, &conn->stat_sources[cnt]));
			++cnt;

			WT_RET(__wt_calloc_def(session,
			    strlen("statistics:") + k.len + 1,
			    &conn->stat_sources[cnt]));
			strcpy(conn->stat_sources[cnt], "statistics:");
			strncat(conn->stat_sources[cnt], k.str, k.len);
			++cnt;
		}
		WT_RET_NOTFOUND_OK(ret);
	}

	WT_RET(__wt_config_gets(session, cfg, "statistics_log.path", &cval));
	WT_RET(__wt_nfilename(session, cval.str, cval.len, &conn->stat_path));

	WT_RET(__wt_config_gets(
	    session, cfg, "statistics_log.timestamp", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->stat_stamp));

	return (0);
}

/*
 * __stat_server_dump --
 *	Dump a single set of statistics.
 */
static int
__stat_server_dump(WT_SESSION_IMPL *session,
    const char *name, const char *cursor_uri, const char *stamp, FILE *fp)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	uint64_t value;
	const char *config, *desc, *pdesc;

	wt_session = (WT_SESSION *)session;
	config = S2C(session)->stat_clear ?
	    "statistics_clear,statistics_fast" : "statistics_fast";

	/*
	 * If we don't find an underlying object, silently ignore it, the object
	 * may exist only intermittently.  User-level APIs return ENOENT instead
	 * of WT_NOTFOUND for missing files, check both, as well as for EBUSY if
	 * the handle is exclusively locked at the moment.
	 */
	ret = wt_session->open_cursor(
	    wt_session, cursor_uri, NULL, config, &cursor);
	if (ret == EBUSY || ret == ENOENT || ret == WT_NOTFOUND)
		return (0);
	WT_RET(ret);

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pdesc, &value)) == 0)
		WT_ERR_TEST((fprintf(fp,
		    "%s %" PRIu64 " %s %s\n",
		    stamp, value, name, desc) < 0), __wt_errno());
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_TRET(cursor->close(cursor));

	return (ret);
}

/*
 * __stat_server --
 *	The statistics server thread.
 */
static void *
__stat_server(void *arg)
{
	struct timespec ts;
	struct tm *tm, _tm;
	FILE *fp;
	WT_CONNECTION_IMPL *conn;
	WT_ITEM path, tmp;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	char **p;

	session = arg;
	conn = S2C(session);

	WT_CLEAR(path);
	WT_CLEAR(tmp);
	fp = NULL;

	/*
	 * We need a temporary place to build a path and an entry prefix.
	 * The length of the path plus 128 should be more than enough.
	 *
	 * We also need a place to store the current path, because that's
	 * how we know when to close/re-open the file.
	 */
	WT_ERR(__wt_buf_init(session, &path, strlen(conn->stat_path) + 128));
	WT_ERR(__wt_buf_init(session, &tmp, strlen(conn->stat_path) + 128));

	/*
	 * The statistics log server may be running before the database is
	 * created (it should run fine because we're looking at statistics
	 * structures that have already been allocated, but it doesn't make
	 * sense and we have the information we need to wait).  Wait for
	 * the wiredtiger_open call.
	 */
	while (!conn->connection_initialized)
		__wt_sleep(1, 0);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/*
		 * If statistics are turned off, wait until it's time to output
		 * statistics and check again.
		 */
		if (conn->statistics == 0) {
			WT_ERR(__wt_cond_wait(
			    session, conn->stat_cond, conn->stat_usecs));
			continue;
		}

		/* Get the current local time of day. */
		WT_ERR(__wt_epoch(session, &ts));
		tm = localtime_r(&ts.tv_sec, &_tm);

		/* Create the logging path name for this time of day. */
		if (strftime(tmp.mem, tmp.memsize, conn->stat_path, tm) == 0)
			WT_ERR_MSG(
			    session, ENOMEM, "strftime path conversion");

		/* If the path has changed, close/open the new log file. */
		if (fp == NULL || strcmp(tmp.mem, path.mem) != 0) {
			if (fp != NULL) {
				(void)fclose(fp);
				fp = NULL;
			}

			(void)strcpy(path.mem, tmp.mem);
			WT_ERR_TEST(
			    (fp = fopen(path.mem, "a")) == NULL, __wt_errno());
		}

		/* Create the entry prefix for this time of day. */
		if (strftime(tmp.mem, tmp.memsize, conn->stat_stamp, tm) == 0)
			WT_ERR_MSG(
			    session, ENOMEM, "strftime timestamp conversion");

		/* Dump the connection statistics. */
		WT_ERR(__stat_server_dump(
		    session, conn->home, "statistics:", tmp.mem, fp));

		/* Dump the object list statistics. */
		if ((p = conn->stat_sources) != NULL)
			for (; *p != NULL; p += 2)
				WT_ERR(__stat_server_dump(
				    session, p[0], p[1], tmp.mem, fp));

		/* Flush. */
		WT_ERR(fflush(fp) == 0 ? 0 : __wt_errno());

		/* Wait until the next event. */
		WT_ERR(
		    __wt_cond_wait(session, conn->stat_cond, conn->stat_usecs));
	}

	if (0) {
err:		__wt_err(session, ret, "statistics log server error");
	}
	if (fp != NULL)
		WT_TRET(fclose(fp) == 0 ? 0 : __wt_errno());
	__wt_buf_free(session, &path);
	__wt_buf_free(session, &tmp);
	return (NULL);
}

/*
 * __wt_statlog_create -
 *	Start the statistics server thread.
 */
int
__wt_statlog_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	int run;

	session = conn->default_session;

	/* Handle configuration. */
	WT_RET(__statlog_config(session, cfg, &run));

	/* If not configured, we're done. */
	if (!run)
		return (0);

	/* The statistics log server gets its own session. */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &conn->stat_session));
	conn->stat_session->name = "statlog-server";

	WT_RET(__wt_cond_alloc(
	    session, "statistics log server", 0, &conn->stat_cond));

	/*
	 * Start the thread.
	 *
	 * Statistics logging creates a thread per database, rather than using
	 * a single thread to do logging for all of the databases.   If we ever
	 * see lots of databases at a time, doing statistics logging, and we
	 * want to reduce the number of threads, there's no reason we have to
	 * have more than one thread, I just didn't feel like writing the code
	 * to figure out the scheduling.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->stat_tid, __stat_server, conn->stat_session));
	conn->stat_tid_set = 1;

	return (0);
}

/*
 * __wt_statlog_destroy -
 *	Destroy the statistics server thread.
 */
int
__wt_statlog_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;
	char **p;

	session = conn->default_session;

	if (conn->stat_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->stat_cond));
		WT_TRET(__wt_thread_join(session, conn->stat_tid));
		conn->stat_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->stat_cond));

	if ((p = conn->stat_sources) != NULL) {
		for (; *p != NULL; ++p)
			__wt_free(session, *p);
		__wt_free(session, conn->stat_sources);
	}
	__wt_free(session, conn->stat_path);
	__wt_free(session, conn->stat_stamp);

	/* Close the server thread's session, free its hazard array. */
	if (conn->stat_session != NULL) {
		wt_session = &conn->stat_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		__wt_free(session, conn->stat_session->hazard);
	}

	return (ret);
}
