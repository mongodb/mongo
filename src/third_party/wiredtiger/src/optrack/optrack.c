/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_optrack_record_funcid --
 *     Allocate and record optrack function ID.
 */
void
__wt_optrack_record_funcid(WT_SESSION_IMPL *session, const char *func, uint16_t *func_idp)
{
    static uint16_t optrack_uid = 0; /* Unique for the process lifetime. */
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    wt_off_t fsize;
    bool locked;

    conn = S2C(session);
    locked = false;

    WT_ERR(__wt_scr_alloc(session, strlen(func) + 32, &tmp));

    __wt_spin_lock(session, &conn->optrack_map_spinlock);
    locked = true;
    if (*func_idp == 0) {
        *func_idp = ++optrack_uid;

        WT_ERR(__wt_buf_fmt(session, tmp, "%" PRIu16 " %s\n", *func_idp, func));
        WT_ERR(__wt_filesize(session, conn->optrack_map_fh, &fsize));
        WT_ERR(__wt_write(session, conn->optrack_map_fh, fsize, tmp->size, tmp->data));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "operation tracking initialization failure"));
    }

    if (locked)
        __wt_spin_unlock(session, &conn->optrack_map_spinlock);
    __wt_scr_free(session, &tmp);
}

/*
 * __optrack_open_file --
 *     Open the per-session operation-tracking file.
 */
static int
__optrack_open_file(WT_SESSION_IMPL *session)
{
    struct timespec ts;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_OPTRACK_HEADER optrack_header = {
      WT_OPTRACK_VERSION, 0, (uint32_t)WT_TSC_DEFAULT_RATIO * WT_THOUSAND, 0, 0};

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_OPTRACK))
        WT_RET_MSG(session, WT_ERROR, "WT_CONN_OPTRACK not set");

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_filename_construct(
      session, conn->optrack_path, "optrack", conn->optrack_pid, session->id, buf));
    WT_ERR(__wt_open(session, (const char *)buf->data, WT_FS_OPEN_FILE_TYPE_REGULAR,
      WT_FS_OPEN_CREATE, &session->optrack_fh));

    /* Indicate whether this is an internal session */
    if (F_ISSET(session, WT_SESSION_INTERNAL))
        optrack_header.optrack_session_internal = 1;

    /*
     * Record the clock ticks to nanoseconds ratio. Multiply it by one thousand, so we can use a
     * fixed width integer.
     */
    optrack_header.optrack_tsc_nsec_ratio = (uint32_t)(__wt_process.tsc_nsec_ratio * WT_THOUSAND);

    /* Record the time in seconds since the Epoch. */
    __wt_epoch(session, &ts);
    optrack_header.optrack_seconds_epoch = (uint64_t)ts.tv_sec;

    /* Write the header into the operation-tracking file. */
    WT_ERR(session->optrack_fh->handle->fh_write(session->optrack_fh->handle, (WT_SESSION *)session,
      0, sizeof(WT_OPTRACK_HEADER), &optrack_header));

    session->optrack_offset = sizeof(WT_OPTRACK_HEADER);

    if (0) {
err:
        WT_TRET(__wt_close(session, &session->optrack_fh));
    }
    __wt_scr_free(session, &buf);

    return (ret);
}

/*
 * __wt_optrack_flush_buffer --
 *     Flush optrack buffer. Returns the number of bytes flushed to the file.
 */
void
__wt_optrack_flush_buffer(WT_SESSION_IMPL *s)
{
    if (s->optrack_fh == NULL && __optrack_open_file(s) != 0)
        return;

    /*
     * We're not using the standard write path deliberately, that's quite a bit of additional code
     * (including atomic operations), and this work should be as light-weight as possible.
     */
    if (s->optrack_fh->handle->fh_write(s->optrack_fh->handle, (WT_SESSION *)s,
          (wt_off_t)s->optrack_offset, s->optrackbuf_ptr * sizeof(WT_OPTRACK_RECORD),
          s->optrack_buf) == 0)
        s->optrack_offset += s->optrackbuf_ptr * sizeof(WT_OPTRACK_RECORD);
}
