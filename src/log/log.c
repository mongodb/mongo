/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

int
__wt_log_filename(WT_SESSION_IMPL *session, uint32_t id, WT_ITEM *buf)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	WT_RET(__wt_buf_initsize(session, buf,
	    strlen(conn->log_path) + ENTRY_SIZE));
	WT_ERR(__wt_buf_fmt(session, buf, "%s/%s.%010" PRIu32,
	    conn->log_path, WT_LOG_FILENAME, id));
	return (0);

err:	__wt_buf_free(session, buf);
	return (ret);
}

static int
__log_openfile(WT_SESSION_IMPL *session, int ok_create, WT_FH **fh, uint32_t id)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(path);
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	WT_RET(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__wt_log_filename(session, id, path));
	WT_VERBOSE_ERR(session, log, "opening log %s",
	    (const char *)path->data);
	fprintf(stderr,"[%d] opening log %s\n",
	    pthread_self(),(const char *)path->data);
	WT_ERR(__wt_open(
	    session, path->data, ok_create, 0, WT_FILE_TYPE_LOG, fh));
	/*
	 * Need to store new fileid in metadata.
	 */
err:	__wt_scr_free(&path);
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
	 * Call __wt_write.  Note, myslot->offset might be a unit of
	 * LOG_ALIGN.  May need to multiply by LOG_ALIGN here if it is to get
	 * real file offset for write().  For now just use it as is.
	 */
	fprintf(stderr,
	    "[%d] log_fill: slot 0x%x state %d writing %d bytes, at offset %d 0x%x\n",
	    pthread_self(), myslot->slot, myslot->slot->slot_state,
	    logrec->len, myslot->offset + myslot->slot->slot_start_offset,
	    myslot->offset + myslot->slot->slot_start_offset);
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
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint32_t recsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	return (lsn->offset + recsize < conn->log_file_max);

}

/*
 * __log_acquire --
 *	Called with the log slot lock held.  Can be called recursively
 *	from __log_newfile when we change log files.
 */
static int
__log_acquire(WT_SESSION_IMPL *session, uint32_t recsize, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	/*
	 * Called locked.  Add recsize to alloc_lsn.  Save our starting LSN
	 * as where the previous allocation finished.  That way when log files
	 * switch, we're waiting for the correct LSN from outstanding writes.
	 */
	slot->slot_start_lsn = log->alloc_lsn;
	if (!__log_size_fit(session, &log->alloc_lsn, recsize)) {
		fprintf(stderr,
	    "[%d] log_acquire: slot 0x%x size %d doesn't fit. Call newfile\n",
		    pthread_self(),slot,recsize);
		WT_RET(__wt_log_newfile(session, 0));
		if (log->log_close_fh != NULL)
			FLD_SET(slot->slot_flags, SLOT_CLOSEFH);
	}
	/*
	 * Need to minimally fill in slot info here.
	 */
	slot->slot_start_offset = log->alloc_lsn.offset;
	log->alloc_lsn.offset += recsize;
	slot->slot_end_lsn = log->alloc_lsn;
	slot->slot_error = 0;
	fprintf(stderr,
"[%d] log_acquire: slot 0x%x recsize %d startlsn %d,%d endlsn %d,%d\n",
	    pthread_self(),slot,recsize,
	    slot->slot_start_lsn.file,slot->slot_start_lsn.offset,
	    slot->slot_end_lsn.file,slot->slot_end_lsn.offset,recsize);
	slot->slot_fh = log->log_fh;
	return (0);
}

