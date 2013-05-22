/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __logger_config --
 *	Parse and setup the logging server options.
 */
static int
__logger_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

fprintf(stderr, "logger_config: cfg %s\n", *cfg);
	/*
	 * The logging configuration is off by default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
fprintf(stderr, "logger_config: enabled %d\n", cval.val);
	*runp = cval.val != 0;
	if (*runp == 0)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
fprintf(stderr, "logger_config: archive %d\n", cval.val);
	conn->archive = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
fprintf(stderr, "logger_config: file_max %"PRId64"\n", (uint64_t)cval.val);
	conn->log_file_max = (uint64_t)cval.val;
	WT_CSTAT_SET(session, log_max_filesize, conn->log_file_max);

	WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
	WT_RET(__wt_nfilename(session, cval.str, cval.len, &conn->log_path));
fprintf(stderr, "logger_config: log_path %s\n", conn->log_path);

	return (0);
}

/*
 * __log_archive_server --
 *	The log archiving server thread.
 */
static void *
__log_archive_server(void *arg)
{
	FILE *fp;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_ITEM path, tmp;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);

	WT_CLEAR(path);
	WT_CLEAR(tmp);

	/*
	 * We need a temporary place to build a path and an entry prefix.
	 * The length of the path plus 128 should be more than enough.
	 *
	 * We also need a place to store the current path, because that's
	 * how we know when to close/re-open the file.
	 */
	WT_ERR(__wt_buf_init(session, &path,
	    strlen(conn->log_path) + ENTRY_SIZE));
	WT_ERR(__wt_buf_init(session, &tmp,
	    strlen(conn->log_path) + ENTRY_SIZE));

	/*
	 * The log archive server may be running before the database is
	 * created.  Wait for the wiredtiger_open call.
	 */
	while (!conn->connection_initialized)
		__wt_sleep(1, 0);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
#if 0
		/*
		 * If archiving is turned off, wait until it's time to archive
		 * and check again.
		 */
		if (conn->archive == 0) {
			WT_ERR(__wt_cond_wait(
			    session, conn->arch_cond, conn->arch_usecs));
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
	XXX

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
		 * Lock the schema and walk the list of open LSM trees, dumping
		 * any that match the list of object sources.
		 *
		 * XXX
		 * This code should be removed when LSM objects are converted to
		 * data handles.
		 */
		if (conn->stat_sources != NULL)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_statlog_lsm_apply(session));
		WT_ERR(ret);
	XXX

		/* Flush. */
		WT_ERR(fflush(fp) == 0 ? 0 : __wt_errno());

#endif
		/* Wait until the next event. */
		WT_ERR(
		    __wt_cond_wait(session, conn->arch_cond, 0));
	}

	if (0) {
err:		__wt_err(session, ret, "log archive server error");
	}
	if (fp != NULL)
		WT_TRET(fclose(fp) == 0 ? 0 : __wt_errno());
	__wt_buf_free(session, &path);
	__wt_buf_free(session, &tmp);
	return (NULL);
}

/*
 * __wt_logger_create --
 *	Start the log subsystem and archive server thread.
 */
int
__wt_logger_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	WT_LOG *log;
	int i, run;

	session = conn->default_session;

	/* Handle configuration. */
fprintf(stderr, "logger_create: called\n");
	WT_RET(__logger_config(session, cfg, &run));

fprintf(stderr, "logger_create: logger_config returned %d\n", run);
	WT_VERBOSE_RET(session, log, "logger_create: run %d", run);
	/* If logging is not configured, we're done. */
	if (!run)
		return (0);

	conn->logging = 1;
	/*
	 * Logging is on, allocate the WT_LOG structure and open the log file.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_LOG), &conn->log));
	log = conn->log;
fprintf(stderr, "logger_create: open the log, size 0x%d\n",conn->log_file_max);
	WT_VERBOSE_RET(session, log, "logger_create: open the log");
	__wt_spin_init(session, &log->log_lock);
	__wt_spin_init(session, &log->log_slot_lock);
	if (FLD_ISSET(conn->direct_io, WT_FILE_TYPE_LOG))
		log->allocsize = LOG_ALIGN_DIRECTIO;
	else
		log->allocsize = LOG_ALIGN;
	WT_RET(__wt_cond_alloc(
	    session, "log slot sync", 0, &log->slot_sync_cond));
#if 0
	for (i = 0; i < SLOT_POOL; i++)
		WT_RET(__wt_cond_alloc(
		    session, "log slot", 0, &log->slot_pool[i].slot_cond));
#endif
	/*
	 * Initialize fileid to 0 so that newfile moves to log file 1.
	 */
	INIT_LSN(&log->alloc_lsn);
	INIT_LSN(&log->ckpt_lsn);
	INIT_LSN(&log->sync_lsn);
	INIT_LSN(&log->write_lsn);
	log->fileid = 0;
	WT_RET(__wt_log_newfile(session, 1));
	/* xxx - need to initialize the LSNs from log_fh file_size */
	WT_RET(__wt_log_slot_init(session));

	/* If archiving is not configured, we're done. */
	if (!conn->archive)
		return (0);

	/* The log archive server gets its own session. */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &conn->arch_session));
	conn->arch_session->name = "archive-server";

	WT_RET(__wt_cond_alloc(
	    session, "log archiving server", 0, &conn->arch_cond));

	/*
	 * Start the thread.
	 */
	WT_RET(__wt_thread_create(session,
	    &conn->arch_tid, __log_archive_server, conn->arch_session));
	conn->arch_tid_set = 1;

	return (0);
}

/*
 * __wt_logger_destroy --
 *	Destroy the log archiving server thread and logging subsystem.
 */
int
__wt_logger_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;
	int i;

	session = conn->default_session;

	if (conn->log != NULL)
		WT_TRET(__wt_log_close(session));
	if (conn->arch_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->arch_cond));
		WT_TRET(__wt_thread_join(session, conn->arch_tid));
		conn->arch_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->arch_cond));

	__wt_free(session, conn->log_path);
	log = conn->log;

	/* Close the server thread's session, free its hazard array. */
	if (conn->arch_session != NULL) {
		wt_session = &conn->arch_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		__wt_free(session, conn->arch_session->hazard);
	}
#if 0
	for (i = 0; i < SLOT_POOL; i++)
		WT_TRET(__wt_cond_destroy(
		    session, &log->slot_pool[i].slot_cond));
#endif
	__wt_spin_destroy(session, &log->log_lock);
	__wt_spin_destroy(session, &log->log_slot_lock);
	__wt_free(session, conn->log);

	return (ret);
}
