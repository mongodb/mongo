/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __log_newfile(WT_SESSION_IMPL *, bool, bool *);
static int __log_openfile(WT_SESSION_IMPL *, uint32_t, uint32_t, WT_FH **);
static int __log_truncate(WT_SESSION_IMPL *, WT_LSN *, bool, bool);
static int __log_write_internal(WT_SESSION_IMPL *, WT_ITEM *, WT_LSN *, uint32_t);

#define WT_LOG_COMPRESS_SKIP (offsetof(WT_LOG_RECORD, record))
#define WT_LOG_ENCRYPT_SKIP (offsetof(WT_LOG_RECORD, record))

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LOG_OPEN_CREATE_OK 0x1u /* Flag to __log_openfile() */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/*
 * __wt_log_printf --
 *     Write a text message to the log.
 */
int
__wt_log_printf(WT_SESSION_IMPL *session, const char *format, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, format);
    ret = __wt_log_vprintf(session, format, ap);
    va_end(ap);
    return (ret);
}

/*
 * __log_checksum_match --
 *     Given a log record, return whether the checksum matches.
 */
static bool
__log_checksum_match(WT_ITEM *buf, uint32_t reclen)
{
    WT_LOG_RECORD *logrec;
    uint32_t checksum_saved, checksum_tmp;
    bool checksum_matched;

    logrec = buf->mem;
    checksum_saved = checksum_tmp = logrec->checksum;
#ifdef WORDS_BIGENDIAN
    checksum_tmp = __wt_bswap32(checksum_tmp);
#endif
    logrec->checksum = 0;
    checksum_matched = __wt_checksum_match(logrec, reclen, checksum_tmp);
    logrec->checksum = checksum_saved;
    return (checksum_matched);
}

/*
 * __log_get_files --
 *     Retrieve the list of all log-related files of the given prefix type.
 */
static int
__log_get_files(WT_SESSION_IMPL *session, const char *file_prefix, char ***filesp, u_int *countp)
{
    WT_CONNECTION_IMPL *conn;
    const char *log_path;

    *countp = 0;
    *filesp = NULL;

    conn = S2C(session);
    log_path = conn->log_path;
    if (log_path == NULL)
        log_path = "";
    return (__wt_fs_directory_list(session, log_path, file_prefix, filesp, countp));
}

/*
 * __log_get_files_single --
 *     Retrieve a single log-related file of the given prefix type.
 */
static int
__log_get_files_single(
  WT_SESSION_IMPL *session, const char *file_prefix, char ***filesp, u_int *countp)
{
    WT_CONNECTION_IMPL *conn;
    const char *log_path;

    *countp = 0;
    *filesp = NULL;

    conn = S2C(session);
    log_path = conn->log_path;
    if (log_path == NULL)
        log_path = "";
    return (__wt_fs_directory_list_single(session, log_path, file_prefix, filesp, countp));
}

/*
 * __log_prealloc_remove --
 *     Remove all previously created pre-allocated files.
 */
static int
__log_prealloc_remove(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_LOG *log;
    uint32_t lognum;
    u_int i, logcount;
    char **logfiles;

    logfiles = NULL;
    logcount = 0;
    log = S2C(session)->log;
    __wt_spin_lock(session, &log->log_fs_lock);
    /*
     * Clean up any old interim pre-allocated files. We clean up these files because settings may
     * have changed upon reboot and we want those settings to take effect right away.
     */
    WT_ERR(__log_get_files(session, WT_LOG_TMPNAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        WT_ERR(__wt_log_remove(session, WT_LOG_TMPNAME, lognum));
    }
    WT_ERR(__wt_fs_directory_list_free(session, &logfiles, logcount));
    WT_ERR(__log_get_files(session, WT_LOG_PREPNAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        WT_ERR(__wt_log_remove(session, WT_LOG_PREPNAME, lognum));
    }
err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    __wt_spin_unlock(session, &log->log_fs_lock);
    return (ret);
}

/*
 * __log_wait_for_earlier_slot --
 *     Wait for write_lsn to catch up to this slot.
 */
static void
__log_wait_for_earlier_slot(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    int yield_count;

    conn = S2C(session);
    log = conn->log;
    yield_count = 0;

    while (__wt_log_cmp(&log->write_lsn, &slot->slot_release_lsn) != 0) {
        /*
         * If we're on a locked path and the write LSN is not advancing, unlock in case an earlier
         * thread is trying to switch its slot and complete its operation.
         */
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT))
            __wt_spin_unlock(session, &log->log_slot_lock);
        /*
         * This may not be initialized if we are starting at an older log file version. So only
         * signal if valid.
         */
        if (conn->log_wrlsn_cond != NULL)
            __wt_cond_signal(session, conn->log_wrlsn_cond);
        if (++yield_count < WT_THOUSAND)
            __wt_yield();
        else
            __wt_cond_wait(session, log->log_write_cond, 200, NULL);
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT))
            __wt_spin_lock(session, &log->log_slot_lock);
    }
}

/*
 * __log_fs_read --
 *     Wrapper when reading from a log file.
 */
static int
__log_fs_read(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
    WT_DECL_RET;

    __wt_capacity_throttle(session, len, WT_THROTTLE_LOG);
    if ((ret = __wt_read(session, fh, offset, len, buf)) != 0)
        WT_RET_MSG(session, ret, "%s: log read failure", fh->name);
    return (ret);
}

/*
 * __log_fs_write --
 *     Wrapper when writing to a log file. If we're writing to a new log file for the first time
 *     wait for writes to the previous log file.
 */
static int
__log_fs_write(
  WT_SESSION_IMPL *session, WT_LOGSLOT *slot, wt_off_t offset, size_t len, const void *buf)
{
    WT_DECL_RET;

    /*
     * If we're writing into a new log file and we're running in compatibility mode to an older
     * release, we have to wait for all writes to the previous log file to complete otherwise there
     * could be a hole at the end of the previous log file that we cannot detect.
     *
     * NOTE: Check for a version less than the one writing the system record since we've had a log
     * version change without any actual file format changes.
     */
    if (S2C(session)->log->log_version < WT_LOG_VERSION_SYSTEM &&
      slot->slot_release_lsn.l.file < slot->slot_start_lsn.l.file) {
        __log_wait_for_earlier_slot(session, slot);
        WT_RET(__wt_log_force_sync(session, &slot->slot_release_lsn));
    }
    __wt_capacity_throttle(session, len, WT_THROTTLE_LOG);
    if ((ret = __wt_write(session, slot->slot_fh, offset, len, buf)) != 0)
        WT_RET_PANIC(session, ret, "%s: fatal log failure", slot->slot_fh->name);
    return (ret);
}

/*
 * __wt_log_ckpt --
 *     Record the given LSN as the checkpoint LSN and signal the removal thread as needed.
 */
void
__wt_log_ckpt(WT_SESSION_IMPL *session, WT_LSN *ckpt_lsn)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    int i;

    conn = S2C(session);
    log = conn->log;
    WT_ASSIGN_LSN(&log->ckpt_lsn, ckpt_lsn);
    if (conn->log_cond != NULL)
        __wt_cond_signal(session, conn->log_cond);
    /*
     * If we are storing debugging LSNs to retain additional log files from removal, then rotate the
     * newest LSN into the array.
     */
    if (conn->debug_ckpt_cnt != 0) {
        for (i = (int)conn->debug_ckpt_cnt - 1; i > 0; --i)
            conn->debug_ckpt[i] = conn->debug_ckpt[i - 1];
        conn->debug_ckpt[0] = *ckpt_lsn;
    }
}

/*
 * __wt_log_flush_lsn --
 *     Force out buffered records and return the LSN, either the write_start_lsn or write_lsn
 *     depending on the argument.
 */
int
__wt_log_flush_lsn(WT_SESSION_IMPL *session, WT_LSN *lsn, bool start)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;
    WT_RET(__wt_log_force_write(session, 1, NULL));
    __wt_log_wrlsn(session, NULL);
    if (start)
        WT_ASSIGN_LSN(lsn, &log->write_start_lsn);
    else
        WT_ASSIGN_LSN(lsn, &log->write_lsn);
    return (0);
}

/*
 * __wt_log_force_sync --
 *     Force a sync of the log and files.
 */
int
__wt_log_force_sync(WT_SESSION_IMPL *session, WT_LSN *min_lsn)
{
    WT_DECL_RET;
    WT_FH *log_fh;
    WT_LOG *log;
    uint64_t fsync_duration_usecs, time_start, time_stop;

    log = S2C(session)->log;
    log_fh = NULL;

    /*
     * We need to wait for the previous log file to get written to disk before we sync out the
     * current one and advance the LSN. Signal the worker thread because we know the LSN has moved
     * into a later log file and there should be a log file ready to close.
     */
    while (log->sync_lsn.l.file < min_lsn->l.file) {
        __wt_cond_signal(session, S2C(session)->log_file_cond);
        __wt_cond_wait(session, log->log_sync_cond, 10000, NULL);
    }
    __wt_spin_lock(session, &log->log_sync_lock);
    WT_ASSERT(session, log->log_dir_fh != NULL);
    /*
     * Sync the directory if the log file entry hasn't been written into the directory.
     */
    if (log->sync_dir_lsn.l.file < min_lsn->l.file) {
        __wt_verbose(session, WT_VERB_LOG,
          "log_force_sync: sync directory %s to LSN %" PRIu32 "/%" PRIu32, log->log_dir_fh->name,
          min_lsn->l.file, min_lsn->l.offset);
        time_start = __wt_clock(session);
        WT_ERR(__wt_fsync(session, log->log_dir_fh, true));
        time_stop = __wt_clock(session);
        fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop, time_start);
        WT_ASSIGN_LSN(&log->sync_dir_lsn, min_lsn);
        WT_STAT_CONN_INCR(session, log_sync_dir);
        WT_STAT_CONN_INCRV(session, log_sync_dir_duration, fsync_duration_usecs);
    }
    /*
     * Sync the log file if needed.
     */
    if (__wt_log_cmp(&log->sync_lsn, min_lsn) < 0) {
        /*
         * Get our own file handle to the log file. It is possible for the file handle in the log
         * structure to change out from under us and either be NULL or point to a different file
         * than we want.
         */
        WT_ERR(__log_openfile(session, min_lsn->l.file, 0, &log_fh));
        __wt_verbose(session, WT_VERB_LOG, "log_force_sync: sync %s to LSN %" PRIu32 "/%" PRIu32,
          log_fh->name, min_lsn->l.file, min_lsn->l.offset);
        time_start = __wt_clock(session);
        WT_ERR(__wt_fsync(session, log_fh, true));
        time_stop = __wt_clock(session);
        fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop, time_start);
        WT_ASSIGN_LSN(&log->sync_lsn, min_lsn);
        WT_STAT_CONN_INCR(session, log_sync);
        WT_STAT_CONN_INCRV(session, log_sync_duration, fsync_duration_usecs);
        __wt_cond_signal(session, log->log_sync_cond);
    }
err:
    __wt_spin_unlock(session, &log->log_sync_lock);
    if (log_fh != NULL)
        WT_TRET(__wt_close(session, &log_fh));
    return (ret);
}

/*
 * __wt_log_needs_recovery --
 *     Return 0 if we encounter a clean shutdown and 1 if recovery must be run in the given
 *     variable.
 */
int
__wt_log_needs_recovery(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn, bool *recp)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_ITEM dummy_key, dummy_value;
    WT_LOG *log;
    uint64_t dummy_txnid;
    uint32_t dummy_fileid, dummy_optype, rectype;

    /*
     * Default is to run recovery always (regardless of whether this connection has logging
     * enabled).
     */
    *recp = true;

    conn = S2C(session);
    log = conn->log;

    if (log == NULL)
        return (0);

    /*
     * See if there are any data modification records between the checkpoint LSN and the end of the
     * log. If there are none then we can skip recovery.
     */
    WT_RET(__wt_curlog_open(session, "log:", NULL, &c));
    c->set_key(c, ckp_lsn->l.file, ckp_lsn->l.offset, 0);
    if ((ret = c->search(c)) == 0) {
        while ((ret = c->next(c)) == 0) {
            /*
             * The only thing we care about is the rectype.
             */
            WT_ERR(c->get_value(
              c, &dummy_txnid, &rectype, &dummy_optype, &dummy_fileid, &dummy_key, &dummy_value));
            if (rectype == WT_LOGREC_COMMIT)
                break;
        }
        /*
         * If we get to the end of the log, we can skip recovery.
         */
        if (ret == WT_NOTFOUND) {
            *recp = false;
            ret = 0;
        }
    } else if (ret == WT_NOTFOUND)
        /*
         * We should always find the checkpoint LSN as it now points to the beginning of a written
         * log record. But if we're running recovery on an earlier database we may not. In that
         * case, we need to run recovery, don't return an error.
         */
        ret = 0;
    else
        WT_ERR(ret);

