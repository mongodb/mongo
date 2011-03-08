/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

void
__wt_buf_init(WT_BUF *buf)
{
	buf->item.data = buf->mem = NULL;
	buf->mem_size = buf->item.size = 0;
}

int
__wt_buf_grow(SESSION *session, WT_BUF *buf, size_t sz)
{
	if (sz > buf->mem_size)
		WT_RET(__wt_realloc(session, &buf->mem_size, sz, &buf->mem));

	buf->item.data = buf->mem;
	WT_ASSERT(session, sz <= UINT32_MAX);
	buf->item.size = (uint32_t)sz;
	return (0);
}

void
__wt_buf_free(SESSION *session, WT_BUF *buf)
{
	if (buf->mem != NULL)
		__wt_free(session, buf->mem, buf->mem_size);

	buf->item.data = NULL;
	buf->mem_size = buf->item.size = 0;
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
			slot = p;
			continue;
		}

		/*
		 * If we find a buffer that's not in-use, check its size.  If it
		 * is large enough, we're done; otherwise, remember it.
		 */
		if (F_ISSET(buf, WT_BUF_INUSE))
			continue;

		if (size == 0 || buf->mem_size >= size) {
			F_SET(buf, WT_BUF_INUSE);
			buf->item.data = buf->mem;
			*scratchp = buf;
			return (0);
		}
		small = buf;
	}

	/*
	 * If small is non-NULL, we found a buffer but it wasn't large enough.
	 * Try and grow it.
	 */
	if (small != NULL) {
		WT_ERR(__wt_buf_grow(session, small, size));

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

	for (i = 0, p = session->scratch; i < session->scratch_alloc; ++i, ++p)
		if (*p != NULL)
			__wt_buf_free(session, *p);

	__wt_free(session,
	    session->scratch, session->scratch_alloc * sizeof(WT_BUF *));
}
