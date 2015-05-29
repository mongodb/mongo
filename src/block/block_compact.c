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

	/* Switch to first-fit allocation. */
	__wt_block_configure_first_fit(block, 1);

	block->compact_pct_tenths = 0;

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

	/* Restore the original allocation plan. */
	__wt_block_configure_first_fit(block, 0);

	block->compact_pct_tenths = 0;

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
	wt_off_t avail_eighty, avail_ninety, eighty, ninety;

	*skipp = 1;				/* Return a default skip. */

	fh = block->fh;

	/*
	 * We do compaction by copying blocks from the end of the file to the
	 * beginning of the file, and we need some metrics to decide if it's
	 * worth doing.  Ignore small files, and files where we are unlikely
	 * to recover 10% of the file.
	 */
	if (fh->size <= WT_MEGABYTE)
		return (0);

	__wt_spin_lock(session, &block->live_lock);

	if (WT_VERBOSE_ISSET(session, WT_VERB_COMPACT))
		WT_ERR(__block_dump_avail(session, block));

	/* Sum the available bytes in the first 80% and 90% of the file. */
	avail_eighty = avail_ninety = 0;
	ninety = fh->size - fh->size / 10;
	eighty = fh->size - ((fh->size / 10) * 2);

	el = &block->live.avail;
	WT_EXT_FOREACH(ext, el->off)
		if (ext->off < ninety) {
			avail_ninety += ext->size;
			if (ext->off < eighty)
				avail_eighty += ext->size;
		}

	WT_ERR(__wt_verbose(session, WT_VERB_COMPACT,
	    "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first "
	    "80%% of the file",
	    block->name,
	    (uintmax_t)avail_eighty / WT_MEGABYTE, (uintmax_t)avail_eighty));
	WT_ERR(__wt_verbose(session, WT_VERB_COMPACT,
	    "%s: %" PRIuMAX "MB (%" PRIuMAX ") available space in the first "
	    "90%% of the file",
	    block->name,
	    (uintmax_t)avail_ninety / WT_MEGABYTE, (uintmax_t)avail_ninety));
	WT_ERR(__wt_verbose(session, WT_VERB_COMPACT,
	    "%s: require 10%% or %" PRIuMAX "MB (%" PRIuMAX ") in the first "
	    "90%% of the file to perform compaction, compaction %s",
	    block->name,
	    (uintmax_t)(fh->size / 10) / WT_MEGABYTE, (uintmax_t)fh->size / 10,
	    *skipp ? "skipped" : "proceeding"));

	/*
	 * Skip files where we can't recover at least 1MB.
	 *
	 * If at least 20% of the total file is available and in the first 80%
	 * of the file, we'll try compaction on the last 20% of the file; else,
	 * if at least 10% of the total file is available and in the first 90%
	 * of the file, we'll try compaction on the last 10% of the file.
	 *
	 * We could push this further, but there's diminishing returns, a mostly
	 * empty file can be processed quickly, so more aggressive compaction is
	 * less useful.
	 */
	if (avail_eighty > WT_MEGABYTE &&
	    avail_eighty >= ((fh->size / 10) * 2)) {
		*skipp = 0;
		block->compact_pct_tenths = 2;
	} else if (avail_ninety > WT_MEGABYTE &&
	    avail_ninety >= fh->size / 10) {
		*skipp = 0;
		block->compact_pct_tenths = 1;
	}

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
	wt_off_t limit, offset;
	uint32_t size, cksum;

	WT_UNUSED(addr_size);
	*skipp = 1;				/* Return a default skip. */

	fh = block->fh;

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, &cksum));

	/*
	 * If this block is in the chosen percentage of the file and there's a
	 * block on the available list that's appears before that percentage of
	 * the file, rewrite the block.  Checking the available list is
	 * necessary (otherwise writing the block would extend the file), but
	 * there's an obvious race if the file is sufficiently busy.
	 */
	__wt_spin_lock(session, &block->live_lock);
	limit = fh->size - ((fh->size / 10) * block->compact_pct_tenths);
	if (offset > limit) {
		el = &block->live.avail;
		WT_EXT_FOREACH(ext, el->off) {
			if (ext->off >= limit)
				break;
			if (ext->size >= size) {
				*skipp = 0;
				break;
			}
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

	WT_RET(__wt_verbose(session, WT_VERB_COMPACT,
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
		WT_RET(__wt_verbose(session, WT_VERB_COMPACT,
		    "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %"
		    PRIuMAX "%%)",
		    i, (uintmax_t)v / WT_MEGABYTE, (uintmax_t)v,
		    (uintmax_t)((v * 100) / (wt_off_t)el->bytes)));
	}
#endif
	for (i = 0; i < WT_ELEMENTS(decile); ++i) {
		v = decile[i] * 512;
		WT_RET(__wt_verbose(session, WT_VERB_COMPACT,
		    "%2u%%: %12" PRIuMAX "MB, (%" PRIuMAX "B, %"
		    PRIuMAX "%%)",
		    i * 10, (uintmax_t)v / WT_MEGABYTE, (uintmax_t)v,
		    (uintmax_t)((v * 100) / (wt_off_t)el->bytes)));
	}

	return (0);
}
