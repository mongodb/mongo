/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
 * __statlog_config --
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
		WT_RET(__wt_calloc_def(session, cnt + 1, &conn->stat_sources));
		WT_RET(__wt_config_subinit(session, &objectconf, &cval));
		for (cnt = 0;
		    (ret = __wt_config_next(&objectconf, &k, &v)) == 0; ++cnt) {
			/*
			 * XXX
			 * Only allow "file:" and "lsm:" for now: "file:" works
			 * because it's been converted to data handles, "lsm:"
			 * works because we can easily walk the list of open LSM
			 * objects, even though it hasn't been converted.
			 */
			if (!WT_PREFIX_MATCH(k.str, "file:") &&
			    !WT_PREFIX_MATCH(k.str, "lsm:"))
				WT_RET_MSG(session, EINVAL,
				    "statistics_log sources configuration only "
				    "supports objects of type \"file\" or "
				    "\"lsm\"");
			WT_RET(__wt_strndup(session,
			    k.str, k.len, &conn->stat_sources[cnt]));
		}
		WT_RET_NOTFOUND_OK(ret);
	}

	WT_RET(__wt_config_gets(session, cfg, "statistics_log.path", &cval));
	WT_RET(__wt_nfilename(session, cval.str, cval.len, &conn->stat_path));

	WT_RET(__wt_config_gets(
	    session, cfg, "statistics_log.timestamp", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->stat_format));

	return (0);
}

/*
 * __statlog_dump --
 *	Dump out the connection statistics.
 */
static int
__statlog_dump(WT_SESSION_IMPL *session, const char *name, int conn_stats)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_SESSION *wt_session;
	uint64_t value;
	const char *config, *desc, *pdesc, *uri;

	conn = S2C(session);

	/* Build the statistics cursor URI. */
	if (conn_stats)
		uri = "statistics:";
	else {
		WT_RET(__wt_scr_alloc(
		    session, strlen("statistics:") + strlen(name) + 5, &tmp));
		(void)strcpy(tmp->mem, "statistics:");
		(void)strcat(tmp->mem, name);
		uri = tmp->mem;
	}

	/*
	 * Open the statistics cursor; immediately free any temporary buffer,
	 * it makes error handling easier.
	 */
	wt_session = (WT_SESSION *)session;
	config = S2C(session)->stat_clear ?
	    "statistics_clear,statistics_fast" : "statistics_fast";
	ret = wt_session->open_cursor(wt_session, uri, NULL, config, &cursor);
	__wt_scr_free(&tmp);

	/*
	 * If we don't find an underlying object, silently ignore it, the object
	 * may exist only intermittently.  User-level APIs return ENOENT instead
	 * of WT_NOTFOUND for missing files, check both, as well as for EBUSY if
	 * the handle is exclusively locked at the moment.
	 */
	if (ret == EBUSY || ret == ENOENT || ret == WT_NOTFOUND)
		return (0);
	WT_RET(ret);

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pdesc, &value)) == 0)
		WT_ERR_TEST((fprintf(conn->stat_fp,
		    "%s %" PRIu64 " %s %s\n",
		    conn->stat_stamp, value, name, desc) < 0), __wt_errno());
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_TRET(cursor->close(cursor));

	return (ret);
}

/*
 * __statlog_apply --
 *	Review a single open handle and dump statistics on demand.
 */
static int
__statlog_apply(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DATA_HANDLE *dhandle;
	char **p;

	WT_UNUSED(cfg);

	dhandle = session->dhandle;

	/* Check for a match on the set of sources. */
	for (p = S2C(session)->stat_sources; *p != NULL; ++p)
		if (WT_PREFIX_MATCH(dhandle->name, *p))
			return (__statlog_dump(session, dhandle->name, 0));
	return (0);
}

/*
 * __wt_statlog_lsm_apply --
 *	Review the list open LSM trees, and dump statistics on demand.
 *
 * XXX
 * This code should be removed when LSM objects are converted to data handles.
 */
