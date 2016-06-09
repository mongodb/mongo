/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __verify_ckptfrag_add(
	WT_SESSION_IMPL *, WT_BLOCK *, wt_off_t, wt_off_t);
static int __verify_ckptfrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_filefrag_add(
	WT_SESSION_IMPL *, WT_BLOCK *, const char *, wt_off_t, wt_off_t, bool);
static int __verify_filefrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_last_avail(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);
static int __verify_set_file_size(WT_SESSION_IMPL *, WT_BLOCK *, WT_CKPT *);

/* The bit list ignores the first block: convert to/from a frag/offset. */
#define	WT_wt_off_TO_FRAG(block, off)					\
	((off) / (block)->allocsize - 1)
#ifdef HAVE_VERBOSE
#define	WT_FRAG_TO_OFF(block, frag)					\
	(((wt_off_t)(frag + 1)) * (block)->allocsize)
#endif

/*
 * __wt_block_verify_start --
 *	Start file verification.
 */
int
__wt_block_verify_start(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_CKPT *ckptbase, const char *cfg[])
{
	WT_CKPT *ckpt;
	WT_CONFIG_ITEM cval;
	wt_off_t size;

	/* Configuration: strict behavior on any error. */
	WT_RET(__wt_config_gets(session, cfg, "strict", &cval));
	block->verify_strict = cval.val != 0;

	/* Configuration: dump the file's layout. */
	WT_RET(__wt_config_gets(session, cfg, "dump_layout", &cval));
	block->verify_layout = cval.val != 0;

	/*
	 * Find the last checkpoint in the list: if there are none, or the only
	 * checkpoint we have is fake, there's no work to do.  Don't complain,
	 * that's not our problem to solve.
	 */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	for (;; --ckpt) {
		if (ckpt->name != NULL && !F_ISSET(ckpt, WT_CKPT_FAKE))
			break;
		if (ckpt == ckptbase)
			return (0);
	}

	/* Set the size of the file to the size of the last checkpoint. */
	WT_RET(__verify_set_file_size(session, block, ckpt));

	/*
	 * We're done if the file has no data pages (this happens if we verify
	 * a file immediately after creation or the checkpoint doesn't reflect
	 * any of the data pages).
	 */
	size = block->size;
	if (size <= block->allocsize)
		return (0);

	/* The file size should be a multiple of the allocation size. */
	if (size % block->allocsize != 0)
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
	 */
	block->frags = (uint64_t)WT_wt_off_TO_FRAG(block, size);
	WT_RET(__bit_alloc(session, block->frags, &block->fragfile));

	/*
	 * Set this before reading any extent lists: don't panic if we see
	 * corruption.
	 */
	block->verify = true;

	/*
	 * We maintain an allocation list that is rolled forward through the
	 * set of checkpoints.
	 */
	WT_RET(__wt_block_extlist_init(
	    session, &block->verify_alloc, "verify", "alloc", false));

	/*
	 * The only checkpoint avail list we care about is the last one written;
	 * get it now and initialize the list of file fragments.
	 */
	WT_RET(__verify_last_avail(session, block, ckpt));

	return (0);
}

/*
 * __verify_last_avail --
 *	Get the last checkpoint's avail list and load it into the list of file
 * fragments.
 */
static int
__verify_last_avail(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckpt)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_DECL_RET;
	WT_EXT *ext;
	WT_EXTLIST *el;

	ci = &_ci;
	WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
	WT_ERR(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));

	el = &ci->avail;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_ERR(__wt_block_extlist_read_avail(
		    session, block, el, ci->file_size));
		WT_EXT_FOREACH(ext, el->off)
			if ((ret = __verify_filefrag_add(
			    session, block, "avail-list chunk",
			    ext->off, ext->size, true)) != 0)
				break;
	}

err:	__wt_block_ckpt_destroy(session, ci);
	return (ret);
}

/*
 * __verify_set_file_size --
 *	Set the file size to the last checkpoint's size.
 */
static int
__verify_set_file_size(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckpt)
{
	WT_BLOCK_CKPT *ci, _ci;
	WT_DECL_RET;
	WT_DECL_ITEM(tmp);

	ci = &_ci;
	WT_RET(__wt_block_ckpt_init(session, ci, ckpt->name));
	WT_ERR(__wt_block_buffer_to_ckpt(session, block, ckpt->raw.data, ci));

	if (block->verify_layout) {
		WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__wt_msg(session, "%s: physical size %s", block->name,
		    __wt_buf_set_size(
		    session, (uint64_t)block->size, true, tmp)));
		WT_ERR(
		    __wt_msg(session, "%s: correcting to %s checkpoint size %s",
		    block->name, ckpt->name, __wt_buf_set_size(
		    session, (uint64_t)ci->file_size, true, tmp)));
	}

	/*
	 * Verify is read-only. Set the block's file size information as if we
	 * truncated the file during checkpoint load, so references to blocks
	 * after last checkpoint's file size fail.
	 */
	block->size = block->extend_size = ci->file_size;

