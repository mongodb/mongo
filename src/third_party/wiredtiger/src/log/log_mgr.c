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
 * __logmgr_sync_cfg --
 *     Interpret the transaction_sync config.
 */
static int
__logmgr_sync_cfg(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_LOG_MANAGER *log_mgr;
    uint32_t txn_logsync;

    log_mgr = &S2C(session)->log_mgr;

    /*
     * Collect all the flag settings into a local variable and then assign into the connection after
     * we're done so that there is no chance of another thread seeing an interim value while we're
     * processing during a reconfigure.
     */
    txn_logsync = 0;
    WT_RET(__wt_config_gets(session, cfg, "transaction_sync.enabled", &cval));
    if (cval.val)
        FLD_SET(txn_logsync, WT_LOG_SYNC_ENABLED);
    else
        FLD_CLR(txn_logsync, WT_LOG_SYNC_ENABLED);

    WT_RET(__wt_config_gets(session, cfg, "transaction_sync.method", &cval));
    if (WT_CONFIG_LIT_MATCH("dsync", cval))
        FLD_SET(txn_logsync, WT_LOG_DSYNC | WT_LOG_FLUSH);
    else if (WT_CONFIG_LIT_MATCH("fsync", cval))
        FLD_SET(txn_logsync, WT_LOG_FSYNC);
    else if (WT_CONFIG_LIT_MATCH("none", cval))
        FLD_SET(txn_logsync, WT_LOG_FLUSH);
    WT_RELEASE_WRITE_WITH_BARRIER(log_mgr->txn_logsync, txn_logsync);
    return (0);
}

/*
 * __logmgr_force_remove --
 *     Force a checkpoint out and then force a removal, waiting for the first log to be removed up
 *     to the given log number.
 */
static int
__logmgr_force_remove(WT_SESSION_IMPL *session, uint32_t lognum)
{
    WT_CONNECTION_IMPL *conn;
    WTI_LOG *log;
    WT_SESSION_IMPL *tmp_session;
    uint64_t sleep_usecs, yield_cnt;

    conn = S2C(session);
    log = conn->log_mgr.log;
    sleep_usecs = yield_cnt = 0;

    WT_RET(__wt_open_internal_session(conn, "compatibility-reconfig", true, 0, 0, &tmp_session));
    while (log->first_lsn.l.file < lognum) {
        /*
         * Force a checkpoint to be written in the new log file and force the removal of all
         * previous log files. We do the checkpoint in the loop because the checkpoint LSN in the
         * log record could still reflect the previous log file in cases such as the write LSN has
         * not yet advanced into the new log file due to another group of threads still in progress
         * with their slot copies or writes.
         */
        WT_RET(tmp_session->iface.checkpoint(&tmp_session->iface, "force=1"));
        /*
         * It's reasonable to start the back off prior to trying at all because the backoff is very
         * gradual.
         */
        __wt_spin_backoff(&yield_cnt, &sleep_usecs);
        WT_STAT_CONN_INCRV(session, log_force_remove_sleep, sleep_usecs);

        WT_RET(WT_SESSION_CHECK_PANIC(tmp_session));
        WT_RET(__wt_log_truncate_files(tmp_session, NULL, true));
    }
    WT_RET(__wt_session_close_internal(tmp_session));
    return (0);
}

/*
 * __logmgr_get_log_version --
 *     Get the log version required for the given WiredTiger version.
 */
static uint16_t
__logmgr_get_log_version(WT_VERSION version)
{
    if (!__wt_version_defined(version))
        return (WT_NO_VALUE);

    if (__wt_version_lt(version, WT_LOG_V2_VERSION))
        return (1);
    else if (__wt_version_lt(version, WT_LOG_V3_VERSION))
        return (2);
    else if (__wt_version_lt(version, WT_LOG_V4_VERSION))
        return (3);
    else if (__wt_version_lt(version, WT_LOG_V5_VERSION))
        return (4);
    else
        return (WTI_LOG_VERSION);
}

/*
 * __wt_logmgr_compat_version --
 *     Set up the compatibility versions in the log manager. This is split out because it is called
 *     much earlier than log subsystem creation on startup so that we can verify the system state in
 *     files before modifying files.
 */
void
__wt_logmgr_compat_version(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    conn->log_mgr.req_max = __logmgr_get_log_version(conn->compat_req_max);
    conn->log_mgr.req_min = __logmgr_get_log_version(conn->compat_req_min);
}

/*
 * __logmgr_version --
 *     Set up the versions in the log manager.
 */
static int
__logmgr_version(WT_SESSION_IMPL *session, bool reconfig)
{
    WT_CONNECTION_IMPL *conn;
    WTI_LOG *log;
    uint32_t first_record, lognum;
    uint16_t new_version;
    bool downgrade;

    conn = S2C(session);
    log = conn->log_mgr.log;
    if (log == NULL)
        return (0);

    /*
     * Set the log file format versions based on compatibility versions set in the connection. The
     * compatibility version must be set at this point. We must set this before we call log_open to
     * open or create a log file.
     */
    WT_ASSERT(session, __wt_version_defined(conn->compat_version));
    new_version = __logmgr_get_log_version(conn->compat_version);

    if (new_version > 1)
        first_record = WTI_LOG_END_HEADER + WTI_LOG_ALIGN;
    else
        first_record = WTI_LOG_END_HEADER;

    __wt_logmgr_compat_version(session);

    /*
     * If the version is the same, there is nothing to do.
     */
    if (log->log_version == new_version)
        return (0);

    /*
     * Note: downgrade in this context means the new version is not the latest possible version. It
     * does not mean the direction of change from the release we may be running currently.
     */
    downgrade = new_version != WTI_LOG_VERSION;

    /*
     * If we are reconfiguring and at a new version we need to force the log file to advance so that
     * we write out a log file at the correct version. When we are downgrading we must force a
     * checkpoint and finally log removal, even if disabled, so that all new version log files are
     * gone.
     *
     * All of the version changes must be handled with locks on reconfigure because other threads
     * may be changing log files, using pre-allocated files.
     */
    /*
     * Set the version. If it is a live change the logging subsystem will do other work as well to
     * move to a new log file.
     */
    WT_RET(__wti_log_set_version(session, new_version, first_record, downgrade, reconfig, &lognum));
    if (reconfig && F_ISSET(&conn->log_mgr, WT_LOG_DOWNGRADED))
        WT_RET(__logmgr_force_remove(session, lognum));
    return (0);
}

