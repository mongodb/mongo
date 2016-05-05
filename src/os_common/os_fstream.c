/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Buffer size for streamed reads/writes. */
#define	WT_STREAM_BUFSIZE	8192

/*
 * __fstream_close --
 *	Close a stream handle.
 */
static int
__fstream_close(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
	WT_DECL_RET;

	if (!F_ISSET(fs, WT_STREAM_READ))
		WT_TRET(fs->flush(session, fs));

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
	if (fs->buf.size > 0) {
		WT_RET(__wt_write(
		    session, fs->fh, fs->off, fs->buf.size, fs->buf.data));
		fs->off += (wt_off_t)fs->buf.size;
		fs->buf.size = 0;
	}

	return (0);
}

/*
 * __fstream_flush_notsup --
 *	Stream flush unsupported.
 */
static int
__fstream_flush_notsup(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
	WT_RET_MSG(session, ENOTSUP, "%s: flush", fs->name);
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
	const char *p;
	size_t len;
	char c;

	/*
	 * We always NUL-terminate the returned string (even if it's empty),
	 * make sure there's buffer space for a trailing NUL in all cases.
	 */
	WT_RET(__wt_buf_init(session, buf, 100));

	for (;;) {
		/* Check if we need to refill the buffer. */
		if (WT_PTRDIFF(fs->buf.data, fs->buf.mem) >= fs->buf.size) {
			len = WT_MIN(WT_STREAM_BUFSIZE,
			    (size_t)(fs->size - fs->off));
			if (len == 0)
				break; /* EOF */
			WT_RET(__wt_buf_initsize(session, &fs->buf, len));
			WT_RET(__wt_read(
			    session, fs->fh, fs->off, len, fs->buf.mem));
			fs->off += (wt_off_t)len;
		}

		c = *(p = fs->buf.data);
		fs->buf.data = ++p;

		/* Leave space for a trailing NUL. */
		WT_RET(__wt_buf_extend(session, buf, buf->size + 2));
		if (c == '\n') {
			if (buf->size == 0)
				continue;
			break;
		}
		((char *)buf->mem)[buf->size++] = c;
	}

	((char *)buf->mem)[buf->size] = '\0';

	return (0);
}

/*
 * __fstream_getline_notsup --
 *	Stream getline unsupported.
 */
static int
__fstream_getline_notsup(WT_SESSION_IMPL *session, WT_FSTREAM *fs, WT_ITEM *buf)
{
	WT_UNUSED(buf);
	WT_RET_MSG(session, ENOTSUP, "%s: getline", fs->name);
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

			return (buf->size >= WT_STREAM_BUFSIZE ?
			    __wt_fflush(session, fs) : 0);
		}
		WT_RET(__wt_buf_extend(session, buf, buf->size + len + 1));
	}
}

/*
 * __fstream_printf_notsup --
 *	ANSI C vfprintf unsupported.
 */
static int
__fstream_printf_notsup(
    WT_SESSION_IMPL *session, WT_FSTREAM *fs, const char *fmt, va_list ap)
{
	WT_UNUSED(fmt);
	WT_UNUSED(ap);
	WT_RET_MSG(session, ENOTSUP, "%s: printf", fs->name);
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

	WT_RET(__wt_open(
	    session, name, WT_OPEN_FILE_TYPE_REGULAR, open_flags, &fh));

	WT_ERR(__wt_calloc_one(session, &fs));
	fs->fh = fh;
	fs->name = fh->name;
	fs->flags = flags;

	fs->close = __fstream_close;
	WT_ERR(__wt_filesize(session, fh, &fs->size));
	if (LF_ISSET(WT_STREAM_APPEND))
		fs->off = fs->size;
	if (LF_ISSET(WT_STREAM_APPEND | WT_STREAM_WRITE)) {
		fs->flush = __fstream_flush;
		fs->getline = __fstream_getline_notsup;
		fs->printf = __fstream_printf;
	} else {
		WT_ASSERT(session, LF_ISSET(WT_STREAM_READ));
		fs->flush = __fstream_flush_notsup;
		fs->getline = __fstream_getline;
		fs->printf = __fstream_printf_notsup;
	}
	*fsp = fs;
	return (0);

err:	WT_TRET(__wt_close(session, &fh));
	__wt_free(session, *fsp);
	return (ret);
}
