/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
	char *p;

	WT_UNUSED(session);
	if (id == NULL || name == NULL)
		return (0);
	if ((p = strrchr(name, '.')) == NULL)
		return (WT_ERROR);
	++p;
	if ((sscanf(++p, "%" PRIu32, id)) != 1)
		return (WT_ERROR);
	return (0);
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
	WT_VERBOSE_ERR(session, log, "opening log %s",
	    (const char *)path->data);
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
	WT_FH *log_fh;
	WT_LOG *log;
	uint32_t firstlog, lastlog, lognum;
	u_int i, logcount;
	char **logfiles;

	conn = S2C(session);
	log = conn->log;
	lastlog = 0;
	firstlog = UINT32_MAX;
	WT_RET(__wt_dirlist(session, conn->log_path,
	    WT_LOG_FILENAME, WT_DIRLIST_INCLUDE, &logfiles, &logcount));
	for (i = 0; i < logcount; i++) {
		WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
		lastlog = WT_MAX(lastlog, lognum);
		firstlog = WT_MIN(firstlog, lognum);
		__wt_free(session, logfiles[i]);
	}
	log->fileid = lastlog;
	WT_VERBOSE_ERR(session, log, "log_open: first log %d last log %d",
	    firstlog, lastlog);
	if (lastlog == 0)
		WT_ERR(__wt_log_newfile(session, 1));
	else {
		WT_ERR(__log_openfile(session, 0, &log_fh, lastlog));
		log->log_fh = log_fh;
		log->alloc_lsn.file = log->fileid;
		log->alloc_lsn.offset =
		    __wt_rduppo2(log->log_fh->size, log->allocsize);
		log->write_lsn = log->alloc_lsn;
		log->sync_lsn = log->alloc_lsn;
		log->first_lsn.file = firstlog;
		log->first_lsn.offset = 0;
		WT_VERBOSE_ERR(session, log,
		    "log_open: open to end of existing log %d,%" PRIu64,
		    log->alloc_lsn.file, log->alloc_lsn.offset);
	}
err:
	__wt_free(session, logfiles);
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
	/*
	 * If we don't have a log open, there's nothing to do.
	 */
	if (log->log_fh == NULL)
		return (0);
	WT_VERBOSE_RET(session, log, "closing log %s", log->log_fh->name);
	WT_RET(__wt_close(session, log->log_fh));
	log->log_fh = NULL;
	return (0);
}

static int
__log_fill(WT_SESSION_IMPL *session,
    WT_MYSLOT *myslot, WT_ITEM *record, WT_LSN *lsnp)
{
	WT_DECL_RET;
	WT_LOG_RECORD *logrec;

	logrec = (WT_LOG_RECORD *)record->mem;
	/*
	 * Call __wt_write.  For now the offset is the real byte offset.
	 * If the offset becomes a unit of LOG_ALIGN this is where we would
	 * multiply by LOG_ALIGN to get the real file byte offset for write().
	 */
	WT_ERR(__wt_write(session, myslot->slot->slot_fh,
	    myslot->offset + myslot->slot->slot_start_offset,
	    logrec->len, (void *)logrec));
	WT_CSTAT_INCRV(session, log_bytes_written, logrec->len);
	if (lsnp != NULL) {
		*lsnp = myslot->slot->slot_start_lsn;
		lsnp->offset += myslot->offset;
	}
err:
	if (ret != 0 && myslot->slot->slot_error == 0)
		myslot->slot->slot_error = ret;
	return (ret);
}

static int
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint64_t recsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	return (lsn->offset + recsize < conn->log_file_max);

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
			FLD_SET(slot->slot_flags, SLOT_CLOSEFH);
	}
	/*
	 * Need to minimally fill in slot info here.  Our slot start LSN
	 * comes after any potential new log file creations.
	 */
	slot->slot_start_lsn = log->alloc_lsn;
	slot->slot_start_offset = log->alloc_lsn.offset;
	log->alloc_lsn.offset += recsize;
	slot->slot_end_lsn = log->alloc_lsn;
	slot->slot_error = 0;
	slot->slot_fh = log->log_fh;
	return (0);
}

