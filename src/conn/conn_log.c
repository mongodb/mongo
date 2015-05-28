/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __logmgr_sync_cfg --
 *	Interpret the transaction_sync config.
 */
static int
__logmgr_sync_cfg(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_RET(
	    __wt_config_gets(session, cfg, "transaction_sync.enabled", &cval));
	if (cval.val)
		FLD_SET(conn->txn_logsync, WT_LOG_FLUSH);
	else
		FLD_CLR(conn->txn_logsync, WT_LOG_FLUSH);

	WT_RET(
	    __wt_config_gets(session, cfg, "transaction_sync.method", &cval));
	FLD_CLR(conn->txn_logsync, WT_LOG_DSYNC | WT_LOG_FSYNC);
	if (WT_STRING_MATCH("dsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_DSYNC);
	else if (WT_STRING_MATCH("fsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_FSYNC);
	return (0);
}

/*
 * __logmgr_config --
 *	Parse and setup the logging server options.
 */
static int
__logmgr_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * The logging configuration is off by default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	*runp = cval.val != 0;

	/*
	 * Setup a log path, compression and encryption even if logging is
	 * disabled in case we are going to print a log.
	 */
	conn->log_compressor = NULL;
	WT_RET(__wt_config_gets_none(session, cfg, "log.compressor", &cval));
	WT_RET(__wt_compressor_config(session, &cval, &conn->log_compressor));

	WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &conn->log_path));

	/* We are done if logging isn't enabled. */
	if (*runp == 0)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
	if (cval.val != 0)
		FLD_SET(conn->log_flags, WT_CONN_LOG_ARCHIVE);

	WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
	conn->log_file_max = (wt_off_t)cval.val;
	WT_STAT_FAST_CONN_SET(session, log_max_filesize, conn->log_file_max);

	WT_RET(__wt_config_gets(session, cfg, "log.prealloc", &cval));
	/*
	 * If pre-allocation is configured, set the initial number to one.
	 * We'll adapt as load dictates.
	 */
	if (cval.val != 0) {
		FLD_SET(conn->log_flags, WT_CONN_LOG_PREALLOC);
		conn->log_prealloc = 1;
	}
	WT_RET(__wt_config_gets_def(session, cfg, "log.recover", 0, &cval));
	if (cval.len != 0  && WT_STRING_MATCH("error", cval.str, cval.len))
		FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR);

	WT_RET(__logmgr_sync_cfg(session, cfg));
	return (0);
}

/*
 * __log_archive_once --
 *	Perform one iteration of log archiving.  Must be called with the
 *	log archive lock held.
 */
static int
__log_archive_once(WT_SESSION_IMPL *session, uint32_t backup_file)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t lognum, min_lognum;
	u_int i, locked, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	logcount = 0;
	logfiles = NULL;

	/*
	 * If we're coming from a backup cursor we want the smaller of
	 * the last full log file copied in backup or the checkpoint LSN.
	 * Otherwise we want the minimum of the last log file written to
	 * disk and the checkpoint LSN.
	 */
	if (backup_file != 0)
		min_lognum = WT_MIN(log->ckpt_lsn.file, backup_file);
	else
		min_lognum = WT_MIN(log->ckpt_lsn.file, log->sync_lsn.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG,
	    "log_archive: archive to log number %" PRIu32, min_lognum));

	/*
	 * Main archive code.  Get the list of all log files and
	 * remove any earlier than the minimum log number.
	 */
	WT_RET(__wt_dirlist(session, conn->log_path,
	    WT_LOG_FILENAME, WT_DIRLIST_INCLUDE, &logfiles, &logcount));

	/*
	 * We can only archive files if a hot backup is not in progress or
	 * if we are the backup.
	 */
	__wt_spin_lock(session, &conn->hot_backup_lock);
	locked = 1;
	if (conn->hot_backup == 0 || backup_file != 0) {
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(
			    session, logfiles[i], &lognum));
			if (lognum < min_lognum) {
				WT_ERR(__wt_log_remove(
				    session, WT_LOG_FILENAME, lognum));
			}
		}
	}
	__wt_spin_unlock(session, &conn->hot_backup_lock);
	locked = 0;
	__wt_log_files_free(session, logfiles, logcount);
	logfiles = NULL;
	logcount = 0;

	/*
	 * Indicate what is our new earliest LSN.  It is the start
	 * of the log file containing the last checkpoint.
	 */
	log->first_lsn.file = min_lognum;
	log->first_lsn.offset = 0;

	if (0)
