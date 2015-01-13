/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __block_dump_avail(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_compact_start --
 *	Start compaction of a file.
 */
int
__wt_block_compact_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_UNUSED(session);

	/*
	 * Save the current allocation plan, switch to first-fit allocation.
	 * We don't need the lock, but it's not a performance question and
	 * might avoid bugs in the future.
	 */
	__wt_spin_lock(session, &block->live_lock);
	block->allocfirst_save = block->allocfirst;
	block->allocfirst = 1;
	__wt_spin_unlock(session, &block->live_lock);

	return (0);
}

/*
 * __wt_block_compact_end --
 *	End compaction of a file.
 */
int
__wt_block_compact_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_UNUSED(session);

	/*
	 * Restore the previous allocation plan.
	 * We don't need the lock, but it's not a performance question and
	 * might avoid bugs in the future.
	 */
	__wt_spin_lock(session, &block->live_lock);
	block->allocfirst = block->allocfirst_save;
	__wt_spin_unlock(session, &block->live_lock);

	return (0);
}

/*
 * __wt_block_compact_skip --
 *	Return if compaction will shrink the file.
 */
int
__wt_block_compact_skip(WT_SESSION_IMPL *session, WT_BLOCK *block, int *skipp)
{
	WT_DECL_RET;
	WT_EXT *ext;
	WT_EXTLIST *el;
	WT_FH *fh;
	wt_off_t avail, ninety;

	*skipp = 1;				/* Return a default skip. */

	fh = block->fh;

	/*
	 * We do compaction by copying blocks from the end of the file to the
	 * beginning of the file, and we need some metrics to decide if it's
	 * worth doing.  Ignore small files, and files where we are unlikely
	 * to recover 10% of the file.
	 */
	if (fh->size <= 10 * 1024)
		return (0);

	__wt_spin_lock(session, &block->live_lock);

	if (WT_VERBOSE_ISSET(session, WT_VERB_COMPACT))
		WT_ERR(__block_dump_avail(session, block));

	/* Sum the number of available bytes in the first 90% of the file. */
	avail = 0;
	ninety = fh->size - fh->size / 10;

	el = &block->live.avail;
	WT_EXT_FOREACH(ext, el->off)
		if (ext->off < ninety)
			avail += ext->size;

	/*
	 * If at least 10% of the total file is available and in the first 90%
	 * of the file, we'll try compaction.
	 */
	if (avail >= fh->size / 10)
		*skipp = 0;

	WT_ERR(__wt_verbose(session, WT_VERB_COMPACT,
	    "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first "
	    "90%% of the file, require 10%% or %" PRIuMAX "MB (%" PRIuMAX
	    ") to perform compaction, compaction %s",
	    block->name,
	    (uintmax_t)avail / WT_MEGABYTE, (uintmax_t)avail,
	    (uintmax_t)(fh->size / 10) / WT_MEGABYTE, (uintmax_t)fh->size / 10,
	    *skipp ? "skipped" : "proceeding"));

err:	__wt_spin_unlock(session, &block->live_lock);

	return (ret);
}

/*
 * __wt_block_compact_page_skip --
 *	Return if writing a particular page will shrink the file.
 */
int
__wt_block_compact_page_skip(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, size_t addr_size, int *skipp)
{
	WT_DECL_RET;
	WT_EXT *ext;
	WT_EXTLIST *el;
	WT_FH *fh;
	wt_off_t ninety, offset;
	uint32_t size, cksum;

	WT_UNUSED(addr_size);
	*skipp = 1;				/* Return a default skip. */

	fh = block->fh;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	__wt_spin_lock(session, &block->live_lock);

	/*
	 * If this block is in the last 10% of the file and there's a block on
	 * the available list that's in the first 90% of the file, rewrite the
	 * block.  Checking the available list is necessary (otherwise writing
	 * the block would extend the file), but there's an obvious race if the
	 * file is sufficiently busy.
	 */
	ninety = fh->size - fh->size / 10;
	if (offset > ninety) {
		el = &block->live.avail;
		WT_EXT_FOREACH(ext, el->off)
			if (ext->off < ninety && ext->size >= size) {
				*skipp = 0;
				break;
			}
	}

	__wt_spin_unlock(session, &block->live_lock);

	return (ret);
}

/*
 * __block_dump_avail --
 *	Dump out the avail list so we can see what compaction will look like.
 */
static int
__block_dump_avail(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_EXTLIST *el;
	WT_EXT *ext;
	wt_off_t decile[10], percentile[100], size, v;
	u_int i;

	el = &block->live.avail;
	size = block->fh->size;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
	    "file size %" PRIuMAX "MB (%" PRIuMAX ") with %" PRIuMAX
	    "%% space available %" PRIuMAX "MB (%" PRIuMAX ")",
	    (uintmax_t)size / WT_MEGABYTE, (uintmax_t)size,
	    ((uintmax_t)el->bytes * 100) / (uintmax_t)size,
	    (uintmax_t)el->bytes / WT_MEGABYTE, (uintmax_t)el->bytes));

	if (el->entries == 0)
		return (0);

	/*
	 * Bucket the available memory into file deciles/percentiles.  Large
	 * pieces of memory will cross over multiple buckets, assign to the
	 * decile/percentile in 512B chunks.
	 */
	memset(decile, 0, sizeof(decile));
	memset(percentile, 0, sizeof(percentile));
	WT_EXT_FOREACH(ext, el->off)
		for (i = 0; i < ext->size / 512; ++i) {
			++decile[((ext->off + i * 512) * 10) / size];
			++percentile[((ext->off + i * 512) * 100) / size];
		}

#ifdef __VERBOSE_OUTPUT_PERCENTILE
	for (i = 0; i < WT_ELEMENTS(percentile); ++i) {
		v = percentile[i] * 512;
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
		    "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %"
		    PRIuMAX "%%)",
		    i, (uintmax_t)v / WT_MEGABYTE, (uintmax_t)v,
		    (uintmax_t)((v * 100) / (wt_off_t)el->bytes)));
	}
#endif
	for (i = 0; i < WT_ELEMENTS(decile); ++i) {
		v = decile[i] * 512;
		WT_RET(__wt_verbose(session, WT_VERB_BLOCK,
		    "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %"
		    PRIuMAX "%%)",
		    i * 10, (uintmax_t)v / WT_MEGABYTE, (uintmax_t)v,
		    (uintmax_t)((v * 100) / (wt_off_t)el->bytes)));
	}

	return (0);
}
