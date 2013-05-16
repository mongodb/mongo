/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__log_record_size(WT_SESSION_IMPL *session,
    size_t *sizep, WT_LOGREC_DESC *recdesc, va_list ap)
{
	return (__wt_struct_sizev(session, sizep, recdesc->fmt, ap));
}

int
__wt_log_filename(WT_SESSION_IMPL *session, WT_LOG *log, WT_ITEM *buf)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	WT_RET(__wt_buf_initsize(session, buf,
	    strlen(conn->log_path) + ENTRY_SIZE));
	WT_ERR(__wt_buf_fmt(session, buf, "%s/%s.%010" PRIu32,
	    conn->log_path, WT_LOG_FILENAME, log->fileid));
	return (0);

err:	__wt_buf_free(session, buf);
	return (ret);
}

#ifdef	NOTDEF
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

#if 0
/*
 * See session_api.c instead for __session_log_printf.
 */
int
__wt_log_printf(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_log_vprintf(session, fmt, ap);
	va_end(ap);

	return (ret);
}
#endif

/*
 * __wt_log_open --
 *	Open the log file.
 */
int
__wt_log_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(path);
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	WT_RET(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__wt_log_filename(session, log, path));
	WT_VERBOSE_ERR(session, log, "opening log %s",
	    (const char *)path->data);
fprintf(stderr,"opening log %s\n", (const char *)path->data);
	WT_ERR(__wt_open(session, path->data, 1, 0, 0, &log->log_fh));
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
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint32_t recsize)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
fprintf(stderr, "log_size_fit: size %d + offset %d = %d ; max %d\n",
recsize, lsn->offset, lsn->offset+recsize,conn->log_file_max);
	return (lsn->offset + recsize < conn->log_file_max);

}

static int
__log_newfile(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Set aside the log file handle to be closed later.  Other threads
	 * may still be using it to write to the log.
	 */
	WT_ASSERT(session, log->log_close_fh == NULL);
	log->log_close_fh = log->log_fh;
	log->fileid++;
	WT_RET(__wt_log_open(session));
	/*
	xxx - need to write log file header record then update lsns.
	*/
	log->alloc_lsn.file = log->fileid;
	log->alloc_lsn.offset = 0;

	return (0);
}

static int
__log_acquire(WT_SESSION_IMPL *session, uint32_t recsize, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
fprintf(stderr, "log_acquire: called\n");
	/*
	 * Called locked.  Add recsize to alloc_lsn.  Save our starting LSN
	 * as where the previous allocation finished.  That way when log files
	 * switch, we're waiting for the correct LSN from outstanding writes.
	 */
	slot->slot_start_lsn = log->alloc_lsn;
	if (!__log_size_fit(session, &log->alloc_lsn, recsize)) {
fprintf(stderr, "log_acquire: slot 0x%x size %d doesn't fit. Call newfile\n",slot,recsize);
		WT_RET(__log_newfile(session));
		if (log->log_close_fh != NULL)
			FLD_SET(slot->slot_flags, SLOT_CLOSEFH);
	}
	/*
	 * Need to minimally fill in slot info here.
	 */
fprintf(stderr, "log_acquire: slot 0x%x allocate at %d,%d, recsize %d\n",slot,log->alloc_lsn.file,log->alloc_lsn.offset,recsize);
	slot->slot_start_offset = log->alloc_lsn.offset;
	log->alloc_lsn.offset += recsize;
	slot->slot_end_lsn = log->alloc_lsn;
fprintf(stderr, "log_acquire: slot 0x%x endlsn %d,%d\n",slot,slot->slot_end_lsn.file,slot->slot_end_lsn.offset,recsize);
	slot->slot_fh = log->log_fh;
	return (0);
}

static int
__log_fill(WT_SESSION_IMPL *session, WT_MYSLOT *myslot, WT_ITEM *record)
{
	WT_LOG_RECORD *logrec;

	logrec = (WT_LOG_RECORD *)record->mem;
	/*
	 * Call __wt_write.  Note, myslot->offset might be a unit of
	 * LOG_ALIGN.  May need to multiply by LOG_ALIGN here if it is to get
	 * real file offset for write().  For now just use it as is.
	 */
fprintf(stderr, "log_fill: slot 0x%x writing %d bytes, at offset %d 0x%x\n",
myslot->slot, logrec->real_len,
myslot->offset + myslot->slot->slot_start_offset,
myslot->offset + myslot->slot->slot_start_offset);
fprintf(stderr, "log_fill: slot 0x%x from address 0x%x\n",myslot->slot, logrec);
	return (__wt_write(session, myslot->slot->slot_fh,
	    myslot->offset + myslot->slot->slot_start_offset,
	    logrec->real_len, (void *)logrec));
}

