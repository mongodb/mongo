/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
__block_buffer_to_addr(uint32_t allocsize,
    const uint8_t **pp, wt_off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	uint64_t o, s, c;

	WT_RET(__wt_vunpack_uint(pp, 0, &o));
	WT_RET(__wt_vunpack_uint(pp, 0, &s));
	WT_RET(__wt_vunpack_uint(pp, 0, &c));

	/*
	 * To avoid storing large offsets, we minimize the value by subtracting
	 * a block for description information, then storing a count of block
	 * allocation units.  That implies there is no such thing as an
	 * "invalid" offset though, they could all be valid (other than very
	 * large numbers), which is what we didn't want to store in the first
	 * place.  Use the size: writing a block of size 0 makes no sense, so
	 * that's the out-of-band value.  Once we're out of this function and
	 * are working with a real file offset, size and checksum triplet, there
	 * can be invalid offsets, that's simpler than testing sizes of 0 all
	 * over the place.
	 */
	if (s == 0) {
		*offsetp = 0;
		*sizep = *cksump = 0;
	} else {
		*offsetp = (wt_off_t)(o + 1) * allocsize;
		*sizep = (uint32_t)s * allocsize;
		*cksump = (uint32_t)c;
	}
	return (0);
}

/*
 * __wt_block_addr_to_buffer --
 *	Convert the filesystem components into its address cookie.
 */
int
__wt_block_addr_to_buffer(WT_BLOCK *block,
    uint8_t **pp, wt_off_t offset, uint32_t size, uint32_t cksum)
{
	uint64_t o, s, c;

	/* See the comment above: this is the reverse operation. */
	if (size == 0) {
		o = WT_BLOCK_INVALID_OFFSET;
		s = c = 0;
	} else {
		o = (uint64_t)offset / block->allocsize - 1;
		s = size / block->allocsize;
		c = cksum;
	}
	WT_RET(__wt_vpack_uint(pp, 0, o));
	WT_RET(__wt_vpack_uint(pp, 0, s));
	WT_RET(__wt_vpack_uint(pp, 0, c));
	return (0);
}

/*
 * __wt_block_buffer_to_addr --
 *	Convert a filesystem address cookie into its components NOT UPDATING
 * the caller's buffer reference.
 */
int
__wt_block_buffer_to_addr(WT_BLOCK *block,
    const uint8_t *p, wt_off_t *offsetp, uint32_t *sizep, uint32_t *cksump)
{
	return (__block_buffer_to_addr(
	    block->allocsize, &p, offsetp, sizep, cksump));
}

/*
 * __wt_block_addr_invalid --
 *	Return an error code if an address cookie is invalid.
 */
int
__wt_block_addr_invalid(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, size_t addr_size, bool live)
{
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(session);
	WT_UNUSED(addr_size);
	WT_UNUSED(live);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

#ifdef HAVE_DIAGNOSTIC
	/*
	 * In diagnostic mode, verify the address isn't on the available list,
	 * or for live systems, the discard list.
	 */
	WT_RET(__wt_block_misplaced(
	    session, block, "addr-valid", offset, size, live));
#endif

	/* Check if the address is past the end of the file. */
	return (offset + size > block->size ? EINVAL : 0);
}

/*
 * __wt_block_addr_string --
 *	Return a printable string representation of an address cookie.
 */
int
__wt_block_addr_string(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
	wt_off_t offset;
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
 * __block_buffer_to_ckpt --
 *	Convert a checkpoint cookie into its components.
 */
static int
__block_buffer_to_ckpt(WT_SESSION_IMPL *session,
    uint32_t allocsize, const uint8_t *p, WT_BLOCK_CKPT *ci)
{
	uint64_t a;
	const uint8_t **pp;

	ci->version = *p++;
	if (ci->version != WT_BM_CHECKPOINT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "unsupported checkpoint version");

	pp = &p;
	WT_RET(__block_buffer_to_addr(allocsize, pp,
	    &ci->root_offset, &ci->root_size, &ci->root_cksum));
	WT_RET(__block_buffer_to_addr(allocsize, pp,
	    &ci->alloc.offset, &ci->alloc.size, &ci->alloc.cksum));
	WT_RET(__block_buffer_to_addr(allocsize, pp,
	    &ci->avail.offset, &ci->avail.size, &ci->avail.cksum));
	WT_RET(__block_buffer_to_addr(allocsize, pp,
	    &ci->discard.offset, &ci->discard.size, &ci->discard.cksum));
	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	ci->file_size = (wt_off_t)a;
	WT_RET(__wt_vunpack_uint(pp, 0, &a));
	ci->ckpt_size = a;

	return (0);
}

/*
 * __wt_block_buffer_to_ckpt --
 *	Convert a checkpoint cookie into its components, block manager version.
 */
int
__wt_block_buffer_to_ckpt(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *p, WT_BLOCK_CKPT *ci)
{
	return (__block_buffer_to_ckpt(session, block->allocsize, p, ci));
}

/*
 * __wt_block_ckpt_decode --
 *	Convert a checkpoint cookie into its components, external utility
 * version.
 */
int
__wt_block_ckpt_decode(WT_SESSION *wt_session,
    size_t allocsize, const uint8_t *p, WT_BLOCK_CKPT *ci)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	return (__block_buffer_to_ckpt(session, (uint32_t)allocsize, p, ci));
}

/*
 * __wt_block_ckpt_to_buffer --
 *	Convert the components into its checkpoint cookie.
 */
int
__wt_block_ckpt_to_buffer(WT_SESSION_IMPL *session,
    WT_BLOCK *block, uint8_t **pp, WT_BLOCK_CKPT *ci)
{
	uint64_t a;

	if (ci->version != WT_BM_CHECKPOINT_VERSION)
		WT_RET_MSG(session, WT_ERROR, "unsupported checkpoint version");

	(*pp)[0] = ci->version;
	(*pp)++;

	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    ci->root_offset, ci->root_size, ci->root_cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    ci->alloc.offset, ci->alloc.size, ci->alloc.cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    ci->avail.offset, ci->avail.size, ci->avail.cksum));
	WT_RET(__wt_block_addr_to_buffer(block, pp,
	    ci->discard.offset, ci->discard.size, ci->discard.cksum));
	a = (uint64_t)ci->file_size;
	WT_RET(__wt_vpack_uint(pp, 0, a));
	a = (uint64_t)ci->ckpt_size;
	WT_RET(__wt_vpack_uint(pp, 0, a));

	return (0);
}