err:
    WT_TRET(c->close(c));
    return (ret);
}

/*
 * __wt_log_written_reset --
 *     Interface to reset the amount of log written during this checkpoint period. Called from the
 *     checkpoint code.
 */
void
__wt_log_written_reset(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        conn->log->log_written = 0;
}

/*
 * __wt_log_get_backup_files --
 *     Retrieve the list of log files for taking a backup, either all of them or only the active
 *     ones (those that are not candidates for removal). The caller is responsible for freeing the
 *     directory list returned.
 */
int
__wt_log_get_backup_files(
  WT_SESSION_IMPL *session, char ***filesp, u_int *countp, uint32_t *maxid, bool active_only)
{
    WT_DECL_RET;
    WT_LOG *log;
    uint32_t id, max, max_file, min_file;
    u_int count, i;
    char **files;

    *filesp = NULL;
    *countp = 0;
    *maxid = 0;

    id = 0;
    log = S2C(session)->log;

    /*
     * Capture the next file utilized for writing to the log, before forcing a new log file. This
     * represents the latest journal file that needs to be copied. Note the checkpoint selected for
     * backup may be writing to an even later log file. In that case, copying the journal files is
     * correct, but wasteful.
     */
    max_file = log->alloc_lsn.l.file;

    /*
     * Capture the journal file the current checkpoint started in. The current checkpoint or a later
     * one may be selected for backing up, requiring log files as early as this file. Together with
     * max_file, this defines the range of journal files to include.
     */
    min_file = log->ckpt_lsn.l.file;

    /*
     * Force the current slot to get written to the file. Also switch to using a new log file. That
     * log file will be removed from the list of files returned. New writes will not be included in
     * the backup.
     */
    F_SET(log, WT_LOG_FORCE_NEWFILE);
    WT_RET(__wt_log_force_write(session, 1, NULL));
    WT_RET(__log_get_files(session, WT_LOG_FILENAME, &files, &count));

    for (max = 0, i = 0; i < count;) {
        WT_ERR(__wt_log_extract_lognum(session, files[i], &id));
        if ((active_only && id < min_file) || id > max_file) {
            /*
             * Any files not being returned are individually freed and the array adjusted.
             */
            __wt_free(session, files[i]);
            files[i] = files[count - 1];
            files[--count] = NULL;
        } else {
            if (id > max)
                max = id;
            i++;
        }
    }

    *maxid = max;
    *filesp = files;
    *countp = count;

    /*
     * Only free on error. The caller is responsible for calling free once it is done using the
     * returned list.
     */
    if (0) {
err:
        WT_TRET(__wt_fs_directory_list_free(session, &files, count));
    }
    return (ret);
}

/*
 * __wt_log_filename --
 *     Given a log number, return a WT_ITEM of a generated log file name of the given prefix type.
 */
int
__wt_log_filename(WT_SESSION_IMPL *session, uint32_t id, const char *file_prefix, WT_ITEM *buf)
{
    return (
      __wt_filename_construct(session, S2C(session)->log_path, file_prefix, UINTMAX_MAX, id, buf));
}

/*
 * __wt_log_extract_lognum --
 *     Given a log file name, extract out the log number.
 */
int
__wt_log_extract_lognum(WT_SESSION_IMPL *session, const char *name, uint32_t *id)
{
    const char *p;

    if (id == NULL || name == NULL)
        WT_RET_MSG(session, EINVAL, "unexpected usage: no id or no name");
    if ((p = strrchr(name, '.')) == NULL ||
      /* NOLINTNEXTLINE(cert-err34-c) */
      sscanf(++p, "%" SCNu32, id) != 1)
        WT_RET_MSG(session, WT_ERROR, "Bad log file name '%s'", name);
    return (0);
}

/*
 * __wt_log_reset --
 *     Reset the existing log file to after the given file number. Called from recovery when
 *     toggling logging back on, it was off the previous open but it was on earlier before that
 *     toggle.
 */
int
__wt_log_reset(WT_SESSION_IMPL *session, uint32_t lognum)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    uint32_t old_lognum;
    u_int i, logcount;
    char **logfiles;

    conn = S2C(session);
    log = conn->log;

    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) || log->fileid > lognum)
        return (0);

    WT_ASSERT(session, F_ISSET(conn, WT_CONN_RECOVERING));
    WT_ASSERT(session, !F_ISSET(conn, WT_CONN_READONLY));
    /*
     * We know we're single threaded and called from recovery only when toggling logging back on.
     * Therefore the only log files we have are old and outdated and the new one created when
     * logging opened before recovery. We have to remove all old log files first and then create the
     * new one so that log file numbers are contiguous in the file system.
     */
    WT_RET(__wt_close(session, &log->log_fh));
    WT_RET(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &old_lognum));
        WT_ASSERT(session, old_lognum < lognum || lognum == 1);
        WT_ERR(__wt_log_remove(session, WT_LOG_FILENAME, old_lognum));
    }
    log->fileid = lognum;

    /* Send in true to update connection creation LSNs. */
    WT_WITH_SLOT_LOCK(session, log, ret = __log_newfile(session, true, NULL));
    WT_ERR(__wt_log_slot_init(session, false));
err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __log_prealloc --
 *     Pre-allocate a log file.
 */
static int
__log_prealloc(WT_SESSION_IMPL *session, WT_FH *fh)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;

    /*
     * If the user configured zero filling, pre-allocate the log file manually. Otherwise use the
     * file extension method to create and zero the log file based on what is available.
     */
    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ZERO_FILL))
        return (__wt_file_zero(session, fh, log->first_record, conn->log_file_max));

    /* If configured to not extend the file, we're done. */
    if (conn->log_extend_len == 0)
        return (0);

    /*
     * We have exclusive access to the log file and there are no other writes happening
     * concurrently, so there are no locking issues.
     */
    ret = __wt_fextend(session, fh, conn->log_extend_len);
    return (ret == EBUSY || ret == ENOTSUP ? 0 : ret);
}

/*
 * __log_size_fit --
 *     Return whether or not recsize will fit in the log file.
 */
static int
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint64_t recsize)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;
    return (
      lsn->l.offset == log->first_record || lsn->l.offset + (wt_off_t)recsize < conn->log_file_max);
}

/*
 * __log_decompress --
 *     Decompress a log record.
 */
static int
__log_decompress(WT_SESSION_IMPL *session, WT_ITEM *in, WT_ITEM *out)
{
    WT_COMPRESSOR *compressor;
    WT_CONNECTION_IMPL *conn;
    WT_LOG_RECORD *logrec;
    size_t result_len, skip;
    uint32_t uncompressed_size;

    conn = S2C(session);
    logrec = (WT_LOG_RECORD *)in->mem;
    skip = WT_LOG_COMPRESS_SKIP;
    compressor = conn->log_compressor;
    if (compressor == NULL || compressor->decompress == NULL)
        WT_RET_MSG(session, WT_ERROR, "Compressed record with no configured compressor");
    uncompressed_size = logrec->mem_len;
    WT_RET(__wt_buf_initsize(session, out, uncompressed_size));
    memcpy(out->mem, in->mem, skip);
    WT_RET(compressor->decompress(compressor, &session->iface, (uint8_t *)in->mem + skip,
      in->size - skip, (uint8_t *)out->mem + skip, uncompressed_size - skip, &result_len));

    /*
     * If checksums were turned off because we're depending on the decompression to fail on any
     * corrupted data, we'll end up here after corruption happens. If we're salvaging the file, it's
     * OK, otherwise it's really, really bad.
     */
    if (result_len != uncompressed_size - WT_LOG_COMPRESS_SKIP)
        WT_RET_MSG(session, WT_ERROR, "decompression failed with incorrect size");

    return (0);
}

/*
 * __log_decrypt --
 *     Decrypt a log record.
 */
static int
__log_decrypt(WT_SESSION_IMPL *session, WT_ITEM *in, WT_ITEM *out)
{
    WT_CONNECTION_IMPL *conn;
    WT_ENCRYPTOR *encryptor;
    WT_KEYED_ENCRYPTOR *kencryptor;

    conn = S2C(session);
    kencryptor = conn->kencryptor;
    if (kencryptor == NULL || (encryptor = kencryptor->encryptor) == NULL ||
      encryptor->decrypt == NULL)
        WT_RET_MSG(session, WT_ERROR, "Encrypted record with no configured decrypt method");

    return (__wt_decrypt(session, encryptor, WT_LOG_ENCRYPT_SKIP, in, out));
}

/*
 * __wt_log_fill --
 *     Copy a thread's log records into the assigned slot.
 */
int
__wt_log_fill(
  WT_SESSION_IMPL *session, WT_MYSLOT *myslot, bool force, WT_ITEM *record, WT_LSN *lsnp)
{
    WT_DECL_RET;

    /*
     * Call write or copy into the buffer. For now the offset is the real byte offset. If the offset
     * becomes a unit of WT_LOG_ALIGN this is where we would multiply by WT_LOG_ALIGN to get the
     * real file byte offset for write().
     */
    if (!force && !F_ISSET(myslot, WT_MYSLOT_UNBUFFERED))
        memcpy((char *)myslot->slot->slot_buf.mem + myslot->offset, record->mem, record->size);
    else
        /*
         * If this is a force or unbuffered write, write it now.
         */
        WT_ERR(__log_fs_write(session, myslot->slot,
          myslot->offset + myslot->slot->slot_start_offset, record->size, record->mem));

    WT_STAT_CONN_INCRV(session, log_bytes_written, record->size);
    if (lsnp != NULL) {
        *lsnp = myslot->slot->slot_start_lsn;
        lsnp->l.offset += (uint32_t)myslot->offset;
    }
err:
    if (ret != 0 && myslot->slot->slot_error == 0)
        myslot->slot->slot_error = ret;
    return (ret);
}

/*
 * __log_file_header --
 *     Create and write a log file header into a file handle. If writing into the main log, it will
 *     be called locked. If writing into a pre-allocated log, it will be called unlocked.
 */
static int
__log_file_header(WT_SESSION_IMPL *session, WT_FH *fh, WT_LSN *end_lsn, bool prealloc)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOGSLOT tmp;
    WT_LOG_DESC *desc;
    WT_LOG_RECORD *logrec;
    WT_MYSLOT myslot;

    conn = S2C(session);
    log = conn->log;

    /*
     * Set up the log descriptor record. Use a scratch buffer to get correct alignment for direct
     * I/O.
     */
    WT_ASSERT(session, sizeof(WT_LOG_DESC) < log->allocsize);
    WT_RET(__wt_scr_alloc(session, log->allocsize, &buf));
    memset(buf->mem, 0, log->allocsize);
    buf->size = log->allocsize;

    logrec = (WT_LOG_RECORD *)buf->mem;
    desc = (WT_LOG_DESC *)logrec->record;
    desc->log_magic = WT_LOG_MAGIC;
    desc->version = log->log_version;
    desc->log_size = (uint64_t)conn->log_file_max;
    __wt_log_desc_byteswap(desc);

    /*
     * Now that the record is set up, initialize the record header.
     *
     * Checksum a little-endian version of the header, and write everything in little-endian format.
     * The checksum is (potentially) returned in a big-endian format, swap it into place in a
     * separate step.
     */
    logrec->len = log->allocsize;
    logrec->checksum = 0;
    __wt_log_record_byteswap(logrec);
    logrec->checksum = __wt_checksum(logrec, log->allocsize);
#ifdef WORDS_BIGENDIAN
    logrec->checksum = __wt_bswap32(logrec->checksum);
#endif

    WT_CLEAR(tmp);
    memset(&myslot, 0, sizeof(myslot));
    myslot.slot = &tmp;

    /*
     * We may recursively call __wt_log_acquire to allocate log space for the log descriptor record.
     * Call __wt_log_fill to write it, but we do not need to call __wt_log_release because we're not
     * waiting for any earlier operations to complete.
     */
    if (prealloc) {
        WT_ASSERT(session, fh != NULL);
        tmp.slot_fh = fh;
    } else {
        WT_ASSERT(session, fh == NULL);
        WT_ERR(__wt_log_acquire(session, log->allocsize, &tmp));
    }
    WT_ERR(__wt_log_fill(session, &myslot, true, buf, NULL));
    /*
     * Make sure the header gets to disk.
     */
    WT_ERR(__wt_fsync(session, tmp.slot_fh, true));
    if (end_lsn != NULL)
        WT_ASSIGN_LSN(end_lsn, &tmp.slot_end_lsn);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __log_openfile --
 *     Open a log file with the given log file number and return the WT_FH.
 */
