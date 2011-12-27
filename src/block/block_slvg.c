/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_block_salvage_start --
 *	Start a file salvage.
 */
int
__wt_block_salvage_start(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	off_t len;
	uint32_t allocsize;

	btree = session->btree;

	/*
	 * Truncate the file to an initial sector plus N allocation size
	 * units (bytes trailing the last multiple of an allocation size
	 * unit must be garbage, by definition).
	 */
	if (btree->fh->file_size > WT_BTREE_DESC_SECTOR) {
		allocsize = btree->allocsize;
		len = btree->fh->file_size - WT_BTREE_DESC_SECTOR;
		len = (len / allocsize) * allocsize;
		len += WT_BTREE_DESC_SECTOR;
		if (len != btree->fh->file_size)
			WT_RET(__wt_ftruncate(session, btree->fh, len));
	}

	/* Reset the description sector. */
	WT_RET(__wt_desc_write(session, btree->fh));

	/* The first sector of the file is the description record, skip it. */
	btree->slvg_off = WT_BTREE_DESC_SECTOR;

	/*
	 * We don't currently need to do anything about the freelist because
	 * we don't read it for salvage operations.
	 */

	return (0);
}

/*
 * __wt_block_salvage_end --
 *	End a file salvage.
 */
int
__wt_block_salvage_end(WT_SESSION_IMPL *session, int success)
{
	if (!success)
		return (0);

	WT_RET(__wt_block_freelist_write(session));
	return (0);
}

/*
 * __wt_block_salvage_next --
 *	Return the next block from the file.
 */
int
__wt_block_salvage_next(WT_SESSION_IMPL *session,
    WT_BUF *buf, uint8_t *addrbuf, uint32_t *addrbuf_lenp, int *eofp)
{
	WT_BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_FH *fh;
	off_t max, off;
	uint32_t addr, allocsize, cksum, size;
	uint8_t *endp;

	*eofp = 0;

	btree = session->btree;
	off = btree->slvg_off;
	fh = btree->fh;
	allocsize = btree->allocsize;
	WT_RET(__wt_buf_initsize(session, buf, allocsize));

	/* Read through the file, looking for pages with valid checksums. */
	for (max = fh->file_size;;) {
		if (off >= max) {			/* Check eof. */
			*eofp = 1;
			return (0);
		}

		/*
		 * Read the start of a possible page (an allocation-size block),
		 * and get a page length from it.
		 */
		WT_RET(__wt_read(session, fh, off, allocsize, buf->mem));
		dsk = buf->mem;

		/*
		 * The page can't be more than the min/max page size, or past
		 * the end of the file.
		 */
		addr = WT_OFF_TO_ADDR(btree, off);
		size = dsk->size;
		cksum = dsk->cksum;
		if (size == 0 ||
		    size % allocsize != 0 ||
		    size > WT_BTREE_PAGE_SIZE_MAX ||
		    off + (off_t)size > max)
			goto skip;

		/*
		 * The page size isn't insane, read the entire page: reading the
		 * page validates the checksum and then decompresses the page as
		 * needed.  If reading the page fails, it's probably corruption,
		 * we ignore this block.
		 */
		WT_RET(__wt_buf_initsize(session, buf, size));
		if (__wt_block_read(session, buf, addr, size, cksum)) {
skip:			WT_VERBOSE(session, salvage,
			    "skipping %" PRIu32 "B at file offset %" PRIu64,
			    allocsize, (uint64_t)off);

			/*
			 * Free the block and make sure we don't return it more
			 * than once.
			 */
			WT_RET(__wt_block_free(session, addr, allocsize));
			btree->slvg_off = off += allocsize;
			continue;
		}

		/* Valid block, return to our caller. */
		break;
	}

	/* Re-create the address cookie that should reference this block. */
	endp = addrbuf;
	WT_RET(__wt_block_addr_to_buffer(&endp, addr, size, cksum));
	*addrbuf_lenp = WT_PTRDIFF32(endp, addrbuf);

	/* We're successfully returning the page, move past it. */
	btree->slvg_off = off + size;

	return (0);
}