/*
 * __wt_logmgr_config --
 *     Parse and setup the logging server options.
 */
int
__wt_logmgr_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_LOG_MANAGER *log_mgr;
    bool enabled;

    /*
     * A note on reconfiguration: the standard "is this configuration string allowed" checks should
     * fail if reconfiguration has invalid strings, for example, "log=(enabled)", or
     * "statistics_log=(path=XXX)", because the connection reconfiguration method doesn't allow
     * those strings. Additionally, the base configuration values during reconfiguration are the
     * currently configured values (so we don't revert to default values when repeatedly
     * reconfiguring), and configuration processing of a currently set value should not change the
     * currently set value.
     *
     * In this code path, log server reconfiguration does not stop/restart the log server, so
     * there's no point in re-evaluating configuration strings that cannot be reconfigured, risking
     * bugs in configuration setup, and depending on evaluation of currently set values to always
     * result in the currently set value. Skip tests for any configuration strings which don't make
     * sense during reconfiguration, but don't worry about error reporting because it should never
     * happen.
     */

    conn = S2C(session);
    log_mgr = &conn->log_mgr;

    WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
    enabled = cval.val != 0;

    /*
     * If we're reconfiguring, enabled must match the already existing setting.
     *
     * If it is off and the user it turning it on, or it is on and the user is turning it off,
     * return an error.
     *
     * See above: should never happen.
     */
    if (reconfig &&
      ((enabled && !F_ISSET(&conn->log_mgr, WT_LOG_ENABLED)) ||
        (!enabled && F_ISSET(&conn->log_mgr, WT_LOG_ENABLED))))
        WT_RET_MSG(
          session, EINVAL, "log manager reconfigure: enabled mismatch with existing setting");

    /* Logging is incompatible with in-memory */
    if (enabled) {
        WT_RET(__wt_config_gets(session, cfg, "in_memory", &cval));
        if (cval.val != 0)
            WT_RET_MSG(
              session, EINVAL, "In-memory configuration incompatible with log=(enabled=true)");
    }

    if (enabled)
        F_SET(log_mgr, WT_LOG_CONFIG_ENABLED);
    else
        F_CLR(log_mgr, WT_LOG_CONFIG_ENABLED);

    /*
     * Setup a log path and compression even if logging is disabled in case we are going to print a
     * log. Only do this on creation. Once a compressor or log path are set they cannot be changed.
     *
     * See above: should never happen.
     */
    if (!reconfig) {
        log_mgr->compressor = NULL;
        WT_RET(__wt_config_gets_none(session, cfg, "log.compressor", &cval));
        WT_RET(__wt_compressor_config(session, &cval, &log_mgr->compressor));

        log_mgr->log_path = NULL;
        WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
        WT_RET(__wt_strndup(session, cval.str, cval.len, &log_mgr->log_path));
    }

    /* We are done if logging isn't enabled. */
    if (!F_ISSET(&conn->log_mgr, WT_LOG_CONFIG_ENABLED))
        return (0);

    /*
     * The configuration string log.archive is deprecated, only take it if it's explicitly set by
     * the application, that is, ignore its default value. Look for an explicit log.remove setting,
     * then an explicit log.archive setting, then the default log.remove setting.
     */
    if (__wt_config_gets(session, cfg + 1, "log.remove", &cval) != 0 &&
      __wt_config_gets(session, cfg + 1, "log.archive", &cval) != 0)
        WT_RET(__wt_config_gets(session, cfg, "log.remove", &cval));
    if (cval.val != 0)
        F_SET(&conn->log_mgr, WT_LOG_REMOVE);

    /*
     * The file size cannot be reconfigured. The amount of memory allocated to the log slots may be
     * based on the log file size at creation and we don't want to re-allocate that memory while
     * running.
     *
     * See above: should never happen.
     */
    if (!reconfig) {
        WT_RET(__wt_config_gets(session, cfg, "log.file_max", &cval));
        log_mgr->file_max = (wt_off_t)cval.val;
        /*
         * With the default log file extend configuration or if the log file extension size is
         * larger than the configured maximum log file size, set the log file extension size to the
         * configured maximum log file size.
         */
        if (log_mgr->extend_len == WT_CONFIG_UNSET || log_mgr->extend_len > log_mgr->file_max)
            log_mgr->extend_len = log_mgr->file_max;
        WT_STAT_CONN_SET(session, log_max_filesize, log_mgr->file_max);
    }

    WT_RET(__wt_config_gets(session, cfg, "log.os_cache_dirty_pct", &cval));
    if (cval.val != 0)
        log_mgr->dirty_max = (log_mgr->file_max * cval.val) / 100;

    /*
     * If pre-allocation is configured, set the initial number to a few. We'll adapt as load
     * dictates.
     */
    WT_RET(__wt_config_gets(session, cfg, "log.prealloc", &cval));
    if (cval.val != 0) {
        WT_RET(__wt_config_gets(session, cfg, "log.prealloc_init_count", &cval));
        log_mgr->prealloc = (uint32_t)cval.val;
        log_mgr->prealloc_init_count = (uint32_t)cval.val;
        WT_ASSERT(session, __wti_log_is_prealloc_enabled(session));
    }

    WT_RET(__wt_config_gets(session, cfg, "log.force_write_wait", &cval));
    if (cval.val != 0)
        log_mgr->force_write_wait = (uint32_t)cval.val;

    /*
     * Note it's meaningless to reconfigure this value during runtime, it only matters on create
     * before recovery runs.
     *
     * See above: should never happen.
     */
    if (!reconfig) {
        WT_RET(__wt_config_gets_def(session, cfg, "log.recover", 0, &cval));
        if (WT_CONFIG_LIT_MATCH("error", cval))
            F_SET(&conn->log_mgr, WT_LOG_RECOVER_ERR);
    }

    WT_RET(__wt_config_gets(session, cfg, "log.zero_fill", &cval));
    if (cval.val != 0) {
        if (F_ISSET(conn, WT_CONN_READONLY))
            WT_RET_MSG(
              session, EINVAL, "Read-only configuration incompatible with zero-filling log files");
        F_SET(&conn->log_mgr, WT_LOG_ZERO_FILL);
    }

    WT_RET(__logmgr_sync_cfg(session, cfg));
    if (log_mgr->server.cond != NULL)
        __wt_cond_signal(session, log_mgr->server.cond);
    return (0);
}

