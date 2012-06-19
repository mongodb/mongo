/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __verify_ckptfrag_add(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_ckptfrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_filefrag_add(
	WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t, int);
static int __verify_filefrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_start_avail(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);
static int __verify_start_filesize(
	WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *, off_t *);

/* The bit list ignores the first sector: convert to/from a frag/offset. */
#define	WT_OFF_TO_FRAG(block, off)					\
	(((off) - WT_BLOCK_DESC_SECTOR) / (block)->allocsize)
#define	WT_FRAG_TO_OFF(block, frag)					\
	(((off_t)(frag)) * (block)->allocsize + WT_BLOCK_DESC_SECTOR)

/*
 * __wt_block_verify_start --
 *	Start file verification.
 */
int
__wt_block_verify_start(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase)
{
	off_t file_size;

	/*
	 * We're done if the file has no data pages (this happens if we verify
	 * a file immediately after creation).
	 */
	if (block->fh->file_size == WT_BLOCK_DESC_SECTOR)
		return (0);

	/*
	 * Opening a WiredTiger file truncates it back to the checkpoint we are
	 * rolling forward, which means it's OK if there are blocks written
	 * after that checkpoint, they'll be ignored.  Find the largest file
	 * size referenced by any checkpoint.
	 */
	WT_RET(__verify_start_filesize(session, block, ckptbase, &file_size));

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

	/*
	 * We maintain an allocation list that is rolled forward through the
	 * set of checkpoints.
	 */
	WT_RET(__wt_block_extlist_init(
	    session, &block->verify_alloc, "verify", "alloc"));

	/*
	 * The only checkpoint avail list we care about is the last one written;
	 * get it now and initialize the list of file fragments.
	 */
	WT_RET(__verify_start_avail(session, block, ckptbase));

	block->verify = 1;
	return (0);
}

/*
 * __verify_start_filesize --
 *	Set the file size for the last checkpoint.
 */
static int
__verify_start_filesize(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_CKPT *ckptbase, off_t *file_sizep)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_CKPT *ckpt;
	off_t file_size;

	ci = &_ci;

	/*
	 * Find the largest file size referenced by any checkpoint: that should
	 * be the last checkpoint taken, but out of sheer, raving paranoia, look
	 * through the list, future changes to checkpoints might break this code
	 * if we make that assumption.
	 */
	file_size = 0;
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		WT_RET(__wt_block_buffer_to_ckpt(
		    session, block, ckpt->raw.data, ci));
		if (ci->file_size > file_size)
			file_size = ci->file_size;
	}

	/* Verify doesn't make any sense if we don't have a checkpoint. */
	if (file_size <= WT_BLOCK_DESC_SECTOR)
		WT_RET_MSG(session, WT_ERROR,
		    "%s has no checkpoints to verify", block->name);

	/*
	 * The file size should be a multiple of the allocsize, offset by the
	 * size of the descriptor sector, the first 512B of the file.
	 */
	file_size -= WT_BLOCK_DESC_SECTOR;
	if (file_size % block->allocsize != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "the checkpoint file size is not a multiple of the "
		    "allocation size");

	*file_sizep = file_size;
	return (0);
}

/*
 * __verify_start_avail --
 *	Get the last checkpoint's avail list and load it into the list of file
 * fragments.
 */
static int
__verify_start_avail(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_CKPT *ckpt;
	WT_DECL_RET;
	WT_EXT *ext;
	WT_EXTLIST *el;

	/* Get the last on-disk checkpoint, if one exists. */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	if (ckpt == ckptbase)
		return (0);
	--ckpt;

	ci = &_ci;
	WT_RET(__wt_block_ckpt_init(session, block, ci, ckpt->name, 0));
	WT_ERR(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));

	el = &ci->avail;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_ERR(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			if ((ret = __verify_filefrag_add(
			    session, block, ext->off, ext->size, 1)) != 0)
				break;
	}

err:	__wt_block_ckpt_destroy(session, ci);
	return (ret);
}

/*
 * __wt_block_verify_end --
 *	End file verification.
 */
int
__wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;

	/* Confirm we verified every file block. */
	ret = __verify_filefrag_chk(session, block);

	/* Discard the accumulated allocation list. */
	__wt_block_extlist_free(session, &block->verify_alloc);

	/* Discard the fragment tracking lists. */
	__wt_free(session, block->fragfile);
	__wt_free(session, block->fragckpt);

	block->verify = 0;
	return (ret);
}