err:		__wt_err(session, ret, "log archive server error");
	if (locked)
		__wt_spin_unlock(session, &conn->hot_backup_lock);
	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);
	return (ret);
}

/*
 * __log_prealloc_once --
 *	Perform one iteration of log pre-allocation.
 */
static int
__log_prealloc_once(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	u_int i, reccount;
	char **recfiles;

	conn = S2C(session);
	log = conn->log;
	reccount = 0;
	recfiles = NULL;

	/*
	 * Allocate up to the maximum number, accounting for any existing
	 * files that may not have been used yet.
	 */
	WT_ERR(__wt_dirlist(session, conn->log_path,
	    WT_LOG_PREPNAME, WT_DIRLIST_INCLUDE,
	    &recfiles, &reccount));
	__wt_log_files_free(session, recfiles, reccount);
	recfiles = NULL;
	/*
	 * Adjust the number of files to pre-allocate if we find that
	 * the critical path had to allocate them since we last ran.
	 */
	if (log->prep_missed > 0) {
		conn->log_prealloc += log->prep_missed;
		WT_ERR(__wt_verbose(session, WT_VERB_LOG,
		    "Now pre-allocating up to %" PRIu32,
		    conn->log_prealloc));
		log->prep_missed = 0;
	}
	WT_STAT_FAST_CONN_SET(session,
	    log_prealloc_max, conn->log_prealloc);
	/*
	 * Allocate up to the maximum number that we just computed and detected.
	 */
	for (i = reccount; i < (u_int)conn->log_prealloc; i++) {
		WT_ERR(__wt_log_allocfile(
		    session, ++log->prep_fileid, WT_LOG_PREPNAME, 1));
		WT_STAT_FAST_CONN_INCR(session, log_prealloc_files);
	}

	if (0)
err:		__wt_err(session, ret, "log pre-alloc server error");
	if (recfiles != NULL)
		__wt_log_files_free(session, recfiles, reccount);
	return (ret);
}

/*
 * __wt_log_truncate_files --
 *	Truncate log files via archive once. Requires that the server is not
 *	currently running.
 */
int
__wt_log_truncate_files(
    WT_SESSION_IMPL *session, WT_CURSOR *cursor, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t backup_file, locked;

	WT_UNUSED(cfg);
	conn = S2C(session);
	log = conn->log;
	if (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE))
		WT_RET_MSG(session, EINVAL,
		    "Attempt to archive manually while a server is running");

	backup_file = 0;
	if (cursor != NULL)
		backup_file = WT_CURSOR_BACKUP_ID(cursor);
	WT_ASSERT(session, backup_file <= log->alloc_lsn.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG,
	    "log_truncate_files: Archive once up to %" PRIu32,
	    backup_file));
	WT_RET(__wt_writelock(session, log->log_archive_lock));
	locked = 1;
	WT_ERR(__log_archive_once(session, backup_file));
	WT_ERR(__wt_writeunlock(session, log->log_archive_lock));
	locked = 0;
err:
	if (locked)
		WT_RET(__wt_writeunlock(session, log->log_archive_lock));
	return (ret);
}

/*
 * __log_file_server --
 *	The log file server thread.  This worker thread manages
 *	log file operations such as closing and syncing.
 */