static int
__log_openfile(WT_SESSION_IMPL *session, uint32_t id, uint32_t flags, WT_FH **fhp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    u_int wtopen_flags;

    conn = S2C(session);
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    /*
     * If we are creating the file then we use a temporary file name. Otherwise it is a log file
     * name.
     */
    if (LF_ISSET(WT_LOG_OPEN_CREATE_OK)) {
        wtopen_flags = WT_FS_OPEN_CREATE;
        WT_ERR(__wt_log_filename(session, id, WT_LOG_TMPNAME, buf));
    } else {
        wtopen_flags = 0;
        WT_ERR(__wt_log_filename(session, id, WT_LOG_FILENAME, buf));
    }
    __wt_verbose(session, WT_VERB_LOG, "opening log %s", (const char *)buf->data);
    if (FLD_ISSET(conn->direct_io, WT_DIRECT_IO_LOG))
        FLD_SET(wtopen_flags, WT_FS_OPEN_DIRECTIO);
    WT_ERR(__wt_open(session, buf->data, WT_FS_OPEN_FILE_TYPE_LOG, wtopen_flags, fhp));
err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __log_open_verify --
 *     Open a log file with the given log file number, verify its header and return various pieces
 *     of system information about this log file.
 */
static int
__log_open_verify(WT_SESSION_IMPL *session, uint32_t id, WT_FH **fhp, WT_LSN *lsnp,
  uint16_t *versionp, bool *need_salvagep)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_FH *fh;
    WT_LOG *log;
    WT_LOG_DESC *desc;
    WT_LOG_RECORD *logrec;
    uint32_t allocsize, rectype;
    const uint8_t *end, *p;
    bool need_salvage, salvage_mode;

    conn = S2C(session);
    fh = NULL;
    log = conn->log;
    need_salvage = false;
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    salvage_mode = (need_salvagep != NULL && F_ISSET(conn, WT_CONN_SALVAGE));

    if (log == NULL)
        allocsize = WT_LOG_ALIGN;
    else
        allocsize = log->allocsize;
    if (lsnp != NULL)
        WT_ZERO_LSN(lsnp);
    WT_ERR(__wt_buf_grow(session, buf, allocsize));
    memset(buf->mem, 0, allocsize);

    /*
     * Any operation that fails from here on out indicates corruption that could be salvaged.
     */
    need_salvage = true;

    /*
     * Read in the log file header and verify it.
     */
    WT_ERR(__log_openfile(session, id, 0, &fh));
    WT_ERR(__log_fs_read(session, fh, 0, allocsize, buf->mem));
    logrec = (WT_LOG_RECORD *)buf->mem;
    __wt_log_record_byteswap(logrec);
    desc = (WT_LOG_DESC *)logrec->record;
    __wt_log_desc_byteswap(desc);
    if (desc->log_magic != WT_LOG_MAGIC) {
        if (salvage_mode)
            WT_ERR_MSG(session, WT_ERROR, "log file %s corrupted: Bad magic number %" PRIu32,
              fh->name, desc->log_magic);
        else
            WT_ERR_MSG(session, WT_TRY_SALVAGE, "log file %s corrupted: Bad magic number %" PRIu32,
              fh->name, desc->log_magic);
    }
    /*
     * We cannot read future log file formats.
     */
    if (desc->version > WT_LOG_VERSION)
        WT_ERR_MSG(session, WT_ERROR,
          "unsupported WiredTiger file version: this build only supports versions up to %d, and "
          "the file is version %" PRIu16,
          WT_LOG_VERSION, desc->version);

    /*
     * We error if the log version is less than the required minimum or larger than the required
     * maximum.
     */
    if (__wt_version_defined(conn->compat_req_max) && desc->version > conn->log_req_max)
        WT_ERR_MSG(session, WT_ERROR,
          WT_COMPAT_MSG_PREFIX
          "unsupported WiredTiger file version: this build requires a maximum version of %" PRIu16
          ", and the file is version %" PRIu16,
          conn->log_req_max, desc->version);

    if (__wt_version_defined(conn->compat_req_min) && desc->version < conn->log_req_min)
        WT_ERR_MSG(session, WT_ERROR,
          WT_COMPAT_MSG_PREFIX
          "unsupported WiredTiger file version: this build requires a minimum version of %" PRIu16
          ", and the file is version %" PRIu16,
          conn->log_req_min, desc->version);

    /*
     * Set up the return values since the header is valid.
     */
    if (versionp != NULL)
        *versionp = desc->version;

    /*
     * Skip reading in the previous LSN if log file is an old version or if the caller doesn't care
     * about the LSN. Otherwise read that record in and set up the LSN. We already have a buffer
     * that is the correct size. Reuse it.
     */
    if (lsnp == NULL || (desc->version < WT_LOG_VERSION_SYSTEM))
        goto err;

    memset(buf->mem, 0, allocsize);
    WT_ERR(__log_fs_read(session, fh, allocsize, allocsize, buf->mem));
    logrec = (WT_LOG_RECORD *)buf->mem;
    /*
     * We have a valid header but the system record is not there. The log ends here. Return without
     * setting the LSN.
     */
    if (logrec->len == 0) {
        __wt_verbose(session, WT_VERB_LOG, "Log %s found empty log after header", fh->name);
        goto err;
    }

    if (!__log_checksum_match(buf, allocsize))
        WT_ERR_MSG(session, WT_ERROR, "%s: System log record checksum mismatch", fh->name);
    __wt_log_record_byteswap(logrec);
    p = WT_LOG_SKIP_HEADER(buf->data);
    end = (const uint8_t *)buf->data + allocsize;
    WT_ERR(__wt_logrec_read(session, &p, end, &rectype));
    if (rectype != WT_LOGREC_SYSTEM)
        WT_ERR_MSG(session, WT_ERROR, "System log record missing");
    WT_ERR(__wt_log_recover_system(session, &p, end, lsnp));

err:
    __wt_scr_free(session, &buf);

    /*
     * Return the file handle if needed, otherwise close it.
     */
    if (fhp != NULL && ret == 0)
        *fhp = fh;
    else if (ret != 0 && need_salvage && salvage_mode) {
        /* Let the caller know this file must be salvaged. */
        ret = 0;
        WT_TRET(__wt_close(session, &fh));
        if (fhp != NULL)
            *fhp = NULL;
        *need_salvagep = true;
    } else
        WT_TRET(__wt_close(session, &fh));

    return (ret);
}

/*
 * __log_record_verify --
 *     Check that values of the log record header are valid. No byteswap of the header has been done
 *     at this point.
 */
static int
__log_record_verify(
  WT_SESSION_IMPL *session, WT_FH *log_fh, uint32_t offset, WT_LOG_RECORD *logrecp, bool *corrupt)
{
    WT_LOG_RECORD logrec;
    size_t i;

    *corrupt = false;

    /*
     * Make our own copy of the header so we can get the bytes in the proper order.
     */
    logrec = *logrecp;
    __wt_log_record_byteswap(&logrec);

    if (F_ISSET(&logrec, ~(WT_LOG_RECORD_ALL_FLAGS))) {
        __wt_verbose_notice(session, WT_VERB_LOG,
          "%s: log record at position %" PRIu32 " has flag corruption 0x%" PRIx16, log_fh->name,
          offset, logrec.flags);
        *corrupt = true;
    }
    for (i = 0; i < sizeof(logrec.unused); i++)
        if (logrec.unused[i] != 0) {
            __wt_verbose_notice(session, WT_VERB_LOG,
              "%s: log record at position %" PRIu32 " has unused[%" WT_SIZET_FMT
              "] corruption 0x%" PRIx8,
              log_fh->name, offset, i, logrec.unused[i]);
            *corrupt = true;
        }
    if (logrec.mem_len != 0 &&
      !F_ISSET(&logrec, WT_LOG_RECORD_COMPRESSED | WT_LOG_RECORD_ENCRYPTED)) {
        __wt_verbose_notice(session, WT_VERB_LOG,
          "%s: log record at position %" PRIu32 " has memory len corruption 0x%" PRIx32,
          log_fh->name, offset, logrec.mem_len);
        *corrupt = true;
    }
    if (logrec.len <= offsetof(WT_LOG_RECORD, record)) {
        __wt_verbose_notice(session, WT_VERB_LOG,
          "%s: log record at position %" PRIu32 " has record len corruption 0x%" PRIx32,
          log_fh->name, offset, logrec.len);
        *corrupt = true;
    }
    return (0);
}

/*
 * __log_alloc_prealloc --
 *     Look for a pre-allocated log file and rename it to use as the next real log file. Called
 *     locked.
 */
static int
__log_alloc_prealloc(WT_SESSION_IMPL *session, uint32_t to_num)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(from_path);
    WT_DECL_ITEM(to_path);
    WT_DECL_RET;
    WT_LOG *log;
    uint32_t from_num;
    u_int logcount;
    char **logfiles;
    bool locked;

    conn = S2C(session);
    log = conn->log;
    logfiles = NULL;
    locked = false;

    /*
     * If there are no pre-allocated files, return WT_NOTFOUND.
     */
    WT_RET(__log_get_files_single(session, WT_LOG_PREPNAME, &logfiles, &logcount));
    if (logcount == 0)
        return (WT_NOTFOUND);

    /* We have a file to use. */
    WT_ERR(__wt_log_extract_lognum(session, logfiles[0], &from_num));

    WT_ERR(__wt_scr_alloc(session, 0, &from_path));
    WT_ERR(__wt_scr_alloc(session, 0, &to_path));
    WT_ERR(__wt_log_filename(session, from_num, WT_LOG_PREPNAME, from_path));
    WT_ERR(__wt_log_filename(session, to_num, WT_LOG_FILENAME, to_path));
    __wt_spin_lock(session, &log->log_fs_lock);
    locked = true;
    __wt_verbose(session, WT_VERB_LOG, "log_alloc_prealloc: rename log %s to %s",
      (const char *)from_path->data, (const char *)to_path->data);
    WT_STAT_CONN_INCR(session, log_prealloc_used);
    /*
     * All file setup, writing the header and pre-allocation was done before. We only need to rename
     * it.
     */
    WT_ERR(__wt_fs_rename(session, from_path->data, to_path->data, false));

err:
    __wt_scr_free(session, &from_path);
    __wt_scr_free(session, &to_path);
    if (locked)
        __wt_spin_unlock(session, &log->log_fs_lock);
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __log_newfile --
 *     Create the next log file and write the file header record into it.
 */
