/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_salvage_start --
 *	Start a file salvage.
 */
int
__wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	off_t len;
	uint32_t allocsize;

	/* Reset the description sector. */
	WT_RET(__wt_desc_init(session, block->fh));

	/*
	 * Salvage creates a new snapshot when it's finished, set up for
	 * rolling an empty file forward.
	 */
	WT_RET(__wt_block_snap_init(session, block, &block->live, 1));

	/*
	 * Truncate the file to an initial sector plus N allocation size
	 * units (bytes trailing the last multiple of an allocation size
	 * unit must be garbage, by definition).
	 */
	if (block->fh->file_size > WT_BLOCK_DESC_SECTOR) {
		allocsize = block->allocsize;
		len = block->fh->file_size - WT_BLOCK_DESC_SECTOR;
		len = (len / allocsize) * allocsize;
		len += WT_BLOCK_DESC_SECTOR;
		if (len != block->fh->file_size)
			WT_RET(__wt_ftruncate(session, block->fh, len));
	} else
		len = WT_BLOCK_DESC_SECTOR;

	/*
	 * The first sector of the file is the description record, skip it as
	 * we read the file.
	 */
	block->slvg_off = WT_BLOCK_DESC_SECTOR;

	/*
	 * The only snapshot extent we care about is the allocation list.  Start
	 * with the entire file on the allocation list, we'll "free" any blocks
	 * we don't want as we process the file.
	 */
	WT_RET(__wt_block_insert_ext(session, &block->live.alloc,
	    WT_BLOCK_DESC_SECTOR, len - WT_BLOCK_DESC_SECTOR));

	block->slvg = 1;
	return (0);
}

/*
 * __wt_block_salvage_end --
 *	End a file salvage.
 */
int
__wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_UNUSED(session);

	block->slvg = 0;

	/* Discard the snapshot. */
	return (__wt_block_snapshot_unload(session, block));
}

/*
 * __wt_block_salvage_next --
 *	Return the next block from the file.
 */
int
__wt_block_salvage_next(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
    uint8_t *addr, uint32_t *addr_sizep, uint64_t *write_genp, int *eofp)
{
	WT_BLOCK_HEADER *blk;
	WT_FH *fh;
	off_t max, offset;
	uint32_t allocsize, cksum, size;
	uint8_t *endp;

	*eofp = 0;

	offset = block->slvg_off;
	fh = block->fh;
	allocsize = block->allocsize;
	WT_RET(__wt_buf_initsize(session, buf, allocsize));

	/* Read through the file, looking for pages with valid checksums. */
	for (max = fh->file_size;;) {
		if (offset >= max) {			/* Check eof. */
			*eofp = 1;
			return (0);
		}

		/*
		 * Read the start of a possible page (an allocation-size block),
		 * and get a page length from it.
		 */
		WT_RET(__wt_read(session, fh, offset, allocsize, buf->mem));
		blk = WT_BLOCK_HEADER_REF(buf->mem);

		/*
		 * The page can't be more than the min/max page size, or past
		 * the end of the file.
		 */
		size = blk->disk_size;
		cksum = blk->cksum;
		if (size == 0 ||
		    size % allocsize != 0 ||
		    size > WT_BTREE_PAGE_SIZE_MAX ||
		    offset + (off_t)size > max)
			goto skip;

		/*
		 * The page size isn't insane, read the entire page: reading the
		 * page validates the checksum and then decompresses the page as
		 * needed.  If reading the page fails, it's probably corruption,
		 * we ignore this block.
		 */
		if (__wt_block_read_off(
		    session, block, buf, offset, size, cksum)) {
skip:			WT_VERBOSE_RET(session, salvage,
			    "skipping %" PRIu32 "B at file offset %" PRIuMAX,
			    allocsize, (uintmax_t)offset);

			/*
			 * Free the block and make sure we don't return it more
			 * than once.
			 */
			WT_RET(__wt_block_off_free(
			    session, block, offset, (off_t)allocsize));
			block->slvg_off = offset += allocsize;
			continue;
		}

		/*
		 * Valid block, return to our caller.
		 *
		 * The buffer may have grown: make sure we read from the full
		 * page image.
		 */
		blk = WT_BLOCK_HEADER_REF(buf->mem);
		break;
	}

	/*
	 * Track the largest write-generation we've seen in the file so future
	 * writes, done after salvage completes, are preferred to these blocks.
	 */
	*write_genp = blk->write_gen;
	if (block->live.write_gen < blk->write_gen)
		block->live.write_gen = blk->write_gen;

	/* Re-create the address cookie that should reference this block. */
	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(block, &endp, offset, size, cksum));
	*addr_sizep = WT_PTRDIFF32(endp, addr);

	/* We're successfully returning the page, move past it. */
	block->slvg_off = offset + size;

	return (0);
}
