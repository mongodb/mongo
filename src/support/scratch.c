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
	WT_ASSERT(session, sz < UINT32_MAX);
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
	WT_BUF *available, *buf;
	uint32_t allocated;
	u_int i;
	int ret;

	/* Don't risk the caller not catching the error. */
	*scratchp = NULL;

	/*
	 * There's an array of scratch buffers in each SESSION that can be used
	 * by any function.  We use DBTs for scratch buffers because we already
	 * have to have functions that do variable-length allocation on DBTs.
	 * Scratch buffers are allocated only by a single thread of control, so
	 * no locking is necessary.
	 *
	 * Walk the list, looking for a buffer we can use.
	 */
	for (i = 0, available = NULL,
	    buf = session->scratch; i < session->scratch_alloc; ++i, ++buf)
		if (!F_ISSET(buf, WT_BUF_INUSE)) {
			if (size == 0 || buf->mem_size >= size) {
				F_SET(buf, WT_BUF_INUSE);
				buf->item.data = buf->mem;
				*scratchp = buf;
				return (0);
			}
			available = buf;
		}

	/* If available set, we found a slot but it wasn't large enough. */
	if (available != NULL) {
		F_SET(available, WT_BUF_INUSE);
		WT_RET(__wt_buf_grow(session, available, size));
		*scratchp = available;
		return (0);
	}

	/* Resize the array, we need more scratch buffers. */
	allocated = session->scratch_alloc * WT_SIZEOF32(WT_BUF);
	WT_ERR(__wt_realloc(session, &allocated,
	    (session->scratch_alloc + 10) * sizeof(WT_BUF), &session->scratch));
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
	WT_BUF *buf;
	u_int i;

	for (i = 0, buf = session->scratch;
	    i < session->scratch_alloc;
	    ++i, ++buf)
		__wt_buf_free(session, buf);

	__wt_free(
	    session, session->scratch, session->scratch_alloc * sizeof(WT_BUF));
}
