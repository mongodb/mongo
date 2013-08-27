/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __logmgr_config --
 *	Parse and setup the logging server options.
 */
static int
__logmgr_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	static const struct {
		const char *name;
		uint32_t flag;
	} *st, sync_types[] = {
		{ "dsync",	WT_LOG_DSYNC},
		{ "fsync",	WT_LOG_FSYNC},
		{ "none",	0 },
		{ NULL, 0 }
	};
	WT_CONFIG_ITEM cval, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * The logging configuration is off by default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	*runp = cval.val != 0;
	if (*runp == 0)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
	conn->archive = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
	conn->log_file_max = (off_t)cval.val;
	WT_CSTAT_SET(session, log_max_filesize, conn->log_file_max);

	WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->log_path));

	conn->txn_logsync = WT_LOG_DSYNC;
	WT_RET(__wt_config_gets(session, cfg, "transaction_log_sync", &cval));
	for (st = sync_types; st->name != NULL; st++) {
		ret = __wt_config_subgets(session, &cval, st->name, &sval);
		if (ret == 0) {
			if (sval.val)
				conn->txn_logsync = st->flag;
		} else if (ret != WT_NOTFOUND)
			goto err;
	}
	ret = 0;
err:
	return (ret);
}

/*
 * __log_archive_server --
 *	The log archiving server thread.
 */
static void *
__log_archive_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LSN lsn;
	WT_SESSION_IMPL *session;
	uint32_t lognum;
	u_int i, logcount;
	char **logfiles;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	logcount = 0;
	logfiles = NULL;

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/*
		 * If archiving is reconfigured and turned off, wait until
		 * it gets turned back on and check again.
		 */
		if (conn->archive == 0) {
			WT_ERR(__wt_cond_wait(session, conn->arch_cond, 0));
			continue;
		}

		lsn = log->ckpt_lsn;
		lsn.offset = 0;
		WT_VERBOSE_ERR(session, log,
		    "log_archive: ckpt LSN %d,%" PRIu64, lsn.file, lsn.offset);
		/*
		 * Main archive code.  Get the list of all log files and
		 * remove any earlier than the checkpoint LSN.
		 */
		WT_ERR(__wt_dirlist(session, conn->log_path,
		    WT_LOG_FILENAME, WT_DIRLIST_INCLUDE, &logfiles, &logcount));

		/*
		 * We can only archive files if a hot backup is not in progress.
		 */
		__wt_spin_lock(session, &conn->hot_backup_lock);
		for (i = 0; i < logcount; i++) {
			if (conn->hot_backup == 0) {
				WT_ERR(__wt_log_extract_lognum(
				    session, logfiles[i], &lognum));
				if (lognum < lsn.file)
					WT_ERR(
					    __wt_log_remove(session, lognum));
			}
		}
		__wt_spin_unlock(session, &conn->hot_backup_lock);
		__wt_log_files_free(session, logfiles, logcount);
		logfiles = NULL;
		logcount = 0;

		/*
		 * Indicate what is our new earliest LSN.  It is the start
		 * of the log file containing the last checkpoint.
		 */
		log->first_lsn = lsn;
		log->first_lsn.offset = 0;

		/* Wait until the next event. */
		WT_ERR(__wt_cond_wait(session, conn->arch_cond, 0));
	}

	if (0) {
err:		__wt_err(session, ret, "log archive server error");
	}
	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);
	return (NULL);
}

/*
 * __wt_logmgr_create --
 *	Start the log subsystem and archive server thread.
 */
int
__wt_logmgr_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	WT_LOG *log;
	int run;

	session = conn->default_session;

	/* Handle configuration. */
	WT_RET(__logmgr_config(session, cfg, &run));

	/* If logging is not configured, we're done. */
	if (!run)
		return (0);

	conn->logging = 1;
	/*
	 * Logging is on, allocate the WT_LOG structure and open the log file.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_LOG), &conn->log));
	log = conn->log;
	__wt_spin_init(session, &log->log_lock);
	__wt_spin_init(session, &log->log_slot_lock);
	if (FLD_ISSET(conn->direct_io, WT_FILE_TYPE_LOG))
		log->allocsize =
		    WT_MAX((uint32_t)conn->buffer_alignment, LOG_ALIGN);
	else
		log->allocsize = LOG_ALIGN;
	INIT_LSN(&log->alloc_lsn);
	INIT_LSN(&log->ckpt_lsn);
	INIT_LSN(&log->first_lsn);
	INIT_LSN(&log->sync_lsn);
	INIT_LSN(&log->write_lsn);
	log->fileid = 0;
	WT_RET(__wt_log_open(session));
	WT_RET(__wt_log_slot_init(session));

	/* If archiving is not configured, we're done. */ 
	if (!conn->archive)
		return (0);

	/*
	 * If an archive thread exists, the user may have reconfigured the
	 * archive thread.  Signal the thread.  Otherwise the user wants
	 * archiving and we need to start up the thread.
	 */
	if (conn->arch_session != NULL) {
		WT_ASSERT(session, conn->arch_cond != NULL);
		WT_ASSERT(session, conn->arch_tid_set != 0);
		WT_RET(__wt_cond_signal(session, conn->arch_cond));
	} else {
		/* The log archive server gets its own session. */
		WT_RET(__wt_open_session(
		    conn, 1, NULL, NULL, &conn->arch_session));
		conn->arch_session->name = "archive-server";
		WT_RET(__wt_cond_alloc(
		    session, "log archiving server", 0, &conn->arch_cond));

		/*
		 * Start the thread.
		 */
		WT_RET(__wt_thread_create(session,
		    &conn->arch_tid, __log_archive_server, conn->arch_session));
		conn->arch_tid_set = 1;
	}

	return (0);
}

/*
 * __wt_logmgr_destroy --
 *	Destroy the log archiving server thread and logging subsystem.
 */
int
__wt_logmgr_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	if (!conn->logging)
		return (0);
	if (conn->arch_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->arch_cond));
		WT_TRET(__wt_thread_join(session, conn->arch_tid));
		conn->arch_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->arch_cond));
	if ((log = conn->log) != NULL)
		WT_TRET(__wt_log_close(session));

	__wt_free(session, conn->log_path);

	/* Close the server thread's session, free its hazard array. */
	if (conn->arch_session != NULL) {
		wt_session = &conn->arch_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		__wt_free(session, conn->arch_session->hazard);
		conn->arch_session = NULL;
	}
	__wt_spin_destroy(session, &log->log_lock);
	__wt_spin_destroy(session, &log->log_slot_lock);
	__wt_free(session, conn->log);

	return (ret);
}