/*
 * __wt_verify_ckpt_load --
 *	Verify work done when a checkpoint is loaded.
 */
int
__wt_verify_ckpt_load(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
{
	WT_EXTLIST *el;
	WT_EXT *ext;
	uint32_t frag, frags;

	/* Set the maximum file size for this checkpoint. */
	block->verify_size = ci->file_size;

	/*
	 * Add the root page and disk blocks used to store the extent lists to
	 * the list of blocks we've "seen" from the file.
	 */
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, ci->root_offset, (off_t)ci->root_size, 1));
	if (ci->alloc.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, ci->alloc.offset, (off_t)ci->alloc.size, 1));
	if (ci->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, ci->avail.offset, (off_t)ci->avail.size, 1));
	if (ci->discard.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, ci->discard.offset, (off_t)ci->discard.size, 1));

	/*
	 * Checkpoint verification is similar to deleting checkpoints.  As we
	 * read each new checkpoint, we merge the allocation lists (accumulating
	 * all allocated pages as we move through the system), and then remove
	 * any pages found in the discard list.   The result should be a
	 * one-to-one mapping to the pages we find in this specific checkpoint.
	 */
	el = &ci->alloc;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_RET(__wt_block_extlist_merge(
		    session, el, &block->verify_alloc));
		__wt_block_extlist_free(session, el);
	}
	el = &ci->discard;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__wt_block_off_remove_overlap(session,
			    &block->verify_alloc, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}

	/*
	 * The root page of the checkpoint appears on the alloc list, but not,
	 * at least until the checkpoint is deleted, on a discard list.   To
	 * handle this case, remove the root page from the accumulated list of
	 * checkpoint pages, so it doesn't add a new requirement for subsequent
	 * checkpoints.
	 */
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_off_remove_overlap(session,
		    &block->verify_alloc, ci->root_offset, ci->root_size));

	/*
	 * Allocate the per-checkpoint bit map.  The per-checkpoint bit map is
	 * the opposite of the per-file bit map, that is, we set all the bits
	 * that we expect to be set based on the checkpoint's allocation and
	 * discard lists, then clear bits as we verify blocks.  When finished
	 * verifying the checkpoint, the bit list should be empty.
	 */
	WT_RET(__bit_alloc(session, block->frags, &block->fragckpt));
	el = &block->verify_alloc;
	WT_EXT_FOREACH(ext, el->off) {
		frag = (uint32_t)WT_OFF_TO_FRAG(block, ext->off);
		frags = (uint32_t)(ext->size / block->allocsize);
		__bit_nset(block->fragckpt, frag, frag + (frags - 1));
	}

	return (0);
}

/*
 * __wt_verify_ckpt_unload --
 *	Verify work done when a checkpoint is unloaded.
 */
int
__wt_verify_ckpt_unload(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
{
	WT_DECL_RET;

	WT_UNUSED(ci);

	/* Confirm we verified every checkpoint block. */
	ret = __verify_ckptfrag_chk(session, block);

	/* Discard the per-checkpoint fragment list. */
	__wt_free(session, block->fragckpt);

	return (ret);
}

/*
 * __wt_block_verify --
 *	Physically verify a disk block, if we haven't already verified it.
 */
int
__wt_block_verify(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
    const uint8_t *addr, uint32_t addr_size, off_t offset, uint32_t size)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	uint32_t frag, frags, i, match;

	/*
	 * If we've already verify this block's physical image, we know it's
	 * good, we don't have to verify it again.
	 */
	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);
	for (match = i = 0; i < frags; ++i)
		if (__bit_test(block->fragfile, frag++))
			++match;
	if (match == frags) {
		WT_VERBOSE_RET(session, verify,
		    "skipping block at %" PRIuMAX "-%" PRIuMAX ", already "
		    "verified",
		    (uintmax_t)offset, (uintmax_t)(offset + size));
		return (0);
	}
	if (match != 0)
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
 *	Update an address in a checkpoint as verified.
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

	/* Add to the per-file list. */ 
	WT_RET(__verify_filefrag_add(session, block, offset, size, 0));

	/*
	 * It's tempting to try and flag a page as "verified" when we read it.
	 * That doesn't work because we may visit a page multiple times when
	 * verifying a single checkpoint (for example, when verifying the
	 * physical image of a row-store leaf page with overflow keys, the
	 * overflow keys are read when checking for key sort issues, and read
	 * again when more general overflow item checking is done).  This
	 * function is called by the btree verification code, once per logical
	 * visit in a checkpoint, so we can detect if a page is referenced
	 * multiple times within a single checkpoint.  This doesn't apply to
	 * the per-file list, because it is expected for the same btree blocks
	 * to appear in multiple checkpoints.
	 *
	 * Add the block to the per-checkpoint list.
	 */
	WT_RET(__verify_ckptfrag_add(session, block, offset, size));

	return (0);
}