static int
__log_release(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	fprintf(stderr,
    "[%d:0x%x] log_release: wait for write_lsn %d,%d, to be LSN %d,%d\n",
	    pthread_self(), pthread_self(),
	    log->write_lsn.file,log->write_lsn.offset,
	    slot->slot_start_lsn.file,slot->slot_start_lsn.offset);
	/*
	 * Wait for earlier groups to finish.  Slot_lsn is my beginning LSN.
	 */
	while (LOG_CMP(&log->write_lsn, &slot->slot_start_lsn) != 0)
		__wt_yield();
	if (FLD_ISSET(slot->slot_flags, SLOT_SYNC)) {
		WT_CSTAT_INCR(session, log_sync);
		WT_ERR(__wt_fsync(session, log->log_fh));
		FLD_CLR(slot->slot_flags, SLOT_SYNC);
		log->sync_lsn = slot->slot_end_lsn;
		fprintf(stderr,
		    "[%d] log_release: slot 0x%x synced to lsn %d,%d\n",
		    pthread_self(),slot,log->write_lsn.file,
		    log->write_lsn.offset);
	}
	log->write_lsn = slot->slot_end_lsn;
	if (FLD_ISSET(slot->slot_flags, SLOT_CLOSEFH)) {
		fprintf(stderr,
		    "[%d] log_release: slot 0x%x closing old fh %x\n",
		    pthread_self(),slot,log->log_close_fh);
		WT_RET(__wt_close(session, log->log_close_fh));
		log->log_close_fh = NULL;
		FLD_CLR(slot->slot_flags, SLOT_CLOSEFH);
	}

err:
	if (ret != 0 && slot->slot_error == 0)
		slot->slot_error = ret;
	return (ret);
}

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
	uint32_t cksum, reclen;

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
	fprintf(stderr, "[%d] log_read: read from LSN %d,%d\n",
	    pthread_self(), lsnp->file, lsnp->offset);
	if (lsnp->offset % log->allocsize != 0 || lsnp->file > log->fileid)
		return (WT_NOTFOUND);

	WT_RET(__log_openfile(session, 0, &log_fh, lsnp->file));
	/*
	 * Read in 4 bytes that is the total size of the log record.
	 * Check for out of bounds values.
	 */
	WT_ERR(__wt_read(
	    session, log_fh, lsnp->offset, sizeof(uint32_t), (void *)&reclen));
	if (reclen > WT_MAX_LOG_OFFSET || reclen == 0) {
		fprintf(stderr, "[%d] log_read: read bad reclen %d %d (%d)\n",
		    pthread_self(), reclen, WT_MAX_LOG_OFFSET, reclen>WT_MAX_LOG_OFFSET);
		ret = WT_NOTFOUND;
		goto err;
	}
	WT_ERR(__wt_buf_init(session, record,
	    __wt_rduppo2(reclen, log->allocsize)));
	WT_ERR(__wt_read(session, log_fh, lsnp->offset, reclen, record->mem));
	/*
	 * We read in the record, verify checksum.
	 */
	logrec = (WT_LOG_RECORD *)record->mem;
	cksum = logrec->checksum;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, logrec->len);
	if (logrec->checksum != cksum) {
		fprintf(stderr, "[%d] log_read: bad cksum\n", pthread_self());
		ret = WT_ERROR;
		goto err;
	}
	record->size = logrec->len;
	WT_CSTAT_INCR(session, log_reads);
err:
	WT_RET(__wt_close(session, log_fh));

	return (ret);
}

