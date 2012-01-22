/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components.
 */
int
__wt_block_buffer_to_addr(WT_BLOCK *block,
    const uint8_t *p, off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	uint64_t a;

	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (offsetp != NULL)
		*offsetp = (off_t)a * block->allocsize + WT_BLOCK_DESC_SECTOR;
	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (sizep != NULL)
		*sizep = (uint32_t)a * block->allocsize;

	if (cksump != NULL) {
		WT_RET(__wt_vunpack_uint(&p, 0, &a));
		*cksump = (uint32_t)a;
	}

	return (0);
}

/*
 * __wt_block_addr_to_buffer --
 *	Convert the filesystem components into its address cookie.
 */
int
__wt_block_addr_to_buffer(WT_BLOCK *block,
    uint8_t **p, off_t offset, uint32_t size, uint32_t cksum)
{
	uint64_t a;

	a = (uint64_t)(offset - WT_BLOCK_DESC_SECTOR) / block->allocsize;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = size / block->allocsize;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = cksum;
	WT_RET(__wt_vpack_uint(p, 0, a));
	return (0);
}

/*
 * __wt_block_addr_valid --
 *	Return if an address cookie is valid.
 */
int
__wt_block_addr_valid(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, uint32_t addr_size)
{
	off_t offset;
	uint32_t size;

	WT_UNUSED(session);
	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, NULL));

	/* All we care about is if it's past the end of the file. */
	return (offset + size > block->fh->file_size ? 0 : 1);
}

/*
 * __wt_block_addr_string --
 *	Return a printable string representation of an address cookie.
 */
int
__wt_block_addr_string(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_BUF *buf, const uint8_t *addr, uint32_t addr_size)
{
	off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/* Printable representation. */
	WT_RET(__wt_buf_fmt(session, buf,
	    "[%" PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
	    (uintmax_t)offset, (uintmax_t)offset + size, size, cksum));

	return (0);
}
