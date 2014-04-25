/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_buf_grow --
 *	Grow a buffer that's currently in-use.
 */
static inline int
__wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	/* Clear buffers previously used for mapped returns. */
	if (F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_buf_clear(buf);

	return (size > buf->memsize ?
	    __wt_buf_grow_worker(session, buf, size) : 0);
}

/*
 * __wt_buf_extend --
 *	Grow a buffer that's currently in-use.
 */
static inline int
__wt_buf_extend(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	/*
	 * The difference between __wt_buf_grow and __wt_buf_extend is that the
	 * latter is expected to be called repeatedly for the same buffer, and
	 * so grows the buffer exponentially to avoid repeated costly calls to
	 * realloc.
	 */
	return (size > buf->memsize ?
	    __wt_buf_grow(session, buf, WT_MAX(size, 2 * buf->memsize)) : 0);
}

/*
 * __wt_buf_init --
 *	Initialize a buffer at a specific size.
 */
static inline int
__wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	buf->data = buf->mem;
	WT_RET(__wt_buf_grow(session, buf, size));
	buf->size = 0;				/* Clear the data length */

	return (0);
}

/*
 * __wt_buf_initsize --
 *	Initialize a buffer at a specific size, and set the data length.
 */
static inline int
__wt_buf_initsize(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	buf->data = buf->mem;
	WT_RET(__wt_buf_grow(session, buf, size));
	buf->size = size;			/* Set the data length. */

	return (0);
}

/*
 * __wt_buf_set --
 *	Set the contents of the buffer.
 */
static inline int
__wt_buf_set(
    WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data, size_t size)
{
	/* Ensure the buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	/* Copy the data, allowing for overlapping strings. */
	memmove(buf->mem, data, size);

	return (0);
}

/*
 * __wt_buf_set_printable --
 *	Set the contents of the buffer to a printable representation of a
 * byte string.
 */
static inline int
__wt_buf_set_printable(
    WT_SESSION_IMPL *session, WT_ITEM *buf, const void *from_arg, size_t size)
{
	return (__wt_raw_to_esc_hex(session, from_arg, size, buf));
}

/*
 * __wt_buf_free --
 *	Free a buffer.
 */
static inline void
__wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf)
{
	if (!F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_free(session, buf->mem);
	__wt_buf_clear(buf);
}

/*
 * __wt_scr_free --
 *	Release a scratch buffer.
 */
static inline void
__wt_scr_free(WT_ITEM **bufp)
{
	if (*bufp == NULL)
		return;
	F_CLR(*bufp, WT_ITEM_INUSE);
	*bufp = NULL;
}
