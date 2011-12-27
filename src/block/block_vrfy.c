/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __verify_addfrag(WT_SESSION_IMPL *, WT_BLOCK *, uint32_t, uint32_t);
static int __verify_checkfrag(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_freelist(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_verify_start --
 *	Start file verification.
 */
int
__wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block, int *emptyp)
{
	/*
	 * We're done if the file has no data pages (this is what happens if
	 * we verify a file immediately after creation).
	 */
	if (block->fh->file_size == WT_BTREE_DESC_SECTOR) {
		*emptyp = 1;
		return (0);
	}
	*emptyp = 0;

	/*
	 * The file size should be a multiple of the allocsize, offset by the
	 * size of the descriptor sector, the first 512B of the file.
	 */
	if ((block->fh->file_size -
	    WT_BTREE_DESC_SECTOR) % block->allocsize != 0) {
		__wt_errx(session,
		    "the file size is not valid for the allocation size");
		    return (WT_ERROR);
	}

	/*
	 * Allocate a bit array, where each bit represents a single allocation
	 * size piece of the file (this is how we track the parts of the file
	 * we've verified, and check for multiply referenced or unreferenced
	 * blocks).  Storing this on the heap seems reasonable; verifying a 1TB
	 * file with an allocation size of 512B would require a 256MB bit array:
	 *
	 *	(((1 * 2^40) / 512) / 8) / 2^20 = 256
	 *
	 * To verify larger files than we can handle in this way, we'd have to
	 * write parts of the bit array into a disk file.
	 */
	block->frags = WT_OFF_TO_ADDR(block, block->fh->file_size);
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
    WT_BLOCK *block, const uint8_t *addrbuf, uint32_t addrbuf_size)
{
	uint32_t addr, size;

	WT_UNUSED(addrbuf_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(addrbuf, &addr, &size, NULL));

	WT_RET(__verify_addfrag(session, block, addr, size));

	return (0);
}

/*
 * __verify_freelist --
 *	Add the freelist fragments to the list of verified fragments.
 */
static int
__verify_freelist(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FREE_ENTRY *fe;
	int ret;

	ret = 0;

	TAILQ_FOREACH(fe, &block->freeqa, qa) {
		if (WT_ADDR_TO_OFF(block, fe->addr) +
		    (off_t)fe->size > block->fh->file_size) {
			__wt_errx(session,
			    "free-list entry addr %" PRIu32 "references "
			    "non-existent file pages",
			    fe->addr);
			return (WT_ERROR);
		}
		WT_VERBOSE(session, verify,
		    "free-list addr/frags %" PRIu32 "/%" PRIu32,
		    fe->addr, fe->size / block->allocsize);
		WT_TRET(__verify_addfrag(session, block, fe->addr, fe->size));
	}

	return (ret);
}

/*
 * __verify_addfrag --
 *	Add the fragments to the list, and complain if we've already verified
 *	this chunk of the file.
 */
static int
__verify_addfrag(
    WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t addr, uint32_t size)
{
	uint32_t frags, i;

	frags = size / block->allocsize;
	for (i = 0; i < frags; ++i)
		if (__bit_test(block->fragbits, addr + i)) {
			__wt_errx(session,
			    "file fragment at addr %" PRIu32
			    " already verified", addr);
			return (WT_ERROR);
		}
	if (frags > 0)
		__bit_nset(block->fragbits, addr, addr + (frags - 1));
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
		if (first == last)
			__wt_errx(session,
			    "file fragment %" PRIu32 " was never verified",
			    first);
		else
			__wt_errx(session,
			    "file fragments %" PRIu32 "-%" PRIu32 " were "
			    "never verified",
			    first, last);
		ret = WT_ERROR;
	}
	return (ret);
}