static int
__log_newfile(WT_SESSION_IMPL *session, bool conn_open, bool *created)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *log_fh;
    WT_LOG *log;
    WT_LSN end_lsn, logrec_lsn;
    u_int yield_cnt;
    bool create_log, skipp;

    conn = S2C(session);
    log = conn->log;

    /*
     * Set aside the log file handle to be closed later. Other threads may still be using it to
     * write to the log. If the log file size is small we could fill a log file before the previous
     * one is closed. Wait for that to close.
     */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));
    for (yield_cnt = 0; log->log_close_fh != NULL;) {
        WT_STAT_CONN_INCR(session, log_close_yields);
        /*
         * Processing slots will conditionally signal the file close server thread. But if we've
         * tried a while, signal the thread directly here.
         */
        __wt_log_wrlsn(session, NULL);
        if (++yield_cnt % WT_THOUSAND == 0) {
            __wt_spin_unlock(session, &log->log_slot_lock);
            __wt_cond_signal(session, conn->log_file_cond);
            __wt_spin_lock(session, &log->log_slot_lock);
        }
        if (++yield_cnt > WT_THOUSAND * 10)
            return (__wt_set_return(session, EBUSY));
        __wt_yield();
    }
    /*
     * Note, the file server worker thread requires the LSN be set once the close file handle is
     * set, force that ordering.
     */
    if (log->log_fh == NULL)
        log->log_close_fh = NULL;
    else {
        WT_ASSIGN_LSN(&log->log_close_lsn, &log->alloc_lsn);
        WT_PUBLISH(log->log_close_fh, log->log_fh);
    }
    log->fileid++;

    /*
     * If pre-allocating log files look for one; otherwise, or if we don't find one, create a log
     * file. We can't use pre-allocated log files while a hot backup is in progress: applications
     * can copy the files in any way they choose, and a log file rename might confuse things.
     */
    create_log = true;
    if (conn->log_prealloc > 0 && conn->hot_backup_start == 0) {
        WT_WITH_HOTBACKUP_READ_LOCK(
          session, ret = __log_alloc_prealloc(session, log->fileid), &skipp);

        if (!skipp) {
            /*
             * If ret is 0 it means we found a pre-allocated file. If ret is WT_NOTFOUND, create the
             * new log file and signal the server, we missed our pre-allocation. If ret is non-zero
             * but not WT_NOTFOUND, return the error.
             */
            WT_RET_NOTFOUND_OK(ret);
            if (ret == 0)
                create_log = false;
            else {
                WT_STAT_CONN_INCR(session, log_prealloc_missed);
                if (conn->log_cond != NULL)
                    __wt_cond_signal(session, conn->log_cond);
            }
        }
    }
    /*
     * If we need to create the log file, do so now.
     */
    if (create_log) {
        /*
         * Increment the missed pre-allocated file counter only if a hot backup is not in progress.
         * We are deliberately not using pre-allocated log files during backup (see comment above).
         */
        if (conn->hot_backup_start == 0)
            log->prep_missed++;
        WT_RET(__wt_log_allocfile(session, log->fileid, WT_LOG_FILENAME));
    }
    /*
     * Since the file system clears the output file handle pointer before searching the handle list
     * and filling in the new file handle, we must pass in a local file handle. Otherwise there is a
     * wide window where another thread could see a NULL log file handle.
     */
    WT_RET(__log_open_verify(session, log->fileid, &log_fh, NULL, NULL, NULL));
    /*
     * Write the LSN at the end of the last record in the previous log file as the first record in
     * this log file.
     */
    if (log->fileid == 1)
        WT_INIT_LSN(&logrec_lsn);
    else
        WT_ASSIGN_LSN(&logrec_lsn, &log->alloc_lsn);
    /*
     * We need to setup the LSNs. Set the end LSN and alloc LSN to the end of the header.
     */
    WT_SET_LSN(&log->alloc_lsn, log->fileid, WT_LOG_END_HEADER);
    /*
     * If we're running the version where we write a system record do so now and update the
     * alloc_lsn.
     */
    if (log->log_version >= WT_LOG_VERSION_SYSTEM) {
        WT_RET(__wt_log_system_record(session, log_fh, &logrec_lsn));
        WT_SET_LSN(&log->alloc_lsn, log->fileid, log->first_record);
    }
    WT_ASSIGN_LSN(&end_lsn, &log->alloc_lsn);
    WT_PUBLISH(log->log_fh, log_fh);

    /*
     * If we're called from connection creation code, we need to update the LSNs since we're the
     * only write in progress.
     */
    if (conn_open) {
        WT_RET(__wt_fsync(session, log->log_fh, true));
        WT_ASSIGN_LSN(&log->sync_lsn, &end_lsn);
        WT_ASSIGN_LSN(&log->write_lsn, &end_lsn);
        WT_ASSIGN_LSN(&log->write_start_lsn, &end_lsn);
    }
    WT_ASSIGN_LSN(&log->dirty_lsn, &log->alloc_lsn);
    if (created != NULL)
        *created = create_log;
    return (0);
}

/*
 * __log_set_version --
 *     Set version related information under lock.
 */
static int
__log_set_version(
  WT_SESSION_IMPL *session, uint16_t version, uint32_t first_rec, bool live_chg, bool downgrade)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;

    log->log_version = version;
    log->first_record = first_rec;
    if (downgrade)
        FLD_SET(conn->log_flags, WT_CONN_LOG_DOWNGRADED);
    else
        FLD_CLR(conn->log_flags, WT_CONN_LOG_DOWNGRADED);
    if (live_chg)
        F_SET(log, WT_LOG_FORCE_NEWFILE);
    if (!F_ISSET(conn, WT_CONN_READONLY))
        return (__log_prealloc_remove(session));

    return (0);
}

/*
 * __wt_log_set_version --
 *     Change the version number in logging. Will be done with locking. We need to force the log
 *     file to advance and remove all old pre-allocated files.
 */
int
__wt_log_set_version(WT_SESSION_IMPL *session, uint16_t version, uint32_t first_rec, bool downgrade,
  bool live_chg, uint32_t *lognump)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;

    /*
     * The steps are:
     * - Set up versions and remove files under lock.
     * - Set a flag so that the next slot change forces a file change.
     * - Force out the slot that is currently active in the current log.
     * - Write a log record to force a record into the new log file.
     */
    WT_WITH_SLOT_LOCK(
      session, log, ret = __log_set_version(session, version, first_rec, live_chg, downgrade));
    if (!live_chg)
        return (ret);
    WT_ERR(ret);
    /*
     * A new log file will be used when we force out the earlier slot.
     */
    WT_ERR(__wt_log_force_write(session, 1, NULL));

    /*
     * We need to write a record to the new version log file so that a potential checkpoint finds
     * LSNs in that new log file and an remove correctly removes all earlier logs. Write an internal
     * printf record.
     */
    WT_ERR(__wt_log_printf(session, "COMPATIBILITY: Version now %" PRIu16, log->log_version));
    if (lognump != NULL)
        *lognump = log->alloc_lsn.l.file;
err:
    return (ret);
}

/*
 * __wt_log_acquire --
 *     Called serially when switching slots. Can be called recursively from __log_newfile when we
 *     change log files.
 */
int
__wt_log_acquire(WT_SESSION_IMPL *session, uint64_t recsize, WT_LOGSLOT *slot)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    bool created_log;

    conn = S2C(session);
    log = conn->log;
    created_log = true;
    /*
     * Add recsize to alloc_lsn. Save our starting LSN where the previous allocation finished for
     * the release LSN. That way when log files switch, we're waiting for the correct LSN from
     * outstanding writes.
     */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));
    /*
     * We need to set the release LSN earlier, before a log file change.
     */
    WT_ASSIGN_LSN(&slot->slot_release_lsn, &log->alloc_lsn);
    /*
     * Make sure that the size can fit in the file. Proactively switch if it cannot. This reduces,
     * but does not eliminate, log files that exceed the maximum file size. We want to minimize the
     * risk of an error due to no space.
     */
    if (F_ISSET(log, WT_LOG_FORCE_NEWFILE) || !__log_size_fit(session, &log->alloc_lsn, recsize)) {
        WT_RET(__log_newfile(session, false, &created_log));
        F_CLR(log, WT_LOG_FORCE_NEWFILE);
        if (log->log_close_fh != NULL)
            F_SET(slot, WT_SLOT_CLOSEFH);
    }

    /*
     * Pre-allocate on the first real write into the log file, if it was just created (i.e. not
     * pre-allocated).
     */
    if (log->alloc_lsn.l.offset == log->first_record && created_log)
        WT_RET(__log_prealloc(session, log->log_fh));
    /*
     * Initialize the slot for activation.
     */
    __wt_log_slot_activate(session, slot);

    return (0);
}

/*
 * __log_truncate_file --
 *     Truncate a log file to the specified offset. If the underlying file system doesn't support
 *     truncate then we need to zero out the rest of the file, doing an effective truncate.
 */
static int
__log_truncate_file(WT_SESSION_IMPL *session, WT_FH *log_fh, wt_off_t offset)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    bool skipp;

    conn = S2C(session);
    log = conn->log;

    if (!F_ISSET(log, WT_LOG_TRUNCATE_NOTSUP) && conn->hot_backup_start == 0) {
        WT_WITH_HOTBACKUP_READ_LOCK(session, ret = __wt_ftruncate(session, log_fh, offset), &skipp);
        if (!skipp) {
            if (ret != ENOTSUP)
                return (ret);
            F_SET(log, WT_LOG_TRUNCATE_NOTSUP);
        }
    }

    return (__wt_file_zero(session, log_fh, offset, conn->log_file_max));
}

/*
 * __log_truncate --
 *     Truncate the log to the given LSN. If this_log is set, it will only truncate the log file
 *     indicated in the given LSN. If not set, it will truncate between the given LSN and the
 *     trunc_lsn. That is, since we pre-allocate log files, it will free that space and allow the
 *     log to be traversed. We use the trunc_lsn because logging has already opened the new/next log
 *     file before recovery ran. If salvage_mode is set, we verify headers of log files visited and
 *     recreate them if they are damaged. This function assumes we are in recovery or other
 *     dedicated time and not during live running.
 */
static int
__log_truncate(WT_SESSION_IMPL *session, WT_LSN *lsn, bool this_log, bool salvage_mode)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *log_fh;
    WT_LOG *log;
    uint32_t lognum, salvage_first, salvage_last;
    u_int i, logcount;
    char **logfiles;
    bool need_salvage, opened;

    conn = S2C(session);
    log = conn->log;
    log_fh = NULL;
    logcount = 0;
    logfiles = NULL;
    salvage_first = salvage_last = 0;
    need_salvage = false;

    /*
     * Truncate the log file to the given LSN.
     *
     * It's possible the underlying file system doesn't support truncate (there are existing
     * examples), which is fine, but we don't want to repeatedly do the setup work just to find that
     * out every time. Check before doing work, and if there's a not-supported error, turn off
     * future truncates.
     */
    WT_ERR(__log_openfile(session, lsn->l.file, 0, &log_fh));
    WT_ERR(__log_truncate_file(session, log_fh, lsn->l.offset));
    WT_ERR(__wt_fsync(session, log_fh, true));
    WT_ERR(__wt_close(session, &log_fh));

    if (salvage_mode)
        __wt_verbose_notice(
          session, WT_VERB_LOG, "salvage: log file %" PRIu32 " truncated", lsn->l.file);

    /*
     * If we just want to truncate the current log, return and skip looking for intervening logs.
     */
    if (this_log)
        goto err;
    WT_ERR(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        if (lognum > lsn->l.file && lognum < log->trunc_lsn.l.file) {
            opened = false;
            if (salvage_mode) {
                /*
                 * When salvaging, we verify that the header of the log file is valid. If not,
                 * create a new, empty one.
                 */
                need_salvage = false;
                WT_ERR(__log_open_verify(session, lognum, &log_fh, NULL, NULL, &need_salvage));
                if (need_salvage) {
                    WT_ASSERT(session, log_fh == NULL);
                    WT_ERR(__wt_log_remove(session, WT_LOG_FILENAME, lognum));
                    WT_ERR(__wt_log_allocfile(session, lognum, WT_LOG_FILENAME));
                } else
                    opened = true;

                if (salvage_first == 0)
                    salvage_first = lognum;
                salvage_last = lognum;
            }
            if (!opened)
                WT_ERR(__log_openfile(session, lognum, 0, &log_fh));
            /*
             * If there are intervening files pre-allocated, truncate them to the end of the log
             * file header.
             */
            WT_ERR(__log_truncate_file(session, log_fh, log->first_record));
            WT_ERR(__wt_fsync(session, log_fh, true));
            WT_ERR(__wt_close(session, &log_fh));
        }
    }
err:
    WT_TRET(__wt_close(session, &log_fh));
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    if (salvage_first != 0) {
        if (salvage_last > salvage_first)
            __wt_verbose_notice(session, WT_VERB_LOG,
              "salvage: log files %" PRIu32 "-%" PRIu32 " truncated at beginning", salvage_first,
              salvage_last);
        else
            __wt_verbose_notice(session, WT_VERB_LOG,
              "salvage: log file %" PRIu32 " truncated at beginning", salvage_first);
    }
    return (ret);
}

/*
 * __wt_log_allocfile --
 *     Given a log number, create a new log file by writing the header, pre-allocating the file and
 *     moving it to the destination name.
 */
int
__wt_log_allocfile(WT_SESSION_IMPL *session, uint32_t lognum, const char *dest)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(from_path);
    WT_DECL_ITEM(to_path);
    WT_DECL_RET;
    WT_FH *log_fh;
    WT_LOG *log;
    uint32_t tmp_id;

    conn = S2C(session);
    log = conn->log;
    log_fh = NULL;

    /*
     * Preparing a log file entails creating a temporary file:
     * - Writing the header.
     * - Truncating to the offset of the first record.
     * - Pre-allocating the file if needed.
     * - Renaming it to the desired file name.
     */
    WT_RET(__wt_scr_alloc(session, 0, &from_path));
    WT_ERR(__wt_scr_alloc(session, 0, &to_path));
    tmp_id = __wt_atomic_add32(&log->tmp_fileid, 1);
    WT_ERR(__wt_log_filename(session, tmp_id, WT_LOG_TMPNAME, from_path));
    WT_ERR(__wt_log_filename(session, lognum, dest, to_path));
    __wt_spin_lock(session, &log->log_fs_lock);
    /*
     * Set up the temporary file.
     */
    WT_ERR(__log_openfile(session, tmp_id, WT_LOG_OPEN_CREATE_OK, &log_fh));
    WT_ERR(__log_file_header(session, log_fh, NULL, true));
    WT_ERR(__log_prealloc(session, log_fh));
    WT_ERR(__wt_fsync(session, log_fh, true));
    WT_ERR(__wt_close(session, &log_fh));
    __wt_verbose(session, WT_VERB_LOG, "log_allocfile: rename %s to %s",
      (const char *)from_path->data, (const char *)to_path->data);
    /*
     * Rename it into place and make it available.
     */
    WT_ERR(__wt_fs_rename(session, from_path->data, to_path->data, false));

