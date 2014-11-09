/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_log_ckpt --
 *	Record the given LSN as the checkpoint LSN and signal the archive
 *	thread as needed.
 */
int
__wt_log_ckpt(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	log->ckpt_lsn = *ckp_lsn;
	if (conn->arch_cond != NULL)
		WT_RET(__wt_cond_signal(session, conn->arch_cond));
	return (0);
}

/*
 * __wt_log_written_reset --
 *	Interface to reset the amount of log written during this
 *	during this checkpoint period.  Called from the checkpoint code.
 */
void
__wt_log_written_reset(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	if (!conn->logging)
		return;
	log = conn->log;
	log->log_written = 0;
	return;
}

/*
 * __wt_log_get_files --
 *	Retrieve the list of all existing log files.
 */
int
__wt_log_get_files(WT_SESSION_IMPL *session, char ***filesp, u_int *countp)
{
	WT_CONNECTION_IMPL *conn;
	const char *log_path;

	*countp = 0;
	*filesp = NULL;

	conn = S2C(session);
	log_path = conn->log_path;
	if (log_path == NULL)
		log_path = "";
	return (__wt_dirlist(session, log_path, WT_LOG_FILENAME,
	    WT_DIRLIST_INCLUDE, filesp, countp));
}

/*
 * __wt_log_get_active_files --
 *	Retrieve the list of active log files (those that are not candidates
 *	for archiving).
 */
int
__wt_log_get_active_files(
	WT_SESSION_IMPL *session, char ***filesp, u_int *countp)
{
	WT_DECL_RET;
	WT_LOG *log;
	char **files;
	uint32_t id;
	u_int count, i;

	id = 0;
	log = S2C(session)->log;

	WT_RET(__wt_log_get_files(session, &files, &count));

	/* Filter out any files that are below the checkpoint LSN. */
	for (i = 0; i < count; ) {
		WT_ERR(__wt_log_extract_lognum(session, files[i], &id));
		if (id < log->ckpt_lsn.file) {
			__wt_free(session, files[i]);
			files[i] = files[count - 1];
			files[--count] = NULL;
		} else
			i++;
	}

	*filesp = files;
	*countp = count;

	if (0) {
err:		__wt_log_files_free(session, files, count);
	}
	return (ret);
}

/*
 * __wt_log_files_free --
 *	Free memory associated with a log file list.
 */
void
__wt_log_files_free(WT_SESSION_IMPL *session, char **files, u_int count)
{
	u_int i;

	for (i = 0; i < count; i++)
		__wt_free(session, files[i]);
	__wt_free(session, files);
}

/*
 * __wt_log_filename --
 *	Given a log number, return a WT_ITEM of a generated log file name.
 */
int
__wt_log_filename(WT_SESSION_IMPL *session, uint32_t id, WT_ITEM *buf)
{
	const char *log_path;

	log_path = S2C(session)->log_path;

	if (log_path != NULL && log_path[0] != '\0')
		WT_RET(__wt_buf_fmt(session, buf, "%s/%s.%010" PRIu32,
		    log_path, WT_LOG_FILENAME, id));
	else
		WT_RET(__wt_buf_fmt(session, buf, "%s.%010" PRIu32,
		    WT_LOG_FILENAME, id));

	return (0);
}

/*
 * __wt_log_extract_lognum --
 *	Given a log file name, extract out the log number.
 */
int
__wt_log_extract_lognum(
    WT_SESSION_IMPL *session, const char *name, uint32_t *id)
{
	const char *p;

	WT_UNUSED(session);

	if (id == NULL || name == NULL)
		return (WT_ERROR);
	if ((p = strrchr(name, '.')) == NULL ||
	    sscanf(++p, "%" PRIu32, id) != 1)
		WT_RET_MSG(session, WT_ERROR, "Bad log file name '%s'", name);
	return (0);
}

/*
 * __wt_log_remove --
 *	Given a log number, remove that log file.
 */
int
__wt_log_remove(WT_SESSION_IMPL *session, uint32_t lognum)
{
	WT_DECL_ITEM(path);
	WT_DECL_RET;

	WT_ERR(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__wt_log_filename(session, lognum, path));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG,
	    "log_remove: remove log %s", (char *)path->data));
	WT_ERR(__wt_remove(session, path->data));
err:	__wt_scr_free(&path);
	return (ret);
}

/*
 * __log_openfile --
 *	Open a log file with the given log file number and return the WT_FH.
 */
static int
__log_openfile(WT_SESSION_IMPL *session, int ok_create, WT_FH **fh, uint32_t id)
{
	WT_DECL_ITEM(path);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__wt_log_filename(session, id, path));
	WT_ERR(__wt_verbose(session, WT_VERB_LOG,
	    "opening log %s", (const char *)path->data));
	WT_ERR(__wt_open(
	    session, path->data, ok_create, 0, WT_FILE_TYPE_LOG, fh));
err:	__wt_scr_free(&path);
	return (ret);
}

/*
 * __wt_log_open --
 *	Open the appropriate log file for the connection.  The purpose is
 *	to find the last log file that exists, open it and set our initial
 *	LSNs to the end of that file.  If none exist, call __wt_log_newfile
 *	to create it.
 */