/*
 * __wt_logmgr_reconfig --
 *     Reconfigure logging.
 */
int
__wt_logmgr_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_RET(__wt_logmgr_config(session, cfg, true));
    return (__logmgr_version(session, true));
}

/*
 * __log_remove_once_int --
 *     Helper for __log_remove_once. Intended to be called while holding the hot backup read lock.
 */
static int
__log_remove_once_int(
  WT_SESSION_IMPL *session, char **logfiles, u_int logcount, uint32_t min_lognum)
{
    uint32_t lognum;
    u_int i;

    for (i = 0; i < logcount; i++) {
        WT_RET(__wti_log_extract_lognum(session, logfiles[i], &lognum));
        if (lognum < min_lognum)
            WT_RET(__wti_log_remove(session, WT_LOG_FILENAME, lognum));
    }

    return (0);
}

/*
 * __compute_min_lognum --
 *     Determine the number of the earliest log file we must keep.
 */
static uint32_t
__compute_min_lognum(WT_SESSION_IMPL *session, WTI_LOG *log, uint32_t backup_file)
{
#ifdef HAVE_DIAGNOSTIC
    struct timespec ts;
#endif
    WT_CONNECTION_IMPL *conn;
    uint32_t min_lognum;

    conn = S2C(session);

    /*
     * If we're coming from a backup cursor we want the smaller of the last full log file copied in
     * backup or the checkpoint LSN. Otherwise we want the minimum of the last log file written to
     * disk and the checkpoint LSN.
     */
    min_lognum = backup_file == 0 ? WT_MIN(log->ckpt_lsn.l.file, log->sync_lsn.l.file) :
                                    WT_MIN(log->ckpt_lsn.l.file, backup_file);

    __wt_readlock(session, &conn->log_mgr.debug_log_retention_lock);

    /* Adjust the number of log files to retain based on debugging options. */

    if (FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_CKPT_RETAIN) && conn->debug_ckpt_cnt != 0)
        min_lognum = WT_MIN(conn->debug_ckpt[conn->debug_ckpt_cnt - 1].l.file, min_lognum);

    if (conn->debug_log_cnt != 0) {
        /*
         * If we're performing checkpoints, apply the retain value as a minimum, increasing the
         * number the log files we keep. If not performing checkpoints, it's an absolute number of
         * log files to keep. This means we can potentially remove log files required for recovery
         * if the number of log files exceeds the configured value and the system has yet to be
         * checkpointed.
         *
         * Check for N+1, that is, we retain N full log files, and one partial.
         */
        if ((conn->debug_log_cnt + 1) >= log->fileid)
            min_lognum = 0;
        else if (WT_IS_INIT_LSN(&log->ckpt_lsn))
            min_lognum = log->fileid - (conn->debug_log_cnt + 1);
        else
            min_lognum = WT_MIN(log->fileid - (conn->debug_log_cnt + 1), min_lognum);
    }

    __wt_readunlock(session, &conn->log_mgr.debug_log_retention_lock);
#ifdef HAVE_DIAGNOSTIC
    __wt_epoch(session, &ts);
    if (min_lognum > WT_INIT_LSN_FILE && min_lognum != log->min_fileid) {
        WT_IGNORE_RET(__wt_log_printf(session,
          "COMPUTE_MIN_LOG: [%" PRIuMAX ":%" PRIuMAX "] min_lognum %" PRIu32 " ckpt file %" PRIu32
          " sync file %" PRIu32 " backup_file %" PRIu32 " debug_log count%" PRIu32
          " old min %" PRIu32,
          (uintmax_t)ts.tv_sec, (uintmax_t)ts.tv_nsec / WT_THOUSAND, min_lognum,
          log->ckpt_lsn.l.file, log->sync_lsn.l.file, backup_file, conn->debug_log_cnt,
          log->min_fileid));
        log->min_fileid = min_lognum;
    }

    /* Encourage race conditions in log subsystem during database shutdown. */
    if (FLD_ISSET(conn->timing_stress_flags, WT_TIMING_STRESS_CLOSE_STRESS_LOG) &&
      F_ISSET_ATOMIC_32(conn, WT_CONN_CLOSING)) {
        for (int i = 0; i < 50; i++)
            WT_IGNORE_RET(__wt_log_printf(session, "DEBUG: Stress for concurrency control"));
    }
#endif

    return (min_lognum);
}

/*
 * __log_remove_once --
 *     Perform one iteration of log removal. Must be called with the log removal lock held.
 */
