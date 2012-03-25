/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components, UPDATING the
 * caller's buffer reference so it can be called repeatedly to load a buffer.
 */
static int
__block_buffer_to_addr(WT_BLOCK *block,
    const uint8_t **pp, off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	uint64_t a;

	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	if (offsetp != NULL)
		*offsetp = (off_t)a * block->allocsize + WT_BLOCK_DESC_SECTOR;
	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	if (sizep != NULL)
		*sizep = (uint32_t)a * block->allocsize;
	if (cksump != NULL) {
		WT_RET(__wt_vunpack_uint(pp, 0, &a));
		*cksump = (uint32_t)a;
	}
	return (0);
}

/*
 * __wt_block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components.
 */
int
__wt_block_buffer_to_addr(WT_BLOCK *block,
    const uint8_t *p, off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	return (__block_buffer_to_addr(block, &p, offsetp, sizep, cksump));
}

/*
 * __wt_block_addr_to_buffer --
 *	Convert the filesystem components into its address cookie.
 */
int
__wt_block_addr_to_buffer(WT_BLOCK *block,
    uint8_t **pp, off_t offset, uint32_t size, uint32_t cksum)
{
	uint64_t a;

	a = (uint64_t)(offset - WT_BLOCK_DESC_SECTOR) / block->allocsize;
	WT_RET(__wt_vpack_uint(pp, 0, a));
	a = size / block->allocsize;
	WT_RET(__wt_vpack_uint(pp, 0, a));
	a = cksum;
	WT_RET(__wt_vpack_uint(pp, 0, a));
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
    WT_BLOCK *block, WT_ITEM *buf, const uint8_t *addr, uint32_t addr_size)
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

/*
 * __wt_block_buffer_to_snapshot --
 *	Convert a filesystem snapshot cookie into its components.
 */
int
__wt_block_buffer_to_snapshot(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *p, WT_BLOCK_SNAPSHOT *si)
{
	uint64_t a;
	const uint8_t **pp;

	si->version = *p++;
	if (si->version != WT_BM_SNAPSHOT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "illegal snapshot address");

	pp = &p;
	WT_RET(__block_buffer_to_addr(block, pp,
	    &si->root_offset, &si->root_size, &si->root_cksum));

	si->alloc.name = "snapshot.alloc";
	WT_RET(__block_buffer_to_addr(block, pp,
	    &si->alloc_offset, &si->alloc_size, &si->alloc_cksum));

	si->avail.name = "snapshot.avail";
	WT_RET(__block_buffer_to_addr(block, pp,
	    &si->avail_offset, &si->avail_size, &si->avail_cksum));

	si->discard.name = "snapshot.discard";
	WT_RET(__block_buffer_to_addr(block, pp,
	    &si->discard_offset, &si->discard_size, &si->discard_cksum));

	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	si->file_size = (off_t)a;

	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	si->write_gen = a;

	return (0);
}

/*
 * __wt_block_snapshot_to_buffer --
 *	Convert the filesystem components into its snapshot cookie.
 */
int
__wt_block_snapshot_to_buffer(WT_SESSION_IMPL *session,
    WT_BLOCK *block, uint8_t **pp, WT_BLOCK_SNAPSHOT *si)
{
	uint64_t a;

	if (si->version != WT_BM_SNAPSHOT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "illegal snapshot address");

	(*pp)[0] = si->version;
	(*pp)++;

	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    si->root_offset, si->root_size, si->root_cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    si->alloc_offset, si->alloc_size, si->alloc_cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    si->avail_offset, si->avail_size, si->avail_cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    si->discard_offset, si->discard_size, si->discard_cksum));
	a = (uint64_t)si->file_size;
	WT_RET(__wt_vpack_uint(pp, 0, a));
	a = si->write_gen;
	WT_RET(__wt_vpack_uint(pp, 0, a));

	return (0);
}
