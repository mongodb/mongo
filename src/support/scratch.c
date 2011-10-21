/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_buf_clear(WT_BUF *);

/*
 * __wt_buf_clear --
 *	Clear a buffer.
 */
static void
__wt_buf_clear(WT_BUF *buf)
{
	buf->data = NULL;
	buf->size = 0;

	buf->mem = NULL;
	buf->memsize = 0;

	/* Note: don't clear the flags, the buffer remains marked in-use. */
}

/*
 * __wt_buf_init --
 *	Initialize a buffer at a specific size.
 */
int
__wt_buf_init(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size)
{
	WT_ASSERT(session, size <= UINT32_MAX);

	if (size > buf->memsize)
		WT_RET(__wt_realloc(session, &buf->memsize, size, &buf->mem));

	buf->data = buf->mem;
	buf->size = 0;

	return (0);
}

/*
 * __wt_buf_initsize --
 *	Initialize a buffer at a specific size, and set the data length.
 */
int
__wt_buf_initsize(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size)
{
	WT_RET(__wt_buf_init(session, buf, size));

	buf->size = WT_STORE_SIZE(size);	/* Set the data length. */

	return (0);
}

/*
 * __wt_buf_grow --
 *	Grow a buffer that's currently in-use.
 */
int
__wt_buf_grow(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size)
{
	size_t offset;

	WT_ASSERT(session, size <= UINT32_MAX);

	if (size > buf->memsize) {
		/*
		 * Reallocate the buffer's memory, but maintain the previous
		 * data reference.
		 */
		offset = (buf->data == NULL) ? 0 :
		    WT_PTRDIFF(buf->data, buf->mem);

		WT_RET(__wt_realloc(session, &buf->memsize, size, &buf->mem));

		buf->data = (uint8_t *)buf->mem + offset;
	}
	return (0);
}

/*
 * __wt_buf_set --
 *	Set the contents of the buffer.
 */
