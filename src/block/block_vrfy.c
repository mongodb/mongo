/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __verify_addfrag(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_checkfrag(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_eof(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_frag_notset(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_frag_set(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);

/*
 * __wt_block_verify_start --
 *	Start file verification.
 */
int
__wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	off_t file_size;

	file_size = block->fh->file_size;

	/*
	 * We're done if the file has no data pages (this is what happens if
	 * we verify a file immediately after creation).
	 */
	if (file_size == WT_BLOCK_DESC_SECTOR)
		return (0);

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
	 * Alternatively, we could switch to maintaining ranges of the file as
	 * we do with the extents, but that has its own failure mode, where we
	 * verify many non-contiguous blocks creating too many entries on the
	 * list to fit into memory.
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
	WT_RET(__bit_alloc(session, block->frags, &block->fragfile));

	block->verify = 1;
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

	__wt_free(session, block->fragfile);
	__wt_free(session, block->fragsnap);

	block->verify = 0;
	return (ret);
}

/*
 * __wt_verify_snap_load --
 *	Verify work done when a snapshot is loaded.
 */
int
__wt_verify_snap_load(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si)
{
	WT_EXTLIST *el;
	WT_EXT *ext;

	/* Allocate the per-snapshot bit map. */
	__wt_free(session, block->fragsnap);
	WT_RET(__bit_alloc(session, block->frags, &block->fragsnap));

	/*
	 * If we're verifying, add the disk blocks used to store the root page
	 * and the extent lists to the list of blocks we've "seen".
	 */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_addfrag(session,
		    block, si->root_offset, (off_t)si->root_size));

	if (si->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_addfrag(session,
		    block, si->avail.offset, (off_t)si->avail.size));
	if (si->alloc.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_addfrag(session,
		    block, si->alloc.offset, (off_t)si->alloc.size));
	if (si->discard.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_addfrag(session,
		    block, si->discard.offset, (off_t)si->discard.size));

	/*
	 * Read the avail and discard lists (blocks that are "available" to the
	 * tree for allocation or have been deleted should not appear in the
	 * snapshot's blocks).
	 */
	el = &si->avail;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__verify_addfrag(
			    session, block, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}
	el = &si->discard;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__verify_addfrag(
			    session, block, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}

	return (0);
}

/*
 * __wt_verify_snap_unload --
 *	Verify work done when a snapshot is unloaded.
 */
int
__wt_verify_snap_unload(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si)
{
	WT_EXTLIST *el;
	WT_EXT *ext;

	/*
	 * Read the alloc list (blocks that were allocated in the snapshot must
	 * appear in the snapshot).
	 */
	el = &si->alloc;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__verify_frag_set(
			    session, block, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}

	/* Discard the per-snapshot fragment list. */
	__wt_free(session, block->fragsnap);

	return (0);
}

/* The bit list ignores the first sector: convert to/from a frag/offset. */
#define	WT_OFF_TO_FRAG(block, off)					\
	(((off) - WT_BLOCK_DESC_SECTOR) / (block)->allocsize)
#define	WT_FRAG_TO_OFF(block, frag)					\
	(((off_t)(frag)) * (block)->allocsize + WT_BLOCK_DESC_SECTOR)

/*
 * __wt_block_verify --
 *	Verify a block found on disk if we haven't already verified it.
 */
int
__wt_block_verify(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
    const uint8_t *addr, uint32_t addr_size, off_t offset, uint32_t size)
{
	WT_ITEM *tmp;
	uint32_t cnt, frag, frags, i;
	int ret;

	/*
	 * If we've already verify this block's physical image, we know it's
	 * good, we don't have to verify it again.
	 */
	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);
	for (cnt = i = 0; i < frags; ++i)
		if (__bit_test(block->fragfile, frag++))
			++cnt;
	if (cnt == frags) {
		WT_VERBOSE(session, verify,
		    "skipping block at %" PRIuMAX "-%" PRIuMAX ", already "
		    "verified",
		    (uintmax_t)offset, (uintmax_t)(offset + size));
		return (0);
	}
	if (cnt != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "block at %" PRIuMAX "-%" PRIuMAX " partially verified",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	/*
	 * Create a string representation of the address cookie and verify the
	 * block.
	 */
	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_block_addr_string(session, block, tmp, addr, addr_size));
	WT_ERR(__wt_verify_dsk(session, (char *)tmp->data, buf));

err:	__wt_scr_free(&tmp);
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
	uint32_t cksum, size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	WT_RET(__verify_addfrag(session, block, offset, (off_t)size));

	return (0);
}

/*
 * __verify_eof --
 *	Check each fragment against the total file size.
 */
static int
__verify_eof(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	/*
	 * Check each fragment against the total file size: this test is usually
	 * not necessary, but for some fragments, like extent lists, corruption
	 * could lead to random offsets, and paranoia in verify is a good thing.
	 */
	if (offset + size > block->fh->file_size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "non-existent file pages",
		    (uintmax_t)offset, (uintmax_t)(offset + size));
	return (0);
}

/*
 * __verify_frag_notset --
 *	Confirm we have NOT seen this chunk of the file.
 */
static int
__verify_frag_notset(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t frag, frags, i;

	WT_RET(__verify_eof(session, block, offset, size));

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);
	for (i = 0; i < frags; ++i)
		if (__bit_test(block->fragsnap, frag++))
			WT_RET_MSG(session, WT_ERROR,
			    "file fragment at offset %" PRIuMAX
			    " referenced multiple times in a single snapshot",
			    (uintmax_t)offset);

	return (0);
}
/*
 * __verify_frag_set --
 *	Confirm we have seen this chunk of the file.
 */
static int
__verify_frag_set(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t frag, frags, i;

	WT_RET(__verify_eof(session, block, offset, size));

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);
	for (i = 0; i < frags; ++i)
		if (!__bit_test(block->fragsnap, frag++))
			WT_RET_MSG(session, WT_ERROR,
			    "file fragment at offset %" PRIuMAX " should have "
			    " been referenced in the snapshot but was not",
			    (uintmax_t)offset);

	return (0);
}

/*
 * __verify_addfrag --
 *	Add the fragments to the list, and complain if we've already verified
 *	this chunk of the file.
 */
static int
__verify_addfrag(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t frag, frags;

	WT_VERBOSE(session, verify,
	    "adding block at %" PRIuMAX "-%" PRIuMAX,
	    (uintmax_t)offset, (uintmax_t)(offset + size));

	/* We should not have seen these fragments in this snapshot. */
	WT_RET(__verify_frag_notset(session, block, offset, size));

	/* Add fragments to the snapshot and total fragment lists. */
	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);
	__bit_nset(block->fragsnap, frag, frag + (frags - 1));
	__bit_nset(block->fragfile, frag, frag + (frags - 1));

	return (0);
}

/*
 * __verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__verify_checkfrag(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	uint32_t first, last;
	int ret;

	ret = 0;

	/*
	 * Check for file fragments we haven't verified -- every time we find
	 * a bit that's clear, complain.  We re-start the search each time
	 * after setting the clear bit(s) we found: it's simpler and this isn't
	 * supposed to happen a lot.
	 */
	for (;;) {
		if (__bit_ffc(block->fragfile, block->frags, &first) != 0)
			break;
		__bit_set(block->fragfile, first);
		for (last = first + 1; last < block->frags; ++last) {
			if (__bit_test(block->fragfile, last))
				break;
			__bit_set(block->fragfile, last);
		}

		__wt_errx(session,
		    "file range %" PRIuMAX "-%" PRIuMAX " was never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
		ret = WT_ERROR;
	}
	return (ret);
}