static int
__log_remove_once(WT_SESSION_IMPL *session, uint32_t backup_file)
{
    WT_DECL_RET;
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;
    uint32_t min_lognum;
    u_int logcount;
    char **logfiles;

    log_mgr = &S2C(session)->log_mgr;
    log = log_mgr->log;
    logcount = 0;
    logfiles = NULL;

    min_lognum = __compute_min_lognum(session, log, backup_file);
    if (min_lognum <= WT_INIT_LSN_FILE)
        /*
         * If the minimum log number is zero or one that means we want to keep all logs and there is
         * nothing to do here. In typical operation, the minimum returned will be one as that is the
         * initial LSN file value. When using debug mode, the minimum returned may be zero.
         */
        return (0);

    __wt_verbose(session, WT_VERB_LOG, "log_remove: remove to log number %" PRIu32, min_lognum);

    /*
     * Main remove code. Get the list of all log files and remove any earlier than the minimum log
     * number.
     */
    WT_ERR(
      __wt_fs_directory_list(session, log_mgr->log_path, WT_LOG_FILENAME, &logfiles, &logcount));

    /*
     * If backup_file is non-zero we know we're coming from an incremental backup cursor. In that
     * case just perform the remove operation without the lock.
     */
    if (backup_file != 0)
        ret = __log_remove_once_int(session, logfiles, logcount, min_lognum);
    else
        WT_WITH_HOTBACKUP_READ_LOCK(
          session, ret = __log_remove_once_int(session, logfiles, logcount, min_lognum), NULL);
    WT_ERR(ret);

    /*
     * Indicate what is our new earliest LSN. It is the start of the log file containing the last
     * checkpoint.
     */
    WT_SET_LSN(&log->first_lsn, min_lognum, 0);

    if (0)
err:
        __wt_err(session, ret, "log removal server error");
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __log_prealloc_once --
 *     Perform one iteration of log pre-allocation.
 */
static int
__log_prealloc_once(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;
    u_int i, reccount;
    char **recfiles;

    log_mgr = &S2C(session)->log_mgr;
    log = log_mgr->log;
    reccount = 0;
    recfiles = NULL;

    /*
     * Allocate up to the maximum number, accounting for any existing files that may not have been
     * used yet.
     */
    WT_ERR(
      __wt_fs_directory_list(session, log_mgr->log_path, WTI_LOG_PREPNAME, &recfiles, &reccount));

    /*
     * Adjust the number of files to pre-allocate if we find that the critical path had to allocate
     * them since we last ran.
     */
    if (log->prep_missed > 0) {
        log_mgr->prealloc += log->prep_missed;
        __wt_verbose(session, WT_VERB_LOG, "Missed %" PRIu32 ". Now pre-allocating up to %" PRIu32,
          log->prep_missed, log_mgr->prealloc);
    } else if (reccount > log_mgr->prealloc / 2 &&
      log_mgr->prealloc > log_mgr->prealloc_init_count) {
        /*
         * If we used less than half, then start adjusting down.
         */
        --log_mgr->prealloc;
        __wt_verbose(session, WT_VERB_LOG,
          "Adjust down. Did not use %" PRIu32 ". Now pre-allocating %" PRIu32, reccount,
          log_mgr->prealloc);
    }

    WT_STAT_CONN_SET(session, log_prealloc_max, log_mgr->prealloc);
    /*
     * Allocate up to the maximum number that we just computed and detected.
     */
    for (i = reccount; i < (u_int)log_mgr->prealloc; i++) {
        WT_ERR(__wti_log_allocfile(session, ++log->prep_fileid, WTI_LOG_PREPNAME));
        WT_STAT_CONN_INCR(session, log_prealloc_files);
    }
    /*
     * Reset the missed count now. If we missed during pre-allocating the log files, it means the
     * allocation is not keeping up, not that we didn't allocate enough. So we don't just want to
     * keep adding in more.
     */
    log->prep_missed = 0;
    if (0)
err:
        __wt_err(session, ret, "log pre-alloc server error");
    WT_TRET(__wt_fs_directory_list_free(session, &recfiles, reccount));
    return (ret);
}

/*
 * __wt_log_truncate_files --
 *     Truncate log files via remove once. Requires that the server is not currently running.
 */
int
__wt_log_truncate_files(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool force)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WTI_LOG *log;
    uint32_t backup_file;

    conn = S2C(session);
    if (!F_ISSET(&conn->log_mgr, WT_LOG_ENABLED))
        return (0);
    if (!force && FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LOG) &&
      F_ISSET(&conn->log_mgr, WT_LOG_REMOVE))
        WT_RET_MSG(session, EINVAL, "Attempt to remove manually while a server is running");

    log = conn->log_mgr.log;

    backup_file = 0;
    if (cursor != NULL) {
        WT_ASSERT(session, force == false);
        backup_file = WT_CURSOR_BACKUP_ID(cursor);
    }
    WT_ASSERT(session, backup_file <= log->alloc_lsn.l.file);
    __wt_verbose(
      session, WT_VERB_LOG, "log_truncate_files: remove once up to %" PRIu32, backup_file);

    __wt_writelock(session, &log->log_remove_lock);
    ret = __log_remove_once(session, backup_file);
    __wt_writeunlock(session, &log->log_remove_lock);
    return (ret);
}

/*
 * __log_file_server --
 *     The log file server thread. This worker thread manages log file operations such as closing
 *     and syncing.
 */
