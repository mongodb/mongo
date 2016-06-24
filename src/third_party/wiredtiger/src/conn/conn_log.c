/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
		FLD_SET(conn->txn_logsync, WT_LOG_SYNC_ENABLED);
	else
		FLD_CLR(conn->txn_logsync, WT_LOG_SYNC_ENABLED);

	WT_RET(
	    __wt_config_gets(session, cfg, "transaction_sync.method", &cval));
	FLD_CLR(conn->txn_logsync, WT_LOG_DSYNC | WT_LOG_FLUSH | WT_LOG_FSYNC);
	if (WT_STRING_MATCH("dsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_DSYNC | WT_LOG_FLUSH);
	else if (WT_STRING_MATCH("fsync", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_FSYNC);
	else if (WT_STRING_MATCH("none", cval.str, cval.len))
		FLD_SET(conn->txn_logsync, WT_LOG_FLUSH);
	return (0);
}

/*
 * __logmgr_config --
 *	Parse and setup the logging server options.
 */
static int
__logmgr_config(
    WT_SESSION_IMPL *session, const char **cfg, bool *runp, bool reconfig)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	bool enabled;

	conn = S2C(session);

	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	enabled = cval.val != 0;

	/*
	 * If we're reconfiguring, enabled must match the already
	 * existing setting.
	 *
	 * If it is off and the user it turning it on, or it is on
	 * and the user is turning it off, return an error.
	 */
	if (reconfig &&
	    ((enabled && !FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED)) ||
	    (!enabled && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))))
		return (EINVAL);

	/* Logging is incompatible with in-memory */
	if (enabled) {
		WT_RET(__wt_config_gets(session, cfg, "in_memory", &cval));
		if (cval.val != 0)
			WT_RET_MSG(session, EINVAL,
			    "In memory configuration incompatible with "
			    "log=(enabled=true)");
	}

	*runp = enabled;

	/*
	 * Setup a log path and compression even if logging is disabled in case
	 * we are going to print a log.  Only do this on creation.  Once a
	 * compressor or log path are set they cannot be changed.
	 */
	if (!reconfig) {
		conn->log_compressor = NULL;
		WT_RET(__wt_config_gets_none(
		    session, cfg, "log.compressor", &cval));
		WT_RET(__wt_compressor_config(
		    session, &cval, &conn->log_compressor));

		WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
		WT_RET(__wt_strndup(
		    session, cval.str, cval.len, &conn->log_path));
	}
	/* We are done if logging isn't enabled. */
	if (!*runp)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "log.archive", &cval));
	if (cval.val != 0)
		FLD_SET(conn->log_flags, WT_CONN_LOG_ARCHIVE);

	if (!reconfig) {
		/*
		 * Ignore if the user tries to change the file size.  The
		 * amount of memory allocated to the log slots may be based
		 * on the log file size at creation and we don't want to
		 * re-allocate that memory while running.
		 */
		WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
		conn->log_file_max = (wt_off_t)cval.val;
		WT_STAT_FAST_CONN_SET(session,
		    log_max_filesize, conn->log_file_max);
	}

	/*
	 * If pre-allocation is configured, set the initial number to a few.
	 * We'll adapt as load dictates.
	 */
	WT_RET(__wt_config_gets(session, cfg, "log.prealloc", &cval));
	if (cval.val != 0)
		conn->log_prealloc = 1;

	/*
	 * Note that it is meaningless to reconfigure this value during
	 * runtime.  It only matters on create before recovery runs.
	 */
	WT_RET(__wt_config_gets_def(session, cfg, "log.recover", 0, &cval));
	if (cval.len != 0  && WT_STRING_MATCH("error", cval.str, cval.len))
		FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR);

	WT_RET(__wt_config_gets(session, cfg, "log.zero_fill", &cval));
	if (cval.val != 0) {
		if (F_ISSET(conn, WT_CONN_READONLY))
			WT_RET_MSG(session, EINVAL,
			    "Read-only configuration incompatible with "
			    "zero-filling log files");
		FLD_SET(conn->log_flags, WT_CONN_LOG_ZERO_FILL);
	}

	WT_RET(__logmgr_sync_cfg(session, cfg));
	if (conn->log_cond != NULL)
		WT_RET(__wt_cond_auto_signal(session, conn->log_cond));
	return (0);
}