int
__wt_log_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	uint32_t firstlog, lastlog, lognum;
	u_int i, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	lastlog = 0;
	firstlog = UINT32_MAX;

	/*
	 * Open up a file handle to the log directory if we haven't.
	 */
	if (log->log_dir_fh == NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG,
		    "log_open: open fh to directory %s", conn->log_path));
		WT_RET(__wt_open(session, conn->log_path,
		    0, 0, WT_FILE_TYPE_DIRECTORY, &log->log_dir_fh));
	}
	WT_RET(__wt_log_get_files(session, &logfiles, &logcount));
	for (i = 0; i < logcount; i++) {
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		lastlog = WT_MAX(lastlog, lognum);
		firstlog = WT_MIN(firstlog, lognum);
	}
	log->fileid = lastlog;
	WT_ERR(__wt_verbose(session, WT_VERB_LOG,
	    "log_open: first log %d last log %d", firstlog, lastlog));
	log->first_lsn.file = firstlog;
	log->first_lsn.offset = 0;

	/*
	 * Start logging at the beginning of the next log file, no matter
	 * where the previous log file ends.
	 */
	WT_ERR(__wt_log_newfile(session, 1));

	/*
	 * If there were log files, run recovery.
	 * XXX belongs at a higher level than this.
	 */
	if (logcount > 0) {
		log->trunc_lsn = log->alloc_lsn;
		WT_ERR(__wt_txn_recover(conn));
	}

err:	__wt_log_files_free(session, logfiles, logcount);
	return (ret);
}

/*
 * __wt_log_close --
 *	Close the log file.
 */
int
__wt_log_close(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	if (log->log_close_fh != NULL && log->log_close_fh != log->log_fh) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG,
		    "closing old log %s", log->log_close_fh->name));
		WT_RET(__wt_close(session, log->log_close_fh));
	}
	if (log->log_fh != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG,
		    "closing log %s", log->log_fh->name));
		WT_RET(__wt_close(session, log->log_fh));
		log->log_fh = NULL;
	}
	if (log->log_dir_fh != NULL) {
		WT_RET(__wt_verbose(session, WT_VERB_LOG,
		    "closing log directory %s", log->log_dir_fh->name));
		WT_RET(__wt_close(session, log->log_dir_fh));
		log->log_dir_fh = NULL;
	}
	return (0);
}

/*
 * __log_fill --
 *	Copy a thread's log records into the assigned slot.
 */
static int
__log_fill(WT_SESSION_IMPL *session,
    WT_MYSLOT *myslot, int direct, WT_ITEM *record, WT_LSN *lsnp)
{
	WT_DECL_RET;
	WT_LOG_RECORD *logrec;

	logrec = (WT_LOG_RECORD *)record->mem;
	/*
	 * Call __wt_write.  For now the offset is the real byte offset.
	 * If the offset becomes a unit of LOG_ALIGN this is where we would
	 * multiply by LOG_ALIGN to get the real file byte offset for write().
	 */
	if (direct)
		WT_ERR(__wt_write(session, myslot->slot->slot_fh,
		    myslot->offset + myslot->slot->slot_start_offset,
		    (size_t)logrec->len, (void *)logrec));
	else
		memcpy((char *)myslot->slot->slot_buf.mem + myslot->offset,
		    logrec, logrec->len);

	WT_STAT_FAST_CONN_INCRV(session, log_bytes_written, logrec->len);
	if (lsnp != NULL) {
		*lsnp = myslot->slot->slot_start_lsn;
		lsnp->offset += (wt_off_t)myslot->offset;
	}
err:
	if (ret != 0 && myslot->slot->slot_error == 0)
		myslot->slot->slot_error = ret;
	return (ret);
}

/*
 * __log_size_fit --
 *	Return whether or not recsize will fit in the log file.
 */
static int
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint64_t recsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	return (lsn->offset + (wt_off_t)recsize < conn->log_file_max);
}

/*
 * __log_truncate --
 *	Truncate the log to the given LSN.  If this_log is set, it will only
 *	truncate the log file indicated in the given LSN.  If not set,
 *	it will truncate between the given LSN and the trunc_lsn.  That is,
 *	since we pre-allocate log files, it will free that space and allow the
 *	log to be traversed.  We use the trunc_lsn because logging has already
 *	opened the new/next log file before recovery ran.  This function assumes
 *	we are in recovery or other dedicated time and not during live running.
 */
