/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __wt_buf_clear(WT_ITEM *);

/*
 * __wt_buf_clear --
 *	Clear a buffer.
 */
static void
__wt_buf_clear(WT_ITEM *buf)
{
	buf->data = NULL;
	buf->size = 0;

	buf->mem = NULL;
	buf->memsize = 0;

	/*
	 * Note: don't clear the flags, the buffer remains marked for aligned
	 * use as well as "in-use".
	 */
	F_CLR(buf, WT_ITEM_MAPPED);
}

/*
 * __wt_buf_grow --
 *	Grow a buffer that's currently in-use.
 */
int
__wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	size_t offset;
	int set_data;

	WT_ASSERT(session, size <= UINT32_MAX);

	/* Clear buffers previously used for mapped returns. */
	if (F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_buf_clear(buf);

	if (size > buf->memsize) {
		/*
		 * Grow the buffer's memory: if the data reference is not set
		 * or references the buffer's memory, maintain it.
		 */
		WT_ASSERT(session, buf->mem == NULL || buf->memsize > 0);
		if (buf->data == NULL) {
			offset = 0;
			set_data = 1;
		} else if (buf->data >= buf->mem &&
		    WT_PTRDIFF(buf->data, buf->mem) < buf->memsize) {
			offset = WT_PTRDIFF(buf->data, buf->mem);
			set_data = 1;
		} else {
			offset = 0;
			set_data = 0;
		}

		if (F_ISSET(buf, WT_ITEM_ALIGNED))
			WT_RET(__wt_realloc_aligned(
			    session, &buf->memsize, size, &buf->mem));
		else
			WT_RET(__wt_realloc(
			    session, &buf->memsize, size, &buf->mem));

		if (set_data)
			buf->data = (uint8_t *)buf->mem + offset;
	}
	return (0);
}

/*
 * __wt_buf_extend --
 *	Extend a buffer that's currently in-use.  The difference from
 *	__wt_buf_grow is that extend is expected to be called repeatedly for
 *	the same buffer, and so grows the buffer exponentially to avoid
 *	repeated costly calls to realloc.
 */
int
__wt_buf_extend(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	if (size <= buf->memsize)
		return (0);

	return (__wt_buf_grow(session, buf, WT_MAX(size, 2 * buf->memsize)));
}

/*
 * __wt_buf_init --
 *	Initialize a buffer at a specific size.
 */
int
__wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	buf->data = buf->mem;
	WT_RET(__wt_buf_grow(session, buf, size));
	buf->size = 0;

	return (0);
}

/*
 * __wt_buf_initsize --
 *	Initialize a buffer at a specific size, and set the data length.
 */
int
__wt_buf_initsize(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
	WT_RET(__wt_buf_init(session, buf, size));
	buf->size = WT_STORE_SIZE(size);	/* Set the data length. */

	return (0);
}

/*
 * __wt_buf_set --
 *	Set the contents of the buffer.
 */
