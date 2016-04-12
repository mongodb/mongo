/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __fstream_close --
 *	Close a stream handle.
 */
static int
__fstream_close(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
	WT_DECL_RET;

	WT_TRET(__wt_close(session, &fs->fh));
	__wt_buf_free(session, &fs->buf);
	__wt_free(session, fs);
	return (ret);
}

/*
 * __fstream_flush --
 *	Flush the data from a stream.
 */
static int
__fstream_flush(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
	WT_UNUSED(session);
	WT_UNUSED(fs);

	if (fs->buf.size > 0) {
		WT_RET(__wt_write(
		    session, fs->fh, fs->off, fs->buf.size, fs->buf.data));
		fs->off += (wt_off_t)fs->buf.size;
		fs->buf.size = 0;
	}

	return (0);
}

/*
 * __fstream_getline --
 *	Get a line from a stream.
 *
 * Implementation of the POSIX getline or BSD fgetln functions (finding the
 * function in a portable way is hard, it's simple enough to write it instead).
 *
 * Note: Unlike the standard getline calls, this function doesn't include the
 * trailing newline character in the returned buffer and discards empty lines
 * (so the caller's EOF marker is a returned line length of 0).
 */
static int
__fstream_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fs, WT_ITEM *buf)
{
	char c;

	/*
	 * We always NUL-terminate the returned string (even if it's empty),
	 * make sure there's buffer space for a trailing NUL in all cases.
	 */
	WT_RET(__wt_buf_init(session, buf, 100));

	for (;;) {
		/* TODO: buffering. */
		if (fs->off == fs->size)
			break;
		WT_RET(__wt_read(session, fs->fh, fs->off, 1, &c));
		++fs->off;

		/* Leave space for a trailing NUL. */
		WT_RET(__wt_buf_extend(session, buf, buf->size + 2));
		if (c == '\n') {
			if (buf->size == 0)
				continue;
			break;
		}
		((char *)buf->mem)[buf->size++] = (char)c;
	}

	((char *)buf->mem)[buf->size] = '\0';

	return (0);
}

/*
 * __fstream_printf --
 *	ANSI C vfprintf.
 */
static int
__fstream_printf(
    WT_SESSION_IMPL *session, WT_FSTREAM *fs, const char *fmt, va_list ap)
{
	WT_ITEM *buf;
	va_list ap_copy;
	size_t len, space;
	char *p;

	buf = &fs->buf;

	for (;;) {
		va_copy(ap_copy, ap);
		p = (char *)((uint8_t *)buf->mem + buf->size);
		WT_ASSERT(session, buf->memsize >= buf->size);
		space = buf->memsize - buf->size;
		len = (size_t)vsnprintf(p, space, fmt, ap_copy);
		va_end(ap_copy);

		if (len < space) {
			buf->size += len;
			/* TODO: buffering */
			return (__wt_fflush(session, fs));
		}
		WT_RET(__wt_buf_extend(session, buf, buf->size + len + 1));
	}
}

/*
 * __wt_fopen --
 *	Open a stream handle.
 */
int
__wt_fopen(WT_SESSION_IMPL *session,
    const char *name, uint32_t open_flags, uint32_t flags, WT_FSTREAM **fsp)
{
	WT_DECL_RET;
	WT_FH *fh;
	WT_FSTREAM *fs;

	fs = NULL;

	WT_RET(__wt_open(session, name, WT_FILE_TYPE_REGULAR, open_flags, &fh));

	WT_ERR(__wt_calloc_one(session, &fs));
	fs->fh = fh;
	fs->name = fh->name;
	fs->close = __fstream_close;
	fs->getline = __fstream_getline;
	fs->flush = __fstream_flush;
	fs->printf = __fstream_printf;
	WT_ERR(__wt_filesize(session, fh, &fs->size));
	if (LF_ISSET(WT_STREAM_APPEND))
		fs->off = fs->size;
	*fsp = fs;
	return (0);

err:	__wt_close(session, &fh);
	__wt_free(session, *fsp);
	return (ret);
}