/*
 * __wt_logmgr_reconfig --
 *	Reconfigure logging.
 */
int
__wt_logmgr_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
	bool dummy;

	return (__logmgr_config(session, cfg, &dummy, true));
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
	u_int i, logcount;
	bool locked;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	logcount = 0;
	locked = false;
	logfiles = NULL;

	/*
	 * If we're coming from a backup cursor we want the smaller of
	 * the last full log file copied in backup or the checkpoint LSN.
	 * Otherwise we want the minimum of the last log file written to
	 * disk and the checkpoint LSN.
	 */
	if (backup_file != 0)
		min_lognum = WT_MIN(log->ckpt_lsn.l.file, backup_file);
	else
		min_lognum = WT_MIN(
		    log->ckpt_lsn.l.file, log->sync_lsn.l.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG,
	    "log_archive: archive to log number %" PRIu32, min_lognum));

	/*
	 * Main archive code.  Get the list of all log files and
	 * remove any earlier than the minimum log number.
	 */
	WT_ERR(__wt_fs_directory_list(
	    session, conn->log_path, WT_LOG_FILENAME, &logfiles, &logcount));

	/*
	 * We can only archive files if a hot backup is not in progress or
	 * if we are the backup.
	 */
	WT_ERR(__wt_readlock(session, conn->hot_backup_lock));
	locked = true;
	if (!conn->hot_backup || backup_file != 0) {
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(
			    session, logfiles[i], &lognum));
			if (lognum < min_lognum)
				WT_ERR(__wt_log_remove(
				    session, WT_LOG_FILENAME, lognum));
		}
	}
	WT_ERR(__wt_readunlock(session, conn->hot_backup_lock));
	locked = false;

	/*
	 * Indicate what is our new earliest LSN.  It is the start
	 * of the log file containing the last checkpoint.
	 */
	WT_SET_LSN(&log->first_lsn, min_lognum, 0);

	if (0)
err:		__wt_err(session, ret, "log archive server error");
	if (locked)
		WT_TRET(__wt_readunlock(session, conn->hot_backup_lock));
	WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
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
	WT_ERR(__wt_fs_directory_list(
	    session, conn->log_path, WT_LOG_PREPNAME, &recfiles, &reccount));

	/*
	 * Adjust the number of files to pre-allocate if we find that
	 * the critical path had to allocate them since we last ran.
	 */
	if (log->prep_missed > 0) {
		conn->log_prealloc += log->prep_missed;
		WT_ERR(__wt_verbose(session, WT_VERB_LOG,
		    "Missed %" PRIu32 ". Now pre-allocating up to %" PRIu32,
		    log->prep_missed, conn->log_prealloc));
	}
	WT_STAT_FAST_CONN_SET(session, log_prealloc_max, conn->log_prealloc);
	/*
	 * Allocate up to the maximum number that we just computed and detected.
	 */
	for (i = reccount; i < (u_int)conn->log_prealloc; i++) {
		WT_ERR(__wt_log_allocfile(
		    session, ++log->prep_fileid, WT_LOG_PREPNAME));
		WT_STAT_FAST_CONN_INCR(session, log_prealloc_files);
	}
	/*
	 * Reset the missed count now.  If we missed during pre-allocating
	 * the log files, it means the allocation is not keeping up, not that
	 * we didn't allocate enough.  So we don't just want to keep adding
	 * in more.
	 */
	log->prep_missed = 0;

	if (0)