int
__wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *lsnp, uint32_t flags,
    void (*func)(WT_SESSION_IMPL *session,
    WT_ITEM *record, WT_LSN *lsnp, void *cookie), void *cookie)
{
	WT_CONNECTION_IMPL *conn;
	WT_ITEM buf;
	WT_DECL_RET;
	WT_FH *log_fh;
	WT_LOG *log;
	WT_LOG_RECORD *logrec;
	WT_LSN rd_lsn, start_lsn;
	uint32_t cksum, rdup_len, reclen;
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
	fprintf(stderr, "[%d] log_scan: start LSN %d,%d\n",
	    pthread_self(), start_lsn.file, start_lsn.offset);
	log_fh = NULL;
	WT_RET(__log_openfile(session, 0, &log_fh, start_lsn.file));
	if (LF_ISSET(WT_LOGSCAN_ONE))
		done = 1;
	else
		done = 0;
	rd_lsn = start_lsn;
	WT_RET(__wt_buf_initsize(session, &buf, LOG_ALIGN));
	do {
		/*
		 * Read in 4 bytes that is the total size of the log record.
		 * Check for out of bounds values.
		 */
		reclen = 0;
		if (__wt_read(session, log_fh, rd_lsn.offset,
		    sizeof(uint32_t), (void *)&reclen) != 0) {
			/*
			 * If we read the last record, go to the next file.
			 */
			WT_ERR(__wt_close(session, log_fh));
			log_fh = NULL;
			rd_lsn.file++;
			rd_lsn.offset = 0;
			fprintf(stderr, "[%d] log_scan: switch to LSN %d,%d\n",
			    pthread_self(), rd_lsn.file, rd_lsn.offset);
			/*
			 * If there is no next file, we're done.  WT_ERR will
			 * break us out of this loop.
			 */
			WT_ERR(__log_openfile(
			    session, 0, &log_fh, rd_lsn.file));
			continue;
		}
		if (reclen > WT_MAX_LOG_OFFSET || reclen == 0) {
			fprintf(stderr,
			    "[%d] log_scan: read bad reclen %d %d (%d)\n",
			    pthread_self(), reclen,
			    WT_MAX_LOG_OFFSET, reclen > WT_MAX_LOG_OFFSET);
			ret = WT_NOTFOUND;
			goto err;
		}
		rdup_len = __wt_rduppo2(reclen, log->allocsize);
		if (reclen > buf.memsize)
			WT_ERR(__wt_buf_grow(session, &buf, rdup_len));

		WT_ERR(__wt_read(
		    session, log_fh, rd_lsn.offset, reclen, buf.mem));
		/*
		 * We read in the record, verify checksum.
		 */
		logrec = (WT_LOG_RECORD *)buf.mem;
		cksum = logrec->checksum;
		logrec->checksum = 0;
		logrec->checksum = __wt_cksum(logrec, logrec->len);
		if (logrec->checksum != cksum) {
			fprintf(stderr,
			    "[%d] log_scan: bad cksum\n", pthread_self());
			ret = WT_ERROR;
			goto err;
		}

		/*
		 * We have a valid log record.  Invoke the callback.
		 */
		(*func)(session, &buf, &rd_lsn, cookie);

		WT_CSTAT_INCR(session, log_scan_records);
		rd_lsn.offset += rdup_len;
		fprintf(stderr, "[%d] log_scan: next read LSN %d,%d\n",
		    pthread_self(), rd_lsn.file, rd_lsn.offset);
	} while (!done);
err:
	WT_CSTAT_INCR(session, log_scans);
	__wt_buf_free(session, &buf);
	if (ret == ENOENT)
		ret = 0;
	if (log_fh != NULL)
		WT_RET(__wt_close(session, log_fh));
	return (0);
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
	uint32_t rdup_len;
	int locked;

	conn = S2C(session);
	log = conn->log;
	locked = 0;
	/*
	 * Assume the WT_ITEM the user passed is a WT_LOG_RECORD, which has
	 * a header at the beginning for us to fill in.
	 */
	logrec = (WT_LOG_RECORD *)record->mem;
	logrec->len = record->size;
	logrec->checksum = 0;
	logrec->checksum = __wt_cksum(logrec, record->size);
	rdup_len = __wt_rduppo2(record->size, log->allocsize);

	memset(&tmp, 0, sizeof(tmp));
	fprintf(stderr,
"[%d] log_write: flags 0x%x lsnp 0x%x len: %d chksum 0x%X\n",
	    pthread_self(),flags, lsnp, logrec->len, logrec->checksum);
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
		fprintf(stderr,
		    "[%d] log_write: myself writing to LSN %d,%d, offset %d\n",
		    pthread_self(), tmp.slot_start_lsn.file,
		    tmp.slot_start_lsn.offset, myslot.offset);
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
		fprintf(stderr,
		    "[%d] log_write: leader notify slot 0x%x, state/size %d\n",
		    pthread_self(), myslot.slot, myslot.slot->slot_state);
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
	return (ret);
}