static int
__log_truncate(WT_SESSION_IMPL *session, WT_LSN *lsn, uint32_t this_log)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *log_fh, *tmp_fh;
	WT_LOG *log;
	uint32_t lognum;
	u_int i, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	log_fh = NULL;
	logcount = 0;
	logfiles = NULL;

	/*
	 * Truncate the log file to the given LSN.
	 */
	WT_ERR(__log_openfile(session, 0, &log_fh, lsn->file));
	WT_ERR(__wt_ftruncate(session, log_fh, lsn->offset));
	tmp_fh = log_fh;
	log_fh = NULL;
	WT_ERR(__wt_close(session, tmp_fh));

	/*
	 * If we just want to truncate the current log, return and skip
	 * looking for intervening logs.
	 */
	if (this_log)
		goto err;
	WT_ERR(__wt_log_get_files(session, &logfiles, &logcount));
	for (i = 0; i < logcount; i++) {
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		if (lognum > lsn->file && lognum < log->trunc_lsn.file) {
			WT_ERR(__log_openfile(session, 0, &log_fh, lognum));
			/*
			 * If there are intervening files pre-allocated,
			 * truncate them to the end of the log file header.
			 */
			WT_ERR(__wt_ftruncate(session,
			    log_fh, LOG_FIRST_RECORD));
			tmp_fh = log_fh;
			log_fh = NULL;
			WT_ERR(__wt_close(session, tmp_fh));
		}
	}
err:	if (log_fh != NULL)
		WT_TRET(__wt_close(session, log_fh));
	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);
	return (ret);
}

/*
 * __log_filesize --
 *	Returns an estimate of the real end of log file.
 */
static int
__log_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *eof)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	wt_off_t log_size, off, off1;
	uint32_t allocsize, bufsz;
	char *buf, *zerobuf;

	conn = S2C(session);
	log = conn->log;
	if (eof == NULL)
		return (0);
	*eof = 0;
	WT_RET(__wt_filesize(session, fh, &log_size));
	if (log == NULL)
		allocsize = LOG_ALIGN;
	else
		allocsize = log->allocsize;

	/*
	 * It can be very slow looking for the last real record in the log
	 * in very small chunks.  Walk backward by a megabyte at a time.  When
	 * we find a part of the log that is not just zeroes, walk to find
	 * the last record.
	 */
	buf = zerobuf = NULL;
	if (allocsize < WT_MEGABYTE && log_size > WT_MEGABYTE)
		bufsz = WT_MEGABYTE;
	else
		bufsz = allocsize;
	WT_RET(__wt_calloc_def(session, bufsz, &buf));
	WT_ERR(__wt_calloc_def(session, bufsz, &zerobuf));

	/*
	 * Read in a chunk starting at the end of the file.  Keep going until
	 * we reach the beginning or we find a chunk that contains any non-zero
	 * bytes.  Compare against a known zero byte chunk.
	 */
	for (off = log_size - (wt_off_t)bufsz;
	    off >= 0;
	    off -= (wt_off_t)bufsz) {
		WT_ERR(__wt_read(session, fh, off, bufsz, buf));
		if (memcmp(buf, zerobuf, bufsz) != 0)
			break;
	}

	/*
	 * If we're walking by large amounts, now walk by the real allocsize
	 * to find the real end, if we found something.  Otherwise we reached
	 * the beginning of the file.  Offset can go negative if the log file
	 * size is not a multiple of a megabyte.  The first chunk of the log
	 * file will always be non-zero.
	 */
	if (off < 0)
		off = 0;

	/*
	 * We know all log records are aligned at log->allocsize.  The first
	 * item in a log record is always a 32-bit length.  Look for any
	 * non-zero length at the allocsize boundary.  This may not be a true
	 * log record since it could be the middle of a large record.  But we
	 * know no log record starts after it.  Return an estimate of the log
	 * file size.
	 */
	for (off1 = bufsz - allocsize;
	    off1 > 0; off1 -= (wt_off_t)allocsize)
		if (memcmp(buf + off1, zerobuf, sizeof(uint32_t)) != 0)
			break;
	off = off + off1;

	/*
	 * Set EOF to the last zero-filled record we saw.
	 */
	*eof = off + (wt_off_t)allocsize;
err:
	if (buf != NULL)
		__wt_free(session, buf);
	if (zerobuf != NULL)
		__wt_free(session, zerobuf);
	return (ret);
}

/*
 * __log_acquire --
 *	Called with the log slot lock held.  Can be called recursively
 *	from __wt_log_newfile when we change log files.
 */
static int
__log_acquire(WT_SESSION_IMPL *session, uint64_t recsize, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	/*
	 * Called locked.  Add recsize to alloc_lsn.  Save our starting LSN
	 * where the previous allocation finished for the release LSN.
	 * That way when log files switch, we're waiting for the correct LSN
	 * from outstanding writes.
	 */
	slot->slot_release_lsn = log->alloc_lsn;
	if (!__log_size_fit(session, &log->alloc_lsn, recsize)) {
		WT_RET(__wt_log_newfile(session, 0));
		if (log->log_close_fh != NULL)
			F_SET(slot, SLOT_CLOSEFH);
	}
	/*
	 * Checkpoints can be configured based on amount of log written.
	 * Add in this log record to the sum and if needed, signal the
	 * checkpoint condition.  The logging subsystem manages the
	 * accumulated field.  There is a bit of layering violation
	 * here checking the connection ckpt field and using its
	 * condition.
	 */
	if (WT_CKPT_LOGSIZE(conn)) {
		log->log_written += (wt_off_t)recsize;
		WT_RET(__wt_checkpoint_signal(session, log->log_written));
	}

	/*
	 * Need to minimally fill in slot info here.  Our slot start LSN
	 * comes after any potential new log file creations.
	 */
	slot->slot_start_lsn = log->alloc_lsn;
	slot->slot_start_offset = log->alloc_lsn.offset;
	/*
	 * Pre-allocate on the first real write into the log file.
	 */
	if (log->alloc_lsn.offset == LOG_FIRST_RECORD) {
		if (!log->log_fh->fallocate_available ||
		    (ret = __wt_fallocate(session, log->log_fh,
		    LOG_FIRST_RECORD, conn->log_file_max)) == ENOTSUP)
			ret = __wt_ftruncate(session, log->log_fh,
			    LOG_FIRST_RECORD + conn->log_file_max);
		WT_RET(ret);
	}

	log->alloc_lsn.offset += (wt_off_t)recsize;
	slot->slot_end_lsn = log->alloc_lsn;
	slot->slot_error = 0;
	slot->slot_fh = log->log_fh;
	return (0);
}