static WT_THREAD_RET
__log_file_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *close_fh;
	WT_LOG *log;
	WT_LSN close_end_lsn, close_lsn, min_lsn;
	WT_SESSION_IMPL *session;
	int locked;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = 0;
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * If there is a log file to close, make sure any outstanding
		 * write operations have completed, then fsync and close it.
		 */
		if ((close_fh = log->log_close_fh) != NULL &&
		    (ret = __wt_log_extract_lognum(session, close_fh->name,
		    &close_lsn.file)) == 0 &&
		    close_lsn.file < log->write_lsn.file) {
			/*
			 * We've copied the file handle, clear out the one in
			 * log structure to allow it to be set again.
			 */
			log->log_close_fh = NULL;
			/*
			 * Set the close_end_lsn to the LSN immediately after
			 * ours.  That is, the beginning of the next log file.
			 * We need to know the LSN file number of our own close
			 * in case earlier calls are still in progress and the
			 * next one to move the sync_lsn into the next file for
			 * later syncs.
			 */
			close_lsn.offset = 0;
			close_end_lsn = close_lsn;
			close_end_lsn.file++;
			WT_ERR(__wt_fsync(session, close_fh));
			__wt_spin_lock(session, &log->log_sync_lock);
			locked = 1;
			WT_ERR(__wt_close(session, &close_fh));
			log->sync_lsn = close_end_lsn;
			WT_ERR(__wt_cond_signal(session, log->log_sync_cond));
			locked = 0;
			__wt_spin_unlock(session, &log->log_sync_lock);
		}
		/*
		 * If a later thread asked for a background sync, do it now.
		 */
		if (WT_LOG_CMP(&log->bg_sync_lsn, &log->sync_lsn) > 0) {
			/*
			 * Save the latest write LSN which is the minimum
			 * we will have written to disk.
			 */
			min_lsn = log->write_lsn;
			/*
			 * The sync LSN we asked for better be smaller than
			 * the current written LSN.
			 */
			WT_ASSERT(session,
			    WT_LOG_CMP(&log->bg_sync_lsn, &min_lsn) <= 0);
			WT_ERR(__wt_fsync(session, log->log_fh));
			__wt_spin_lock(session, &log->log_sync_lock);
			locked = 1;
			/*
			 * The sync LSN could have advanced while we were
			 * writing to disk.
			 */
			if (WT_LOG_CMP(&log->sync_lsn, &min_lsn) <= 0) {
				log->sync_lsn = min_lsn;
				WT_ERR(__wt_cond_signal(
				    session, log->log_sync_cond));
			}
			locked = 0;
			__wt_spin_unlock(session, &log->log_sync_lock);
		}
		/* Wait until the next event. */
		WT_ERR(__wt_cond_wait(
		    session, conn->log_file_cond, WT_MILLION));
	}

	if (0) {
err:		__wt_err(session, ret, "log close server error");
	}
	if (locked)
		__wt_spin_unlock(session, &log->log_sync_lock);
	return (WT_THREAD_RET_VALUE);
}

/*
 * Simple structure for sorting written slots.
 */
typedef struct {
	WT_LSN	lsn;
	uint32_t slot_index;
} WT_LOG_WRLSN_ENTRY;

/*
 * WT_WRLSN_ENTRY_CMP_LT --
 *	Return comparison of a written slot pair by LSN.
 */
#define	WT_WRLSN_ENTRY_CMP_LT(entry1, entry2)				\
	((entry1).lsn.file < (entry2).lsn.file ||			\
	((entry1).lsn.file == (entry2).lsn.file &&			\
	(entry1).lsn.offset < (entry2).lsn.offset))

/*
 * __log_wrlsn_server --
 *	The log wrlsn server thread.
 */
static WT_THREAD_RET
__log_wrlsn_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_WRLSN_ENTRY written[WT_SLOT_POOL];
	WT_LOGSLOT *slot;
	WT_SESSION_IMPL *session;
	size_t written_i;
	uint32_t i, save_i;
	int yield;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	yield = 0;
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * No need to use the log_slot_lock because the slot pool
		 * is statically allocated and any slot in the
		 * WT_LOG_SLOT_WRITTEN state is exclusively ours for now.
		 */
		i = 0;
		written_i = 0;
		/*
		 * Walk the array once saving any slots that are in the
		 * WT_LOG_SLOT_WRITTEN state.
		 */
		while (i < WT_SLOT_POOL) {
			save_i = i;
			slot = &log->slot_pool[i++];
			if (slot->slot_state != WT_LOG_SLOT_WRITTEN)
				continue;
			written[written_i].slot_index = save_i;
			written[written_i++].lsn = slot->slot_release_lsn;
		}
		/*
		 * If we found any written slots process them.  We sort them
		 * based on the release LSN, and then look for them in order.
		 */
		if (written_i > 0) {
			yield = 0;
			WT_INSERTION_SORT(written, written_i,
			    WT_LOG_WRLSN_ENTRY, WT_WRLSN_ENTRY_CMP_LT);

			/*
			 * We know the written array is sorted by LSN.  Go
			 * through them either advancing write_lsn or stop
			 * as soon as one is not in order.
			 */
			for (i = 0; i < written_i; i++) {
				if (WT_LOG_CMP(&log->write_lsn,
				    &written[i].lsn) != 0)
					break;
				/*
				 * If we get here we have a slot to process.
				 * Advance the LSN and process the slot.
				 */
				slot = &log->slot_pool[written[i].slot_index];
				WT_ASSERT(session, WT_LOG_CMP(&written[i].lsn,
				    &slot->slot_release_lsn) == 0);
				log->write_start_lsn = slot->slot_start_lsn;
				log->write_lsn = slot->slot_end_lsn;
				WT_ERR(__wt_cond_signal(session,
				    log->log_write_cond));
				WT_STAT_FAST_CONN_INCR(session, log_write_lsn);

				/*
				 * Signal the close thread if needed.
				 */
				if (F_ISSET(slot, WT_SLOT_CLOSEFH))
					WT_ERR(__wt_cond_signal(session,
					    conn->log_file_cond));
				WT_ERR(__wt_log_slot_free(session, slot));
			}
		}
		/*
		 * If we saw a later write, we always want to yield because
		 * we know something is in progress.
		 */
		if (yield++ < 1000)
			__wt_yield();
		else
			/* Wait until the next event. */
			WT_ERR(__wt_cond_wait(session,
			    conn->log_wrlsn_cond, 100000));
	}

	if (0)
