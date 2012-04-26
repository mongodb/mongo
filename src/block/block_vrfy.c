/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __verify_filefrag_add(
	WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t, int);
static int __verify_filefrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_snapfrag_add(WT_SESSION_IMPL *, WT_BLOCK *, off_t, off_t);
static int __verify_snapfrag_chk(WT_SESSION_IMPL *, WT_BLOCK *);
static int __verify_start_avail(WT_SESSION_IMPL *, WT_BLOCK *, WT_SNAPSHOT *);
static int __verify_start_filesize(
	WT_SESSION_IMPL *, WT_BLOCK *, WT_SNAPSHOT *, off_t *);

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
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_SNAPSHOT *snapbase)
{
	off_t file_size;

	memset(&block->verify_alloc, 0, sizeof(block->verify_alloc));
	block->verify_alloc.name = "verify_alloc";
	block->verify_alloc.offset = WT_BLOCK_INVALID_OFFSET;

	/*
	 * We're done if the file has no data pages (this happens if we verify
	 * a file immediately after creation).
	 */
	if (block->fh->file_size == WT_BLOCK_DESC_SECTOR)
		return (0);

	/*
	 * Opening a WiredTiger file truncates it back to the snapshot we are
	 * rolling forward, which means it's OK if there are blocks written
	 * after that snapshot, they'll be ignored.  Find the largest file size
	 * referenced by any snapshot.
	 */
	WT_RET(__verify_start_filesize(session, block, snapbase, &file_size));

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
	 * The only snapshot avail list we care about is the last one written;
	 * get it now and initialize the list of file fragments.
	 */
	WT_RET(__verify_start_avail(session, block, snapbase));

	block->verify = 1;
	return (0);
}

/*
 * __verify_start_filesize --
 *	Set the file size for the last snapshot.
 */
static int
__verify_start_filesize(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_SNAPSHOT *snapbase, off_t *file_sizep)
{
	WT_BLOCK_SNAPSHOT *si, _si;
	WT_SNAPSHOT *snap;
	off_t file_size;

	si = &_si;

	/*
	 * Find the largest file size referenced by any snapshot -- that should
	 * be the last snapshot taken, but out of sheer, raving paranoia, look
	 * through the list, future changes to snapshots might break this code
	 * if we make that assumption.
	 */
	file_size = 0;
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		WT_RET(__wt_block_buffer_to_snapshot(
		    session, block, snap->raw.data, si));
		if (si->file_size > file_size)
			file_size = si->file_size;
	}

	/* Verify doesn't make any sense if we don't have a snapshot. */
	if (file_size <= WT_BLOCK_DESC_SECTOR)
		WT_RET_MSG(session, WT_ERROR,
		    "%s has no snapshots to verify", block->name);

	/*
	 * The file size should be a multiple of the allocsize, offset by the
	 * size of the descriptor sector, the first 512B of the file.
	 */
	file_size -= WT_BLOCK_DESC_SECTOR;
	if (file_size % block->allocsize != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "the snapshot file size is not a multiple of the "
		    "allocation size");

	*file_sizep = file_size;
	return (0);
}

/*
 * __verify_start_avail --
 *	Get the last snapshot's avail list and load it into the list of file
 * fragments.
 */
static int
__verify_start_avail(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_SNAPSHOT *snapbase)
{
	WT_BLOCK_SNAPSHOT *si, _si;
	WT_EXT *ext;
	WT_EXTLIST *el;
	WT_SNAPSHOT *snap;
	int ret;

	ret = 0;

	/* Get the last on-disk snapshot, if one exists. */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		;
	if (snap == snapbase)
		return (0);
	--snap;

	si = &_si;
	WT_RET(__wt_block_snap_init(session, block, si, 0));
	WT_RET(__wt_block_buffer_to_snapshot(
	    session, block, snap->raw.data, si));
	el = &si->avail;
	if (el->offset == WT_BLOCK_INVALID_OFFSET)
		return (0);

	WT_RET(__wt_block_extlist_read(session, block, el));
	WT_EXT_FOREACH(ext, el->off)
		if ((ret = __verify_filefrag_add(
		    session, block, ext->off, ext->size, 1)) != 0)
			break;

	__wt_block_extlist_free(session, el);
	return (ret);
}

/*
 * __wt_block_verify_end --
 *	End file verification.
 */