static int
__wt_statlog_lsm_apply(WT_SESSION_IMPL *session)
{
#define	WT_LSM_TREE_LIST_SLOTS	100
	WT_LSM_TREE *lsm_tree, *list[WT_LSM_TREE_LIST_SLOTS];
	WT_DECL_RET;
	int cnt, locked;
	char **p;

	cnt = locked = 0;

	/*
	 * Walk the list of LSM trees, checking for a match on the set of
	 * sources.
	 *
	 * XXX
	 * We can't hold the schema lock for the traversal because the LSM
	 * statistics code acquires the tree lock, and the LSM cursor code
	 * acquires the tree lock and then acquires the schema lock, it's a
	 * classic deadlock.  This is temporary code so I'm not going to do
	 * anything fancy.
	 * It is OK to not keep holding the schema lock after populating
	 * the list of matching LSM trees, since the __wt_lsm_tree_get call
	 * will bump a reference count, so the tree won't go away.
	 */
	__wt_spin_lock(session, &S2C(session)->schema_lock);
	locked = 1;
	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q) {
		if (cnt == WT_LSM_TREE_LIST_SLOTS)
			break;
		for (p = S2C(session)->stat_sources; *p != NULL; ++p)
			if (WT_PREFIX_MATCH(lsm_tree->name, *p)) {
				WT_ERR(__wt_lsm_tree_get(
				    session, lsm_tree->name, 0, &list[cnt++]));
				break;
			}
	}
	__wt_spin_unlock(session, &S2C(session)->schema_lock);
	locked = 0;

	while (cnt > 0) {
		--cnt;
		WT_TRET(__statlog_dump(session, list[cnt]->name, 0));
		__wt_lsm_tree_release(session, list[cnt]);
	}

err:	if (locked)
		__wt_spin_lock(session, &S2C(session)->schema_lock);
	return (ret);
}

/*
 * __statlog_server --
 *	The statistics server thread.
 */
static void *
__statlog_server(void *arg)
{
	struct timespec ts;
	struct tm *tm, _tm;
	FILE *fp;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_ITEM path, tmp;
	WT_SESSION_IMPL *session;

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
	WT_ERR(__wt_buf_init(session, &path,
	    strlen(conn->stat_path) + ENTRY_SIZE));
	WT_ERR(__wt_buf_init(session, &tmp,
	    strlen(conn->stat_path) + ENTRY_SIZE));

	/*
	 * The statistics log server may be running before the database is
	 * created (it should run fine because we're looking at statistics
	 * structures that have already been allocated, but it doesn't make
	 * sense and we have the information we need to wait).  Wait for
	 * the wiredtiger_open call.
	 */
	while (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    !conn->connection_initialized)
		__wt_sleep(0, 1000);

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
		if (strftime(tmp.mem, tmp.memsize, conn->stat_format, tm) == 0)
			WT_ERR_MSG(
			    session, ENOMEM, "strftime timestamp conversion");

		/* Reference temporary values from the connection structure. */
		conn->stat_fp = fp;
		conn->stat_stamp = tmp.mem;

		/* Dump the connection statistics. */
		WT_ERR(__statlog_dump(session, conn->home, 1));

		/*
		 * Lock the schema and walk the list of open handles, dumping
		 * any that match the list of object sources.
		 */
		if (conn->stat_sources != NULL)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_conn_btree_apply(
			    session, __statlog_apply, NULL));
		WT_ERR(ret);

		/*
		 * Walk the list of open LSM trees, dumping any that match the
		 * the list of object sources.
		 *
		 * XXX
		 * This code should be removed when LSM objects are converted to
		 * data handles.
		 */
		if (conn->stat_sources != NULL)
			WT_ERR(__wt_statlog_lsm_apply(session));

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
 * __wt_statlog_create --
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
	    session, &conn->stat_tid, __statlog_server, conn->stat_session));
	conn->stat_tid_set = 1;

	return (0);
}

/*
 * __wt_statlog_destroy --
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
	__wt_free(session, conn->stat_format);

	/* Close the server thread's session, free its hazard array. */
	if (conn->stat_session != NULL) {
		wt_session = &conn->stat_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		__wt_free(session, conn->stat_session->hazard);
	}

	return (ret);
}
