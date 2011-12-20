/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components.
 */
int
__wt_block_buffer_to_addr(
    const uint8_t *p, uint32_t *addr, uint32_t *size, uint32_t *cksum)
{
	uint64_t a;

	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (addr != NULL)
		*addr = (uint32_t)a;

	WT_RET(__wt_vunpack_uint(&p, 0, &a));
	if (size != NULL)
		*size = (uint32_t)a;

	if (cksum != NULL) {
		WT_RET(__wt_vunpack_uint(&p, 0, &a));
		*cksum = (uint32_t)a;
	}

	return (0);
}

/*
 * __wt_block_addr_to_buffer --
 *	Convert the filesystem components into its address cookie.
 */
int
__wt_block_addr_to_buffer(
    uint8_t **p, uint32_t addr, uint32_t size, uint32_t cksum)
{
	uint64_t a;

	a = addr;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = size;
	WT_RET(__wt_vpack_uint(p, 0, a));
	a = cksum;
	WT_RET(__wt_vpack_uint(p, 0, a));
	return (0);
}

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
	uint32_t addr, size;

	/* Crack the cookie. */
	WT_UNUSED(addrbuf_size);
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, NULL));

	/* Printable representation. */
	WT_RET(__wt_buf_fmt(session, buf,
	    "[%" PRIu32 "-%" PRIu32 ", %" PRIu32 "]",
	    addr, addr + (size / 512 - 1), size));

	return (0);
}