int
__wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	int ret;

	/* Confirm we verified every file block. */
	ret = __verify_filefrag_chk(session, block);

	/* Discard the accumulated allocation list. */
	__wt_block_extlist_free(session, &block->verify_alloc);

	/* Discard the fragment tracking lists. */
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
	uint32_t frag, frags;

	/* Set the maximum file size for this snapshot. */
	block->verify_size = si->file_size;

	/*
	 * Add the root page and disk blocks used to store the extent lists to
	 * the list of blocks we've "seen" from the file.
	 */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, si->root_offset, (off_t)si->root_size, 1));
	if (si->alloc.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, si->alloc.offset, (off_t)si->alloc.size, 1));
	if (si->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, si->avail.offset, (off_t)si->avail.size, 1));
	if (si->discard.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__verify_filefrag_add(session,
		    block, si->discard.offset, (off_t)si->discard.size, 1));

	/*
	 * Snapshot verification is similar to deleting snapshots.  As we read
	 * each new snapshot, we merge the allocation lists (accumulating all
	 * allocated pages as we move through the system), and then remove any
	 * pages found in the discard list.   The result should be a one-to-one
	 * mapping to the pages we find in this particular snapshot.
	 */
	el = &si->alloc;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_RET(__wt_block_extlist_merge(
		    session, el, &block->verify_alloc));
		__wt_block_extlist_free(session, el);
	}
	el = &si->discard;
	if (el->offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, el));
		WT_EXT_FOREACH(ext, el->off)
			WT_RET(__wt_block_off_remove(session,
			    &block->verify_alloc, ext->off, ext->size));
		__wt_block_extlist_free(session, el);
	}

	/*
	 * The root page of the snapshot appears on the alloc list, but not, at
	 * least until the snapshot is deleted, on a discard list.   To handle
	 * this case, remove the root page from the accumulated list of snapshot
	 * pages, so it doesn't add a new requirement for subsequent snapshots.
	 */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_off_remove(session,
		    &block->verify_alloc, si->root_offset, si->root_size));

	/*
	 * Allocate the per-snapshot bit map.  The per-snapshot bit map is the
	 * opposite of the per-file bit map, that is, we set all the bits that
	 * we expect to be set based on the snapshot's allocation and discard
	 * lists, then clear bits as we verify blocks.  When finished verifying
	 * the snapshot, the bit list should be empty.
	 */
	WT_RET(__bit_alloc(session, block->frags, &block->fragsnap));
	el = &block->verify_alloc;
	WT_EXT_FOREACH(ext, el->off) {
		frag = (uint32_t)WT_OFF_TO_FRAG(block, ext->off);
		frags = (uint32_t)(ext->size / block->allocsize);
		__bit_nset(block->fragsnap, frag, frag + (frags - 1));
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
	int ret;

	WT_UNUSED(si);

	/* Confirm we verified every snapshot block. */
	ret = __verify_snapfrag_chk(session, block);

	/* Discard the per-snapshot fragment list. */
	__wt_free(session, block->fragsnap);

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
	WT_ITEM *tmp;
	uint32_t frag, frags, i, match;
	int ret;

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
		WT_VERBOSE(session, verify,
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
 *	Update an address in a snapshot as verified.
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
	 * verifying a single snapshot (for example, when verifying the physical
	 * image of a row-store leaf page with overflow keys, the overflow keys
	 * are read when checking for key sort issues, and read again when more
	 * general overflow item checking is done).  This function is called by
	 * the btree verification code, once per logical visit in a snapshot, so
	 * we can detect if a page is referenced multiple times within a single
	 * snapshot.  This doesn't apply to the per-file list, because it is
	 * expected for the same btree blocks to appear in multiple snapshots.
	 *
	 * Add the block to the per-snapshot list.
	 */
	WT_RET(__verify_snapfrag_add(session, block, offset, size));

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

	WT_VERBOSE(session, verify,
	    "adding file block at %" PRIuMAX "-%" PRIuMAX,
	    (uintmax_t)offset, (uintmax_t)(offset + size));

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

/*
 * __verify_snapfrag_add --
 *	Clear the fragments in the per-snapshot fragment list, and complain if
 * we've already verified this chunk of the snapshot.
 */
static int
__verify_snapfrag_add(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, off_t size)
{
	uint32_t f, frag, frags, i;

	WT_VERBOSE(session, verify,
	    "adding snapshot block at %" PRIuMAX "-%" PRIuMAX,
	    (uintmax_t)offset, (uintmax_t)(offset + size));

	/*
	 * Check each chunk against the snapshot's size, a snapshot should never
	 * reference a block outside of the snapshot's stored size.
	 */
	if (offset + size > block->verify_size)
		WT_RET_MSG(session, WT_ERROR,
		    "fragment %" PRIuMAX "-%" PRIuMAX " references "
		    "file blocks outside the snapshot",
		    (uintmax_t)offset, (uintmax_t)(offset + size));

	frag = (uint32_t)WT_OFF_TO_FRAG(block, offset);
	frags = (uint32_t)(size / block->allocsize);

	/* It is illegal to reference a particular chunk more than once. */
	for (f = frag, i = 0; i < frags; ++f, ++i)
		if (!__bit_test(block->fragsnap, f))
			WT_RET_MSG(session, WT_ERROR,
			    "snapshot fragment at %" PRIuMAX " referenced "
			    "multiple times in a single snapshot or found in "
			    "the snapshot but not listed in the snapshot's "
			    "allocation list",
			    (uintmax_t)offset);

	/* Remove fragments from the snapshot's allocation list. */
	__bit_nclr(block->fragsnap, frag, frag + (frags - 1));

	return (0);
}

/*
 * __verify_snapfrag_chk --
 *	Verify we've checked all the fragments in the snapshot.
 */
static int
__verify_snapfrag_chk(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	uint32_t first, last;
	int ret;

	ret = 0;

	/*
	 * Check for snapshot fragments we haven't verified -- every time we
	 * find a bit that's set, complain.  We re-start the search each time
	 * after clearing the set bit(s) we found: it's simpler and this isn't
	 * supposed to happen a lot.
	 */
	for (;;) {
		if (__bit_ffs(block->fragsnap, block->frags, &first) != 0)
			break;
		__bit_clear(block->fragsnap, first);
		for (last = first + 1; last < block->frags; ++last) {
			if (!__bit_test(block->fragsnap, last))
				break;
			__bit_clear(block->fragsnap, last);
		}

		__wt_errx(session,
		    "snapshot range %" PRIuMAX "-%" PRIuMAX " was never "
		    "verified",
		    (uintmax_t)WT_FRAG_TO_OFF(block, first),
		    (uintmax_t)WT_FRAG_TO_OFF(block, last));
		ret = WT_ERROR;
	}
	return (ret);
}