err:	__wt_block_ckpt_destroy(session, ci);
	__wt_scr_free(session, &tmp);
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

	block->verify = false;
	block->verify_strict = false;
	block->verify_size = 0;

	/* Discard the accumulated allocation list. */
	__wt_block_extlist_free(session, &block->verify_alloc);

	/* Discard the fragment tracking lists. */
	block->frags = 0;
	__wt_free(session, block->fragfile);
	__wt_free(session, block->fragckpt);

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
	uint64_t frag, frags;

	/* Set the maximum file size for this checkpoint. */
	block->verify_size = ci->file_size;

	/*
	 * Add the root page and disk blocks used to store the extent lists to
	 * the list of blocks we've "seen" from the file.
	 */
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "checkpoint",
		    ci->root_offset, (wt_off_t)ci->root_size, true));
	if (ci->alloc.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "alloc list",
		    ci->alloc.offset, (wt_off_t)ci->alloc.size, true));
	if (ci->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "avail list",
		    ci->avail.offset, (wt_off_t)ci->avail.size, true));
	if (ci->discard.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session, block, "discard list",
		    ci->discard.offset, (wt_off_t)ci->discard.size, true));

	/*
	 * Checkpoint verification is similar to deleting checkpoints.  As we
	 * read each new checkpoint, we merge the allocation lists (accumulating
	 * all allocated pages as we move through the system), and then remove
	 * any pages found in the discard list. The result should be a
	 * one-to-one mapping to the pages we find in this specific checkpoint.
	 */
	el = &ci->alloc;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(
		    session, block, el, ci->file_size));
		WT_RET(__wt_block_extlist_merge(
		    session, block, el, &block->verify_alloc));
		__wt_block_extlist_free(session, el);
	}
	el = &ci->discard;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(
		    session, block, el, ci->file_size));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__wt_block_off_remove_overlap(session, block,
			    &block->verify_alloc, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}

	/*
	 * We don't need the blocks on a checkpoint's avail list, but we read it
	 * to ensure it wasn't corrupted.  We could confirm correctness of the
	 * intermediate avail lists (that is, if they're logically the result
	 * of the allocations and discards to this point). We don't because the
	 * only avail list ever used is the one for the last checkpoint, which
	 * is separately verified by checking it against all of the blocks found
	 * in the file.
	 */
	el = &ci->avail;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(
		    session, block, el, ci->file_size));
		__wt_block_extlist_free(session, el);
	}

	/*
	 * The root page of the checkpoint appears on the alloc list, but not,
	 * at least until the checkpoint is deleted, on a discard list. To
	 * handle this case, remove the root page from the accumulated list of
	 * checkpoint pages, so it doesn't add a new requirement for subsequent
	 * checkpoints.
	 */
	if (ci->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_off_remove_overlap(session, block,
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
		frag = (uint64_t)WT_wt_off_TO_FRAG(block, ext->off);
		frags = (uint64_t)(ext->size / block->allocsize);
		__bit_nset(block->fragckpt, frag, frag + (frags - 1));
	}

	return (0);
}

/*
 * __wt_verify_ckpt_unload --
 *	Verify work done when a checkpoint is unloaded.
 */
int
__wt_verify_ckpt_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;

	/* Confirm we verified every checkpoint block. */
	ret = __verify_ckptfrag_chk(session, block);

	/* Discard the per-checkpoint fragment list. */
	__wt_free(session, block->fragckpt);

	return (ret);
}

/*
 * __wt_block_verify_addr --
 *	Update an address in a checkpoint as verified.
 */