static WT_THREAD_RET
__log_file_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *close_fh;
    WTI_LOG *log;
    WT_LSN close_end_lsn;
    WT_SESSION_IMPL *session;
    uint32_t filenum;

    session = arg;
    conn = S2C(session);
    log = conn->log_mgr.log;
    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LOG)) {
        /*
         * If there is a log file to close, make sure any outstanding write operations have
         * completed, then fsync and close it.
         *
         * The read from the log close file handle is ordered with the read from the log close lsn.
         * Writers will set the log close lsn first and then the log close file handle, so we need
         * to read them in the reverse order to see a consistent state.
         */
        WT_ACQUIRE_READ_WITH_BARRIER(close_fh, log->log_close_fh);
        if (close_fh != NULL) {
            WT_ERR(__wti_log_extract_lognum(session, close_fh->name, &filenum));
            /*
             * The closing file handle should have a correct close LSN.
             */
            WT_ASSERT(session, log->log_close_lsn.l.file == filenum);

            if (__wt_log_cmp(&log->write_lsn, &log->log_close_lsn) >= 0) {
                /*
                 * We've copied the file handle, clear out the one in the log structure to allow it
                 * to be set again. Copy the LSN before clearing the file handle. Use a barrier to
                 * make sure the compiler does not reorder the following two statements.
                 */
                WT_ASSIGN_LSN(&close_end_lsn, &log->log_close_lsn);

                /*
                 * Based on the code investigation, there is no reason to use a FULL_BARRIER here.
                 * We send a message that `log_close_lsn` is read by setting `log_close_fh` to NULL,
                 * making it available to `__log_newfile` to assign a new one. Therefore, release
                 * semantics should be sufficient.
                 *
                 * For now, this is placed under the TSAN_BUILD macro only, since weakening barriers
                 * requires extra care.
                 *
                 * FIXME-WT-15337: Consider using a release write in production.
                 */
#if defined(TSAN_BUILD)
                WT_RELEASE_WRITE(log->log_close_fh, NULL);
#else
                WT_FULL_BARRIER();
                __wt_atomic_store_pointer(&log->log_close_fh, NULL);
#endif

                /*
                 * Set the close_end_lsn to the LSN immediately after ours. That is, the beginning
                 * of the next log file. We need to know the LSN file number of our own close in
                 * case earlier calls are still in progress and the next one to move the sync_lsn
                 * into the next file for later syncs.
                 */
                WT_ERR(__wt_fsync(session, close_fh, true));

                /*
                 * We want to have the file size reflect actual data with minimal pre-allocated
                 * zeroed space. We can't truncate the file during hot backup, or the underlying
                 * file system may not support truncate: both are OK, it's just more work during
                 * cursor traversal.
                 */
                if (__wt_atomic_load64(&conn->hot_backup_start) == 0 &&
                  conn->log_mgr.cursors == 0) {
                    WT_WITH_HOTBACKUP_READ_LOCK(session,
                      ret = __wt_ftruncate(session, close_fh, __wt_lsn_offset(&close_end_lsn)),
                      NULL);
                    WT_ERR_ERROR_OK(ret, ENOTSUP, false);
                }
                WT_SET_LSN(&close_end_lsn, close_end_lsn.l.file + 1, 0);
                __wt_spin_lock(session, &log->log_sync_lock);
                WT_ERR(__wt_close(session, &close_fh));
                WT_ASSERT(session, __wt_log_cmp(&close_end_lsn, &log->sync_lsn) >= 0);
                WT_ASSIGN_LSN(&log->sync_lsn, &close_end_lsn);
                __wt_cond_signal(session, log->log_sync_cond);
                __wt_spin_unlock(session, &log->log_sync_lock);
            }
        }

        /* Wait until the next event. */
        __wt_cond_wait(session, conn->log_mgr.file.cond, 100 * WT_THOUSAND, NULL);
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "log close server error"));
    }
    __wt_spin_unlock_if_owned(session, &log->log_sync_lock);
    return (WT_THREAD_RET_VALUE);
}

/*
 * Simple structure for sorting written slots.
 */
typedef struct {
    WT_LSN lsn;
    uint32_t slot_index;
} WT_LOG_WRLSN_ENTRY;

/*
 * WT_WRLSN_ENTRY_CMP_LT --
 *	Return comparison of a written slot pair by LSN.
 */
#define WT_WRLSN_ENTRY_CMP_LT(entry1, entry2)        \
    ((entry1).lsn.l.file < (entry2).lsn.l.file ||    \
      ((entry1).lsn.l.file == (entry2).lsn.l.file && \
        __wt_lsn_offset(&(entry1).lsn) < __wt_lsn_offset(&(entry2).lsn)))

/*
 * __wti_log_wrlsn --
 *     Process written log slots and attempt to coalesce them if the LSNs are contiguous. The
 *     purpose of this function is to advance the write_lsn in LSN order after the buffer is written
 *     to the log file.
 */