/*
 * __verify_filefrag_add --
 *	Add the fragments to the per-file fragment list, optionally complain if
 * we've already verified this chunk of the file.
 */
static int
__verify_filefrag_add(WT_SESSION_IMPL *session,
    WT_BLOCK *block, off_t offset, off_t size, int nodup)
{
	uint32_t f, frag, frags, i;

	WT_VERBOSE_RET(session, verify,
	    "adding file block at %" PRIuMAX "-%" PRIuMAX " (%" PRIuMAX ")",
	    (uintmax_t)offset, (uintmax_t)(offset + size), (uintmax_t)size);

	/* Check each chunk against the total file size. */
	if (offset + size > block->fh->file_size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "non-existent file blocks",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);

	/* It may be illegal to reference a particular chunk more than once. */
	if (nodup)
		for (f = frag, i = 0; i < frags; ++f, ++i)
			if (__bit_test(block->fragfile, f))
				WT_RET_MSG(session, WT_ERROR,
				    "file fragment at %" PRIuMAX " referenced "
				    "multiple times",
				    (uintmax_t)offset);

	/* Add fragments to the file's fragment list. */
	__bit_nset(block->fragfile, frag, frag + (frags - 1));

	return (0);
}

/*
 * __verify_filefrag_chk --
 *	Verify we've checked all the fragments in the file.
 */
static int
__verify_filefrag_chk(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;
	uint32_t first, last;

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
		    "file range %" PRIuMAX "-%" PRIuMAX " never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __verify_ckptfrag_add --
 *	Clear the fragments in the per-checkpoint fragment list, and complain if
 * we've already verified this chunk of the checkpoint.
 */
static int
__verify_ckptfrag_add(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t f, frag, frags, i;

	WT_VERBOSE_RET(session, verify,
	    "add checkpoint block at %" PRIuMAX "-%" PRIuMAX " (%" PRIuMAX ")",
	    (uintmax_t)offset, (uintmax_t)(offset + size), (uintmax_t)size);

	/*
	 * Check each chunk against the checkpoint's size, a checkpoint should
	 * never reference a block outside of the checkpoint's stored size.
	 */
	if (offset + size > block->verify_size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "file blocks outside the checkpoint",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);

	/* It is illegal to reference a particular chunk more than once. */
	for (f = frag, i = 0; i < frags; ++f, ++i)
		if (!__bit_test(block->fragckpt, f))
			WT_RET_MSG(session, WT_ERROR,
			    "checkpoint fragment at %" PRIuMAX " referenced "
			    "multiple times in a single checkpoint or found in "
			    "the checkpoint but not listed in the checkpoint's "
			    "allocation list",
			    (uintmax_t)offset);

	/* Remove fragments from the checkpoint's allocation list. */
	__bit_nclr(block->fragckpt, frag, frag + (frags - 1));

	return (0);
}

/*
 * __verify_ckptfrag_chk --
 *	Verify we've checked all the fragments in the checkpoint.
 */
static int
__verify_ckptfrag_chk(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;
	uint32_t first, last;

	/*
	 * Check for checkpoint fragments we haven't verified -- every time we
	 * find a bit that's set, complain.  We re-start the search each time
	 * after clearing the set bit(s) we found: it's simpler and this isn't
	 * supposed to happen a lot.
	 */
	for (;;) {
		if (__bit_ffs(block->fragckpt, block->frags, &first) != 0)
			break;
		__bit_clear(block->fragckpt, first);
		for (last = first + 1; last < block->frags; ++last) {
			if (!__bit_test(block->fragckpt, last))
				break;
			__bit_clear(block->fragckpt, last);
		}

		__wt_errx(session,
		    "checkpoint range %" PRIuMAX "-%" PRIuMAX " never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
		ret = WT_ERROR;
	}
	return (ret);
}