err:
    __wt_scr_free(session, &from_path);
    __wt_scr_free(session, &to_path);
    __wt_spin_unlock(session, &log->log_fs_lock);
    WT_TRET(__wt_close(session, &log_fh));
    return (ret);
}

/*
 * __wt_log_remove --
 *     Given a log number, remove that log file.
 */
int
__wt_log_remove(WT_SESSION_IMPL *session, const char *file_prefix, uint32_t lognum)
{
    WT_DECL_ITEM(path);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, 0, &path));
    WT_ERR(__wt_log_filename(session, lognum, file_prefix, path));
    __wt_verbose(session, WT_VERB_LOG, "log_remove: remove log %s", (const char *)path->data);
    WT_ERR(__wt_fs_remove(session, path->data, false));
err:
    __wt_scr_free(session, &path);
    return (ret);
}

/*
 * __wt_log_compat_verify --
 *     Verify the last log when opening for the compatibility settings. This is separate because we
 *     need to do it very early in the startup process.
 */
int
__wt_log_compat_verify(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    uint32_t lastlog, lognum;
    u_int i, logcount;
    char **logfiles;

    lastlog = 0;

    WT_ERR(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        lastlog = WT_MAX(lastlog, lognum);
    }
    if (lastlog != 0)
        WT_ERR(__log_open_verify(session, lastlog, NULL, NULL, NULL, NULL));
err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __wt_log_open --
 *     Open the appropriate log file for the connection. The purpose is to find the last log file
 *     that exists, open it and set our initial LSNs to the end of that file. If none exist, call
 *     __log_newfile to create it.
 */
int
__wt_log_open(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    uint32_t firstlog, lastlog, lognum;
    uint16_t version;
    u_int i, logcount;
    char **logfiles;
    bool need_salvage;

    conn = S2C(session);
    log = conn->log;
    logfiles = NULL;
    logcount = 0;

    /*
     * Open up a file handle to the log directory if we haven't.
     */
    if (log->log_dir_fh == NULL) {
        __wt_verbose(session, WT_VERB_LOG, "log_open: open fh to directory %s", conn->log_path);
        WT_RET(
          __wt_open(session, conn->log_path, WT_FS_OPEN_FILE_TYPE_DIRECTORY, 0, &log->log_dir_fh));
    }

    if (!F_ISSET(conn, WT_CONN_READONLY))
        WT_ERR(__log_prealloc_remove(session));

again:
    /*
     * Now look at the log files and set our LSNs.
     */
    lastlog = 0;
    firstlog = UINT32_MAX;
    need_salvage = false;

    WT_ERR(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
    for (i = 0; i < logcount; i++) {
        WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
        lastlog = WT_MAX(lastlog, lognum);
        firstlog = WT_MIN(firstlog, lognum);
    }
    log->fileid = lastlog;
    __wt_verbose(
      session, WT_VERB_LOG, "log_open: first log %" PRIu32 " last log %" PRIu32, firstlog, lastlog);
    if (firstlog == UINT32_MAX) {
        WT_ASSERT(session, logcount == 0);
        WT_INIT_LSN(&log->first_lsn);
    } else {
        WT_SET_LSN(&log->first_lsn, firstlog, 0);
        /*
         * If we have existing log files, check the last log now before we create a new log file so
         * that we can detect an unsupported version before modifying the file space.
         */
        WT_ERR(__log_open_verify(session, lastlog, NULL, NULL, &version, &need_salvage));

        /*
         * If we were asked to salvage and the last log file was indeed corrupt, remove it and try
         * all over again.
         */
        if (need_salvage) {
            WT_ERR(__wt_log_remove(session, WT_LOG_FILENAME, lastlog));
            __wt_verbose_notice(
              session, WT_VERB_LOG, "salvage: log file %" PRIu32 " removed", lastlog);
            WT_ERR(__wt_fs_directory_list_free(session, &logfiles, logcount));
            logfiles = NULL;
            goto again;
        }
    }

    /*
     * Start logging at the beginning of the next log file, no matter where the previous log file
     * ends.
     */
    if (!F_ISSET(conn, WT_CONN_READONLY)) {
        WT_WITH_SLOT_LOCK(session, log, ret = __log_newfile(session, true, NULL));
        WT_ERR(ret);
    }

    /* If we found log files, save the new state. */
    if (logcount > 0) {
        /*
         * If we're running in a downgraded mode and there are earlier logs detect if they're at a
         * higher version. If so, we need to force recovery (to write a full checkpoint) and force
         * removal to remove all higher version logs.
         */
        if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_DOWNGRADED)) {
            for (i = 0; i < logcount; ++i) {
                WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
                /*
                 * By sending in a NULL file handle, we don't have to close the file.
                 */
                WT_ERR(__log_open_verify(session, lognum, NULL, NULL, &version, NULL));
                /*
                 * If we find any log file at the wrong version set the flag and we're done.
                 */
                if (log->log_version != version) {
                    FLD_SET(conn->log_flags, WT_CONN_LOG_FORCE_DOWNGRADE);
                    break;
                }
            }
        }
        WT_ASSIGN_LSN(&log->trunc_lsn, &log->alloc_lsn);
        FLD_SET(conn->log_flags, WT_CONN_LOG_EXISTED);
    }

err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    if (ret == 0)
        F_SET(log, WT_LOG_OPENED);
    return (ret);
}

/*
 * __wt_log_close --
 *     Close the log file.
 */
int
__wt_log_close(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;

    if (log->log_close_fh != NULL && log->log_close_fh != log->log_fh) {
        __wt_verbose(session, WT_VERB_LOG, "closing old log %s", log->log_close_fh->name);
        if (!F_ISSET(conn, WT_CONN_READONLY))
            WT_RET(__wt_fsync(session, log->log_close_fh, true));
        WT_RET(__wt_close(session, &log->log_close_fh));
    }
    if (log->log_fh != NULL) {
        __wt_verbose(session, WT_VERB_LOG, "closing log %s", log->log_fh->name);
        if (!F_ISSET(conn, WT_CONN_READONLY))
            WT_RET(__wt_fsync(session, log->log_fh, true));
        WT_RET(__wt_close(session, &log->log_fh));
        log->log_fh = NULL;
    }
    if (log->log_dir_fh != NULL) {
        __wt_verbose(session, WT_VERB_LOG, "closing log directory %s", log->log_dir_fh->name);
        if (!F_ISSET(conn, WT_CONN_READONLY))
            WT_RET(__wt_fsync(session, log->log_dir_fh, true));
        WT_RET(__wt_close(session, &log->log_dir_fh));
        log->log_dir_fh = NULL;
    }
    F_CLR(log, WT_LOG_OPENED);
    return (0);
}

/*
 * __log_has_hole --
 *     Determine if the current offset represents a hole in the log file (i.e. there is valid data
 *     somewhere after the hole), or if this is the end of this log file and the remainder of the
 *     file is zeroes.
 */
static int
__log_has_hole(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t log_size, wt_off_t offset,
  wt_off_t *error_offset, bool *hole)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOG_RECORD *logrec;
    wt_off_t off, remainder;
    size_t allocsize, buf_left, bufsz, rdlen;
    char *buf, *p, *zerobuf;
    bool corrupt;

    *error_offset = 0;
    corrupt = *hole = false;

    conn = S2C(session);
    log = conn->log;
    remainder = log_size - offset;

    /*
     * It can be very slow looking for the last real record in the log in very small chunks. Walk a
     * megabyte at a time. If we find a part of the log that is not just zeroes we know this log
     * file has a hole in it.
     */
    buf = zerobuf = NULL;
    if (log == NULL || log->allocsize < WT_MEGABYTE)
        bufsz = WT_MEGABYTE;
    else
        bufsz = log->allocsize;

    if ((size_t)remainder < bufsz)
        bufsz = (size_t)remainder;
    WT_RET(__wt_calloc_def(session, bufsz, &buf));
    WT_ERR(__wt_calloc_def(session, bufsz, &zerobuf));

    /*
     * Read in a chunk starting at the given offset. Compare against a known zero byte chunk.
     */
    for (off = offset; remainder > 0; remainder -= (wt_off_t)rdlen, off += (wt_off_t)rdlen) {
        rdlen = WT_MIN(bufsz, (size_t)remainder);
        WT_ERR(__log_fs_read(session, fh, off, rdlen, buf));
        allocsize = (log == NULL ? WT_LOG_ALIGN : log->allocsize);
        if (memcmp(buf, zerobuf, rdlen) != 0) {
            /*
             * Find where the next log record starts after the hole.
             */
            for (p = buf, buf_left = rdlen; buf_left > 0; buf_left -= rdlen, p += rdlen) {
                rdlen = WT_MIN(allocsize, buf_left);
                if (memcmp(p, zerobuf, rdlen) != 0)
                    break;
            }
            /*
             * A presumed log record begins here where the buffer becomes non-zero. If we have
             * enough of a log record present in the buffer, we either have a valid header or
             * corruption. Verify the header of this record to determine whether it is just a hole
             * or corruption.
             *
             * We don't bother making this check for backup copies, as records may have their
             * beginning zeroed, hence the part after a hole may in fact be the middle of the
             * record.
             */
            if (!F_ISSET(conn, WT_CONN_WAS_BACKUP)) {
                logrec = (WT_LOG_RECORD *)p;
                if (buf_left >= sizeof(WT_LOG_RECORD)) {
                    off += p - buf;
                    WT_ERR(__log_record_verify(session, fh, (uint32_t)off, logrec, &corrupt));
                    if (corrupt)
                        *error_offset = off;
                }
            }
            *hole = true;
            break;
        }
    }

err:
    __wt_free(session, buf);
    __wt_free(session, zerobuf);
    return (ret);
}

/*
 * __log_check_partial_write --
 *     Determine if the log record may be a partial write. If that's possible, return true,
 *     otherwise false. Since the log file is initially zeroed up to a predetermined size, any
 *     record that falls within that boundary that ends in one or more zeroes may be partial (or the
 *     initial record may have been padded with zeroes before writing). The only way we have any
 *     certainty is if the last byte is non-zero, when that happens, we know that the write cannot
 *     be partial.
 */
static bool
__log_check_partial_write(WT_SESSION_IMPL *session, WT_ITEM *buf, uint32_t reclen)
{
    uint8_t *rec;

    WT_UNUSED(session);

    /*
     * We only check the final byte since that's the only way have any certainty. Even if the second
     * to last byte is non-zero and the last byte is zero, that could still technically be the
     * result of a partial write, however unlikely it may be.
     */
    rec = buf->mem;
    return (reclen > 0 && rec[reclen - 1] == 0);
}

/*
 * __wt_log_release --
 *     Release a log slot.
 */
