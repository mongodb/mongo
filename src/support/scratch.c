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
	buf->mem_size = 0;

	/* Note: don't clear the flags, the buffer remains marked in-use. */
}

/*
 * __wt_buf_init --
 *	Initialize a buffer at a specific size.
 */
int
__wt_buf_init(SESSION *session, WT_BUF *buf, size_t size)
{
	WT_ASSERT(session, size <= UINT32_MAX);

	if (size > buf->mem_size)
		WT_RET(__wt_realloc(session, &buf->mem_size, size, &buf->mem));

	buf->data = buf->mem;
	buf->size = 0;

	return (0);
}

/*
 * __wt_buf_initsize --
 *	Initialize a buffer at a specific size, and set the data length.
 */
int
__wt_buf_initsize(SESSION *session, WT_BUF *buf, size_t size)
{
	WT_RET(__wt_buf_init(session, buf, size));

	buf->size = (uint32_t)size;		/* Set the data length. */

	return (0);
}

/*
 * __wt_buf_grow --
 *	Grow a buffer that's currently in-use.
 */
int
__wt_buf_grow(SESSION *session, WT_BUF *buf, size_t size)
{
	uint32_t offset;

	WT_ASSERT(session, size <= UINT32_MAX);

	if (size <= buf->mem_size)
		return (0);

	/*
	 * If we reallocate the buffer's memory, maintain the previous values
	 * for the data/size pair.
	 */
	offset = buf->data == NULL ? 0 : WT_PTRDIFF32(buf->data, buf->mem);

	WT_RET(__wt_realloc(session, &buf->mem_size, size, &buf->mem));

	buf->data = (uint8_t *)buf->mem + offset;

	return (0);
}

/*
 * __wt_buf_set --
 *	Set the contents of the buffer.
 */
int
__wt_buf_set(SESSION *session, WT_BUF *buf, const void *data, uint32_t size)
{
	/* Ensure the buffer is large enough. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	memcpy(buf->mem, data, size);

	return (0);
}

/*
 * __wt_buf_steal --
 *	Steal a buffer for another purpose.
 */
void
__wt_buf_steal(
    SESSION *session, WT_BUF *buf, const void *datap, uint32_t *sizep)
{
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
		    (uint8_t *)buf->mem + buf->mem_size &&
		    (uint8_t *)buf->data + buf->size <=
		    (uint8_t *)buf->mem + buf->mem_size);
		memmove(buf->mem, buf->data, buf->size);
	}

	/* Second, give our caller the buffer's memory. */
	*(void **)datap = buf->mem;
	*sizep = buf->size;

	/* Third, discard the buffer's memory. */
	__wt_buf_clear(buf);
}

/*
 * __wt_buf_free --
 *	Free a buffer.
 */
void
__wt_buf_free(SESSION *session, WT_BUF *buf)
{
	if (buf->mem != NULL)
		__wt_free(session, buf->mem);
	__wt_buf_clear(buf);
}

/*
 * __wt_scr_alloc --
 *	Scratch buffer allocation function.
 */
int
__wt_scr_alloc(SESSION *session, uint32_t size, WT_BUF **scratchp)
{
	WT_BUF *buf, **p, *small, **slot;
	uint32_t allocated;
	u_int i;
	int ret;

	/* Don't risk the caller not catching the error. */
	*scratchp = NULL;

	/*
	 * There's an array of scratch buffers in each SESSION that can be used
	 * by any function.  We use WT_BUF structures for scratch memory because
	 * we already have to have functions that do variable-length allocation
	 * on WT_BUFs.  Scratch buffers are allocated only by a single thread of
	 * control, so no locking is necessary.
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
		 * If we find a buffer that's not in-use, check its size.  If it
		 * is large enough, we're done; otherwise, remember it.
		 */
		if (buf->mem_size >= size) {
			WT_ERR(__wt_buf_init(session, buf, size));
			F_SET(buf, WT_BUF_INUSE);

			*scratchp = buf;
			return (0);
		}
		if (small == NULL)
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
	allocated = session->scratch_alloc * WT_SIZEOF32(WT_BUF *);
	WT_ERR(__wt_realloc(session, &allocated,
	    (session->scratch_alloc + 10) * sizeof(WT_BUF *),
	    &session->scratch));
	session->scratch_alloc += 10;
	return (__wt_scr_alloc(session, size, scratchp));

err:	__wt_errx(session,
	    "SESSION unable to allocate more scratch buffers");
	return (ret);
}

/*
 * __wt_scr_release --
 *	Release a scratch buffer.
 */
void
__wt_scr_release(WT_BUF **bufp)
{
	WT_BUF *buf;

	buf = *bufp;
	*bufp = NULL;

	F_CLR(buf, WT_BUF_INUSE);
}

/*
 * __wt_scr_free --
 *	Free all memory associated with the scratch buffers.
 */
void
__wt_scr_free(SESSION *session)
{
	WT_BUF **p;
	u_int i;

	for (i = 0,
	    p = session->scratch; i < session->scratch_alloc; ++i, ++p) {
		if (*p != NULL)
			__wt_buf_free(session, *p);
		__wt_free(session, *p);
	}

	__wt_free(session, session->scratch);
}