/*
 * __log_release --
 *	Release a log slot.
 */
static int
__log_release(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *close_fh;
	WT_LOG *log;
	WT_LSN sync_lsn;
	size_t write_size;
	int locked;
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	conn = S2C(session);
	log = conn->log;
	locked = 0;

	/*
	 * If we're going to have to close our log file, make a local copy
	 * of the file handle structure.
	 */
	close_fh = NULL;
	if (F_ISSET(slot, SLOT_CLOSEFH)) {
		close_fh = log->log_close_fh;
		log->log_close_fh = NULL;
		F_CLR(slot, SLOT_CLOSEFH);
	}

	/* Write the buffered records */
	if (F_ISSET(slot, SLOT_BUFFERED)) {
		write_size = (size_t)
		    (slot->slot_end_lsn.offset - slot->slot_start_offset);
		WT_ERR(__wt_write(session, slot->slot_fh,
		    slot->slot_start_offset, write_size, slot->slot_buf.mem));
	}

	/*
	 * Wait for earlier groups to finish, otherwise there could be holes
	 * in the log file.
	 */
	while (LOG_CMP(&log->write_lsn, &slot->slot_release_lsn) != 0)
		__wt_yield();
	log->write_lsn = slot->slot_end_lsn;

	/*
	 * Try to consolidate calls to fsync to wait less.  Acquire a spin lock
	 * so that threads finishing writing to the log will wait while the
	 * current fsync completes and advance log->sync_lsn.
	 */
	while (F_ISSET(slot, SLOT_SYNC | SLOT_SYNC_DIR)) {
		if (__wt_spin_trylock(session, &log->log_sync_lock, &id) != 0) {
			WT_ERR(__wt_cond_wait(
			    session, log->log_sync_cond, 10000));
			continue;
		}
		locked = 1;

		/*
		 * Record the current end of log after we grabbed the lock.
		 * That is how far our calls can guarantee.
		 */
		sync_lsn = log->write_lsn;
		/*
		 * Check if we have to sync the parent directory.  Some
		 * combinations of sync flags may result in the log file
		 * not yet stable in its parent directory.  Do that
		 * now if needed.
		 */
		if (F_ISSET(slot, SLOT_SYNC_DIR) &&
		    (log->sync_dir_lsn.file < sync_lsn.file)) {
			WT_ASSERT(session, log->log_dir_fh != NULL);
			WT_ERR(__wt_verbose(session, WT_VERB_LOG,
			    "log_release: sync directory %s",
			    log->log_dir_fh->name));
			WT_ERR(__wt_directory_sync_fh(
			    session, log->log_dir_fh));
			log->sync_dir_lsn = sync_lsn;
			F_CLR(slot, SLOT_SYNC_DIR);
		}

		/*
		 * Sync the log file if needed.
		 */
		if (F_ISSET(slot, SLOT_SYNC) &&
		    LOG_CMP(&log->sync_lsn, &slot->slot_end_lsn) < 0) {
			WT_ERR(__wt_verbose(session, WT_VERB_LOG,
			    "log_release: sync log %s", log->log_fh->name));
			WT_STAT_FAST_CONN_INCR(session, log_sync);
			WT_ERR(__wt_fsync(session, log->log_fh));
			F_CLR(slot, SLOT_SYNC);
			log->sync_lsn = sync_lsn;
			WT_ERR(__wt_cond_signal(session, log->log_sync_cond));
		}
		locked = 0;
		__wt_spin_unlock(session, &log->log_sync_lock);
		break;
	}
	if (F_ISSET(slot, SLOT_BUF_GROW)) {
		WT_STAT_FAST_CONN_INCR(session, log_buffer_grow);
		F_CLR(slot, SLOT_BUF_GROW);
		WT_STAT_FAST_CONN_INCRV(session,
		    log_buffer_size, slot->slot_buf.memsize);
		WT_ERR(__wt_buf_grow(session,
		    &slot->slot_buf, slot->slot_buf.memsize * 2));
	}
	/*
	 * If we have a file to close, close it now.
	 */
	if (close_fh)
		WT_ERR(__wt_close(session, close_fh));

err:	if (locked)
		__wt_spin_unlock(session, &log->log_sync_lock);
	if (ret != 0 && slot->slot_error == 0)
		slot->slot_error = ret;
	return (ret);
}