static int
__log_release(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_FH *close_fh;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	/*
	 * If we're going to have to close our log file, make a local copy
	 * of the file handle structure.
	 */
	close_fh = NULL;
	if (FLD_ISSET(slot->slot_flags, SLOT_CLOSEFH)) {
		close_fh = log->log_close_fh;
		log->log_close_fh = NULL;
		FLD_CLR(slot->slot_flags, SLOT_CLOSEFH);
	}
	/*
	 * Wait for earlier groups to finish.
	 */
	while (LOG_CMP(&log->write_lsn, &slot->slot_release_lsn) != 0)
		__wt_yield();
	if (FLD_ISSET(slot->slot_flags, SLOT_SYNC)) {
		WT_CSTAT_INCR(session, log_sync);
		WT_ERR(__wt_fsync(session, log->log_fh));
		FLD_CLR(slot->slot_flags, SLOT_SYNC);
		log->sync_lsn = slot->slot_end_lsn;
	}
	log->write_lsn = slot->slot_end_lsn;
	/*
	 * If we have a file to close, close it now.
	 */
	if (close_fh)
		WT_ERR(__wt_close(session, close_fh));

err:	if (ret != 0 && slot->slot_error == 0)
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
	 * may still be using it to write to the log.
	 */
	WT_ASSERT(session, log->log_close_fh == NULL);
	log->log_close_fh = log->log_fh;
	log->fileid++;
	WT_RET(__log_openfile(session, 1, &log->log_fh, log->fileid));
	log->alloc_lsn.file = log->fileid;
	log->alloc_lsn.offset = log->log_fh->size;

	/*
	 * Set up the log descriptor record.  Use a scratch buffer to
	 * get correct alignment for direct I/O.
	 */
	WT_RET(__wt_scr_alloc(session, log->allocsize, &buf));
	memset(buf->mem, 0, log->allocsize);
	logrec = (WT_LOG_RECORD *)buf->mem;
	desc = (WT_LOG_DESC *)&logrec->record;
	desc->log_magic = WT_LOG_MAGIC;
	desc->majorv = WT_LOG_MAJOR_VERSION;
	desc->minorv = WT_LOG_MINOR_VERSION;
	desc->log_size = conn->log_file_max;

	/*
	 * Now that the record is set up, initialize the record header.
	 */
	logrec->len = log->allocsize;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, log->allocsize);
	memset(&tmp, 0, sizeof(tmp));
	myslot.slot = &tmp;
	myslot.offset = 0;
	/*
	 * Recursively call __log_acquire to allocate log space for the
	 * log descriptor record.  Call __log_fill to write it, but we
	 * do not need to call __log_release because we're not waiting for
	 * earlier operations to complete.
	 */
	WT_ERR(__log_acquire(session, logrec->len, &tmp));
	WT_ERR(__log_fill(session, &myslot, buf, NULL));
	/*
	 * If we're called from connection creation code, we need to update
	 * the write_lsn since we're the only write in progress.
	 */
	if (conn_create)
		log->write_lsn = tmp.slot_end_lsn;

err:
	__wt_scr_free(&buf);
	return (0);
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
	uint64_t rdup_len, reclen;
	uint32_t cksum;

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
	WT_ERR(__wt_read(
	    session, log_fh, lsnp->offset, log->allocsize, record->mem));
	/*
	 * First 4 bytes is the real record length.  See if we
	 * need to read more than the allocation size.  We expect
	 * that we rarely will have to read more.  Most log records
	 * will be fairly small.
	 */
	reclen = *(uint64_t *)record->mem;
	if (reclen > WT_MAX_LOG_OFFSET || reclen == 0) {
		ret = WT_NOTFOUND;
		goto err;
	}
	if (reclen > log->allocsize) {
		rdup_len = __wt_rduppo2(reclen, log->allocsize);
		WT_ERR(__wt_buf_grow(session, record, rdup_len));
		WT_ERR(__wt_read(
		    session, log_fh, lsnp->offset, rdup_len, record->mem));
	}
	/*
	 * We read in the record, verify checksum.
	 */
	logrec = (WT_LOG_RECORD *)record->mem;
	cksum = logrec->checksum;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, logrec->len);
	if (logrec->checksum != cksum) {
		WT_VERBOSE_ERR(session, log, "log_read: Bad checksum");
		ret = WT_ERROR;
		goto err;
	}
	record->size = logrec->len;
	WT_CSTAT_INCR(session, log_reads);