int
__wt_buf_set(
    WT_SESSION_IMPL *session, WT_BUF *buf, const void *data, size_t size)
{
	/* Ensure the buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	memcpy(buf->mem, data, size);

	return (0);
}

/*
 * __wt_buf_set_printable --
 *	Set the contents of the buffer to a printable representation.
 */
int
__wt_buf_set_printable(
    WT_SESSION_IMPL *session, WT_BUF *buf, const void *from_arg, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	const char *from;
	char *to;

	/*
	 * The maximum size is the byte-string length, all hex characters, plus
	 * a trailing nul byte.  Throw in a few extra bytes for fun.
	 */
	WT_RET(__wt_buf_init(session, buf, size * 2 + 20));

	buf->size = 0;
	for (from = from_arg, to = buf->mem; size > 0; --size, ++from)
		if (isprint(from[0]))
			to[buf->size++] = from[0];
		else {
			to[buf->size++] = hex[(from[0] & 0xf0) >> 4];
			to[buf->size++] = hex[from[0] & 0x0f];
		}
	return (0);
}

/*
 * __wt_buf_steal --
 *	Steal a buffer for another purpose.
 */
void *
__wt_buf_steal(WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t *sizep)
{
	void *retp;

	/*
	 * Sometimes we steal a buffer for a different purpose, for example,
	 * we've read in an overflow item, and now it's going to become a key
	 * on an in-memory page, eventually freed when the page is discarded.
	 *
	 * First, correct for the possibility the data field doesn't point to
	 * the start of memory (if we only have a single memory reference, it
	 * must point to the start of the memory chunk, otherwise freeing the
	 * memory isn't going to work out).  This is possibly a common case:
	 * it happens when we read in overflow items and we want to skip over
	 * the page header, so buf->data references a location past buf->mem.
	 */
	if (buf->data != buf->mem) {
		WT_ASSERT(session,
		    buf->data > buf->mem &&
		    (uint8_t *)buf->data <
		    (uint8_t *)buf->mem + buf->memsize &&
		    (uint8_t *)buf->data + buf->size <=
		    (uint8_t *)buf->mem + buf->memsize);
		memmove(buf->mem, buf->data, buf->size);
	}

	/* Second, give our caller the buffer's memory. */
	retp = buf->mem;
	if (sizep != NULL)
		*sizep = buf->size;

	/* Third, discard the buffer's memory. */
	__wt_buf_clear(buf);

	return (retp);
}

/*
 * __wt_buf_swap --
 *	Swap a pair of buffers.
 */
void
__wt_buf_swap(WT_BUF *a, WT_BUF *b)
{
	WT_BUF tmp;

	tmp = *a;
	*a = *b;
	*b = tmp;
}

/*
 * __wt_buf_free --
 *	Free a buffer.
 */
void
__wt_buf_free(WT_SESSION_IMPL *session, WT_BUF *buf)
{
	__wt_free(session, buf->mem);
	__wt_buf_clear(buf);
}

/*
 * __wt_buf_fmt --
 *	Grow a buffer to accommodate a formatted string.
 */
int
__wt_buf_fmt(WT_SESSION_IMPL *session, WT_BUF *buf, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	va_list ap;
	size_t len;

	for (;;) {
		va_start(ap, fmt);
		len =
		    (size_t)vsnprintf(buf->mem, (size_t)buf->memsize, fmt, ap);
		va_end(ap);

		/* Check if there was enough space. */
		if (len < buf->memsize) {
			buf->data = buf->mem;
			buf->size = WT_STORE_SIZE(len);
			return (0);
		}

		/*
		 * If not, double the size of the buffer: we're dealing with
		 * strings, and we don't expect these numbers to get huge.
		 */
		WT_RET(__wt_buf_grow(
		    session, buf, WT_MAX(len + 1, buf->memsize * 2)));
	}
}

/*
 * __wt_buf_catfmt --
 *	Grow a buffer to append a formatted string.
 */
int
__wt_buf_catfmt(WT_SESSION_IMPL *session, WT_BUF *buf, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	va_list ap;
	size_t len, space;
	char *p;

	for (;;) {
		va_start(ap, fmt);
		p = (char *)((uint8_t *)buf->mem + buf->size);
		WT_ASSERT(session, buf->memsize >= buf->size);
		space = buf->memsize - buf->size;
		len = (size_t)vsnprintf(p, (size_t)space, fmt, ap);
		va_end(ap);

		/* Check if there was enough space. */
		if (len < space) {
			buf->size += WT_STORE_SIZE(len);
			return (0);
		}

		/*
		 * If not, double the size of the buffer: we're dealing with
		 * strings, and we don't expect these numbers to get huge.
		 */
		WT_RET(__wt_buf_grow(session, buf,
		    WT_MAX(buf->size + len + 1, buf->memsize * 2)));
	}
}

/*
 * __wt_scr_alloc --
 *	Scratch buffer allocation function.
 */
int
__wt_scr_alloc(WT_SESSION_IMPL *session, uint32_t size, WT_BUF **scratchp)
{
	WT_BUF *buf, **p, *small, **slot;
	size_t allocated;
	u_int i;
	int ret;

	/* Don't risk the caller not catching the error. */
	*scratchp = NULL;

	/*
	 * There's an array of scratch buffers in each WT_SESSION_IMPL that can
	 * be used by any function.  We use WT_BUF structures for scratch
	 * memory because we already have to have functions that do
	 * variable-length allocation on WT_BUFs.  Scratch buffers are
	 * allocated only by a single thread of control, so no locking is
	 * necessary.
	 *
	 * Walk the array, looking for a buffer we can use.
	 */
	for (i = 0, small = NULL, slot = NULL,
	    p = session->scratch; i < session->scratch_alloc; ++i, ++p) {
		/* If we find an empty slot, remember it. */
		if ((buf = *p) == NULL) {
			if (slot == NULL)
				slot = p;
			continue;
		}

		if (F_ISSET(buf, WT_BUF_INUSE))
			continue;

		/*
		 * If we find a buffer that's not in-use, check its size: if it
		 * is large enough, and we'd only waste 4KB by taking it, take
		 * it.  If we don't want this one, remember it -- if we have two
		 * buffers we can "remember", then remember the smallest one.
		 */
		if (buf->memsize >= size &&
		    (buf->memsize - size) < 4 * 1024) {
			WT_ERR(__wt_buf_init(session, buf, size));
			F_SET(buf, WT_BUF_INUSE);
			*scratchp = buf;
			return (0);
		}
		if (small == NULL)
			small = buf;
		else if (small->memsize > buf->memsize)
			small = buf;
	}

	/*
	 * If small is non-NULL, we found a buffer but it wasn't large enough.
	 * Try and grow it.
	 */
	if (small != NULL) {
		WT_ERR(__wt_buf_init(session, small, size));
		F_SET(small, WT_BUF_INUSE);

		*scratchp = small;
		return (0);
	}

	/*
	 * If slot is non-NULL, we found an empty slot, try and allocate a
	 * buffer and call recursively to find and grow the buffer.
	 */
	if (slot != NULL) {
		WT_ERR(__wt_calloc_def(session, 1, slot));
		return (__wt_scr_alloc(session, size, scratchp));
	}

	/*
	 * Resize the array, we need more scratch buffers, then call recursively
	 * to find the empty slot, and so on and so forth.
	 */
	allocated = session->scratch_alloc * sizeof(WT_BUF *);
	WT_ERR(__wt_realloc(session, &allocated,
	    (session->scratch_alloc + 10) * sizeof(WT_BUF *),
	    &session->scratch));
	session->scratch_alloc += 10;
	return (__wt_scr_alloc(session, size, scratchp));

err:	__wt_errx(session,
	    "WT_SESSION_IMPL unable to allocate more scratch buffers");
	return (ret);
}

/*
 * __wt_scr_free --
 *	Release a scratch buffer.
 */
void
__wt_scr_free(WT_BUF **bufp)
{
	if (*bufp == NULL)
		return;
	F_CLR(*bufp, WT_BUF_INUSE);
	*bufp = NULL;
}

/*
 * __wt_scr_discard --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_scr_discard(WT_SESSION_IMPL *session)
{
	WT_BUF **bufp;
	u_int i;

	for (i = 0,
	    bufp = session->scratch; i < session->scratch_alloc; ++i, ++bufp) {
		if (*bufp != NULL)
			__wt_buf_free(session, *bufp);
		__wt_free(session, *bufp);
	}

	__wt_free(session, session->scratch);
}

/*
 * __wt_scr_alloc_ext --
 *	Allocate a scratch buffer, and return the memory reference.
 */
void *
__wt_scr_alloc_ext(WT_SESSION *wt_session, size_t size)
{
	WT_BUF *buf;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	return (__wt_scr_alloc(
	    session, (uint32_t)size, &buf) == 0 ? buf->mem : NULL);
}

/*
 * __wt_scr_free_ext --
 *	Free a scratch buffer based on the memory reference.
 */
void
__wt_scr_free_ext(WT_SESSION *wt_session, void *p)
{
	WT_BUF **bufp;
	WT_SESSION_IMPL *session;
	u_int i;

	session = (WT_SESSION_IMPL *)wt_session;

	for (i = 0,
	    bufp = session->scratch; i < session->scratch_alloc; ++i, ++bufp)
		if (*bufp != NULL && (*bufp)->mem == p) {
			/*
			 * Do NOT call __wt_scr_free() here, it clears the
			 * caller's pointer, which would truncate the list.
			 */
			F_CLR(*bufp, WT_BUF_INUSE);
			return;
		}
}