/*
 * __wt_log_newfile --
 *	Create the next log file and write the file header record into it.
 */
int
__wt_log_newfile(WT_SESSION_IMPL *session, int conn_create)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_DESC *desc;
	WT_LOG_RECORD *logrec;
	WT_LOGSLOT tmp;
	WT_MYSLOT myslot;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Set aside the log file handle to be closed later.  Other threads
	 * may still be using it to write to the log.  If the log file size
	 * is small we could fill a log file before the previous one is closed.
	 * Wait for that to close.
	 */
	while (log->log_close_fh != NULL) {
		__wt_errx(session,
		    "log_newfile: Log file size %" PRIuMAX " too small",
		    (uintmax_t)conn->log_file_max);
		WT_STAT_FAST_CONN_INCR(session, log_close_yields);
		__wt_yield();
	}
	log->log_close_fh = log->log_fh;
	log->fileid++;
	WT_RET(__log_openfile(session, 1, &log->log_fh, log->fileid));
	log->alloc_lsn.file = log->fileid;
	log->alloc_lsn.offset = log->log_fh->size;

	/*
	 * Set up the log descriptor record.  Use a scratch buffer to
	 * get correct alignment for direct I/O.
	 */
	WT_ASSERT(session, sizeof(WT_LOG_DESC) < log->allocsize);
	WT_RET(__wt_scr_alloc(session, log->allocsize, &buf));
	memset(buf->mem, 0, log->allocsize);
	logrec = (WT_LOG_RECORD *)buf->mem;
	desc = (WT_LOG_DESC *)logrec->record;
	desc->log_magic = WT_LOG_MAGIC;
	desc->majorv = WT_LOG_MAJOR_VERSION;
	desc->minorv = WT_LOG_MINOR_VERSION;
	desc->log_size = (uint64_t)conn->log_file_max;

	/*
	 * Now that the record is set up, initialize the record header.
	 */
	logrec->len = log->allocsize;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, log->allocsize);
	WT_CLEAR(tmp);
	myslot.slot = &tmp;
	myslot.offset = 0;

	/*
	 * Recursively call __log_acquire to allocate log space for the
	 * log descriptor record.  Call __log_fill to write it, but we
	 * do not need to call __log_release because we're not waiting for
	 * earlier operations to complete.
	 */
	WT_ERR(__log_acquire(session, logrec->len, &tmp));
	WT_ERR(__log_fill(session, &myslot, 1, buf, NULL));

	/*
	 * If we're called from connection creation code, we need to update
	 * the LSNs since we're the only write in progress.
	 */
	if (conn_create) {
		WT_ERR(__wt_fsync(session, log->log_fh));
		log->sync_lsn = tmp.slot_end_lsn;
		log->write_lsn = tmp.slot_end_lsn;
	}

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_log_read --
 *	Read the log record at the given LSN.  Return the record (including
 *	the log header) in the WT_ITEM.  Caller is responsible for freeing it.
 */
int
__wt_log_read(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	uint32_t cksum, rdup_len, reclen;

	WT_UNUSED(flags);
	/*
	 * If the caller didn't give us an LSN or something to return,
	 * there's nothing to do.
	 */
	if (lsnp == NULL || record == NULL)
		return (0);
	conn = S2C(session);
	log = conn->log;
	/*
	 * If the offset isn't on an allocation boundary it must be wrong.
	 */
	if (lsnp->offset % log->allocsize != 0 || lsnp->file > log->fileid)
		return (WT_NOTFOUND);

	WT_RET(__log_openfile(session, 0, &log_fh, lsnp->file));
	/*
	 * Read the minimum allocation size a record could be.
	 */
	WT_ERR(__wt_buf_init(session, record, log->allocsize));
	WT_ERR(__wt_read(session,
	    log_fh, lsnp->offset, (size_t)log->allocsize, record->mem));
	/*
	 * First 4 bytes is the real record length.  See if we
	 * need to read more than the allocation size.  We expect
	 * that we rarely will have to read more.  Most log records
	 * will be fairly small.
	 */
	reclen = *(uint32_t *)record->mem;
	if (reclen == 0) {
		ret = WT_NOTFOUND;
		goto err;
	}
	if (reclen > log->allocsize) {
		rdup_len = __wt_rduppo2(reclen, log->allocsize);
		WT_ERR(__wt_buf_grow(session, record, rdup_len));
		WT_ERR(__wt_read(session,
		    log_fh, lsnp->offset, (size_t)rdup_len, record->mem));
	}
	/*
	 * We read in the record, verify checksum.
	 */
	logrec = (WT_LOG_RECORD *)record->mem;
	cksum = logrec->checksum;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, logrec->len);
	if (logrec->checksum != cksum)
		WT_ERR_MSG(session, WT_ERROR, "log_read: Bad checksum");
	record->size = logrec->len;
	WT_STAT_FAST_CONN_INCR(session, log_reads);
