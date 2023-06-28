/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Define functions that increment histogram statistics for filesystem operations latency.
 */
WT_STAT_MSECS_HIST_INCR_FUNC(fsread, perf_hist_fsread_latency, 10)
WT_STAT_MSECS_HIST_INCR_FUNC(fswrite, perf_hist_fswrite_latency, 10)

/*
 * __wt_fsync --
 *     POSIX fsync.
 */
static inline int
__wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *handle;

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: handle-sync", fh->handle->name);

    handle = fh->handle;
    /*
     * There is no way to check when the non-blocking sync-file-range is complete, but we track the
     * time taken in the call for completeness.
     */
    WT_STAT_CONN_INCR_ATOMIC(session, thread_fsync_active);
    WT_STAT_CONN_INCR(session, fsync_io);
    if (block)
        ret = (handle->fh_sync == NULL ? 0 : handle->fh_sync(handle, (WT_SESSION *)session));
    else
        ret =
          (handle->fh_sync_nowait == NULL ? 0 :
                                            handle->fh_sync_nowait(handle, (WT_SESSION *)session));
    WT_STAT_CONN_DECR_ATOMIC(session, thread_fsync_active);
    return (ret);
}

/*
 * __wt_fextend --
 *     Extend a file.
 */
static inline int
__wt_fextend(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
{
    WT_FILE_HANDLE *handle;
#ifdef HAVE_DIAGNOSTIC
    wt_off_t cur_size;
#endif

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));
    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: handle-extend: to %" PRIuMAX, fh->handle->name,
      (uintmax_t)offset);

    /*
     * Our caller is responsible for handling any locking issues, all we have to do is find a
     * function to call.
     */
    handle = fh->handle;
#ifdef HAVE_DIAGNOSTIC
    /* Make sure we don't try to shrink the file during backup. */
    if (handle->fh_size != NULL) {
        WT_RET(handle->fh_size(handle, (WT_SESSION *)session, &cur_size));
        WT_ASSERT(session, cur_size <= offset || S2C(session)->hot_backup_start == 0);
    }
#endif
    if (handle->fh_extend_nolock != NULL)
        return (handle->fh_extend_nolock(handle, (WT_SESSION *)session, offset));
    if (handle->fh_extend != NULL)
        return (handle->fh_extend(handle, (WT_SESSION *)session, offset));
    return (__wt_set_return(session, ENOTSUP));
}

/*
 * __wt_file_lock --
 *     Lock/unlock a file.
 */
static inline int
__wt_file_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
{
    WT_FILE_HANDLE *handle;

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: handle-lock: %s", fh->handle->name,
      lock ? "lock" : "unlock");

    handle = fh->handle;
    return (handle->fh_lock == NULL ? 0 : handle->fh_lock(handle, (WT_SESSION *)session, lock));
}

/*
 * __wt_read --
 *     POSIX pread.
 */
static inline int
__wt_read(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
    WT_DECL_RET;
    uint64_t time_start, time_stop;

    __wt_verbose_debug2(session, WT_VERB_HANDLEOPS,
      "%s: handle-read: %" WT_SIZET_FMT " at %" PRIuMAX, fh->handle->name, len, (uintmax_t)offset);

    WT_STAT_CONN_INCR_ATOMIC(session, thread_read_active);
    WT_STAT_CONN_INCR(session, read_io);
    time_start = __wt_clock(session);

    ret = fh->handle->fh_read(fh->handle, (WT_SESSION *)session, offset, len, buf);

    /* Flag any failed read: if we're in startup, it may be fatal. */
    if (ret != 0)
        F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);

    time_stop = __wt_clock(session);
    __wt_stat_msecs_hist_incr_fsread(session, WT_CLOCKDIFF_MS(time_stop, time_start));
    WT_STAT_CONN_DECR_ATOMIC(session, thread_read_active);
    return (ret);
}

/*
 * __wt_filesize --
 *     Get the size of a file in bytes, by file handle.
 */
static inline int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: handle-size", fh->handle->name);

    return (fh->handle->fh_size(fh->handle, (WT_SESSION *)session, sizep));
}

/*
 * __wt_ftruncate --
 *     Truncate a file.
 */
static inline int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
{
    WT_FILE_HANDLE *handle;
#ifdef HAVE_DIAGNOSTIC
    wt_off_t cur_size;
#endif

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: handle-truncate: to %" PRIuMAX, fh->handle->name,
      (uintmax_t)offset);

    /*
     * Our caller is responsible for handling any locking issues, all we have to do is find a
     * function to call.
     */
    handle = fh->handle;
#ifdef HAVE_DIAGNOSTIC
    /* Make sure we don't try to shrink the file during backup. */
    if (handle->fh_size != NULL) {
        WT_RET(handle->fh_size(handle, (WT_SESSION *)session, &cur_size));
        WT_ASSERT(session, cur_size <= offset || S2C(session)->hot_backup_start == 0);
    }
#endif
    if (handle->fh_truncate != NULL)
        return (handle->fh_truncate(handle, (WT_SESSION *)session, offset));
    return (__wt_set_return(session, ENOTSUP));
}

/*
 * __wt_write --
 *     POSIX pwrite.
 */
static inline int
__wt_write(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
    WT_DECL_RET;
    uint64_t time_start, time_stop;

    WT_ASSERT(session,
      !F_ISSET(S2C(session), WT_CONN_READONLY) ||
        WT_STRING_MATCH(fh->name, WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)));

    __wt_verbose_debug2(session, WT_VERB_HANDLEOPS,
      "%s: handle-write: %" WT_SIZET_FMT " at %" PRIuMAX, fh->handle->name, len, (uintmax_t)offset);

    /*
     * Do a final panic check before I/O, so we stop writing as quickly as possible if there's an
     * unanticipated error. We aren't handling the error correctly by definition, and writing won't
     * make things better.
     */
    WT_RET(WT_SESSION_CHECK_PANIC(session));

    WT_STAT_CONN_INCR(session, write_io);
    WT_STAT_CONN_INCR_ATOMIC(session, thread_write_active);
    time_start = __wt_clock(session);

    ret = fh->handle->fh_write(fh->handle, (WT_SESSION *)session, offset, len, buf);

    time_stop = __wt_clock(session);
    __wt_stat_msecs_hist_incr_fswrite(session, WT_CLOCKDIFF_MS(time_stop, time_start));
    (void)__wt_atomic_addv64(&fh->written, len);
    WT_STAT_CONN_DECR_ATOMIC(session, thread_write_active);
    return (ret);
}