err:
	WT_TRET(__wt_close(session, log_fh));
	return (ret);
}

int
__wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *lsnp, uint32_t flags,
    int (*func)(WT_SESSION_IMPL *session,
    WT_ITEM *record, WT_LSN *lsnp, void *cookie), void *cookie)
{
	WT_CONNECTION_IMPL *conn;
	WT_ITEM buf;
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN end_lsn, rd_lsn, start_lsn;
	uint64_t log_size, rdup_len, reclen;
	uint32_t cksum;
	int done;

	conn = S2C(session);
	log = conn->log;
	WT_CLEAR(buf);
	/*
	 * Check for correct usage.
	 */
	if (LF_ISSET(WT_LOGSCAN_FIRST|WT_LOGSCAN_FROM_CKP) && lsnp != NULL)
		return (WT_ERROR);
	/*
	 * If the caller did not give us a callback function there is nothing
	 * to do.
	 */
	if (func == NULL)
		return (0);
	/*
	 * If the offset isn't on an allocation boundary it must be wrong.
	 */
	if (lsnp != NULL &&
	    (lsnp->offset % log->allocsize != 0 || lsnp->file > log->fileid))
		return (WT_NOTFOUND);

	if (LF_ISSET(WT_LOGSCAN_FIRST))
		start_lsn = log->first_lsn;
	else if (LF_ISSET(WT_LOGSCAN_FROM_CKP))
		start_lsn = log->ckpt_lsn;
	else
		start_lsn = *lsnp;
	end_lsn = log->alloc_lsn;
	log_fh = NULL;
	WT_RET(__log_openfile(session, 0, &log_fh, start_lsn.file));
	WT_ERR(__wt_filesize(session, log_fh, (off_t *)&log_size));
	if (LF_ISSET(WT_LOGSCAN_ONE))
		done = 1;
	else
		done = 0;
	rd_lsn = start_lsn;
	WT_ERR(__wt_buf_initsize(session, &buf, LOG_ALIGN));
	do {
		/*
		 * Read in 4 bytes that is the total size of the log record.
		 * Check for out of bounds values.
		 */
		reclen = 0;
		if (rd_lsn.offset >= log_size) {
			/*
			 * If we read the last record, go to the next file.
			 */
			WT_ERR(__wt_close(session, log_fh));
			log_fh = NULL;
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
			WT_ERR(__wt_filesize(
			    session, log_fh, (off_t *)&log_size));
			continue;
		}
		/*
		 * Read the minimum allocation size a record could be.
		 */
		WT_ASSERT(session, buf.memsize >= log->allocsize);
		WT_ERR(__wt_read(
		    session, log_fh, rd_lsn.offset, log->allocsize, buf.mem));
		/*
		 * First 4 bytes is the real record length.  See if we
		 * need to read more than the allocation size.  We expect
		 * that we rarely will have to read more.  Most log records
		 * will be fairly small.
		 */
		reclen = *(uint64_t *)buf.mem;
		if (reclen > WT_MAX_LOG_OFFSET || reclen == 0) {
			ret = WT_NOTFOUND;
			goto err;
		}
		rdup_len = __wt_rduppo2(reclen, log->allocsize);
		if (reclen > log->allocsize) {
			WT_ERR(__wt_buf_grow(session, &buf, rdup_len));
			WT_ERR(__wt_read(
			    session, log_fh, rd_lsn.offset, reclen, buf.mem));
			WT_CSTAT_INCR(session, log_scan_rereads);
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
			ret = WT_ERROR;
			goto err;
		}

		/*
		 * We have a valid log record.  If it is not the log file
		 * header, invoke the callback.
		 */
		if (rd_lsn.offset != 0)
			WT_ERR((*func)(session, &buf, &rd_lsn, cookie));

		WT_CSTAT_INCR(session, log_scan_records);
		rd_lsn.offset += rdup_len;
	} while (!done);

err:	WT_CSTAT_INCR(session, log_scans);
	__wt_buf_free(session, &buf);
	if (ret == ENOENT)
		ret = 0;
	if (log_fh != NULL)
		WT_TRET(__wt_close(session, log_fh));
	return (ret);
}