static void
__scan_call(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, void *cookie)
{
	WT_LOG_RECORD *logrec;

	logrec = (WT_LOG_RECORD *)record->mem;
	/*
	 * Return if log file header record
	 */
	if (lsnp->offset == 0)
		return;
	fprintf(stderr,
	    "[%d] scan_call: SCAN LSN %d,%d  record: 0x%x: %s\n",
	    pthread_self(),
	    lsnp->file,lsnp->offset,
	    logrec->record, (char *)&logrec->record);
}

int
__wt_log_vprintf(WT_SESSION_IMPL *session, pthread_t tid, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_ITEM *buf;
	WT_LOG_RECORD *logrec;
	va_list ap_copy;
	size_t len;
WT_DECL_RET;
WT_LOG *log;
WT_LSN lsn;
WT_ITEM rdbuf;
uint32_t sync;

	conn = S2C(session);
log = conn->log;
WT_CLEAR(rdbuf);

	if (!conn->logging)
		return (0);

	buf = &session->logprint_buf;

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + sizeof(WT_LOG_RECORD);
	va_end(ap_copy);

	WT_RET(__wt_buf_initsize(session, buf, len));

	logrec = (WT_LOG_RECORD *)buf->mem;
	(void)vsnprintf(&logrec->record, len, fmt, ap);

	WT_VERBOSE_RET(session, log,
	    "VERB: log record: %s\n", (char *)&logrec->record);
	fprintf(stderr, "[%d] log_printf: log record: 0x%x: %s\n",
	    tid, &logrec->record, (char *)&logrec->record);
#if 0
	return (__wt_log_write(session, buf, NULL, 0));
#else
	if (len % 2 == 0)
		sync = WT_LOG_SYNC;
	else
		sync = 0;
	ret = __wt_log_write(session, buf, &lsn, sync);
	/*
	 * Only read some records.  Randomize on sync.
	 */
	if (ret == 0 && sync) {
		if (lsn.file == 2 && !F_ISSET(log, LOG_AUTOREMOVE)) {
			F_SET(log, LOG_AUTOREMOVE);
			ret = __wt_log_scan(
			    session, &lsn, WT_LOGSCAN_ONE, __scan_call, NULL);
			ret = __wt_log_scan(
			    session, NULL, WT_LOGSCAN_FIRST, __scan_call, NULL);
			ret = 0;
		} else {
			ret = __wt_log_read(session, &rdbuf, &lsn, 0);
			if (ret == 0) {
				logrec = (WT_LOG_RECORD *)rdbuf.mem;
				fprintf(stderr,
    "[%d] log_printf: READ log record %d,%d: 0x%x: %s\n",
				    tid, lsn.file, lsn.offset,
				    logrec->record, (char *)&logrec->record);
				__wt_buf_free(session, &rdbuf);
			}
		}
	}
	return (0);
#endif
}

#if 0
int
__wt_log_put(WT_SESSION_IMPL *session, WT_LOGREC_DESC *recdesc, ...)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	va_list ap;
	size_t size;

	buf = &session->logrec_buf;
	size = 0;

	va_start(ap, recdesc);
	ret = __log_record_size(session, &size, recdesc, ap);
	va_end(ap);
	WT_RET(ret);

	WT_RET(__wt_buf_initsize(session, buf, size));

	va_start(ap, recdesc);
	ret = __wt_struct_packv(session, buf->mem, size, recdesc->fmt, ap);
	va_end(ap);
	return (ret);
}
#endif
