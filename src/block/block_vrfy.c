/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __verify_addfrag(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_checkfrag(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_freelist(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_verify_start --
 *	Start file verification.
 */
int
__wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block, int *emptyp)
{
	off_t file_size;

	file_size = block->fh->file_size;

	/*
	 * We're done if the file has no data pages (this is what happens if
	 * we verify a file immediately after creation).
	 */
	if (file_size == WT_BLOCK_DESC_SECTOR) {
		*emptyp = 1;
		return (0);
	}
	*emptyp = 0;

	/*
	 * The file size should be a multiple of the allocsize, offset by the
	 * size of the descriptor sector, the first 512B of the file.
	 */
	if (file_size > WT_BLOCK_DESC_SECTOR)
		file_size -= WT_BLOCK_DESC_SECTOR;
	if (file_size % block->allocsize != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "the file size is not a multiple of the allocation size");

	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file (this is how we track the parts of the file
	 * we've verified, and check for multiply referenced or unreferenced
	 * blocks).  Storing this on the heap seems reasonable, verifying a 1TB
	 * file with an 512B allocation size would require a 256MB bit array:
	 *
	 *	(((1 * 2^40) / 512) / 8) = 256 * 2^20
	 *
	 * To verify larger files than we can handle in this way, we'd have to
	 * write parts of the bit array into a disk file.
	 *
	 * We also have a minimum maximum verifiable file size of 16TB because
	 * the underlying bit package takes a 32-bit count of bits to allocate:
	 *
	 *	2^32 * 512 * 8 = 16 * 2^40
	 */
	if (file_size / block->allocsize > UINT32_MAX)
		WT_RET_MSG(
		    session, WT_ERROR, "the file is too large to verify");

	block->frags = (uint32_t)(file_size / block->allocsize);
	WT_RET(__bit_alloc(session, block->frags, &block->fragbits));

	/* Verify the free-list. */
	WT_RET(__verify_freelist(session, block));

	return (0);
}

/*
 * __wt_block_verify_end --
 *	End file verification.
 */
int
__wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	int ret;

	/* Verify we read every file block. */
	ret = __verify_checkfrag(session, block);

	__wt_free(session, block->fragbits);

	return (ret);
}

/*
 * __wt_block_verify_addr --
 *	Verify an address.
 */
int
__wt_block_verify_addr(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, uint32_t addr_size)
{
	off_t offset;
	uint32_t size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, NULL));

	WT_RET(__verify_addfrag(session, block, offset, (off_t)size));

	return (0);
}

/*
 * __verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__verify_freelist(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_EXT *ext;
	int ret;

	ret = 0;

	WT_EXT_FOREACH(ext, block->free.off) {
		if (ext->off + (off_t)ext->size > block->fh->file_size)
			WT_RET_MSG(session, WT_ERROR,
			    "free-list entry offset %" PRIuMAX "references "
			    "non-existent file pages",
			    (uintmax_t)ext->off);

		WT_VERBOSE(session, verify,
		    "free-list range %" PRIdMAX "-%" PRIdMAX,
		    (intmax_t)ext->off, (intmax_t)(ext->off + ext->size));

		WT_TRET(__verify_addfrag(session, block, ext->off, ext->size));
	}

	return (ret);
}

/* The bit list ignores the first sector: convert to/from an frag/offset. */
#define	WT_OFF_TO_FRAG(block, off)					\
	(((off) - WT_BLOCK_DESC_SECTOR) / (block)->allocsize)
#define	WT_FRAG_TO_OFF(block, frag)					\
	(((off_t)(frag)) * (block)->allocsize + WT_BLOCK_DESC_SECTOR)

/*
 * __verify_addfrag --
 *	Add the fragments to the list, and complain if we've already verified
 *	this chunk of the file.
 */
static int
__verify_addfrag(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t frag, frags, i;

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);

	for (i = 0; i < frags; ++i)
		if (__bit_test(block->fragbits, frag + i))
			WT_RET_MSG(session, WT_ERROR,
			    "file fragment at offset %" PRIuMAX
			    " already verified",
			    (uintmax_t)offset);

	__bit_nset(block->fragbits, frag, frag + (frags - 1));
	return (0);
}

/*
 * __verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__verify_checkfrag(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	uint32_t first, last, frags;
	uint8_t *fragbits;
	int ret;

	fragbits = block->fragbits;
	frags = block->frags;
	ret = 0;

	/*
	 * Check for file fragments we haven't verified -- every time we find
	 * a bit that's clear, complain.  We re-start the search each time
	 * after setting the clear bit(s) we found: it's simpler and this isn't
	 * supposed to happen a lot.
	 */
	for (;;) {
		if (__bit_ffc(fragbits, frags, &first) != 0)
			break;
		__bit_set(fragbits, first);
		for (last = first + 1; last < frags; ++last) {
			if (__bit_test(fragbits, last))
				break;
			__bit_set(fragbits, last);
		}

		__wt_errx(session,
		    "file range %" PRIuMAX "-%" PRIuMAX " was never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
		ret = WT_ERROR;
	}
	return (ret);
}