int
__wt_block_verify_addr(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, size_t addr_size)
{
	wt_off_t offset;
	uint32_t cksum, size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/* Add to the per-file list. */ 
	WT_RET(
	    __verify_filefrag_add(session, block, NULL, offset, size, false));

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
__verify_filefrag_add(WT_SESSION_IMPL *session, WT_BLOCK *block,
    const char *type, wt_off_t offset, wt_off_t size, bool nodup)
{
	uint64_t f, frag, frags, i;

	WT_RET(__wt_verbose(session, WT_VERB_VERIFY,
	    "add file block%s%s%s at %" PRIuMAX "-%" PRIuMAX " (%" PRIuMAX ")",
	    type == NULL ? "" : " (",
	    type == NULL ? "" : type,
	    type == NULL ? "" : ")",
	    (uintmax_t)offset, (uintmax_t)(offset + size), (uintmax_t)size));

	/* Check each chunk against the total file size. */
	if (offset + size > block->size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "non-existent file blocks",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	frag = (uint64_t)WT_wt_off_TO_FRAG(block, offset);
	frags = (uint64_t)(size / block->allocsize);

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
	uint64_t count, first, last;

	/* If there's nothing to verify, it was a fast run. */
	if (block->frags == 0)
		return (0);

	/*
	 * It's OK if we have not verified blocks at the end of the file: that
	 * happens if the file is truncated during a checkpoint or load or was
	 * extended after writing a checkpoint.  We should never see unverified
	 * blocks anywhere else, though.
	 *
	 * I'm deliberately testing for a last fragment of 0, it makes no sense
	 * there would be no fragments verified, complain if the first fragment
	 * in the file wasn't verified.
	 */
	for (last = block->frags - 1; last != 0; --last) {
		if (__bit_test(block->fragfile, last))
			break;
		__bit_set(block->fragfile, last);
	}

	/*
	 * Check for any other file fragments we haven't verified -- every time
	 * we find a bit that's clear, complain.  We re-start the search each
	 * time after setting the clear bit(s) we found: it's simpler and this
	 * isn't supposed to happen a lot.
	 */
	for (count = 0;; ++count) {
		if (__bit_ffc(block->fragfile, block->frags, &first) != 0)
			break;
		__bit_set(block->fragfile, first);
		for (last = first + 1; last < block->frags; ++last) {
			if (__bit_test(block->fragfile, last))
				break;
			__bit_set(block->fragfile, last);
		}

#ifdef HAVE_VERBOSE
		if (!WT_VERBOSE_ISSET(session, WT_VERB_VERIFY))
			continue;

		__wt_errx(session,
		    "file range %" PRIuMAX "-%" PRIuMAX " never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
#endif
	}
	if (count == 0)
		return (0);

	__wt_errx(session, "file ranges never verified: %" PRIu64, count);
	return (block->verify_strict ? WT_ERROR : 0);
}

/*
 * __verify_ckptfrag_add --
 *	Clear the fragments in the per-checkpoint fragment list, and complain if
 * we've already verified this chunk of the checkpoint.
 */
static int
__verify_ckptfrag_add(
    WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t offset, wt_off_t size)
{
	uint64_t f, frag, frags, i;

	WT_RET(__wt_verbose(session, WT_VERB_VERIFY,
	    "add checkpoint block at %" PRIuMAX "-%" PRIuMAX " (%" PRIuMAX ")",
	    (uintmax_t)offset, (uintmax_t)(offset + size), (uintmax_t)size));

	/*
	 * Check each chunk against the checkpoint's size, a checkpoint should
	 * never reference a block outside of the checkpoint's stored size.
	 */
	if (offset + size > block->verify_size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "file blocks outside the checkpoint",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	frag = (uint64_t)WT_wt_off_TO_FRAG(block, offset);
	frags = (uint64_t)(size / block->allocsize);

	/* It is illegal to reference a particular chunk more than once. */
	for (f = frag, i = 0; i < frags; ++f, ++i)
		if (!__bit_test(block->fragckpt, f))
			WT_RET_MSG(session, WT_ERROR,
			    "fragment at %" PRIuMAX " referenced multiple "
			    "times in a single checkpoint or found in the "
			    "checkpoint but not listed in the checkpoint's "
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
	uint64_t count, first, last;

	/*
	 * The checkpoint fragment memory is only allocated as a checkpoint
	 * is successfully loaded; don't check if there's nothing there.
	 */
	if (block->fragckpt == NULL)
		return (0);

	/*
	 * Check for checkpoint fragments we haven't verified -- every time we
	 * find a bit that's set, complain.  We re-start the search each time
	 * after clearing the set bit(s) we found: it's simpler and this isn't
	 * supposed to happen a lot.
	 */
	for (count = 0;; ++count) {
		if (__bit_ffs(block->fragckpt, block->frags, &first) != 0)
			break;
		__bit_clear(block->fragckpt, first);
		for (last = first + 1; last < block->frags; ++last) {
			if (!__bit_test(block->fragckpt, last))
				break;
			__bit_clear(block->fragckpt, last);
		}

#ifdef HAVE_VERBOSE
		if (!WT_VERBOSE_ISSET(session, WT_VERB_VERIFY))
			continue;

		__wt_errx(session,
		    "checkpoint range %" PRIuMAX "-%" PRIuMAX " never verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
#endif
	}

	if (count == 0)
		return (0);

	__wt_errx(session,
	    "checkpoint ranges never verified: %" PRIu64, count);
	return (block->verify_strict ? WT_ERROR : 0);
}