err:		__wt_err(session, ret, "log pre-alloc server error");
	WT_TRET(__wt_fs_directory_list_free(session, &recfiles, reccount));
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
	uint32_t backup_file;
	bool locked;

	WT_UNUSED(cfg);
	conn = S2C(session);
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return (0);
	if (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE))
		WT_RET_MSG(session, EINVAL,
		    "Attempt to archive manually while a server is running");

	log = conn->log;

	backup_file = 0;
	if (cursor != NULL)
		backup_file = WT_CURSOR_BACKUP_ID(cursor);
	WT_ASSERT(session, backup_file <= log->alloc_lsn.l.file);
	WT_RET(__wt_verbose(session, WT_VERB_LOG,
	    "log_truncate_files: Archive once up to %" PRIu32,
	    backup_file));

	WT_RET(__wt_writelock(session, log->log_archive_lock));
	locked = true;
	WT_ERR(__log_archive_once(session, backup_file));
	WT_ERR(__wt_writeunlock(session, log->log_archive_lock));
	locked = false;
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
	WT_LSN close_end_lsn, min_lsn;
	WT_SESSION_IMPL *session;
	uint32_t filenum;
	bool locked;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = false;
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * If there is a log file to close, make sure any outstanding
		 * write operations have completed, then fsync and close it.
		 */
		if ((close_fh = log->log_close_fh) != NULL) {
			WT_ERR(__wt_log_extract_lognum(session, close_fh->name,
			    &filenum));
			/*
			 * We update the close file handle before updating the
			 * close LSN when changing files.  It is possible we
			 * could see mismatched settings.  If we do, yield
			 * until it is set.  This should rarely happen.
			 */
			while (log->log_close_lsn.l.file < filenum)
				__wt_yield();

			if (__wt_log_cmp(
			    &log->write_lsn, &log->log_close_lsn) >= 0) {
				/*
				 * We've copied the file handle, clear out the
				 * one in the log structure to allow it to be
				 * set again.  Copy the LSN before clearing
				 * the file handle.
				 * Use a barrier to make sure the compiler does
				 * not reorder the following two statements.
				 */
				close_end_lsn = log->log_close_lsn;
				WT_FULL_BARRIER();
				log->log_close_fh = NULL;
				/*
				 * Set the close_end_lsn to the LSN immediately
				 * after ours.  That is, the beginning of the
				 * next log file.   We need to know the LSN
				 * file number of our own close in case earlier
				 * calls are still in progress and the next one
				 * to move the sync_lsn into the next file for
				 * later syncs.
				 */
				WT_ERR(__wt_fsync(session, close_fh, true));
				/*
				 * We want to make sure the file size reflects
				 * actual data and has minimal pre-allocated
				 * zeroed space.
				 */
				WT_ERR(__wt_ftruncate(session,
				    close_fh, close_end_lsn.l.offset));
				WT_SET_LSN(&close_end_lsn,
				    close_end_lsn.l.file + 1, 0);
				__wt_spin_lock(session, &log->log_sync_lock);
				locked = true;
				WT_ERR(__wt_close(session, &close_fh));
				WT_ASSERT(session, __wt_log_cmp(
				    &close_end_lsn, &log->sync_lsn) >= 0);
				log->sync_lsn = close_end_lsn;
				WT_ERR(__wt_cond_signal(
				    session, log->log_sync_cond));
				locked = false;
				__wt_spin_unlock(session, &log->log_sync_lock);
			}
		}
		/*
		 * If a later thread asked for a background sync, do it now.
		 */
		if (__wt_log_cmp(&log->bg_sync_lsn, &log->sync_lsn) > 0) {
			/*
			 * Save the latest write LSN which is the minimum
			 * we will have written to disk.
			 */
			min_lsn = log->write_lsn;
			/*
			 * We have to wait until the LSN we asked for is
			 * written.  If it isn't signal the wrlsn thread
			 * to get it written.
			 *
			 * We also have to wait for the written LSN and the
			 * sync LSN to be in the same file so that we know we
			 * have synchronized all earlier log files.
			 */
			if (__wt_log_cmp(&log->bg_sync_lsn, &min_lsn) <= 0) {
				/*
				 * If the sync file is behind either the one
				 * wanted for a background sync or the write LSN
				 * has moved to another file continue to let
				 * this worker thread process that older file
				 * immediately.
				 */
				if ((log->sync_lsn.l.file <
				    log->bg_sync_lsn.l.file) ||
				    (log->sync_lsn.l.file < min_lsn.l.file))
					continue;
				WT_ERR(__wt_fsync(session, log->log_fh, true));
				__wt_spin_lock(session, &log->log_sync_lock);
				locked = true;
				/*
				 * The sync LSN could have advanced while we
				 * were writing to disk.
				 */
				if (__wt_log_cmp(
				    &log->sync_lsn, &min_lsn) <= 0) {
					WT_ASSERT(session,
					    min_lsn.l.file ==
					    log->sync_lsn.l.file);
					log->sync_lsn = min_lsn;
					WT_ERR(__wt_cond_signal(
					    session, log->log_sync_cond));
				}
				locked = false;
				__wt_spin_unlock(session, &log->log_sync_lock);
			} else {
				WT_ERR(__wt_cond_auto_signal(
				    session, conn->log_wrlsn_cond));
				/*
				 * We do not want to wait potentially a second
				 * to process this.  Yield to give the wrlsn
				 * thread a chance to run and try again in
				 * this case.
				 */
				__wt_yield();
				continue;
			}
		}
		/* Wait until the next event. */
		WT_ERR(__wt_cond_wait(
		    session, conn->log_file_cond, WT_MILLION / 10));
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
	((entry1).lsn.l.file < (entry2).lsn.l.file ||		\
	((entry1).lsn.l.file == (entry2).lsn.l.file &&		\
	(entry1).lsn.l.offset < (entry2).lsn.l.offset))

