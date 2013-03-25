/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_compact_skip --
 *	Return if compaction will shrink the file.
 */
int
__wt_block_compact_skip(
    WT_SESSION_IMPL *session, WT_BLOCK *block, int trigger, int *skipp)
{
	WT_EXT *ext;
	WT_EXTLIST *el;
	WT_FH *fh;
	off_t avail, half;
	int pct;

	fh = block->fh;
	*skipp = 1;

	/*
	 * We do compaction by copying blocks from the end of the file to the
	 * beginning of the file, and we need some metrics to decide if it's
	 * worth doing.  Ignore small files, and files where we are unlikely
	 * to recover the specified percentage of the file.  (The calculation
	 * is if at least N % of the file appears in the available list, and
	 * in the first half of the file.  In other words, don't bother with
	 * compaction unless we have an expectation of moving N % of the file
	 * from the last half of the file to the first half of the file.)
	 */
	if (fh->file_size <= 10 * 1024)
		return (0);

	__wt_spin_lock(session, &block->live_lock);

	avail = 0;
	half = fh->file_size / 2;

	el = &block->live.avail;
	WT_EXT_FOREACH(ext, el->off)
		if (ext->off < half)
			avail += ext->size;
	pct = (int)((avail * 100) / fh->file_size);

	__wt_spin_unlock(session, &block->live_lock);

	if (pct >= trigger)
		*skipp = 0;

	WT_VERBOSE_RET(session, block,
	    "%s: compaction %s, %d%% of the free space in the available "
	    "list appears in the first half of the file",
	    block->name, pct < trigger ? "skipped" : "proceeding", pct);

	return (0);
}

/*
 * __wt_block_compact_page_skip --
 *	Return if writing a particular page will shrink the file.
 */
int
__wt_block_compact_page_skip(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, uint32_t addr_size, int *skipp)
{
	WT_FH *fh;
	off_t offset;
	uint32_t size, cksum;

	WT_UNUSED(addr_size);
	*skipp = 0;			/* Paranoia: skip on error. */

	fh = block->fh;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/*
	 * If this block appears in the last half of the file, rewrite it.
	 *
	 * It's unclear we need to lock: the chances of a smashed read are close
	 * to non-existent and the worst thing that can happen is we rewrite a
	 * block we didn't want to rewrite.   On the other hand, compaction is
	 * not expected to be a common operation in WiredTiger, we shouldn't be
	 * here a lot.
	 */
	__wt_spin_lock(session, &block->live_lock);
	*skipp = offset > fh->file_size / 2 ? 0 : 1;
	__wt_spin_unlock(session, &block->live_lock);

	return (0);
}
