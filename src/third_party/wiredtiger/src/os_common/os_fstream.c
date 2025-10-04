/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Buffer size for streamed reads/writes. */
#define WT_STREAM_BUFSIZE 8192

/*
 * __fstream_close --
 *     Close a stream handle.
 */
static int
__fstream_close(WT_SESSION_IMPL *session, WT_FSTREAM *fstr)
{
    WT_DECL_RET;

    if (!F_ISSET(fstr, WT_STREAM_READ))
        WT_TRET(fstr->fstr_flush(session, fstr));

    WT_TRET(__wt_close(session, &fstr->fh));
    __wt_buf_free(session, &fstr->buf);
    __wt_free(session, fstr);
    return (ret);
}

/*
 * __fstream_flush --
 *     Flush the data from a stream.
 */
static int
__fstream_flush(WT_SESSION_IMPL *session, WT_FSTREAM *fstr)
{
    if (fstr->buf.size > 0) {
        WT_RET(__wt_write(session, fstr->fh, fstr->off, fstr->buf.size, fstr->buf.data));
        fstr->off += (wt_off_t)fstr->buf.size;
        fstr->buf.size = 0;
    }

    return (0);
}

/*
 * __fstream_flush_notsup --
 *     Stream flush unsupported.
 */
static int
__fstream_flush_notsup(WT_SESSION_IMPL *session, WT_FSTREAM *fstr)
{
    WT_RET_MSG(session, ENOTSUP, "%s: flush", fstr->name);
}

/*
 * __fstream_getline --
 *     Get a line from a stream. Implementation of the POSIX getline or BSD fgetln functions
 *     (finding the function in a portable way is hard, it's simple enough to write it instead).
 *     Note: Unlike the standard getline calls, this function doesn't include the trailing newline
 *     character in the returned buffer and discards empty lines (so the caller's EOF marker is a
 *     returned line length of 0).
 */
static int
__fstream_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, WT_ITEM *buf)
{
    size_t len;
    char c;
    const char *p;

    /*
     * We always NUL-terminate the returned string (even if it's empty), make sure there's buffer
     * space for a trailing NUL in all cases.
     */
    WT_RET(__wt_buf_init(session, buf, 100));

    for (;;) {
        /* Check if we need to refill the buffer. */
        if (WT_PTRDIFF(fstr->buf.data, fstr->buf.mem) >= fstr->buf.size) {
            len = WT_MIN(WT_STREAM_BUFSIZE, (size_t)(fstr->size - fstr->off));
            if (len == 0)
                break; /* EOF */
            WT_RET(__wt_buf_initsize(session, &fstr->buf, len));
            WT_RET(__wt_read(session, fstr->fh, fstr->off, len, fstr->buf.mem));
            fstr->off += (wt_off_t)len;
        }

        c = *(p = fstr->buf.data);
        fstr->buf.data = ++p;

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
 *     Stream getline unsupported.
 */
static int
__fstream_getline_notsup(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, WT_ITEM *buf)
{
    WT_UNUSED(buf);
    WT_RET_MSG(session, ENOTSUP, "%s: getline", fstr->name);
}

/*
 * __fstream_printf --
 *     ANSI C vfprintf.
 */
static int
__fstream_printf(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt, va_list ap)
{
    WT_ITEM *buf;
    size_t len, space;
    char *p;
    va_list ap_copy;

    buf = &fstr->buf;

    for (;;) {
        va_copy(ap_copy, ap);
        if ((p = buf->mem) != NULL)
            p += buf->size;
        space = buf->memsize - buf->size;

        WT_ASSERT(session, buf->memsize >= buf->size);
        if (buf->mem == NULL)
            WT_ASSERT(session, space == 0);

        WT_RET(__wt_vsnprintf_len_set(p, space, &len, fmt, ap_copy));
        va_end(ap_copy);

        if (len < space) {
            buf->size += len;

            return (buf->size >= WT_STREAM_BUFSIZE ? __wt_fflush(session, fstr) : 0);
        }
        WT_RET(__wt_buf_extend(session, buf, buf->size + len + 1));
    }
}

/*
 * __fstream_printf_notsup --
 *     ANSI C vfprintf unsupported.
 */
static int
__fstream_printf_notsup(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt, va_list ap)
{
    WT_UNUSED(fmt);
    WT_UNUSED(ap);
    WT_RET_MSG(session, ENOTSUP, "%s: printf", fstr->name);
}

/*
 * __wt_fopen --
 *     Open a stream handle.
 */
int
__wt_fopen(WT_SESSION_IMPL *session, const char *name, uint32_t open_flags, uint32_t flags,
  WT_FSTREAM **fstrp)
{
    WT_DECL_RET;
    WT_FH *fh;
    WT_FSTREAM *fstr;

    *fstrp = NULL;

    fstr = NULL;

    WT_RET(__wt_open(session, name, WT_FS_OPEN_FILE_TYPE_REGULAR, open_flags, &fh));

    WT_ERR(__wt_calloc_one(session, &fstr));
    fstr->fh = fh;
    fstr->name = fh->name;
    fstr->flags = flags;

    fstr->close = __fstream_close;
    WT_ERR(__wt_filesize(session, fh, &fstr->size));
    if (LF_ISSET(WT_STREAM_APPEND))
        fstr->off = fstr->size;
    if (LF_ISSET(WT_STREAM_APPEND | WT_STREAM_WRITE)) {
        fstr->fstr_flush = __fstream_flush;
        fstr->fstr_getline = __fstream_getline_notsup;
        fstr->fstr_printf = __fstream_printf;
    } else {
        WT_ASSERT(session, LF_ISSET(WT_STREAM_READ));
        fstr->fstr_flush = __fstream_flush_notsup;
        fstr->fstr_getline = __fstream_getline;
        fstr->fstr_printf = __fstream_printf_notsup;
    }
    *fstrp = fstr;
    return (0);

err:
    WT_TRET(__wt_close(session, &fh));
    __wt_free(session, fstr);
    return (ret);
}