err:
	WT_TRET(__wt_close(session, log_fh));
	return (ret);
}

/*
 * __wt_log_scan --
 *	Scan the logs, calling a function on each record found.
 */
int
__wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *lsnp, uint32_t flags,
    int (*func)(WT_SESSION_IMPL *session,
    WT_ITEM *record, WT_LSN *lsnp, void *cookie), void *cookie)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_ITEM buf;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN end_lsn, rd_lsn, start_lsn;
	wt_off_t log_size;
	uint32_t allocsize, cksum, firstlog, lastlog, lognum, rdup_len, reclen;
	u_int i, logcount;
	int eol;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	log_fh = NULL;
	logcount = 0;
	logfiles = NULL;
	eol = 0;
	WT_CLEAR(buf);

	/*
	 * If the caller did not give us a callback function there is nothing
	 * to do.
	 */
	if (func == NULL)
		return (0);

	if (LF_ISSET(WT_LOGSCAN_RECOVER))
		WT_RET(__wt_verbose(session, WT_VERB_LOG,
		    "__wt_log_scan truncating to %u/%" PRIuMAX,
		    log->trunc_lsn.file, (uintmax_t)log->trunc_lsn.offset));

	if (log != NULL) {
		allocsize = log->allocsize;

		if (lsnp == NULL) {
			if (LF_ISSET(WT_LOGSCAN_FIRST))
				start_lsn = log->first_lsn;
			else if (LF_ISSET(WT_LOGSCAN_FROM_CKP))
				start_lsn = log->ckpt_lsn;
			else
				return (WT_ERROR);	/* Illegal usage */
		} else {
			if (LF_ISSET(WT_LOGSCAN_FIRST|WT_LOGSCAN_FROM_CKP))
				WT_RET_MSG(session, WT_ERROR,
			    "choose either a start LSN or a start flag");

			/* Offsets must be on allocation boundaries. */
			if (lsnp->offset % allocsize != 0 ||
			    lsnp->file > log->fileid)
				return (WT_NOTFOUND);

			/*
			 * Log cursors may not know the starting LSN.  If an
			 * LSN pointer is passed in, but it is the INIT_LSN,
			 * start from the first_lsn.
			 */
			start_lsn = *lsnp;
			if (IS_INIT_LSN(&start_lsn))
				start_lsn = log->first_lsn;
		}
		end_lsn = log->alloc_lsn;
	} else {
		/*
		 * If logging is not configured, we can still print out the log
		 * if log files exist.  We just need to set the LSNs from what
		 * is in the files versus what is in the live connection.
		 */
		/*
		 * Set allocsize to the minimum alignment it could be.  Larger
		 * records and larger allocation boundaries should always be
		 * a multiple of this.
		 */
		allocsize = LOG_ALIGN;
		lastlog = 0;
		firstlog = UINT32_MAX;
		WT_RET(__wt_log_get_files(session, &logfiles, &logcount));
		if (logcount == 0)
			/*
			 * Return it is not supported if none don't exist.
			 */
			return (ENOTSUP);
		for (i = 0; i < logcount; i++) {
			WT_ERR(__wt_log_extract_lognum(session, logfiles[i],
			    &lognum));
			lastlog = WT_MAX(lastlog, lognum);
			firstlog = WT_MIN(firstlog, lognum);
		}
		start_lsn.file = firstlog;
		end_lsn.file = lastlog;
		start_lsn.offset = end_lsn.offset = 0;
		__wt_log_files_free(session, logfiles, logcount);
		logfiles = NULL;
	}
	WT_ERR(__log_openfile(session, 0, &log_fh, start_lsn.file));
	WT_ERR(__log_filesize(session, log_fh, &log_size));
	rd_lsn = start_lsn;
	WT_ERR(__wt_buf_initsize(session, &buf, LOG_ALIGN));
	for (;;) {
		if (rd_lsn.offset + allocsize > log_size) {
advance:
			/*
			 * If we read the last record, go to the next file.
			 */
			WT_ERR(__wt_close(session, log_fh));
			log_fh = NULL;
			eol = 1;
			/*
			 * Truncate this log file before we move to the next.
			 */
			if (LF_ISSET(WT_LOGSCAN_RECOVER))
				WT_ERR(__log_truncate(session, &rd_lsn, 1));
			rd_lsn.file++;
			rd_lsn.offset = 0;
			/*
			 * Avoid an error message when we reach end of log
			 * by checking here.
			 */
			if (rd_lsn.file > end_lsn.file)
				break;
			WT_ERR(__log_openfile(
			    session, 0, &log_fh, rd_lsn.file));
			WT_ERR(__log_filesize(session, log_fh, &log_size));
			continue;
		}
		/*
		 * Read the minimum allocation size a record could be.
		 */
		WT_ASSERT(session, buf.memsize >= allocsize);
		WT_ERR(__wt_read(session,
		    log_fh, rd_lsn.offset, (size_t)allocsize, buf.mem));
		/*
		 * First 8 bytes is the real record length.  See if we
		 * need to read more than the allocation size.  We expect
		 * that we rarely will have to read more.  Most log records
		 * will be fairly small.
		 */
		reclen = *(uint32_t *)buf.mem;
		/*
		 * Log files are pre-allocated.  We never expect a zero length
		 * unless we've reached the end of the log.  The log can be
		 * written out of order, so when recovery finds the end of
		 * the log, truncate the file and remove any later log files
		 * that may exist.
		 */
		if (reclen == 0) {
			/* This LSN is the end. */
			break;
		}
		rdup_len = __wt_rduppo2(reclen, allocsize);
		if (reclen > allocsize) {
			/*
			 * The log file end could be the middle of this
			 * log record.
			 */
			if (rd_lsn.offset + rdup_len > log_size)
				goto advance;
			/*
			 * We need to round up and read in the full padded
			 * record, especially for direct I/O.
			 */
			WT_ERR(__wt_buf_grow(session, &buf, rdup_len));
			WT_ERR(__wt_read(session,
			    log_fh, rd_lsn.offset, (size_t)rdup_len, buf.mem));
			WT_STAT_FAST_CONN_INCR(session, log_scan_rereads);
		}
		/*
		 * We read in the record, verify checksum.
		 */
		buf.size = reclen;
		logrec = (WT_LOG_RECORD *)buf.mem;
		cksum = logrec->checksum;
		logrec->checksum = 0;
		logrec->checksum = __wt_cksum(logrec, logrec->len);
		if (logrec->checksum != cksum) {
			/*
			 * A checksum mismatch means we have reached the end of
			 * the useful part of the log.  This should be found on
			 * the first pass through recovery.  In the second pass
			 * where we truncate the log, this is where it should
			 * end.
			 */
			if (log != NULL)
				log->trunc_lsn = rd_lsn;
			break;
		}

		/*
		 * We have a valid log record.  If it is not the log file
		 * header, invoke the callback.
		 */
		WT_STAT_FAST_CONN_INCR(session, log_scan_records);
		if (rd_lsn.offset != 0) {
			WT_ERR((*func)(session, &buf, &rd_lsn, cookie));
			if (LF_ISSET(WT_LOGSCAN_ONE))
				break;
		}
		rd_lsn.offset += (wt_off_t)rdup_len;
	}

	/* Truncate if we're in recovery. */
	if (LF_ISSET(WT_LOGSCAN_RECOVER) &&
	    LOG_CMP(&rd_lsn, &log->trunc_lsn) < 0)
		WT_ERR(__log_truncate(session, &rd_lsn, 0));