int
__wt_log_release(WT_SESSION_IMPL *session, WT_LOGSLOT *slot, bool *freep)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    WT_LSN sync_lsn;
    uint64_t fsync_duration_usecs, time_start, time_stop;
    int64_t release_buffered, release_bytes;
    bool locked;

    conn = S2C(session);
    log = conn->log;
    locked = false;
    if (freep != NULL)
        *freep = 1;
    release_buffered = WT_LOG_SLOT_RELEASED_BUFFERED(slot->slot_state);
    release_bytes = release_buffered + slot->slot_unbuffered;

    /*
     * Checkpoints can be configured based on amount of log written. Add in this log record to the
     * sum and if needed, signal the checkpoint condition. The logging subsystem manages the
     * accumulated field. There is a bit of layering violation here checking the connection ckpt
     * field and using its condition.
     */
    if (WT_CKPT_LOGSIZE(conn)) {
        log->log_written += (wt_off_t)release_bytes;
        __wt_checkpoint_signal(session, log->log_written);
    }

    /* Write the buffered records */
    if (release_buffered != 0)
        WT_ERR(__log_fs_write(
          session, slot, slot->slot_start_offset, (size_t)release_buffered, slot->slot_buf.mem));

    /*
     * If we have to wait for a synchronous operation, we do not pass handling of this slot off to
     * the worker thread. The caller is responsible for freeing the slot in that case. Otherwise the
     * worker thread will free it.
     */
    if (!F_ISSET(slot, WT_SLOT_FLUSH | WT_SLOT_SYNC_FLAGS)) {
        if (freep != NULL)
            *freep = 0;
        slot->slot_state = WT_LOG_SLOT_WRITTEN;
        /*
         * After this point the worker thread owns the slot. There is nothing more to do but return.
         */
        /*
         * !!! Signaling the wrlsn_cond condition here results in
         * worse performance because it causes more scheduling churn
         * and more walking of the slot pool for a very small number
         * of slots to process.  Don't signal here.
         */
        return (0);
    }

    /*
     * Wait for earlier groups to finish, otherwise there could be holes in the log file.
     */
    WT_STAT_CONN_INCR(session, log_release_write_lsn);
    __log_wait_for_earlier_slot(session, slot);

    WT_ASSIGN_LSN(&log->write_start_lsn, &slot->slot_start_lsn);
    WT_ASSIGN_LSN(&log->write_lsn, &slot->slot_end_lsn);

    WT_ASSERT(session, slot != log->active_slot);
    __wt_cond_signal(session, log->log_write_cond);
    F_CLR(slot, WT_SLOT_FLUSH);

    /*
     * Signal the close thread if needed.
     */
    if (F_ISSET(slot, WT_SLOT_CLOSEFH))
        __wt_cond_signal(session, conn->log_file_cond);

    if (F_ISSET(slot, WT_SLOT_SYNC_DIRTY) && !F_ISSET(slot, WT_SLOT_SYNC) &&
      (ret = __wt_fsync(session, log->log_fh, false)) != 0) {
        /*
         * Ignore ENOTSUP, but don't try again.
         */
        if (ret != ENOTSUP)
            WT_ERR(ret);
        conn->log_dirty_max = 0;
    }

    /*
     * Try to consolidate calls to fsync to wait less. Acquire a spin lock so that threads finishing
     * writing to the log will wait while the current fsync completes and advance log->sync_lsn.
     */
    while (F_ISSET(slot, WT_SLOT_SYNC | WT_SLOT_SYNC_DIR)) {
        /*
         * We have to wait until earlier log files have finished their sync operations. The most
         * recent one will set the LSN to the beginning of our file.
         */
        if (log->sync_lsn.l.file < slot->slot_end_lsn.l.file ||
          __wt_spin_trylock(session, &log->log_sync_lock) != 0) {
            __wt_cond_wait(session, log->log_sync_cond, 10000, NULL);
            continue;
        }
        locked = true;

        /*
         * Record the current end of our update after the lock. That is how far our calls can
         * guarantee.
         */
        WT_ASSIGN_LSN(&sync_lsn, &slot->slot_end_lsn);
        /*
         * Check if we have to sync the parent directory. Some combinations of sync flags may result
         * in the log file not yet stable in its parent directory. Do that now if needed.
         */
        if (F_ISSET(slot, WT_SLOT_SYNC_DIR) && (log->sync_dir_lsn.l.file < sync_lsn.l.file)) {
            WT_ASSERT(session, log->log_dir_fh != NULL);
            __wt_verbose(session, WT_VERB_LOG,
              "log_release: sync directory %s to LSN %" PRIu32 "/%" PRIu32, log->log_dir_fh->name,
              sync_lsn.l.file, sync_lsn.l.offset);
            time_start = __wt_clock(session);
            WT_ERR(__wt_fsync(session, log->log_dir_fh, true));
            time_stop = __wt_clock(session);
            fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_ASSIGN_LSN(&log->sync_dir_lsn, &sync_lsn);
            WT_STAT_CONN_INCR(session, log_sync_dir);
            WT_STAT_CONN_INCRV(session, log_sync_dir_duration, fsync_duration_usecs);
        }

        /*
         * Sync the log file if needed.
         */
        if (F_ISSET(slot, WT_SLOT_SYNC) && __wt_log_cmp(&log->sync_lsn, &slot->slot_end_lsn) < 0) {
            __wt_verbose(session, WT_VERB_LOG,
              "log_release: sync log %s to LSN %" PRIu32 "/%" PRIu32, log->log_fh->name,
              sync_lsn.l.file, sync_lsn.l.offset);
            WT_STAT_CONN_INCR(session, log_sync);
            time_start = __wt_clock(session);
            WT_ERR(__wt_fsync(session, log->log_fh, true));
            time_stop = __wt_clock(session);
            fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_STAT_CONN_INCRV(session, log_sync_duration, fsync_duration_usecs);
            WT_ASSIGN_LSN(&log->sync_lsn, &sync_lsn);
            __wt_cond_signal(session, log->log_sync_cond);
        }
        /*
         * Clear the flags before leaving the loop.
         */
        F_CLR(slot, WT_SLOT_SYNC | WT_SLOT_SYNC_DIR);
        locked = false;
        __wt_spin_unlock(session, &log->log_sync_lock);
    }
err:
    if (locked)
        __wt_spin_unlock(session, &log->log_sync_lock);
    if (ret != 0 && slot->slot_error == 0)
        slot->slot_error = ret;
    return (ret);
}

/*
 * __log_salvage_message --
 *     Show messages consistently for a salvageable error.
 */
static int
__log_salvage_message(
  WT_SESSION_IMPL *session, const char *log_name, const char *extra_msg, wt_off_t offset)
{
    __wt_verbose_notice(session, WT_VERB_LOG,
      "log file %s corrupted%s at position %" PRIuMAX ", truncated", log_name, extra_msg,
      (uintmax_t)offset);
    F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
    return (WT_ERROR);
}

/*
 * __wt_log_scan --
 *     Scan the logs, calling a function on each record found.
 */