void
__wti_log_wrlsn(WT_SESSION_IMPL *session, int *yield)
{
    WT_CONNECTION_IMPL *conn;
    WTI_LOG *log;
    WTI_LOGSLOT *coalescing, *slot;
    WT_LOG_WRLSN_ENTRY written[WTI_SLOT_POOL];
    WT_LSN save_lsn;
    size_t written_i;
    uint32_t i, save_i, slot_last_offset;

    conn = S2C(session);
    log = conn->log_mgr.log;
    __wt_spin_lock(session, &log->log_writelsn_lock);
restart:
    coalescing = NULL;
    WT_INIT_LSN(&save_lsn);
    written_i = 0;
    i = 0;

    /*
     * Walk the array once saving any slots that are in the WTI_LOG_SLOT_WRITTEN state.
     */
    while (i < WTI_SLOT_POOL) {
        save_i = i;
        slot = &log->slot_pool[i++];
        if (__wt_atomic_loadiv64(&slot->slot_state) != WTI_LOG_SLOT_WRITTEN)
            continue;
        written[written_i].slot_index = save_i;
        WT_ASSIGN_LSN(&written[written_i++].lsn, &slot->slot_release_lsn);
    }
    /*
     * If we found any written slots process them. We sort them based on the release LSN, and then
     * look for them in order.
     */
    if (written_i > 0) {
        if (yield != NULL)
            *yield = 0;
        WT_INSERTION_SORT(written, written_i, WT_LOG_WRLSN_ENTRY, WT_WRLSN_ENTRY_CMP_LT);
        /*
         * We know the written array is sorted by LSN. Go through them either advancing write_lsn or
         * coalesce contiguous ranges of written slots.
         */
        for (i = 0; i < written_i; i++) {
            slot = &log->slot_pool[written[i].slot_index];
            /*
             * The log server thread pushes out slots periodically. Sometimes they are empty slots.
             * If we find an empty slot, where empty means the start and end LSN are the same, free
             * it and continue.
             */
            if (__wt_log_cmp(&slot->slot_start_lsn, &slot->slot_release_lsn) == 0 &&
              __wt_log_cmp(&slot->slot_start_lsn, &slot->slot_end_lsn) == 0) {
                __wti_log_slot_free(session, slot);
                continue;
            }
            if (coalescing != NULL) {
                /*
                 * If the write_lsn changed, we may be able to process slots. Try again.
                 */
                if (__wt_log_cmp(&log->write_lsn, &save_lsn) != 0)
                    goto restart;
                if (__wt_log_cmp(&coalescing->slot_end_lsn, &written[i].lsn) != 0) {
                    coalescing = slot;
                    continue;
                }
                /*
                 * If we get here we have a slot to coalesce and free.
                 */
                __wt_atomic_storeiv64(
                  &coalescing->slot_last_offset, __wt_atomic_loadi64(&slot->slot_last_offset));
                WT_ASSIGN_LSN(&coalescing->slot_end_lsn, &slot->slot_end_lsn);
                WT_STAT_CONN_INCR(session, log_slot_coalesced);
                /*
                 * Copy the flag for later closing.
                 */
                if (F_ISSET_ATOMIC_16(slot, WTI_SLOT_CLOSEFH))
                    F_SET_ATOMIC_16(coalescing, WTI_SLOT_CLOSEFH);
            } else {
                /*
                 * If this written slot is not the next LSN, try to start coalescing with later
                 * slots. A synchronous write may update write_lsn so save the last one we saw to
                 * check when coalescing slots.
                 */
                WT_ASSIGN_LSN(&save_lsn, &log->write_lsn);
                if (__wt_log_cmp(&log->write_lsn, &written[i].lsn) != 0) {
                    coalescing = slot;
                    continue;
                }
                /*
                 * If we get here we have a slot to process. Advance the LSN and process the slot.
                 */
                WT_ASSERT(session, __wt_log_cmp(&written[i].lsn, &slot->slot_release_lsn) == 0);
                /*
                 * We need to maintain the starting offset of a log record so that the checkpoint
                 * LSN refers to the beginning of a real record. The last offset in a slot is kept
                 * so that the checkpoint LSN is close to the end of the record.
                 */
                slot_last_offset = (uint32_t)__wt_atomic_loadi64(&slot->slot_last_offset);
                if (__wt_lsn_offset(&slot->slot_start_lsn) != slot_last_offset)
                    __wt_atomic_store32(&slot->slot_start_lsn.l.offset, slot_last_offset);
                WT_ASSIGN_LSN(&log->write_start_lsn, &slot->slot_start_lsn);
                WT_ASSIGN_LSN(&log->write_lsn, &slot->slot_end_lsn);
                __wt_cond_signal(session, log->log_write_cond);
                WT_STAT_CONN_INCR(session, log_write_lsn);
                /*
                 * Signal the close thread if needed.
                 */
                if (F_ISSET_ATOMIC_16(slot, WTI_SLOT_CLOSEFH))
                    __wt_cond_signal(session, conn->log_mgr.file.cond);
            }
            __wti_log_slot_free(session, slot);
        }
    }
    __wt_spin_unlock(session, &log->log_writelsn_lock);
}

/*
 * __log_wrlsn_server --
 *     The log wrlsn server thread.
 */
static WT_THREAD_RET
__log_wrlsn_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;
    WT_LSN prev;
    WT_SESSION_IMPL *session;
    int yield;
    bool did_work;

    session = arg;
    conn = S2C(session);
    log_mgr = &conn->log_mgr;
    log = log_mgr->log;
    yield = 0;
    WT_INIT_LSN(&prev);
    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LOG)) {
        /*
         * Write out any log record buffers if anything was done since last time. Only call the
         * function to walk the slots if the system is not idle. On an idle system the alloc_lsn
         * will not advance and the written lsn will match the alloc_lsn.
         */
        if (__wt_log_cmp(&prev, &log->alloc_lsn) != 0 ||
          __wt_log_cmp(&log->write_lsn, &log->alloc_lsn) != 0)
            __wti_log_wrlsn(session, &yield);
        else
            WT_STAT_CONN_INCR(session, log_write_lsn_skip);
        prev = log->alloc_lsn;
        did_work = (yield == 0);

        /*
         * If __wti_log_wrlsn did work we want to yield instead of sleep.
         */
        if (yield++ < WT_THOUSAND)
            __wt_yield();
        else
            __wt_cond_auto_wait(session, log_mgr->wrlsn.cond, did_work, NULL);
    }
    /*
     * On close we need to do this one more time because there could be straggling log writes that
     * need to be written. It is possible to return EBUSY due to the database shutting down.
     * Therefore loop until we finish the log write.
     */
    do {
        WT_ERR_ERROR_OK(__wti_log_force_write(session, true, NULL), EBUSY, true);
    } while (ret == EBUSY);
    __wti_log_wrlsn(session, NULL);
    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "log wrlsn server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __log_server --
 *     The log server thread.
 */
