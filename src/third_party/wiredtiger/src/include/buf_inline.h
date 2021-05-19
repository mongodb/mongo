/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_buf_grow --
 *     Grow a buffer that may be in-use, and ensure that all data is local to the buffer.
 */
static inline int
__wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
    /*
     * Take any offset in the buffer into account when calculating the size to allocate, it saves
     * complex calculations in our callers to decide if the buffer is large enough in the case of
     * buffers with offset data pointers.
     */
    return (!WT_DATA_IN_ITEM(buf) || size + WT_PTRDIFF(buf->data, buf->mem) > buf->memsize ?
        __wt_buf_grow_worker(session, buf, size) :
        0);
}

/*
 * __wt_buf_extend --
 *     Grow a buffer that's currently in-use.
 */
static inline int
__wt_buf_extend(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
    /*
     * The difference between __wt_buf_grow and __wt_buf_extend is that the latter is expected to be
     * called repeatedly for the same buffer, and so grows the buffer exponentially to avoid
     * repeated costly calls to realloc.
     */
    return (size > buf->memsize ? __wt_buf_grow(session, buf, WT_MAX(size, 2 * buf->memsize)) : 0);
}

/*
 * __wt_buf_init --
 *     Create an empty buffer at a specific size.
 */
static inline int
__wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
    /*
     * The buffer grow function does what we need, but anticipates data referenced by the buffer.
     * Avoid any data copy by setting data to reference the buffer's allocated memory, and clearing
     * it.
     */
    buf->data = buf->mem;
    buf->size = 0;
    return (__wt_buf_grow(session, buf, size));
}

/*
 * __wt_buf_initsize --
 *     Create an empty buffer at a specific size, and set the data length.
 */
static inline int
__wt_buf_initsize(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
    WT_RET(__wt_buf_init(session, buf, size));

    buf->size = size; /* Set the data length. */

    return (0);
}

/*
 * __wt_buf_set --
 *     Set the contents of the buffer.
 */
static inline int
__wt_buf_set(WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data, size_t size)
{
    /*
     * The buffer grow function does what we need, but expects the data to be referenced by the
     * buffer. If we're copying data from outside the buffer, set it up so it makes sense to the
     * buffer grow function. (No test needed, this works if WT_ITEM.data is already set to "data".)
     */
    buf->data = data;
    buf->size = size;
    return (__wt_buf_grow(session, buf, size));
}

/*
 * __wt_buf_setstr --
 *     Set the contents of the buffer to a NUL-terminated string.
 */
static inline int
__wt_buf_setstr(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *s)
{
    return (__wt_buf_set(session, buf, s, strlen(s) + 1));
}

/*
 * __wt_buf_free --
 *     Free a buffer.
 */
static inline void
__wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf)
{
    __wt_free(session, buf->mem);

    memset(buf, 0, sizeof(WT_ITEM));
}

/*
 * __wt_scr_free --
 *     Release a scratch buffer.
 */
static inline void
__wt_scr_free(WT_SESSION_IMPL *session, WT_ITEM **bufp)
{
    WT_ITEM *buf;

    if ((buf = *bufp) == NULL)
        return;
    *bufp = NULL;

    if (session->scratch_cached + buf->memsize >= S2C(session)->session_scratch_max) {
        __wt_free(session, buf->mem);
        buf->memsize = 0;
    } else
        session->scratch_cached += buf->memsize;

    buf->data = NULL;
    buf->size = 0;
    F_CLR(buf, WT_ITEM_INUSE);
}