int
__wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *start_lsnp, WT_LSN *end_lsnp, uint32_t flags,
  int (*func)(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, WT_LSN *next_lsnp,
    void *cookie, int firstrecord),
  void *cookie)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(decryptitem);
    WT_DECL_ITEM(uncitem);
    WT_DECL_RET;
    WT_FH *log_fh;
    WT_ITEM *cbbuf;
    WT_LOG *log;
    WT_LOG_RECORD *logrec;
    WT_LSN end_lsn, next_lsn, prev_eof, prev_lsn, rd_lsn, start_lsn;
    wt_off_t bad_offset, log_size;
    uint32_t allocsize, firstlog, lastlog, lognum, rdup_len, reclen;
    uint16_t version;
    u_int i, logcount;
    int firstrecord;
    char **logfiles;
    bool corrupt, eol, need_salvage, partial_record;

    conn = S2C(session);
    log = conn->log;
    log_fh = NULL;
    logcount = 0;
    logfiles = NULL;
    corrupt = eol = false;
    firstrecord = 1;
    need_salvage = false;

    /*
     * If the caller did not give us a callback function there is nothing to do.
     */
    if (func == NULL)
        return (0);

    if (start_lsnp != NULL && LF_ISSET(WT_LOGSCAN_FIRST | WT_LOGSCAN_FROM_CKP))
        WT_RET_MSG(session, WT_ERROR, "choose either a start LSN or a start flag");
    /*
     * Set up the allocation size, starting and ending LSNs. The values for those depend on whether
     * logging is currently enabled or not.
     */
    lastlog = 0;
    if (log != NULL) {
        allocsize = log->allocsize;
        WT_ASSIGN_LSN(&end_lsn, &log->alloc_lsn);
        WT_ASSIGN_LSN(&start_lsn, &log->first_lsn);
        if (start_lsnp == NULL) {
            if (LF_ISSET(WT_LOGSCAN_FROM_CKP))
                WT_ASSIGN_LSN(&start_lsn, &log->ckpt_lsn);
            else if (!LF_ISSET(WT_LOGSCAN_FIRST))
                WT_RET_MSG(session, WT_ERROR, "WT_LOGSCAN_FIRST not set");
        }
        lastlog = log->fileid;
    } else {
        /*
         * If logging is not configured, we can still print out the log if log files exist. We just
         * need to set the LSNs from what is in the files versus what is in the live connection.
         */
        /*
         * Set allocsize to the minimum alignment it could be. Larger records and larger allocation
         * boundaries should always be a multiple of this.
         */
        allocsize = WT_LOG_ALIGN;
        firstlog = UINT32_MAX;
        WT_RET(__log_get_files(session, WT_LOG_FILENAME, &logfiles, &logcount));
        if (logcount == 0)
            WT_RET_MSG(session, ENOTSUP, "no log files found");
        for (i = 0; i < logcount; i++) {
            WT_ERR(__wt_log_extract_lognum(session, logfiles[i], &lognum));
            lastlog = WT_MAX(lastlog, lognum);
            firstlog = WT_MIN(firstlog, lognum);
        }
        WT_SET_LSN(&start_lsn, firstlog, 0);
        WT_SET_LSN(&end_lsn, lastlog, 0);
        WT_ERR(__wt_fs_directory_list_free(session, &logfiles, logcount));
    }

    if (start_lsnp != NULL) {
        /*
         * Offsets must be on allocation boundaries. An invalid LSN from a user should just return
         * WT_NOTFOUND. It is not an error. But if it is from recovery, we expect valid LSNs so give
         * more information about that.
         */
        if (start_lsnp->l.offset % allocsize != 0) {
            if (LF_ISSET(WT_LOGSCAN_RECOVER | WT_LOGSCAN_RECOVER_METADATA))
                WT_ERR_MSG(session, WT_NOTFOUND, "__wt_log_scan unaligned LSN %" PRIu32 "/%" PRIu32,
                  start_lsnp->l.file, start_lsnp->l.offset);
            else
                WT_ERR(WT_NOTFOUND);
        }
        /*
         * If the file is in the future it doesn't exist. An invalid LSN from a user should just
         * return WT_NOTFOUND. It is not an error. But if it is from recovery, we expect valid LSNs
         * so give more information about that.
         */
        if (start_lsnp->l.file > lastlog) {
            if (LF_ISSET(WT_LOGSCAN_RECOVER | WT_LOGSCAN_RECOVER_METADATA))
                WT_ERR_MSG(session, WT_NOTFOUND,
                  "__wt_log_scan LSN %" PRIu32 "/%" PRIu32 " larger than biggest log file %" PRIu32,
                  start_lsnp->l.file, start_lsnp->l.offset, lastlog);
            else
                WT_ERR(WT_NOTFOUND);
        }
        /*
         * Log cursors may not know the starting LSN. If an LSN is passed in that it is equal to the
         * smallest LSN, start from the beginning of the log.
         */
        if (!WT_IS_INIT_LSN(start_lsnp))
            WT_ASSIGN_LSN(&start_lsn, start_lsnp);
    }
    WT_ERR(__log_open_verify(session, start_lsn.l.file, &log_fh, &prev_lsn, NULL, &need_salvage));
    if (need_salvage)
        WT_ERR_MSG(session, WT_ERROR, "log file requires salvage");
    WT_ERR(__wt_filesize(session, log_fh, &log_size));
    WT_ASSIGN_LSN(&rd_lsn, &start_lsn);
    if (LF_ISSET(WT_LOGSCAN_RECOVER | WT_LOGSCAN_RECOVER_METADATA))
        __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
          "Recovering log %" PRIu32 " through %" PRIu32, rd_lsn.l.file, end_lsn.l.file);

    WT_ERR(__wt_scr_alloc(session, WT_LOG_ALIGN, &buf));
    WT_ERR(__wt_scr_alloc(session, 0, &decryptitem));
    WT_ERR(__wt_scr_alloc(session, 0, &uncitem));
    for (;;) {
        if (rd_lsn.l.offset + allocsize > log_size) {
advance:
            if (rd_lsn.l.offset == log_size)
                partial_record = false;
            else {
                /*
                 * See if there is anything non-zero at the end of this log file.
                 */
                WT_ERR(__log_has_hole(
                  session, log_fh, log_size, rd_lsn.l.offset, &bad_offset, &partial_record));
                if (bad_offset != 0) {
                    need_salvage = true;
                    WT_ERR(__log_salvage_message(session, log_fh->name, "", bad_offset));
                }
            }
            /*
             * If we read the last record, go to the next file.
             */
            WT_ERR(__wt_close(session, &log_fh));
            log_fh = NULL;
            eol = true;
            /*
             * Truncate this log file before we move to the next.
             */
            if (LF_ISSET(WT_LOGSCAN_RECOVER) && __wt_log_cmp(&rd_lsn, &log->trunc_lsn) < 0) {
                __wt_verbose(session, WT_VERB_LOG, "Truncate end of log %" PRIu32 "/%" PRIu32,
                  rd_lsn.l.file, rd_lsn.l.offset);
                WT_ERR(__log_truncate(session, &rd_lsn, true, false));
            }
            /*
             * If we had a partial record, we'll want to break now after closing and truncating.
             * Although for now log_truncate does not modify the LSN passed in, this code does not
             * assume it is unmodified after that call which is why it uses the boolean set earlier.
             */
            if (partial_record)
                break;
            /*
             * Avoid an error message when we reach end of log by checking here.
             */
            prev_eof = rd_lsn;
            WT_SET_LSN(&rd_lsn, rd_lsn.l.file + 1, 0);
            if (rd_lsn.l.file > end_lsn.l.file)
                break;
            if (LF_ISSET(WT_LOGSCAN_RECOVER | WT_LOGSCAN_RECOVER_METADATA))
                __wt_verbose(session, WT_VERB_RECOVERY_PROGRESS,
                  "Recovering log %" PRIu32 " through %" PRIu32, rd_lsn.l.file, end_lsn.l.file);
            WT_ERR(__log_open_verify(
              session, rd_lsn.l.file, &log_fh, &prev_lsn, &version, &need_salvage));
            if (need_salvage)
                WT_ERR_MSG(session, WT_ERROR, "log file requires salvage");
            /*
             * Opening the log file reads with verify sets up the previous LSN from the first
             * record. This detects a "hole" at the end of the previous log file.
             */
            if (LF_ISSET(WT_LOGSCAN_RECOVER) && !WT_IS_INIT_LSN(&prev_lsn) &&
              !WT_IS_ZERO_LSN(&prev_lsn) && prev_lsn.l.offset != prev_eof.l.offset) {
                WT_ASSERT(session, prev_eof.l.file == prev_lsn.l.file);
                break;
            }
            /*
             * If we read a current version log file without a previous LSN record the log ended
             * after writing that header. We're done.
             */
            if (LF_ISSET(WT_LOGSCAN_RECOVER) && version == WT_LOG_VERSION_SYSTEM &&
              WT_IS_ZERO_LSN(&prev_lsn)) {
                __wt_verbose(session, WT_VERB_LOG,
                  "log_scan: Stopping, no system record detected in %s.", log_fh->name);
                break;
            }
            WT_ERR(__wt_filesize(session, log_fh, &log_size));
            eol = false;
            continue;
        }
        /*
         * Read the minimum allocation size a record could be. Conditionally set the need_salvage
         * flag so that if the read fails, we know this is an situation we can salvage.
         */
        WT_ASSERT(session, buf->memsize >= allocsize);
        need_salvage = F_ISSET(conn, WT_CONN_SALVAGE);
        WT_ERR(__log_fs_read(session, log_fh, rd_lsn.l.offset, (size_t)allocsize, buf->mem));
        need_salvage = false;
        /*
         * See if we need to read more than the allocation size. We expect that we rarely will have
         * to read more. Most log records will be fairly small.
         */
        reclen = ((WT_LOG_RECORD *)buf->mem)->len;
#ifdef WORDS_BIGENDIAN
        reclen = __wt_bswap32(reclen);
#endif
        /*
         * Log files are pre-allocated. We need to detect the difference between a hole in the file
         * (where this location would be considered the end of log) and the last record in the log
         * and we're at the zeroed part of the file. If we find a zeroed record, scan forward in the
         * log looking for any data. If we detect any we have a hole and stop. Otherwise if the rest
         * is all zeroes advance to the next file. When recovery finds the end of the log, truncate
         * the file and remove any later log files that may exist.
         */
        if (reclen == 0) {
            WT_ERR(__log_has_hole(session, log_fh, log_size, rd_lsn.l.offset, &bad_offset, &eol));
            if (bad_offset != 0) {
                need_salvage = true;
                WT_ERR(__log_salvage_message(session, log_fh->name, "", bad_offset));
            }
            if (eol)
                /* Found a hole. This LSN is the end. */
                break;
            /* Last record in log.  Look for more. */
            goto advance;
        }
        rdup_len = __wt_rduppo2(reclen, allocsize);
        if (reclen > allocsize) {
            /*
             * The log file end could be the middle of this log record. If we have a partially
             * written record then this is considered the end of the log.
             */
            if (rd_lsn.l.offset + rdup_len > log_size) {
                eol = true;
                break;
            }
            /*
             * We need to round up and read in the full padded record, especially for direct I/O.
             */
            WT_ERR(__wt_buf_grow(session, buf, rdup_len));
            WT_ERR(__log_fs_read(session, log_fh, rd_lsn.l.offset, (size_t)rdup_len, buf->mem));
            WT_STAT_CONN_INCR(session, log_scan_rereads);
        }
        /*
         * We read in the record, now verify the checksum. A failed checksum does not imply
         * corruption, it may be the result of a partial write.
         */
        buf->size = reclen;
        logrec = (WT_LOG_RECORD *)buf->mem;
        if (!__log_checksum_match(buf, reclen)) {
            /*
             * A checksum mismatch means we have reached the end of the useful part of the log. This
             * should be found on the first pass through recovery. In the second pass where we
             * truncate the log, this is where it should end. Continue processing where possible, so
             * remember any error returns, but don't skip to the error handler.
             */
            if (log != NULL)
                WT_ASSIGN_LSN(&log->trunc_lsn, &rd_lsn);
            /*
             * If the user asked for a specific LSN and it is not a valid LSN, return WT_NOTFOUND.
             */
            if (LF_ISSET(WT_LOGSCAN_ONE))
                ret = WT_NOTFOUND;

            /*
             * When we have a checksum mismatch, we would like
             * to determine whether it may be the result of:
             *  1) some expected corruption that can occur during
             *     backups
             *  2) a partial write that can naturally occur when
             *     an application crashes
             *  3) some other corruption
             * so that we can (in case 3) flag cases of file system
             * or hardware failures. Unfortunately, we have found
             * on some systems that file system writes may in fact
             * be lost, and this can readily be triggered with
             * normal operations. Rather than force users to
             * salvage in these situations, we merely truncate the
             * log at this point and issue a message.
             */
            if (F_ISSET(conn, WT_CONN_WAS_BACKUP))
                break;

            if (!__log_check_partial_write(session, buf, reclen)) {
                /*
                 * It's not a partial write, and we have a bad checksum. We treat it as a corruption
                 * that must be salvaged.
                 */
                need_salvage = true;
                WT_TRET(
                  __log_salvage_message(session, log_fh->name, ", bad checksum", rd_lsn.l.offset));
            } else {
                /*
                 * It may be a partial write, or it's possible that the header is corrupt. Make a
                 * sanity check of the log record header.
                 */
                WT_TRET(__log_record_verify(session, log_fh, rd_lsn.l.offset, logrec, &corrupt));
                if (corrupt) {
                    need_salvage = true;
                    WT_TRET(__log_salvage_message(session, log_fh->name, "", rd_lsn.l.offset));
                }
            }
            break;
        }
        __wt_log_record_byteswap(logrec);

        /*
         * We have a valid log record. If it is not the log file header, invoke the callback.
         */
        WT_STAT_CONN_INCR(session, log_scan_records);
        WT_ASSIGN_LSN(&next_lsn, &rd_lsn);
        next_lsn.l.offset += rdup_len;
        if (rd_lsn.l.offset != 0) {
            /*
             * We need to manage the different buffers here. Buf is the buffer this function uses to
             * read from the disk. The callback buffer may change based on whether encryption and
             * compression are used.
             *
             * We want to free any buffers from compression and encryption but keep the one we use
             * for reading.
             */
            cbbuf = buf;
            if (F_ISSET(logrec, WT_LOG_RECORD_ENCRYPTED)) {
                WT_ERR(__log_decrypt(session, cbbuf, decryptitem));
                cbbuf = decryptitem;
            }
            if (F_ISSET(logrec, WT_LOG_RECORD_COMPRESSED)) {
                WT_ERR(__log_decompress(session, cbbuf, uncitem));
                cbbuf = uncitem;
            }
            WT_ERR((*func)(session, cbbuf, &rd_lsn, &next_lsn, cookie, firstrecord));

            firstrecord = 0;

            if (LF_ISSET(WT_LOGSCAN_ONE))
                break;
        }

        /*
         * Exit the scanning loop if the next LSN seen is greater than our user set end range LSN.
         */
        if (end_lsnp != NULL && __wt_log_cmp(&next_lsn, end_lsnp) > 0)
            break;

        WT_ASSIGN_LSN(&rd_lsn, &next_lsn);
    }

    /* Truncate if we're in recovery. */
    if (LF_ISSET(WT_LOGSCAN_RECOVER) && __wt_log_cmp(&rd_lsn, &log->trunc_lsn) < 0) {
        __wt_verbose(session, WT_VERB_LOG,
          "End of recovery truncate end of log %" PRIu32 "/%" PRIu32, rd_lsn.l.file,
          rd_lsn.l.offset);
        /* Preserve prior error and fall through to error handling. */
        WT_TRET(__log_truncate(session, &rd_lsn, false, false));
    }

err:
    WT_STAT_CONN_INCR(session, log_scans);
    /*
     * If we are salvaging and failed a salvageable operation, then truncate the log at the fail
     * point.
     */
    if (ret != 0 && ret != WT_PANIC && need_salvage) {
        WT_TRET(__wt_close(session, &log_fh));
        log_fh = NULL;
        WT_TRET(__log_truncate(session, &rd_lsn, false, true));
        ret = 0;
    }

    /*
     * If the first attempt to read a log record results in an error recovery is likely going to
     * fail. Try to provide a helpful failure message.
     */
    if (ret != 0 && firstrecord && LF_ISSET(WT_LOGSCAN_RECOVER | WT_LOGSCAN_RECOVER_METADATA)) {
        __wt_err(session, ret, "WiredTiger is unable to read the recovery log");
        __wt_err(session, ret,
          "This may be due to the log files being encrypted, being from an older version or due to "
          "corruption on disk");
        __wt_err(session, ret,
          "You should confirm that you have opened the database with the correct options including "
          "all encryption and compression options");
    }

    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));

    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &decryptitem);
    __wt_scr_free(session, &uncitem);

    /*
     * If the caller wants one record and it is at the end of log, return WT_NOTFOUND.
     */
    if (LF_ISSET(WT_LOGSCAN_ONE) && eol && ret == 0)
        ret = WT_NOTFOUND;
    WT_TRET(__wt_close(session, &log_fh));
    return (ret);
}

/*
 * __wt_log_force_write --
 *     Force a switch and release and write of the current slot. Wrapper function that takes the
 *     lock.
 */
int
__wt_log_force_write(WT_SESSION_IMPL *session, bool retry, bool *did_work)
{
    WT_LOG *log;
    WT_MYSLOT myslot;

    log = S2C(session)->log;
    memset(&myslot, 0, sizeof(myslot));
    WT_STAT_CONN_INCR(session, log_force_write);
    if (did_work != NULL)
        *did_work = true;
    myslot.slot = log->active_slot;
    return (__wt_log_slot_switch(session, &myslot, retry, true, did_work));
}

/*
 * __wt_log_write --
 *     Write a record into the log, compressing as necessary.
 */
