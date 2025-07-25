/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "log_private.h"

/*
 * __wt_log_system_backup_id --
 *     Write a system log record for the incremental backup IDs.
 */
int
__wt_log_system_backup_id(WT_SESSION_IMPL *session)
{
    WT_BLKINCR *blk;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(logrec);
    WT_DECL_RET;
    WTI_LOG *log;
    size_t recsize;
    uint32_t i, rectype;
    char nul;
    const char *fmt;

    conn = S2C(session);
    nul = '\0';
    /*
     * If we're not logging or incremental backup isn't turned on or this version doesn't support
     * the system log record, we're done.
     */
    if (!F_ISSET(&conn->log_mgr, WT_LOG_ENABLED) || !F_ISSET(&conn->log_mgr, WT_LOG_INCR_BACKUP))
        return (0);
    log = conn->log_mgr.log;
    if (log->log_version < WTI_LOG_VERSION_SYSTEM)
        return (0);

    /*
     * We use the WT_LOG_INCR_BACKUP flag and not WT_CONN_INCR_BACKUP. The logging flag indicates we
     * need to write the log record. We may have to do that even if connection incremental backup is
     * not enabled because it could be checkpoint and switch after a force stop.
     */
    /* Set up the system log record itself. */
    rectype = WT_LOGREC_SYSTEM;
    fmt = WT_UNCHECKED_STRING(I);
    WT_ERR(__wt_struct_size(session, &recsize, fmt, rectype));
    WT_ERR(__wt_logrec_alloc(session, recsize, &logrec));
    WT_ERR(
      __wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, recsize, fmt, rectype));
    logrec->size += recsize;

    /*
     * Now set up the log operation component. The pack function will grow the log record buffer as
     * necessary.
     */
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk = &conn->incr_backups[i];
        /*
         * If incremental backup has been used write a log record. If the slot is not valid, either
         * it hasn't yet been used or it is empty after a force stop, write a record with no string
         * and a granularity that is out of range.
         */
        if (F_ISSET(blk, WT_BLKINCR_VALID)) {
            WT_ASSERT(session, conn->incr_granularity != 0);
            WT_ASSERT(session, blk->granularity == conn->incr_granularity);
            WT_ERR(__wt_logop_backup_id_pack(session, logrec, i, blk->granularity, blk->id_str));
        } else
            WT_ERR(__wt_logop_backup_id_pack(session, logrec, i, UINT64_MAX, &nul));
    }
    WT_ERR(__wt_log_write(session, logrec, NULL, 0));
err:
    __wt_logrec_free(session, &logrec);
    return (ret);
}

/*
 * __wti_log_system_prevlsn --
 *     Write a system log record for the previous LSN.
 */
int
__wti_log_system_prevlsn(WT_SESSION_IMPL *session, WT_FH *log_fh, WT_LSN *lsn)
{
    WT_DECL_ITEM(logrec_buf);
    WT_DECL_RET;
    WTI_LOGSLOT tmp;
    WT_LOG_RECORD *logrec;
    WTI_MYSLOT myslot;
    size_t recsize;
    uint32_t rectype;
    const char *fmt;

    rectype = WT_LOGREC_SYSTEM;
    fmt = WT_UNCHECKED_STRING(I);

    WT_RET(__wt_logrec_alloc(session, WTI_LOG_ALIGN, &logrec_buf));
    memset((uint8_t *)logrec_buf->mem, 0, WTI_LOG_ALIGN);

    WT_ERR(__wt_struct_size(session, &recsize, fmt, rectype));
    WT_ERR(__wt_struct_pack(
      session, (uint8_t *)logrec_buf->data + logrec_buf->size, recsize, fmt, rectype));
    logrec_buf->size += recsize;
    WT_ERR(__wt_logop_prev_lsn_pack(session, logrec_buf, lsn));
    WT_ASSERT(session, logrec_buf->size <= WTI_LOG_ALIGN);

    logrec = (WT_LOG_RECORD *)logrec_buf->mem;

    /*
     * We know system records are this size. And we have to adjust the size now because we're not
     * going through the normal log write path and the packing functions needed the correct offset
     * earlier.
     */
    logrec_buf->size = logrec->len = WTI_LOG_ALIGN;

    /* We do not compress nor encrypt this record. */
    logrec->checksum = 0;
    logrec->flags = 0;
    __wti_log_record_byteswap(logrec);
    logrec->checksum = __wt_checksum(logrec, WTI_LOG_ALIGN);
#ifdef WORDS_BIGENDIAN
    logrec->checksum = __wt_bswap32(logrec->checksum);
#endif
    WT_CLEAR(tmp);
    memset(&myslot, 0, sizeof(myslot));
    myslot.slot = &tmp;
    __wti_log_slot_activate(session, &tmp);
    /*
     * Override the file handle to the one we're using.
     */
    tmp.slot_fh = log_fh;
    WT_ERR(__wti_log_fill(session, &myslot, true, logrec_buf, NULL));
err:
    __wt_logrec_free(session, &logrec_buf);
    return (ret);
}