err:	WT_STAT_FAST_CONN_INCR(session, log_scans);
	if (logfiles != NULL)
		__wt_log_files_free(session, logfiles, logcount);
	__wt_buf_free(session, &buf);
	/*
	 * If the caller wants one record and it is at the end of log,
	 * return WT_NOTFOUND.
	 */
	if (LF_ISSET(WT_LOGSCAN_ONE) && eol && ret == 0)
		ret = WT_NOTFOUND;
	if (ret == ENOENT)
		ret = 0;
	if (log_fh != NULL)
		WT_TRET(__wt_close(session, log_fh));
	return (ret);
}

/*
 * __log_direct_write --
 *	Write a log record without using the consolidation arrays.
 */
static int
__log_direct_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT tmp;
	WT_MYSLOT myslot;
	int locked;
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	log = S2C(session)->log;
	myslot.slot = &tmp;
	myslot.offset = 0;
	WT_CLEAR(tmp);

	/* Fast path the contended case. */
	if (__wt_spin_trylock(session, &log->log_slot_lock, &id) != 0)
		return (EAGAIN);
	locked = 1;

	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
		F_SET(&tmp, SLOT_SYNC_DIR);
	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(&tmp, SLOT_SYNC);
	WT_ERR(__log_acquire(session, record->size, &tmp));
	__wt_spin_unlock(session, &log->log_slot_lock);
	locked = 0;
	WT_ERR(__log_fill(session, &myslot, 1, record, lsnp));
	WT_ERR(__log_release(session, &tmp));

err:	if (locked)
		__wt_spin_unlock(session, &log->log_slot_lock);
	return (ret);
}

/*
 * __wt_log_write --
 *	Write a record into the log.
 */
