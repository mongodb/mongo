/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_optrack_record_funcid --
 *	Record optrack function id
 */
void
__wt_optrack_record_funcid(
    WT_SESSION_IMPL *session, const char *func, uint16_t *func_idp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	wt_off_t fsize;

	conn = S2C(session);

	WT_ERR(__wt_scr_alloc(session, strlen(func) + 32, &tmp));

	__wt_spin_lock(session, &conn->optrack_map_spinlock);
	if (*func_idp == 0) {
		*func_idp = ++conn->optrack_uid;

		WT_ERR(__wt_buf_fmt(
		    session, tmp, "%" PRIu16 " %s\n", *func_idp, func));
		WT_ERR(__wt_filesize(session, conn->optrack_map_fh, &fsize));
		WT_ERR(__wt_write(session,
		    conn->optrack_map_fh, fsize, tmp->size, tmp->data));
	}

	if (0) {
err:		WT_PANIC_MSG(session, ret, "%s", __func__);
	}

	__wt_spin_unlock(session, &conn->optrack_map_spinlock);
	__wt_scr_free(session, &tmp);
}

/*
 * __wt_optrack_open_file --
 *	Open the per-session operation-tracking file.
 */
int
__wt_optrack_open_file(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_OPTRACK_HEADER optrack_header = {WT_OPTRACK_VERSION, 0};

	conn = S2C(session);

	if (!F_ISSET(conn, WT_CONN_OPTRACK))
		return (WT_ERROR);

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_filename_construct(session, conn->optrack_path,
	    "optrack", conn->optrack_pid, session->id, buf));
	WT_ERR(__wt_open(session,
	    (const char *)buf->data, WT_FS_OPEN_FILE_TYPE_REGULAR,
	    WT_FS_OPEN_CREATE, &session->optrack_fh));

	/* Write the header into the operation-tracking file. */
	if (F_ISSET(session, WT_SESSION_INTERNAL))
		optrack_header.optrack_session_internal = 1;

	WT_ERR(session->optrack_fh->handle->fh_write(
	    session->optrack_fh->handle, (WT_SESSION *)session,
	    0, sizeof(WT_OPTRACK_HEADER), &optrack_header));

	session->optrack_offset = sizeof(WT_OPTRACK_HEADER);

	if (0) {
err:		WT_TRET(__wt_close(session, &session->optrack_fh));
	}
	__wt_scr_free(session, &buf);

	return (ret);
}

/*
 * __wt_optrack_flush_buffer --
 *	Flush optrack buffer. Returns the number of bytes flushed to the file.
 */
size_t
__wt_optrack_flush_buffer(WT_SESSION_IMPL *s)
{
	WT_DECL_RET;

	if (s->optrack_fh == NULL)
		if (__wt_optrack_open_file(s))
			return (0);

	ret = s->optrack_fh->handle->fh_write(s->optrack_fh->handle,
	    (WT_SESSION *)s, (wt_off_t)s->optrack_offset,
	    s->optrackbuf_ptr * sizeof(WT_OPTRACK_RECORD), s->optrack_buf);
	if (ret == 0)
		return (s->optrackbuf_ptr * sizeof(WT_OPTRACK_RECORD));
	else
		return (0);
}

/*
 * __wt_optrack_get_expensive_timestamp --
 *       Obtain a timestamp via a system call on platforms where obtaining it
 *       directly from the hardware register is not supported.
 */
uint64_t
__wt_optrack_get_expensive_timestamp(WT_SESSION_IMPL *session)
{
	struct timespec tsp;

	__wt_epoch_raw(session, &tsp);
	return ((uint64_t)(tsp.tv_sec * WT_BILLION + tsp.tv_nsec));
}