static int
__log_release(WT_SESSION_IMPL *session, uint32_t size, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
fprintf(stderr, "log_release: wait for write_lsn %d,%d, to be LSN %d,%d\n",
log->write_lsn.file,log->write_lsn.offset,slot->slot_start_lsn.file,slot->slot_start_lsn.offset);
	/*
	 * Wait for earlier groups to finish.  Slot_lsn is my beginning LSN.
	 */
	while (LOG_CMP(&log->write_lsn, &slot->slot_start_lsn) != 0) {
		__wt_yield();
	}
	log->write_lsn = slot->slot_end_lsn;
	if (FLD_ISSET(slot->slot_flags, SLOT_CLOSEFH)) {
fprintf(stderr, "log_release: slot 0x%x closing old fh %x\n",slot,log->log_close_fh);
		WT_RET(__wt_close(session, log->log_close_fh));
		log->log_close_fh = NULL;
		FLD_CLR(slot->slot_flags, SLOT_CLOSEFH);
	}
	if (FLD_ISSET(slot->slot_flags, SLOT_SYNC)) {
		WT_RET(__wt_fsync(session, log->log_fh));
		FLD_CLR(slot->slot_flags, SLOT_SYNC);
		log->sync_lsn = log->write_lsn;
fprintf(stderr, "log_release: slot 0x%x synced to lsn %d,%d\n",slot,log->write_lsn.file,log->write_lsn.offset);
	}
fprintf(stderr, "log_release: slot 0x%x after write_lsn %d,%d\n",slot,
log->write_lsn.file,log->write_lsn.offset);
	return (0);
}

int
__wt_log_read(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	return (0);
}

int
__wt_log_scan(WT_SESSION_IMPL *session, WT_ITEM *record, uint32_t flags,
    int (*func)(WT_SESSION_IMPL *session, WT_ITEM *record, void *cookie),
    void *cookie)
{
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
	WT_MYSLOT myslot;
	int locked;

	conn = S2C(session);
	log = conn->log;
	locked = 0;
	/*
	 * Assume the WT_ITEM the user passed is a WT_LOG_RECORD, which has
	 * a header at the beginning for us to fill in.
	 */
	logrec = (WT_LOG_RECORD *)record->mem;
	logrec->real_len = record->size;
	logrec->total_len = __wt_rduppo2(record->size, LOG_ALIGN);
	logrec->checksum = 0;
	logrec->checksum = __wt_hash_fnv64(logrec, record->size);

	memset(tmp, 0, sizeof(tmp));
fprintf(stderr, "log_write: log real_len: %d, total_len %d, chksum 0x%X\n",logrec->real_len, logrec->total_len, logrec->checksum);
	if (__wt_spin_trylock(session, &log->log_slot_lock) == 0) {
fprintf(stderr, "log_write: got lock\n");
		/*
		 * No contention, just write our record.  We're not using
		 * the consolidation arrays, so send in the tmp slot.
		 */
		locked = 1;
		myslot.slot = &tmp;
		myslot.offset = 0;
fprintf(stderr, "log_write: myself call log_acquire\n");
		WT_ERR(__log_acquire(session, logrec->total_len, &tmp));
		__wt_spin_unlock(session, &log->log_slot_lock);
		locked = 0;
fprintf(stderr, "log_write: myself writing to LSN %d,%d, offset %d\n",
tmp.slot_start_lsn.file,tmp.slot_start_lsn.offset, myslot.offset);
		WT_ERR(__log_fill(session, &myslot, record));
		WT_ERR(__log_release(session, record, &tmp));
		return (0);
	}
fprintf(stderr, "log_write: did not get lock\n");
	WT_ERR(__wt_log_slot_join(session, logrec->total_len, &myslot));
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
	WT_ERR(__log_fill(session, &myslot, record));
	if (__wt_log_slot_release(myslot.slot, logrec->total_len)
	    == WT_LOG_SLOT_DONE) {
		WT_ERR(__log_release(session, record, myslot.slot));
		WT_ERR(__wt_log_slot_free(myslot.slot));
	}
err:
	if (locked)
		__wt_spin_unlock(session, &log->log_slot_lock);
	return (ret);
}

int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_ITEM *buf;
	WT_LOG_RECORD *logrec;
	va_list ap_copy;
	size_t len;

	conn = S2C(session);

	if (!conn->logging)
		return (0);

	buf = &session->logprint_buf;

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + sizeof(WT_LOG_RECORD);
	va_end(ap_copy);

	WT_RET(__wt_buf_initsize(session, buf, len));

	fprintf(stderr, "log_printf: need record len: %d\n",len);
	logrec = (WT_LOG_RECORD *)buf->mem;
	fprintf(stderr, "log_printf: mem at 0x%x, record at 0x%x\n",buf->mem,&logrec->record);
	(void)vsnprintf(&logrec->record, len, fmt, ap);

	WT_VERBOSE_RET(session, log, "log record: %s\n", (char *)&logrec->record);
	fprintf(stderr, "log_printf: log record: 0x%x: %s\n",&logrec->record, (char *)&logrec->record);
	return (__wt_log_write(session, buf, NULL, 0));
}