int
__wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN lsn;
	WT_MYSLOT myslot;
	uint32_t rdup_len;
	int locked;

	conn = S2C(session);
	log = conn->log;
	locked = 0;
	INIT_LSN(&lsn);
	myslot.slot = NULL;
	/*
	 * Assume the WT_ITEM the user passed is a WT_LOG_RECORD, which has
	 * a header at the beginning for us to fill in.
	 *
	 * If using direct_io, the caller should pass us an aligned record.
	 * But we need to make sure it is big enough and zero-filled so
	 * that we can write the full amount.  Do this whether or not
	 * direct_io is in use because it makes the reading code cleaner.
	 */
	WT_STAT_FAST_CONN_INCRV(session, log_bytes_user, record->size);
	rdup_len = __wt_rduppo2((uint32_t)record->size, log->allocsize);
	WT_ERR(__wt_buf_grow(session, record, rdup_len));
	WT_ASSERT(session, record->data == record->mem);
	/*
	 * If the caller's record only partially fills the necessary
	 * space, we need to zero-fill the remainder.
	 */
	if (record->size != rdup_len) {
		memset((uint8_t *)record->mem + record->size, 0,
		    rdup_len - record->size);
		record->size = rdup_len;
	}
	logrec = (WT_LOG_RECORD *)record->mem;
	logrec->len = (uint32_t)record->size;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, record->size);

	WT_STAT_FAST_CONN_INCR(session, log_writes);

	if (!F_ISSET(log, WT_LOG_FORCE_CONSOLIDATE)) {
		ret = __log_direct_write(session, record, lsnp, flags);
		if (ret == 0)
			return (0);
		if (ret != EAGAIN)
			WT_ERR(ret);
		/*
		 * An EAGAIN return means we failed to get the try lock -
		 * fall through to the consolidation code in that case.
		 */
	}

	/*
	 * As soon as we see contention for the log slot, disable direct
	 * log writes. We get better performance by forcing writes through
	 * the consolidation code. This is because individual writes flood
	 * the I/O system faster than they contend on the log slot lock.
	 */
	F_SET(log, WT_LOG_FORCE_CONSOLIDATE);
	if ((ret = __wt_log_slot_join(
	    session, rdup_len, flags, &myslot)) == ENOMEM) {
		/*
		 * If we couldn't find a consolidated slot for this record
		 * write the record directly.
		 */
		while ((ret = __log_direct_write(
		    session, record, lsnp, flags)) == EAGAIN)
			;
		WT_ERR(ret);
		/*
		 * Increase the buffer size of any slots we can get access
		 * to, so future consolidations are likely to succeed.
		 */
		WT_ERR(__wt_log_slot_grow_buffers(session, 4 * rdup_len));
		return (0);
	}
	WT_ERR(ret);
	if (myslot.offset == 0) {
		__wt_spin_lock(session, &log->log_slot_lock);
		locked = 1;
		WT_ERR(__wt_log_slot_close(session, myslot.slot));
		WT_ERR(__log_acquire(
		    session, myslot.slot->slot_group_size, myslot.slot));
		__wt_spin_unlock(session, &log->log_slot_lock);
		locked = 0;
		WT_ERR(__wt_log_slot_notify(session, myslot.slot));
	} else
		WT_ERR(__wt_log_slot_wait(session, myslot.slot));
	WT_ERR(__log_fill(session, &myslot, 0, record, &lsn));
	if (__wt_log_slot_release(myslot.slot, rdup_len) == WT_LOG_SLOT_DONE) {
		WT_ERR(__log_release(session, myslot.slot));
		WT_ERR(__wt_log_slot_free(myslot.slot));
	} else if (LF_ISSET(WT_LOG_FSYNC)) {
		/* Wait for our writes to reach disk */
		while (LOG_CMP(&log->sync_lsn, &lsn) <= 0 &&
		    myslot.slot->slot_error == 0)
			(void)__wt_cond_wait(
			    session, log->log_sync_cond, 10000);
	}
err:
	if (locked)
		__wt_spin_unlock(session, &log->log_slot_lock);
	if (ret == 0 && lsnp != NULL)
		*lsnp = lsn;
	/*
	 * If we're synchronous and some thread had an error, we don't know
	 * if our write made it out to the file or not.  The error could be
	 * before or after us.  So, if anyone got an error, we report it.
	 * If we're not synchronous, only report if our own operation got
	 * an error.
	 */
	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC) && ret == 0 &&
	    myslot.slot != NULL)
		ret = myslot.slot->slot_error;
	return (ret);
}

/*
 * __wt_log_vprintf --
 *	Write a message into the log.
 */
int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(logrec);
	WT_DECL_RET;
	va_list ap_copy;
	const char *rec_fmt = WT_UNCHECKED_STRING(I);
	uint32_t rectype = WT_LOGREC_MESSAGE;
	size_t header_size, len;

	conn = S2C(session);

	if (!conn->logging)
		return (0);

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + 1;
	va_end(ap_copy);

	WT_RET(
	    __wt_logrec_alloc(session, sizeof(WT_LOG_RECORD) + len, &logrec));

	/*
	 * We're writing a record with the type (an integer) followed by a
	 * string (NUL-terminated data).  To avoid writing the string into
	 * a buffer before copying it, we write the header first, then the
	 * raw bytes of the string.
	 */
	WT_ERR(__wt_struct_size(session, &header_size, rec_fmt, rectype));
	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, header_size,
	    rec_fmt, rectype));
	logrec->size += (uint32_t)header_size;

	(void)vsnprintf((char *)logrec->data + logrec->size, len, fmt, ap);

	WT_ERR(__wt_verbose(session, WT_VERB_LOG,
	    "log_printf: %s", (char *)logrec->data + logrec->size));

	logrec->size += len;
	WT_ERR(__wt_log_write(session, logrec, NULL, 0));
err:	__wt_scr_free(&logrec);
	return (ret);
}
