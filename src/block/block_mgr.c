/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bm_addr_valid --
 *	Return if a filesystem address cookie is valid for the file.
 */
int
__wt_bm_addr_valid(
    WT_SESSION_IMPL *session, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	WT_BTREE *btree;
	uint32_t addr, size;

	btree = session->btree;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, NULL));

	/* All we care about is if it's past the end of the file. */
	return ((WT_ADDR_TO_OFF(btree, addr) +
	    (off_t)size > btree->fh->file_size) ? 0 : 1);
}

/*
 * __wt_bm_addr_string
 *	Return a printable string representation of a filesystem address cookie.
 */
int
__wt_bm_addr_string(WT_SESSION_IMPL *session,
    WT_BUF *buf, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	uint32_t addr, cksum, size;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, &cksum));

	/* Printable representation. */
	WT_RET(__wt_buf_fmt(session, buf,
	    "[%" PRIu32 "-%" PRIu32 ", %" PRIu32 ", %" PRIu32 "]",
	    addr, addr + (size / 512 - 1), size, cksum));

	return (0);
}

/*
 * __wt_bm_create --
 *	Create a new file.
 */
int
__wt_bm_create(WT_SESSION_IMPL *session, const char *filename)
{
	return (__wt_block_create(session, filename));
}

/*
 * __wt_bm_open --
 *	Open a file.
 */
int
__wt_bm_open(WT_SESSION_IMPL *session)
{
	return (__wt_block_open(session));
}

/*
 * __wt_bm_close --
 *	Close a file.
 */
int
__wt_bm_close(WT_SESSION_IMPL *session)
{
	return (__wt_block_close(session));
}

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
    WT_BUF *buf, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	uint32_t addr, size, cksum;
	int ret;

	btree = session->btree;
	ret = 0;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, &cksum));

	/* Re-size the buffer as necessary. */
	WT_RET(__wt_buf_initsize(session, buf, size));

	/* Read the block. */
	WT_RET(__wt_block_read(session, buf, addr, size, cksum));

	/* Optionally verify the page. */
	if (btree->fragbits == NULL)
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
#ifdef HAVE_DIAGNOSTIC
	{
	int ret;
	if ((ret = __wt_verify_dsk(session,
	    "[write-check]", (WT_PAGE_DISK *)buf->data, buf->size)) != 0)
		return (ret);
	}
#endif

	WT_RET(__wt_block_write(session, buf, &addr, &size, &cksum));

	endp = addrbuf;
	WT_RET(__wt_block_addr_to_buffer(&endp, addr, size, cksum));
	*addrbuf_size = WT_PTRDIFF32(endp, addrbuf);

	return (0);
}

/*
 * __wt_bm_salvage_start --
 *	Start a block manager salvage.
 */
int
__wt_bm_salvage_start(WT_SESSION_IMPL *session)
{
	return (__wt_block_salvage_start(session));
}

/*
 * __wt_bm_salvage_next --
 *	Return the next block from the file.
 */
int
__wt_bm_salvage_next(WT_SESSION_IMPL *session,
    WT_BUF *buf, uint8_t *addrbuf, uint32_t *addrbuf_lenp, int *eofp)
{
	return (
	    __wt_block_salvage_next(session, buf, addrbuf, addrbuf_lenp, eofp));
}

/*
 * __wt_bm_salvage_end --
 *	End a block manager salvage.
 */
int
__wt_bm_salvage_end(WT_SESSION_IMPL *session, int success)
{
	return (__wt_block_salvage_end(session, success));
}

/*
 * __wt_bm_verify_start --
 *	Start a block manager salvage.
 */
int
__wt_bm_verify_start(WT_SESSION_IMPL *session)
{
	return (__wt_block_verify_start(session));
}

/*
 * __wt_bm_verify_end --
 *	End a block manager salvage.
 */
int
__wt_bm_verify_end(WT_SESSION_IMPL *session)
{
	return (__wt_block_verify_end(session));
}

/*
 * __wt_bm_verify_addr --
 *	Verify an address.
 */
int
__wt_bm_verify_addr(WT_SESSION_IMPL *session,
     const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	return (__wt_block_verify_addr(session, addrbuf, addrbuf_size));
}