/*
 * __wt_log_wrlsn --
 *	Process written log slots and attempt to coalesce them if the LSNs
 *	are contiguous.  The purpose of this function is to advance the
 *	write_lsn in LSN order after the buffer is written to the log file.
 */
int
__wt_log_wrlsn(WT_SESSION_IMPL *session, int *yield)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_WRLSN_ENTRY written[WT_SLOT_POOL];
	WT_LOGSLOT *coalescing, *slot;
	WT_LSN save_lsn;
	size_t written_i;
	uint32_t i, save_i;

	conn = S2C(session);
	log = conn->log;
	__wt_spin_lock(session, &log->log_writelsn_lock);
restart:
	coalescing = NULL;
	WT_INIT_LSN(&save_lsn);
	written_i = 0;
	i = 0;

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
		if (yield != NULL)
			*yield = 0;
		WT_INSERTION_SORT(written, written_i,
		    WT_LOG_WRLSN_ENTRY, WT_WRLSN_ENTRY_CMP_LT);
		/*
		 * We know the written array is sorted by LSN.  Go
		 * through them either advancing write_lsn or coalesce
		 * contiguous ranges of written slots.
		 */
		for (i = 0; i < written_i; i++) {
			slot = &log->slot_pool[written[i].slot_index];
			/*
			 * The log server thread pushes out slots periodically.
			 * Sometimes they are empty slots.  If we find an
			 * empty slot, where empty means the start and end LSN
			 * are the same, free it and continue.
			 */
			if (__wt_log_cmp(&slot->slot_start_lsn,
			    &slot->slot_release_lsn) == 0 &&
			    __wt_log_cmp(&slot->slot_start_lsn,
			    &slot->slot_end_lsn) == 0) {
				__wt_log_slot_free(session, slot);
				continue;
			}
			if (coalescing != NULL) {
				/*
				 * If the write_lsn changed, we may be able to
				 * process slots.  Try again.
				 */
				if (__wt_log_cmp(
				    &log->write_lsn, &save_lsn) != 0)
					goto restart;
				if (__wt_log_cmp(&coalescing->slot_end_lsn,
				    &written[i].lsn) != 0) {
					coalescing = slot;
					continue;
				}
				/*
				 * If we get here we have a slot to coalesce
				 * and free.
				 */
				coalescing->slot_last_offset =
				    slot->slot_last_offset;
				coalescing->slot_end_lsn = slot->slot_end_lsn;
				WT_STAT_FAST_CONN_INCR(
				    session, log_slot_coalesced);
				/*
				 * Copy the flag for later closing.
				 */
				if (F_ISSET(slot, WT_SLOT_CLOSEFH))
					F_SET(coalescing, WT_SLOT_CLOSEFH);
			} else {
				/*
				 * If this written slot is not the next LSN,
				 * try to start coalescing with later slots.
				 * A synchronous write may update write_lsn
				 * so save the last one we saw to check when
				 * coalescing slots.
				 */
				save_lsn = log->write_lsn;
				if (__wt_log_cmp(
				    &log->write_lsn, &written[i].lsn) != 0) {
					coalescing = slot;
					continue;
				}
				/*
				 * If we get here we have a slot to process.
				 * Advance the LSN and process the slot.
				 */
				WT_ASSERT(session, __wt_log_cmp(&written[i].lsn,
				    &slot->slot_release_lsn) == 0);
				/*
				 * We need to maintain the starting offset of
				 * a log record so that the checkpoint LSN
				 * refers to the beginning of a real record.
				 * The last offset in a slot is kept so that
				 * the checkpoint LSN is close to the end of
				 * the record.
				 */
				if (slot->slot_start_lsn.l.offset !=
				    slot->slot_last_offset)
					slot->slot_start_lsn.l.offset =
					    (uint32_t)slot->slot_last_offset;
				log->write_start_lsn = slot->slot_start_lsn;
				log->write_lsn = slot->slot_end_lsn;
				WT_ERR(__wt_cond_signal(
				    session, log->log_write_cond));
				WT_STAT_FAST_CONN_INCR(session, log_write_lsn);
				/*
				 * Signal the close thread if needed.
				 */
				if (F_ISSET(slot, WT_SLOT_CLOSEFH))
					WT_ERR(__wt_cond_signal(
					    session, conn->log_file_cond));
			}
			__wt_log_slot_free(session, slot);
		}
	}