static WT_THREAD_RET
__log_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;
    WT_SESSION_IMPL *session;
    uint64_t force_write_time_start, force_write_timediff;
    uint64_t time_start, time_stop, timediff;
    bool did_work, signalled;

    session = arg;
    conn = S2C(session);
    log_mgr = &conn->log_mgr;
    log = log_mgr->log;
    force_write_timediff = 0;
    signalled = false;

    /*
     * Set this to the number of milliseconds we want to run log force write, remove and
     * pre-allocation. Start it so that we run on the first time through.
     */
    timediff = WT_THOUSAND;
    force_write_time_start = time_start = __wt_clock(session);

    /*
     * The log server thread does a variety of work. It forces out any buffered log writes. It
     * pre-allocates log files and it performs log removal. The reason the wrlsn thread does not
     * force out the buffered writes is because we want to process and move the write_lsn forward as
     * quickly as possible. The same reason applies to why the log file server thread does not force
     * out the writes. That thread does fsync calls which can take a long time and we don't want log
     * records sitting in the buffer over the time it takes to sync out an earlier file.
     */
    did_work = true;
    while (FLD_ISSET(conn->server_flags, WT_CONN_SERVER_LOG)) {
        /*
         * Slots depend on future activity. Force out buffered writes in case we are idle. This
         * cannot be part of the wrlsn thread because of interaction advancing the write_lsn and a
         * buffer may need to wait for the write_lsn to advance in the case of a synchronous buffer.
         * We end up with a hang.
         */
        if (log_mgr->force_write_wait == 0 ||
          force_write_timediff >= log_mgr->force_write_wait * WT_THOUSAND) {
            WT_ERR_ERROR_OK(__wti_log_force_write(session, false, &did_work), EBUSY, false);
            force_write_time_start = __wt_clock(session);
        }
        /*
         * We don't want to remove or pre-allocate files as often as we want to force out log
         * buffers. Only do it once per second or if the condition was signalled.
         */
        if (timediff >= WT_THOUSAND || signalled) {

            /*
             * Perform log pre-allocation.
             */
            if (__wti_log_is_prealloc_enabled(session)) {
                /*
                 * Log file pre-allocation is disabled when a hot backup cursor is open because we
                 * have agreed not to rename or remove any files in the database directory.
                 */
                WT_WITH_HOTBACKUP_READ_LOCK(session, ret = __log_prealloc_once(session), NULL);
                WT_ERR(ret);
            }

            /*
             * Perform the removal.
             */
            if (F_ISSET(&conn->log_mgr, WT_LOG_REMOVE)) {
                if (__wt_try_writelock(session, &log->log_remove_lock) == 0) {
                    ret = __log_remove_once(session, 0);
                    __wt_writeunlock(session, &log->log_remove_lock);
                    WT_ERR(ret);
                } else
                    __wt_verbose(session, WT_VERB_LOG, "%s",
                      "log_remove: Blocked due to open log cursor holding remove lock");
            }
            time_start = __wt_clock(session);
        }

        /* Wait until the next event. */
        __wt_cond_auto_wait_signal(session, log_mgr->server.cond, did_work, NULL, &signalled);
        time_stop = __wt_clock(session);
        timediff = WT_CLOCKDIFF_MS(time_stop, time_start);
        force_write_timediff = WT_CLOCKDIFF_MS(time_stop, force_write_time_start);
    }

    /*
     * On close, force out buffered writes to move the write_lsn forward. The write_lsn needs to be
     * updated to avoid hangs from the wrlsn thread. It is possible to return EBUSY due to the
     * database shutting down. Therefore loop until we finish the log write.
     */
    do {
        WT_ERR_ERROR_OK(__wti_log_force_write(session, false, &did_work), EBUSY, true);
    } while (ret == EBUSY);
    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "log server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_logmgr_create --
 *     Initialize the log subsystem (before running recovery).
 */
int
__wt_logmgr_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WTI_LOG *log;
    WT_LOG_MANAGER *log_mgr;
    uint64_t now;

    conn = S2C(session);
    log_mgr = &conn->log_mgr;

    /*
     * Logging configuration is parsed early on for compatibility checking. It is separated from
     * turning on the subsystem. We only need to proceed here if logging is enabled.
     */
    if (!F_ISSET(log_mgr, WT_LOG_CONFIG_ENABLED))
        return (0);

    F_SET(log_mgr, WT_LOG_ENABLED);
    /*
     * Logging is on, allocate the WTI_LOG structure and open the log file.
     */
    WT_RET(__wt_calloc_one(session, &log_mgr->log));
    log = log_mgr->log;
    WT_RET(__wt_spin_init(session, &log->log_lock, "log"));
    WT_RET(__wt_spin_init(session, &log->log_fs_lock, "log files"));
    WT_RET(__wt_spin_init(session, &log->log_slot_lock, "log slot"));
    WT_RET(__wt_spin_init(session, &log->log_sync_lock, "log sync"));
    WT_RET(__wt_spin_init(session, &log->log_writelsn_lock, "log write LSN"));
    WT_RET(__wt_rwlock_init(session, &log->log_remove_lock));

    WT_INIT_LSN(&log->alloc_lsn);
    WT_INIT_LSN(&log->ckpt_lsn);
    WT_INIT_LSN(&log->first_lsn);
    WT_INIT_LSN(&log->sync_lsn);
    /*
     * We only use file numbers for directory sync, so this needs to initialized to zero.
     */
    WT_ZERO_LSN(&log->sync_dir_lsn);
    WT_INIT_LSN(&log->trunc_lsn);
    WT_INIT_LSN(&log->write_lsn);
    WT_INIT_LSN(&log->write_start_lsn);
    log->fileid = 0;
    WT_RET(__logmgr_version(session, false));

    WT_RET(__wt_cond_alloc(session, "log sync", &log->log_sync_cond));
    WT_RET(__wt_cond_alloc(session, "log write", &log->log_write_cond));
    WT_RET(__wti_log_open(session));
    WT_RET(__wti_log_slot_init(session, true));

    /* Write the start log record on creation, which is before recovery is run. */
    __wt_seconds(session, &now);
    WT_RET(__wt_log_printf(session, "SYSTEM: Log manager created at %" PRIu64, now));
    return (0);
}

/*
 * __wt_logmgr_open --
 *     Start the log service threads.
 */
