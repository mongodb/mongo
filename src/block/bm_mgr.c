/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bm_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_bm_free(
    WT_SESSION_IMPL *session, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	uint32_t addr, size;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, NULL));

	return (__wt_block_free(session, addr, size));
}

/*
 * __wt_bm_read --
 *	Read a address cookie-referenced block into a buffer.
 */
int
__wt_bm_read(WT_SESSION_IMPL *session,
    WT_BUF *buf, const uint8_t *addrbuf, uint32_t addrbuf_size, uint32_t flags)
{
	WT_BUF *tmp;
	uint32_t addr, size, cksum;
	int ret;

	ret = 0;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, &cksum));

	/* Re-size the buffer as necessary. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	/* Read the block. */
	WT_RET(__wt_block_read(session, buf, addr, size, cksum));

	/* Optionally verify the page: used by verify. */
	if (!LF_ISSET(WT_VERIFY))
		return (0);

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_bm_addr_string(session, tmp, addrbuf, addrbuf_size));
	WT_ERR(__wt_verify_dsk(
	    session, (char *)tmp->data, buf->mem, buf->size));

err:	__wt_scr_free(&tmp);

	return (ret);
}

/*
 * __wt_bm_write --
 *	Write a buffer into a block, returning the block's address cookie.
 */
int
__wt_bm_write(WT_SESSION_IMPL *session,
    WT_BUF *buf, uint8_t *addrbuf, uint32_t *addrbuf_size)
{
	uint32_t addr, size, cksum;

	uint8_t *endp;

	/*
	 * We're passed a table's page image: WT_BUF->{data,size} are the image
	 * and byte count.
	 *
	 * Diagnostics: verify the disk page: this violates layering, but it's
	 * the place we can ensure we never write a corrupted page.
	 */
	WT_ASSERT(session, __wt_verify_dsk(
	    session, "[NoAddr]", (WT_PAGE_DISK *)buf->data, buf->size) == 0);

	WT_RET(__wt_block_write(session, buf, &addr, &size, &cksum));

	endp = addrbuf;
	WT_RET(__wt_block_addr_to_buffer(&endp, addr, size, cksum));
	*addrbuf_size = WT_PTRDIFF32(endp, addrbuf);

	return (0);
}