err:	__wt_spin_unlock(session, &log->log_writelsn_lock);
	return (ret);
}

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
	WT_LSN prev;
	WT_SESSION_IMPL *session;
	int yield;
	bool did_work;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	yield = 0;
	WT_INIT_LSN(&prev);
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * Write out any log record buffers if anything was done
		 * since last time.  Only call the function to walk the
		 * slots if the system is not idle.  On an idle system
		 * the alloc_lsn will not advance and the written lsn will
		 * match the alloc_lsn.
		 */
		if (__wt_log_cmp(&prev, &log->alloc_lsn) != 0 ||
		    __wt_log_cmp(&log->write_lsn, &log->alloc_lsn) != 0)
			WT_ERR(__wt_log_wrlsn(session, &yield));
		else
			WT_STAT_FAST_CONN_INCR(session, log_write_lsn_skip);
		prev = log->alloc_lsn;
		did_work = yield == 0;

		/*
		 * If __wt_log_wrlsn did work we want to yield instead of sleep.
		 */
		if (yield++ < WT_THOUSAND)
			__wt_yield();
		else
			/*
			 * Send in false because if we did any work we would
			 * not be on this path.
			 */
			WT_ERR(__wt_cond_auto_wait(
			    session, conn->log_wrlsn_cond, did_work));
	}
	/*
	 * On close we need to do this one more time because there could
	 * be straggling log writes that need to be written.
	 */
	WT_ERR(__wt_log_force_write(session, 1, NULL));
	WT_ERR(__wt_log_wrlsn(session, NULL));
	if (0) {
err:		__wt_err(session, ret, "log wrlsn server error");
	}
	return (WT_THREAD_RET_VALUE);
}

/*
 * __log_server --
 *	The log server thread.
 */