err:		__wt_err(session, ret, "log wrlsn server error");
	return (WT_THREAD_RET_VALUE);
}

/*
 * __log_server --
 *	The log server thread.
 */
static WT_THREAD_RET
__log_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION_IMPL *session;
	u_int locked;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = 0;
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * Perform log pre-allocation.
		 */
		if (conn->log_prealloc > 0)
			WT_ERR(__log_prealloc_once(session));

		/*
		 * Perform the archive.
		 */
		if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE)) {
			if (__wt_try_writelock(
			    session, log->log_archive_lock) == 0) {
				locked = 1;
				WT_ERR(__log_archive_once(session, 0));
				WT_ERR(	__wt_writeunlock(
				    session, log->log_archive_lock));
				locked = 0;
			} else
				WT_ERR(__wt_verbose(session, WT_VERB_LOG,
				    "log_archive: Blocked due to open log "
				    "cursor holding archive lock"));
		}
		/* Wait until the next event. */
		WT_ERR(__wt_cond_wait(session, conn->log_cond, WT_MILLION));
	}

	if (0) {
err:		__wt_err(session, ret, "log server error");
	}
	if (locked)
		(void)__wt_writeunlock(session, log->log_archive_lock);
	return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_logmgr_create --
 *	Initialize the log subsystem (before running recovery).
 */
int
__wt_logmgr_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int run;

	conn = S2C(session);

	/* Handle configuration. */
	WT_RET(__logmgr_config(session, cfg, &run));

	/* If logging is not configured, we're done. */
	if (!run)
		return (0);

	FLD_SET(conn->log_flags, WT_CONN_LOG_ENABLED);
	/*
	 * Logging is on, allocate the WT_LOG structure and open the log file.
	 */
	WT_RET(__wt_calloc_one(session, &conn->log));
	log = conn->log;
	WT_RET(__wt_spin_init(session, &log->log_lock, "log"));
	WT_RET(__wt_spin_init(session, &log->log_slot_lock, "log slot"));
	WT_RET(__wt_spin_init(session, &log->log_sync_lock, "log sync"));
	WT_RET(__wt_rwlock_alloc(session,
	    &log->log_archive_lock, "log archive lock"));
	if (FLD_ISSET(conn->direct_io, WT_FILE_TYPE_LOG))
		log->allocsize =
		    WT_MAX((uint32_t)conn->buffer_alignment, WT_LOG_ALIGN);
	else
		log->allocsize = WT_LOG_ALIGN;
	WT_INIT_LSN(&log->alloc_lsn);
	WT_INIT_LSN(&log->ckpt_lsn);
	WT_INIT_LSN(&log->first_lsn);
	WT_INIT_LSN(&log->sync_lsn);
	/*
	 * We only use file numbers for directory sync, so this needs to
	 * initialized to zero.
	 */
	WT_ZERO_LSN(&log->sync_dir_lsn);
	WT_INIT_LSN(&log->trunc_lsn);
	WT_INIT_LSN(&log->write_lsn);
	WT_INIT_LSN(&log->write_start_lsn);
	log->fileid = 0;
	WT_RET(__wt_cond_alloc(session, "log sync", 0, &log->log_sync_cond));
	WT_RET(__wt_cond_alloc(session, "log write", 0, &log->log_write_cond));
	WT_RET(__wt_log_open(session));
	WT_RET(__wt_log_slot_init(session));

	return (0);
}

/*
 * __wt_logmgr_open --
 *	Start the log service threads.
 */