/*
 * __wti_log_recover_prevlsn --
 *     Process a system log record for the previous LSN in recovery.
 */
int
__wti_log_recover_prevlsn(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_LSN *lsnp)
{
    WT_DECL_RET;

    if ((ret = __wt_logop_prev_lsn_unpack(session, pp, end, lsnp)) != 0)
        WT_RET_MSG(session, ret, "log_recover_prevlsn: unpack failure");

    return (0);
}

/*
 * __wt_verbose_dump_log --
 *     Dump information about the logging subsystem.
 */
int
__wt_verbose_dump_log(WT_SESSION_IMPL *session)
{
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;

    log_mgr = &S2C(session)->log_mgr;
    log = log_mgr->log;

    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    WT_RET(__wt_msg(
      session, "Logging subsystem: Enabled: %s", F_ISSET(log_mgr, WT_LOG_ENABLED) ? "yes" : "no"));
    if (!F_ISSET(log_mgr, WT_LOG_ENABLED))
        return (0);
    /*
     * Logging is enabled, print out the other information.
     */
    WT_RET(__wt_msg(session, "Removing: %s", F_ISSET(log_mgr, WT_LOG_REMOVE) ? "yes" : "no"));
    WT_RET(__wt_msg(
      session, "Running downgraded: %s", F_ISSET(log_mgr, WT_LOG_DOWNGRADED) ? "yes" : "no"));
    WT_RET(
      __wt_msg(session, "Zero fill files: %s", F_ISSET(log_mgr, WT_LOG_ZERO_FILL) ? "yes" : "no"));
    WT_RET(__wt_msg(
      session, "Pre-allocate files: %s", __wti_log_is_prealloc_enabled(session) ? "yes" : "no"));
    WT_RET(__wt_msg(
      session, "Initial number of pre-allocated files: %" PRIu32, log_mgr->prealloc_init_count));
    WT_RET(__wt_msg(session, "Logging directory: %s", log_mgr->log_path));
    WT_RET(__wt_msg(session, "Logging maximum file size: %" PRId64, (int64_t)log_mgr->file_max));
    WT_RET(__wt_msg(session, "Log sync setting: %s",
      !FLD_ISSET(log_mgr->txn_logsync, WT_LOG_SYNC_ENABLED) ? "none" :
        FLD_ISSET(log_mgr->txn_logsync, WT_LOG_DSYNC)       ? "dsync" :
        FLD_ISSET(log_mgr->txn_logsync, WT_LOG_FLUSH)       ? "write to OS" :
        FLD_ISSET(log_mgr->txn_logsync, WT_LOG_FSYNC)       ? "fsync to disk" :
                                                              "unknown sync setting"));
    WT_RET(__wt_msg(session, "Log record allocation alignment: %" PRId32, WTI_LOG_ALIGN));
    WT_RET(__wt_msg(session, "Current log file number: %" PRIu32, log->fileid));
    WT_RET(__wt_msg(session, "Current log version number: %" PRIu16, log->log_version));
    WT_RET(WTI_LSN_MSG(&log->alloc_lsn, "Next allocation"));
    WT_RET(WTI_LSN_MSG(&log->ckpt_lsn, "Last checkpoint"));
    WT_RET(WTI_LSN_MSG(&log->sync_dir_lsn, "Last directory sync"));
    WT_RET(WTI_LSN_MSG(&log->sync_lsn, "Last sync"));
    WT_RET(WTI_LSN_MSG(&log->trunc_lsn, "Recovery truncate"));
    WT_RET(WTI_LSN_MSG(&log->write_lsn, "Last written"));
    WT_RET(WTI_LSN_MSG(&log->write_start_lsn, "Start of last written"));
    /*
     * If we wanted a dump of the slots, it would go here. Walking the slot pool may not require a
     * lock since they're statically allocated, but output could be inconsistent without it.
     */

    return (0);
}