static WT_THREAD_RET
__log_server(void *arg)
{
	struct timespec start, now;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION_IMPL *session;
	uint64_t timediff;
	bool did_work, locked, signalled;

	session = arg;
	conn = S2C(session);
	log = conn->log;
	locked = signalled = false;

	/*
	 * Set this to the number of milliseconds we want to run archive and
	 * pre-allocation.  Start it so that we run on the first time through.
	 */
	timediff = WT_THOUSAND;

	/*
	 * The log server thread does a variety of work.  It forces out any
	 * buffered log writes.  It pre-allocates log files and it performs
	 * log archiving.  The reason the wrlsn thread does not force out
	 * the buffered writes is because we want to process and move the
	 * write_lsn forward as quickly as possible.  The same reason applies
	 * to why the log file server thread does not force out the writes.
	 * That thread does fsync calls which can take a long time and we
	 * don't want log records sitting in the buffer over the time it
	 * takes to sync out an earlier file.
	 */
	did_work = true;
	while (F_ISSET(conn, WT_CONN_LOG_SERVER_RUN)) {
		/*
		 * Slots depend on future activity.  Force out buffered
		 * writes in case we are idle.  This cannot be part of the
		 * wrlsn thread because of interaction advancing the write_lsn
		 * and a buffer may need to wait for the write_lsn to advance
		 * in the case of a synchronous buffer.  We end up with a hang.
		 */
		WT_ERR_BUSY_OK(__wt_log_force_write(session, 0, &did_work));

		/*
		 * We don't want to archive or pre-allocate files as often as
		 * we want to force out log buffers.  Only do it once per second
		 * or if the condition was signalled.
		 */
		if (timediff >= WT_THOUSAND || signalled) {

			/*
			 * Perform log pre-allocation.
			 */
			if (conn->log_prealloc > 0) {
				/*
				 * Log file pre-allocation is disabled when a
				 * hot backup cursor is open because we have
				 * agreed not to rename or remove any files in
				 * the database directory.
				 */
				WT_ERR(__wt_readlock(
				    session, conn->hot_backup_lock));
				locked = true;
				if (!conn->hot_backup)
					WT_ERR(__log_prealloc_once(session));
				WT_ERR(__wt_readunlock(
				    session, conn->hot_backup_lock));
				locked = false;
			}

			/*
			 * Perform the archive.
			 */
			if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ARCHIVE)) {
				if (__wt_try_writelock(
				    session, log->log_archive_lock) == 0) {
					ret = __log_archive_once(session, 0);
					WT_TRET(__wt_writeunlock(
					    session, log->log_archive_lock));
					WT_ERR(ret);
				} else
					WT_ERR(
					    __wt_verbose(session, WT_VERB_LOG,
					    "log_archive: Blocked due to open "
					    "log cursor holding archive lock"));
			}
		}

		/* Wait until the next event. */

		WT_ERR(__wt_epoch(session, &start));
		WT_ERR(__wt_cond_auto_wait_signal(session, conn->log_cond,
		    did_work, &signalled));
		WT_ERR(__wt_epoch(session, &now));
		timediff = WT_TIMEDIFF_MS(now, start);
	}

	if (0) {
err:		__wt_err(session, ret, "log server error");
		if (locked)
			WT_TRET(__wt_readunlock(
			    session, conn->hot_backup_lock));
	}
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
	bool run;

	conn = S2C(session);

	/* Handle configuration. */
	WT_RET(__logmgr_config(session, cfg, &run, false));

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
	WT_RET(__wt_spin_init(session, &log->log_writelsn_lock,
	    "log write LSN"));
	WT_RET(__wt_rwlock_alloc(session,
	    &log->log_archive_lock, "log archive lock"));
	if (FLD_ISSET(conn->direct_io, WT_DIRECT_IO_LOG))
		log->allocsize = (uint32_t)
		    WT_MAX(conn->buffer_alignment, WT_LOG_ALIGN);
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
	WT_RET(__wt_cond_alloc(
	    session, "log sync", false, &log->log_sync_cond));
	WT_RET(__wt_cond_alloc(
	    session, "log write", false, &log->log_write_cond));
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
	uint32_t session_flags;

	conn = S2C(session);

	/* If no log thread services are configured, we're done. */ 
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		return (0);

	/*
	 * Start the log close thread.  It is not configurable.
	 * If logging is enabled, this thread runs.
	 */
	session_flags = WT_SESSION_NO_DATA_HANDLES;
	WT_RET(__wt_open_internal_session(conn,
	    "log-close-server", false, session_flags, &conn->log_file_session));
	WT_RET(__wt_cond_alloc(conn->log_file_session,
	    "log close server", false, &conn->log_file_cond));

	/*
	 * Start the log file close thread.
	 */
	WT_RET(__wt_thread_create(conn->log_file_session,
	    &conn->log_file_tid, __log_file_server, conn->log_file_session));
	conn->log_file_tid_set = true;

	/*
	 * Start the log write LSN thread.  It is not configurable.
	 * If logging is enabled, this thread runs.
	 */
	WT_RET(__wt_open_internal_session(conn, "log-wrlsn-server",
	    false, session_flags, &conn->log_wrlsn_session));
	WT_RET(__wt_cond_auto_alloc(conn->log_wrlsn_session,
	    "log write lsn server", false, 10000, WT_MILLION,
	    &conn->log_wrlsn_cond));
	WT_RET(__wt_thread_create(conn->log_wrlsn_session,
	    &conn->log_wrlsn_tid, __log_wrlsn_server, conn->log_wrlsn_session));
	conn->log_wrlsn_tid_set = true;

	/*
	 * If a log server thread exists, the user may have reconfigured
	 * archiving or pre-allocation.  Signal the thread.  Otherwise the
	 * user wants archiving and/or allocation and we need to start up
	 * the thread.
	 */
	if (conn->log_session != NULL) {
		WT_ASSERT(session, conn->log_cond != NULL);
		WT_ASSERT(session, conn->log_tid_set == true);
		WT_RET(__wt_cond_auto_signal(session, conn->log_cond));
	} else {
		/* The log server gets its own session. */
		WT_RET(__wt_open_internal_session(conn,
		    "log-server", false, session_flags, &conn->log_session));
		WT_RET(__wt_cond_auto_alloc(conn->log_session,
		    "log server", false, 50000, WT_MILLION, &conn->log_cond));

		/*
		 * Start the thread.
		 */
		WT_RET(__wt_thread_create(conn->log_session,
		    &conn->log_tid, __log_server, conn->log_session));
		conn->log_tid_set = true;
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
		WT_TRET(__wt_cond_auto_signal(session, conn->log_cond));
		WT_TRET(__wt_thread_join(session, conn->log_tid));
		conn->log_tid_set = false;
	}
	if (conn->log_file_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->log_file_cond));
		WT_TRET(__wt_thread_join(session, conn->log_file_tid));
		conn->log_file_tid_set = false;
	}
	if (conn->log_file_session != NULL) {
		wt_session = &conn->log_file_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_file_session = NULL;
	}
	if (conn->log_wrlsn_tid_set) {
		WT_TRET(__wt_cond_auto_signal(session, conn->log_wrlsn_cond));
		WT_TRET(__wt_thread_join(session, conn->log_wrlsn_tid));
		conn->log_wrlsn_tid_set = false;
	}
	if (conn->log_wrlsn_session != NULL) {
		wt_session = &conn->log_wrlsn_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_wrlsn_session = NULL;
	}

	WT_TRET(__wt_log_slot_destroy(session));
	WT_TRET(__wt_log_close(session));

	/* Close the server thread's session. */
	if (conn->log_session != NULL) {
		wt_session = &conn->log_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		conn->log_session = NULL;
	}

	/* Destroy the condition variables now that all threads are stopped */
	WT_TRET(__wt_cond_auto_destroy(session, &conn->log_cond));
	WT_TRET(__wt_cond_destroy(session, &conn->log_file_cond));
	WT_TRET(__wt_cond_auto_destroy(session, &conn->log_wrlsn_cond));

	WT_TRET(__wt_cond_destroy(session, &conn->log->log_sync_cond));
	WT_TRET(__wt_cond_destroy(session, &conn->log->log_write_cond));
	WT_TRET(__wt_rwlock_destroy(session, &conn->log->log_archive_lock));
	__wt_spin_destroy(session, &conn->log->log_lock);
	__wt_spin_destroy(session, &conn->log->log_slot_lock);
	__wt_spin_destroy(session, &conn->log->log_sync_lock);
	__wt_spin_destroy(session, &conn->log->log_writelsn_lock);
	__wt_free(session, conn->log_path);
	__wt_free(session, conn->log);
	return (ret);
}