int
__wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
{
    WT_COMPRESSOR *compressor;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(citem);
    WT_DECL_ITEM(eitem);
    WT_DECL_RET;
    WT_ITEM *ip;
    WT_KEYED_ENCRYPTOR *kencryptor;
    WT_LOG *log;
    WT_LOG_RECORD *newlrp;
    size_t dst_len, len, new_size, result_len, src_len;
    uint8_t *dst, *src;
    int compression_failed;

    conn = S2C(session);
    log = conn->log;
    /*
     * An error during opening the logging subsystem can result in it being enabled, but without an
     * open log file. In that case, just return. We can also have logging opened for reading in a
     * read-only database and attempt to write a record on close.
     */
    if (!F_ISSET(log, WT_LOG_OPENED) || F_ISSET(conn, WT_CONN_READONLY))
        return (0);
    ip = record;
    if ((compressor = conn->log_compressor) != NULL && record->size < log->allocsize) {
        WT_STAT_CONN_INCR(session, log_compress_small);
    } else if (compressor != NULL) {
        /* Skip the log header */
        src = (uint8_t *)record->mem + WT_LOG_COMPRESS_SKIP;
        src_len = record->size - WT_LOG_COMPRESS_SKIP;

        /*
         * Compute the size needed for the destination buffer. We only allocate enough memory for a
         * copy of the original by default, if any compressed version is bigger than the original,
         * we won't use it. However, some compression engines (snappy is one example), may need more
         * memory because they don't stop just because there's no more memory into which to
         * compress.
         */
        if (compressor->pre_size == NULL)
            len = src_len;
        else
            WT_ERR(compressor->pre_size(compressor, &session->iface, src, src_len, &len));

        new_size = len + WT_LOG_COMPRESS_SKIP;
        WT_ERR(__wt_scr_alloc(session, new_size, &citem));

        /* Skip the header bytes of the destination data. */
        dst = (uint8_t *)citem->mem + WT_LOG_COMPRESS_SKIP;
        dst_len = len;

        compression_failed = 0;
        WT_ERR(compressor->compress(compressor, &session->iface, src, src_len, dst, dst_len,
          &result_len, &compression_failed));
        result_len += WT_LOG_COMPRESS_SKIP;

        /*
         * If compression fails, or doesn't gain us at least one unit of allocation, fallback to the
         * original version. This isn't unexpected: if compression doesn't work for some chunk of
         * data for some reason (noting likely additional format/header information which compressed
         * output requires), it just means the uncompressed version is as good as it gets, and
         * that's what we use.
         */
        if (compression_failed || result_len / log->allocsize >= record->size / log->allocsize)
            WT_STAT_CONN_INCR(session, log_compress_write_fails);
        else {
            WT_STAT_CONN_INCR(session, log_compress_writes);
            WT_STAT_CONN_INCRV(session, log_compress_mem, record->size);
            WT_STAT_CONN_INCRV(session, log_compress_len, result_len);

            /*
             * Copy in the skipped header bytes, set the final data size.
             */
            memcpy(citem->mem, record->mem, WT_LOG_COMPRESS_SKIP);
            citem->size = result_len;
            ip = citem;
            newlrp = (WT_LOG_RECORD *)citem->mem;
            F_SET(newlrp, WT_LOG_RECORD_COMPRESSED);
            WT_ASSERT(session, result_len < UINT32_MAX && record->size < UINT32_MAX);
            newlrp->mem_len = WT_STORE_SIZE(record->size);
        }
    }
    if ((kencryptor = conn->kencryptor) != NULL) {
        /*
         * Allocate enough space for the original record plus the encryption size constant plus the
         * length we store.
         */
        __wt_encrypt_size(session, kencryptor, ip->size, &new_size);
        WT_ERR(__wt_scr_alloc(session, new_size, &eitem));

        WT_ERR(__wt_encrypt(session, kencryptor, WT_LOG_ENCRYPT_SKIP, ip, eitem));

        /*
         * Final setup of new buffer. Set the flag for encryption in the record header.
         */
        ip = eitem;
        newlrp = (WT_LOG_RECORD *)eitem->mem;
        F_SET(newlrp, WT_LOG_RECORD_ENCRYPTED);
        WT_ASSERT(session, new_size < UINT32_MAX && ip->size < UINT32_MAX);
    }
    ret = __log_write_internal(session, ip, lsnp, flags);

err:
    __wt_scr_free(session, &citem);
    __wt_scr_free(session, &eitem);
    return (ret);
}

/*
 * __log_write_internal --
 *     Write a record into the log.
 */
static int
__log_write_internal(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOG_RECORD *logrec;
    WT_LSN lsn;
    WT_MYSLOT myslot;
    int64_t release_size;
    uint32_t fill_size, force, rdup_len;
    bool free_slot;

    conn = S2C(session);
    log = conn->log;
    if (record->size > UINT32_MAX)
        WT_RET_MSG(session, EFBIG,
          "Log record size of %" WT_SIZET_FMT " exceeds the maximum supported size of %" PRIu32,
          record->size, UINT32_MAX);
    WT_INIT_LSN(&lsn);
    myslot.slot = NULL;
    memset(&myslot, 0, sizeof(myslot));
    /*
     * Assume the WT_ITEM the caller passed is a WT_LOG_RECORD, which has a header at the beginning
     * for us to fill in.
     *
     * If using direct_io, the caller should pass us an aligned record. But we need to make sure it
     * is big enough and zero-filled so that we can write the full amount. Do this whether or not
     * direct_io is in use because it makes the reading code cleaner.
     */
    WT_STAT_CONN_INCRV(session, log_bytes_payload, record->size);
    rdup_len = __wt_rduppo2((uint32_t)record->size, log->allocsize);
    WT_ERR(__wt_buf_grow(session, record, rdup_len));
    WT_ASSERT(session, record->data == record->mem);
    /*
     * If the caller's record only partially fills the necessary space, we need to zero-fill the
     * remainder.
     *
     * The cast is safe, we've already checked to make sure it's in range.
     */
    fill_size = rdup_len - (uint32_t)record->size;
    if (fill_size != 0) {
        memset((uint8_t *)record->mem + record->size, 0, fill_size);
        /*
         * Set the last byte of the log record to a non-zero value, that allows us, on the input
         * side, to tell that a log record was completely written; there couldn't have been a
         * partial write. That means that any checksum mismatch in those conditions is a log
         * corruption.
         *
         * Without this changed byte, when we see a zeroed last byte, we must always treat a
         * checksum error as a possible partial write. Since partial writes can happen as a result
         * of an interrupted process (for example, a shutdown), we must treat a checksum error as a
         * normal occurrence, and merely the place where the log must be truncated. So any real
         * corruption within log records is hard to detect as such.
         *
         * However, we can only make this modification if there is more than one byte being filled,
         * as the first zero byte past the actual record is needed to terminate the loop in
         * txn_commit_apply.
         *
         * This is not a log format change, as we only are changing a byte in the padding portion of
         * a record, and no logging code has ever checked that it is any particular value up to now.
         */
        if (fill_size > 1)
            *((uint8_t *)record->mem + rdup_len - 1) = WT_DEBUG_BYTE;
        record->size = rdup_len;
    }
    /*
     * Checksum a little-endian version of the header, and write everything in little-endian format.
     * The checksum is (potentially) returned in a big-endian format, swap it into place in a
     * separate step.
     */
    logrec = (WT_LOG_RECORD *)record->mem;
    logrec->len = (uint32_t)record->size;
    logrec->checksum = 0;
    __wt_log_record_byteswap(logrec);
    logrec->checksum = __wt_checksum(logrec, record->size);
#ifdef WORDS_BIGENDIAN
    logrec->checksum = __wt_bswap32(logrec->checksum);
#endif

    WT_STAT_CONN_INCR(session, log_writes);

    /*
     * The only time joining a slot should ever return an error is if it detects a panic.
     */
    __wt_log_slot_join(session, rdup_len, flags, &myslot);
    /*
     * If the addition of this record crosses the buffer boundary, switch in a new slot.
     */
    force = LF_ISSET(WT_LOG_FLUSH | WT_LOG_FSYNC);
    ret = 0;
    if (myslot.end_offset >= WT_LOG_SLOT_BUF_MAX || F_ISSET(&myslot, WT_MYSLOT_UNBUFFERED) || force)
        ret = __wt_log_slot_switch(session, &myslot, true, false, NULL);
    if (ret == 0)
        ret = __wt_log_fill(session, &myslot, false, record, &lsn);
    release_size = __wt_log_slot_release(&myslot, (int64_t)rdup_len);
    /*
     * If we get an error we still need to do proper accounting in the slot fields. XXX On error we
     * may still need to call release and free.
     */
    if (ret != 0)
        myslot.slot->slot_error = ret;
    WT_ASSERT(session, ret == 0);
    if (WT_LOG_SLOT_DONE(release_size)) {
        WT_ERR(__wt_log_release(session, myslot.slot, &free_slot));
        if (free_slot)
            __wt_log_slot_free(session, myslot.slot);
    } else if (force) {
        /*
         * If we are going to wait for this slot to get written, signal the log server thread.
         *
         * XXX I've seen times when conditions are NULL.
         */
        if (conn->log_cond != NULL) {
            __wt_cond_signal(session, conn->log_cond);
            __wt_yield();
        } else
            WT_ERR(__wt_log_force_write(session, 1, NULL));
    }
    if (LF_ISSET(WT_LOG_FLUSH)) {
        /* Wait for our writes to reach the OS */
        while (__wt_log_cmp(&log->write_lsn, &lsn) <= 0 && myslot.slot->slot_error == 0)
            __wt_cond_wait(session, log->log_write_cond, 10000, NULL);
    } else if (LF_ISSET(WT_LOG_FSYNC)) {
        /* Wait for our writes to reach disk */
        while (__wt_log_cmp(&log->sync_lsn, &lsn) <= 0 && myslot.slot->slot_error == 0)
            __wt_cond_wait(session, log->log_sync_cond, 10000, NULL);
    }

err:
    if (ret == 0 && lsnp != NULL)
        *lsnp = lsn;
    /*
     * If we're synchronous and some thread had an error, we don't know if our write made it out to
     * the file or not. The error could be before or after us. So, if anyone got an error, we report
     * it. If we're not synchronous, only report if our own operation got an error.
     */
    if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC) && ret == 0 && myslot.slot != NULL)
        ret = myslot.slot->slot_error;

    /*
     * If one of the sync flags is set, assert the proper LSN has moved to match on success.
     */
    WT_ASSERT(
      session, ret != 0 || !LF_ISSET(WT_LOG_FLUSH) || __wt_log_cmp(&log->write_lsn, &lsn) >= 0);
    WT_ASSERT(
      session, ret != 0 || !LF_ISSET(WT_LOG_FSYNC) || __wt_log_cmp(&log->sync_lsn, &lsn) >= 0);
    return (ret);
}

/*
 * __wt_log_vprintf --
 *     Write a message into the log.
 */
int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(logrec);
    WT_DECL_RET;
    size_t header_size, len;
    uint32_t rectype;
    const char *rec_fmt;
    va_list ap_copy;

    conn = S2C(session);
    rectype = WT_LOGREC_MESSAGE;
    rec_fmt = WT_UNCHECKED_STRING(I);

    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        return (0);

    va_copy(ap_copy, ap);
    len = 1;
    ret = __wt_vsnprintf_len_incr(NULL, 0, &len, fmt, ap_copy);
    va_end(ap_copy);
    WT_RET(ret);

    WT_RET(__wt_logrec_alloc(session, sizeof(WT_LOG_RECORD) + len, &logrec));

    /*
     * We're writing a record with the type (an integer) followed by a string (NUL-terminated data).
     * To avoid writing the string into a buffer before copying it, we write the header first, then
     * the raw bytes of the string.
     */
    WT_ERR(__wt_struct_size(session, &header_size, rec_fmt, rectype));
    WT_ERR(__wt_struct_pack(
      session, (uint8_t *)logrec->data + logrec->size, header_size, rec_fmt, rectype));
    logrec->size += (uint32_t)header_size;

    WT_ERR(__wt_vsnprintf((char *)logrec->data + logrec->size, len, fmt, ap));

    __wt_verbose(session, WT_VERB_LOG, "log_printf: %s", (char *)logrec->data + logrec->size);

    logrec->size += len;
    WT_ERR(__wt_log_write(session, logrec, NULL, 0));
err:
    __wt_scr_free(session, &logrec);
    return (ret);
}

/*
 * __wt_log_flush --
 *     Forcibly flush the log to the synchronization level specified. Wait until it has been
 *     completed.
 */
int
__wt_log_flush(WT_SESSION_IMPL *session, uint32_t flags)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    WT_LSN last_lsn, lsn;

    conn = S2C(session);
    WT_ASSERT(session, FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED));
    log = conn->log;
    /*
     * We need to flush out the current slot first to get the real end of log LSN in log->alloc_lsn.
     */
    WT_RET(__wt_log_flush_lsn(session, &lsn, false));
    WT_ASSIGN_LSN(&last_lsn, &log->alloc_lsn);

    /*
     * If the last write caused a switch to a new log file, we should only wait for the last write
     * to be flushed. Otherwise, if the workload is single-threaded we could wait here forever
     * because the write LSN doesn't switch into the new file until it contains a record.
     */
    if (last_lsn.l.offset == log->first_record)
        WT_ASSIGN_LSN(&last_lsn, &log->log_close_lsn);

    /*
     * Wait until all current outstanding writes have been written to the file system.
     */
    while (__wt_log_cmp(&last_lsn, &lsn) > 0) {
        __wt_sleep(0, WT_THOUSAND);
        WT_RET(__wt_log_flush_lsn(session, &lsn, false));
    }

    __wt_verbose(session, WT_VERB_LOG, "log_flush: flags %#" PRIx32 " LSN %" PRIu32 "/%" PRIu32,
      flags, lsn.l.file, lsn.l.offset);
    /*
     * If the user wants write-no-sync, there is nothing more to do. If the user wants background
     * sync, set the LSN and we're done. If the user wants sync, force it now.
     */
    if (LF_ISSET(WT_LOG_FSYNC))
        WT_RET(__wt_log_force_sync(session, &lsn));
    return (0);
}