int
__wt_buf_set(
    WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data, size_t size)
{
	/* Ensure the buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	memcpy(buf->mem, data, size);

	return (0);
}

/*
 * __wt_buf_set_printable --
 *	Set the contents of the buffer to a printable representation of a
 * byte string.
 */
int
__wt_buf_set_printable(
    WT_SESSION_IMPL *session, WT_ITEM *buf, const void *from_arg, size_t size)
{
	return (__wt_raw_to_esc_hex(session, from_arg, size, buf));
}

/*
 * __wt_buf_steal --
 *	Steal a buffer for another purpose.
 */
void *
__wt_buf_steal(WT_SESSION_IMPL *session, WT_ITEM *buf, uint32_t *sizep)
{
	void *retp;

	WT_ASSERT(session, !F_ISSET(buf, WT_ITEM_MAPPED));

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
		WT_ASSERT(session, buf->data > buf->mem &&
		    WT_PTRDIFF(buf->data, buf->mem) + buf->size <=
		    buf->memsize);
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
 * __wt_buf_free --
 *	Free a buffer.
 */
void
__wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf)
{
	if (!F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_free(session, buf->mem);
	__wt_buf_clear(buf);
}

/*
 * __wt_buf_fmt --
 *	Grow a buffer to accommodate a formatted string.
 */
int
__wt_buf_fmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	va_list ap;
	size_t len;

	/* Clear buffers previously used for mapped returns. */
	if (F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_buf_clear(buf);

	for (;;) {
		va_start(ap, fmt);
		len = (size_t)vsnprintf(buf->mem, buf->memsize, fmt, ap);
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
__wt_buf_catfmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	va_list ap;
	size_t len, space;
	char *p;

	/* Clear buffers previously used for mapped returns. */
	if (F_ISSET(buf, WT_ITEM_MAPPED))
		__wt_buf_clear(buf);

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
__wt_scr_alloc_func(WT_SESSION_IMPL *session,
    size_t size, WT_ITEM **scratchp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_DECL_RET;
	WT_ITEM *buf, **p, **best, **slot;
	size_t allocated;
	u_int i;

	/* Don't risk the caller not catching the error. */
	*scratchp = NULL;

	/*
	 * Each WT_SESSION_IMPL has an array of scratch buffers available for
	 * use by any function.  We use WT_ITEM structures for scratch memory
	 * because we already have functions that do variable-length allocation
	 * on a WT_ITEM.  Scratch buffers are allocated only by a single thread
	 * of control, so no locking is necessary.
	 *
	 * Walk the array, looking for a buffer we can use.
	 */
	for (i = 0, best = slot = NULL,
	    p = session->scratch; i < session->scratch_alloc; ++i, ++p) {
		/* If we find an empty slot, remember it. */
		if ((buf = *p) == NULL) {
			if (slot == NULL)
				slot = p;
			continue;
		}

		if (F_ISSET(buf, WT_ITEM_INUSE))
			continue;

		/*
		 * If we find a buffer that's not in-use, check its size: we
		 * want the smallest buffer larger than the requested size,
		 * or the largest buffer if none are large enough.
		 */
		if (best == NULL ||
		    ((*best)->memsize < size &&
		    buf->memsize > (*best)->memsize) ||
		    (buf->memsize >= size && buf->memsize < (*best)->memsize))
			best = p;

		/* If we find a perfect match, use it. */
		if ((*best)->memsize == size)
			break;
	}

	/*
	 * If we didn't find a free buffer, extend the array and use the first
	 * slot we allocated.
	 */
	if (best == NULL && slot == NULL) {
		allocated = session->scratch_alloc * sizeof(WT_ITEM *);
		WT_ERR(__wt_realloc(session, &allocated,
		    (session->scratch_alloc + 10) * sizeof(WT_ITEM *),
		    &session->scratch));
#ifdef HAVE_DIAGNOSTIC
		allocated = session->scratch_alloc * sizeof(WT_SCRATCH_TRACK);
		WT_ERR(__wt_realloc(session, &allocated,
		    (session->scratch_alloc + 10) * sizeof(WT_SCRATCH_TRACK),
		    &session->scratch_track));
#endif
		slot = session->scratch + session->scratch_alloc;
		session->scratch_alloc += 10;
	}

	/*
	 * If slot is non-NULL, we found an empty slot, try and allocate a
	 * buffer.
	 */
	if (best == NULL) {
		WT_ASSERT(session, slot != NULL);
		best = slot;

		WT_ERR(__wt_calloc_def(session, 1, best));

		/* Scratch buffers must be aligned. */
		F_SET(*best, WT_ITEM_ALIGNED);
	}

	/* Grow the buffer as necessary and return. */
	WT_ERR(__wt_buf_init(session, *best, size));
	F_SET(*best, WT_ITEM_INUSE);

#ifdef HAVE_DIAGNOSTIC
	session->scratch_track[best - session->scratch].file = file;
	session->scratch_track[best - session->scratch].line = line;
#endif

	*scratchp = *best;
	return (0);

err:	WT_RET_MSG(session, ret,
	    "session unable to allocate a scratch buffer");
}

/*
 * __wt_scr_free --
 *	Release a scratch buffer.
 */
void
__wt_scr_free(WT_ITEM **bufp)
{
	if (*bufp == NULL)
		return;
	F_CLR(*bufp, WT_ITEM_INUSE);
	*bufp = NULL;
}

/*
 * __wt_scr_discard --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_scr_discard(WT_SESSION_IMPL *session)
{
	WT_ITEM **bufp;
	u_int i;

	for (i = 0,
	    bufp = session->scratch; i < session->scratch_alloc; ++i, ++bufp) {
		if (*bufp == NULL)
			continue;
		if (F_ISSET(*bufp, WT_ITEM_INUSE))
			__wt_errx(session,
			    "scratch buffer allocated and never discarded"
#ifdef HAVE_DIAGNOSTIC
			    ": %s: %d",
			    session->
			    scratch_track[bufp - session->scratch].file,
			    session->
			    scratch_track[bufp - session->scratch].line
#endif
			    );

		__wt_buf_free(session, *bufp);
		__wt_free(session, *bufp);
	}

	__wt_free(session, session->scratch);
#ifdef HAVE_DIAGNOSTIC
	__wt_free(session, session->scratch_track);
#endif
}

/*
 * __wt_ext_scr_alloc --
 *	Allocate a scratch buffer, and return the memory reference.
 */
void *
__wt_ext_scr_alloc(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, size_t size)
{
	WT_ITEM *buf;
	WT_SESSION_IMPL *session;

	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	return (__wt_scr_alloc(
	    session, (uint32_t)size, &buf) == 0 ? buf->mem : NULL);
}

/*
 * __wt_ext_scr_free --
 *	Free a scratch buffer based on the memory reference.
 */
void
__wt_ext_scr_free(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, void *p)
{
	WT_ITEM **bufp;
	WT_SESSION_IMPL *session;
	u_int i;

	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	for (i = 0,
	    bufp = session->scratch; i < session->scratch_alloc; ++i, ++bufp)
		if (*bufp != NULL && (*bufp)->mem == p) {
			/*
			 * Do NOT call __wt_scr_free() here, it clears the
			 * caller's pointer, which would truncate the list.
			 */
			F_CLR(*bufp, WT_ITEM_INUSE);
			return;
		}
	__wt_errx(session, "extension free'd non-existent scratch buffer");
}