int
__wt_logmgr_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* If no log thread services are configured, we're done. */ 
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return (0);

	/*
	 * Start the log close thread.  It is not configurable.
	 * If logging is enabled, this thread runs.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "log-close-server", 0, 0, &conn->log_file_session));
	WT_RET(__wt_cond_alloc(conn->log_file_session,
	    "log close server", 0, &conn->log_file_cond));

	/*
	 * Start the log file close thread.
	 */
	WT_RET(__wt_thread_create(conn->log_file_session,
	    &conn->log_file_tid, __log_file_server, conn->log_file_session));
	conn->log_file_tid_set = 1;

	/*
	 * Start the log write LSN thread.  It is not configurable.
	 * If logging is enabled, this thread runs.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "log-wrlsn-server", 0, 0, &conn->log_wrlsn_session));
	WT_RET(__wt_cond_alloc(conn->log_wrlsn_session,
	    "log write lsn server", 0, &conn->log_wrlsn_cond));
	WT_RET(__wt_thread_create(conn->log_wrlsn_session,
	    &conn->log_wrlsn_tid, __log_wrlsn_server, conn->log_wrlsn_session));
	conn->log_wrlsn_tid_set = 1;

	/* If no log thread services are configured, we're done. */ 
	if (!FLD_ISSET(conn->log_flags,
	    (WT_CONN_LOG_ARCHIVE | WT_CONN_LOG_PREALLOC)))
		return (0);

	/*
	 * If a log server thread exists, the user may have reconfigured
	 * archiving or pre-allocation.  Signal the thread.  Otherwise the
	 * user wants archiving and/or allocation and we need to start up
	 * the thread.
	 */
	if (conn->log_session != NULL) {
		WT_ASSERT(session, conn->log_cond != NULL);
		WT_ASSERT(session, conn->log_tid_set != 0);
		WT_RET(__wt_cond_signal(session, conn->log_cond));
	} else {
		/* The log server gets its own session. */
		WT_RET(__wt_open_internal_session(
		    conn, "log-server", 0, 0, &conn->log_session));
		WT_RET(__wt_cond_alloc(conn->log_session,
		    "log server", 0, &conn->log_cond));

		/*
		 * Start the thread.
		 */
		WT_RET(__wt_thread_create(conn->log_session,
		    &conn->log_tid, __log_server, conn->log_session));
		conn->log_tid_set = 1;
	}

	return (0);
}

/*
 * __wt_logmgr_destroy --
 *	Destroy the log archiving server thread and logging subsystem.
 */
int
__wt_logmgr_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED)) {
		/*
		 * We always set up the log_path so printlog can work without
		 * recovery. Therefore, always free it, even if logging isn't
		 * on.
		 */
		__wt_free(session, conn->log_path);
		return (0);
	}
	if (conn->log_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_cond));
		WT_TRET(__wt_thread_join(session, conn->log_tid));
		conn->log_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->log_cond));
	if (conn->log_file_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_file_cond));
		WT_TRET(__wt_thread_join(session, conn->log_file_tid));
		conn->log_file_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->log_file_cond));
	if (conn->log_file_session != NULL) {
		wt_session = &conn->log_file_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_file_session = NULL;
	}
	if (conn->log_wrlsn_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_wrlsn_cond));
		WT_TRET(__wt_thread_join(session, conn->log_wrlsn_tid));
		conn->log_wrlsn_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->log_wrlsn_cond));
	if (conn->log_wrlsn_session != NULL) {
		wt_session = &conn->log_wrlsn_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_wrlsn_session = NULL;
	}

	WT_TRET(__wt_log_close(session));

	/* Close the server thread's session. */
	if (conn->log_session != NULL) {
		wt_session = &conn->log_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_session = NULL;
	}

	WT_TRET(__wt_log_slot_destroy(session));
	WT_TRET(__wt_cond_destroy(session, &conn->log->log_sync_cond));
	WT_TRET(__wt_cond_destroy(session, &conn->log->log_write_cond));
	WT_TRET(__wt_rwlock_destroy(session, &conn->log->log_archive_lock));
	__wt_spin_destroy(session, &conn->log->log_lock);
	__wt_spin_destroy(session, &conn->log->log_slot_lock);
	__wt_spin_destroy(session, &conn->log->log_sync_lock);
	__wt_free(session, conn->log_path);
	__wt_free(session, conn->log);
	return (ret);
}