int
__wt_logmgr_open(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG_MANAGER *log_mgr;
    uint64_t now;
    uint32_t session_flags;

    conn = S2C(session);
    log_mgr = &conn->log_mgr;

    /* If no log thread services are configured, we're done. */
    if (!F_ISSET(&conn->log_mgr, WT_LOG_ENABLED))
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_LOG);

    /*
     * Start the log close thread. It is not configurable. If logging is enabled, this thread runs.
     */
    session_flags = WT_SESSION_NO_DATA_HANDLES;
    WT_RET(__wt_open_internal_session(
      conn, "log-close-server", false, session_flags, 0, &log_mgr->file.session));
    WT_RET(__wt_cond_alloc(log_mgr->file.session, "log close server", &log_mgr->file.cond));

    /*
     * Start the log file close thread.
     */
    WT_RET(__wt_thread_create(
      log_mgr->file.session, &log_mgr->file.tid, __log_file_server, log_mgr->file.session));
    log_mgr->file.tid_set = true;

    /*
     * Start the log write LSN thread. It is not configurable. If logging is enabled, this thread
     * runs.
     */
    WT_RET(__wt_open_internal_session(
      conn, "log-wrlsn-server", false, session_flags, 0, &log_mgr->wrlsn.session));
    WT_RET(__wt_cond_auto_alloc(log_mgr->wrlsn.session, "log write lsn server", 10 * WT_THOUSAND,
      WT_MILLION, &log_mgr->wrlsn.cond));
    WT_RET(__wt_thread_create(
      log_mgr->wrlsn.session, &log_mgr->wrlsn.tid, __log_wrlsn_server, log_mgr->wrlsn.session));
    log_mgr->wrlsn.tid_set = true;

    /*
     * If a log server thread exists, the user may have reconfigured removal or pre-allocation.
     * Signal the thread. Otherwise the user wants removal and/or allocation and we need to start up
     * the thread.
     */
    if (log_mgr->server.session != NULL) {
        WT_ASSERT(session, log_mgr->server.cond != NULL);
        WT_ASSERT(session, log_mgr->server.tid_set == true);
        __wt_cond_signal(session, log_mgr->server.cond);
    } else {
        /* The log server gets its own session. */
        WT_RET(__wt_open_internal_session(
          conn, "log-server", false, session_flags, 0, &log_mgr->server.session));
        WT_RET(__wt_cond_auto_alloc(log_mgr->server.session, "log server", 50 * WT_THOUSAND,
          WT_MILLION, &log_mgr->server.cond));

        /*
         * Start the thread.
         */
        WT_RET(__wt_thread_create(
          log_mgr->server.session, &log_mgr->server.tid, __log_server, log_mgr->server.session));
        log_mgr->server.tid_set = true;
    }

    /* Write another startup log record with timestamp after recovery completes. */
    __wt_seconds(session, &now);
    WT_RET(__wt_log_printf(
      session, "SYSTEM: Log manager threads started post-recovery at %" PRIu64, now));
    return (0);
}

/*
 * __wt_logmgr_destroy --
 *     Destroy the log removal server thread and logging subsystem.
 */
int
__wt_logmgr_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG_MANAGER *log_mgr;

    conn = S2C(session);
    log_mgr = &conn->log_mgr;

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_LOG);

    if (!F_ISSET(&conn->log_mgr, WT_LOG_ENABLED)) {
        /*
         * We always set up the log_path so printlog can work without recovery. Therefore, always
         * free it, even if logging isn't on.
         */
        __wt_free(session, log_mgr->log_path);
        return (0);
    }
    if (log_mgr->server.tid_set) {
        __wt_cond_signal(session, log_mgr->server.cond);
        WT_TRET(__wt_thread_join(session, &log_mgr->server.tid));
        log_mgr->server.tid_set = false;
    }
    if (log_mgr->file.tid_set) {
        __wt_cond_signal(session, log_mgr->file.cond);
        WT_TRET(__wt_thread_join(session, &log_mgr->file.tid));
        log_mgr->file.tid_set = false;
    }
    if (log_mgr->file.session != NULL) {
        WT_TRET(__wt_session_close_internal(log_mgr->file.session));
        log_mgr->file.session = NULL;
    }
    if (log_mgr->wrlsn.tid_set) {
        __wt_cond_signal(session, log_mgr->wrlsn.cond);
        WT_TRET(__wt_thread_join(session, &log_mgr->wrlsn.tid));
        log_mgr->wrlsn.tid_set = false;
    }
    if (log_mgr->wrlsn.session != NULL) {
        WT_TRET(__wt_session_close_internal(log_mgr->wrlsn.session));
        log_mgr->wrlsn.session = NULL;
    }

    WT_TRET(__wti_log_slot_destroy(session));
    WT_TRET(__wti_log_close(session));

    /* Close the server thread's session. */
    if (log_mgr->server.session != NULL) {
        WT_TRET(__wt_session_close_internal(log_mgr->server.session));
        log_mgr->server.session = NULL;
    }

    /* Destroy the condition variables now that all threads are stopped */
    __wt_cond_destroy(session, &log_mgr->server.cond);
    __wt_cond_destroy(session, &log_mgr->file.cond);
    __wt_cond_destroy(session, &log_mgr->wrlsn.cond);

    __wt_cond_destroy(session, &log_mgr->log->log_sync_cond);
    __wt_cond_destroy(session, &log_mgr->log->log_write_cond);
    __wt_rwlock_destroy(session, &log_mgr->log->log_remove_lock);
    __wt_spin_destroy(session, &log_mgr->log->log_lock);
    __wt_spin_destroy(session, &log_mgr->log->log_fs_lock);
    __wt_spin_destroy(session, &log_mgr->log->log_slot_lock);
    __wt_spin_destroy(session, &log_mgr->log->log_sync_lock);
    __wt_spin_destroy(session, &log_mgr->log->log_writelsn_lock);
    __wt_free(session, log_mgr->log_path);
    __wt_free(session, log_mgr->log);
    return (ret);
}