int
__wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LOGSLOT tmp;
	WT_LSN tmp_lsn;
	WT_MYSLOT myslot;
	uint64_t rdup_len;
	int locked;

	conn = S2C(session);
	log = conn->log;
	locked = 0;
	/*
	 * Assume the WT_ITEM the user passed is a WT_LOG_RECORD, which has
	 * a header at the beginning for us to fill in.
	 *
	 * If using direct_io, the caller should pass us an aligned record.
	 * But we need to make sure it is big enough and zero-filled so
	 * that we can write the full amount.  Do this whether or not
	 * direct_io is in use because it makes the reading code cleaner.
	 */
	WT_CSTAT_INCRV(session, log_bytes_user, record->size);
	rdup_len = __wt_rduppo2(record->size, log->allocsize);
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
	logrec->len = record->size;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, record->size);

	memset(&tmp, 0, sizeof(tmp));
	WT_CSTAT_INCR(session, log_writes);
	if (__wt_spin_trylock(session, &log->log_slot_lock) == 0) {
		/*
		 * No contention, just write our record.  We're not using
		 * the consolidation arrays, so send in the tmp slot.
		 */
		locked = 1;
		myslot.slot = &tmp;
		myslot.offset = 0;
		if (LF_ISSET(WT_LOG_SYNC))
			FLD_SET(tmp.slot_flags, SLOT_SYNC);
		WT_ERR(__log_acquire(session, rdup_len, &tmp));
		__wt_spin_unlock(session, &log->log_slot_lock);
		locked = 0;
		WT_ERR(__log_fill(session, &myslot, record, lsnp));
		WT_ERR(__log_release(session, &tmp));
		return (0);
	}
	WT_ERR(__wt_log_slot_join(session, rdup_len, flags, &myslot));
	if (myslot.offset == 0) {
		__wt_spin_lock(session, &log->log_slot_lock);
		locked = 1;
		WT_ERR(__wt_log_slot_close(session, myslot.slot));
		WT_ERR(__log_acquire(
		    session, myslot.slot->slot_group_size, myslot.slot));
		__wt_spin_unlock(session, &log->log_slot_lock);
		locked = 0;
		WT_ERR(__wt_log_slot_notify(myslot.slot));
	} else
		WT_ERR(__wt_log_slot_wait(myslot.slot));
	WT_ERR(__log_fill(session, &myslot, record, &tmp_lsn));
	if (__wt_log_slot_release(myslot.slot, rdup_len) ==
	    WT_LOG_SLOT_DONE) {
		WT_ERR(__log_release(session, myslot.slot));
		WT_ERR(__wt_log_slot_free(myslot.slot));
	} else if (LF_ISSET(WT_LOG_SYNC)) {
		while (LOG_CMP(&log->sync_lsn, &tmp_lsn) <= 0 &&
		    myslot.slot->slot_error == 0)
			__wt_yield();
	}
err:
	if (locked)
		__wt_spin_unlock(session, &log->log_slot_lock);
	if (ret == 0 && lsnp != NULL)
		*lsnp = tmp_lsn;
	/*
	 * If we're synchronous and some thread had an error, we don't know
	 * if our write made it out to the file or not.  The error could be
	 * before or after us.  So, if anyone got an error, we report it.
	 * If we're not synchronous, only report if our own operation got
	 * an error.
	 */
	if (LF_ISSET(WT_LOG_SYNC) && ret == 0)
		ret = myslot.slot->slot_error;
	if (LF_ISSET(WT_LOG_CKPT) && ret == 0) {
		log->ckpt_lsn = tmp_lsn;
		if (conn->arch_cond != NULL)
			WT_ERR(__wt_cond_signal(session, conn->arch_cond));
	}
	return (ret);
}

int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(buf);
	WT_LOG_RECORD *logrec;
	va_list ap_copy;
	size_t len;

	conn = S2C(session);

	if (!conn->logging)
		return (0);

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + sizeof(WT_LOG_RECORD);
	va_end(ap_copy);

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_RET(__wt_buf_initsize(session, buf, len));

	logrec = (WT_LOG_RECORD *)buf->mem;
	(void)vsnprintf((char *)&logrec->record, len, fmt, ap);

	WT_VERBOSE_RET(session, log,
	    "log_printf: %s", (char *)&logrec->record);
	WT_RET(__wt_log_write(session, buf, NULL, 0));
	__wt_scr_free(&buf);
	return (0);
}
