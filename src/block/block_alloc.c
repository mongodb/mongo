/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __block_extend(WT_SESSION_IMPL *, WT_BLOCK *, off_t *, uint32_t);
static int  __block_truncate(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_alloc --
 *	Alloc a chunk of space from the underlying file.
 */
int
__wt_block_alloc(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t *offsetp, uint32_t size)
{
	WT_FREE_ENTRY *fe, *new;
	int n, ret;

	ret = 0;

	if (size % block->allocsize != 0) {
		__wt_errx(session,
		    "cannot allocate a block size %" PRIu32 " that is not "
		    "a multiple of the allocation size %" PRIu32,
		    size, block->allocsize);
		return (WT_ERROR);
	}

	WT_BSTAT_INCR(session, alloc);

	__wt_spin_lock(session, &block->freelist_lock);

	n = 0;
	TAILQ_FOREACH(fe, &block->freeqa, qa) {
		if (fe->size < size) {
#ifndef HAVE_BLOCK_SMARTS
			/* Limit how many blocks we will examine */
			if (++n > 100)
				break;
#endif
			continue;
		}

		/* Nothing fancy: first fit on the queue. */
		*offsetp = fe->offset;

		/*
		 * If the size is exact, remove it from the linked lists and
		 * free the entry.
		 */
		if (fe->size == size) {
			WT_VERBOSE(session, block,
			    "allocate block %" PRIuMAX "/%" PRIu32,
			    (uintmax_t)fe->offset, size);

			TAILQ_REMOVE(&block->freeqa, fe, qa);
			TAILQ_REMOVE(&block->freeqs, fe, qs);
			--block->freelist_entries;
			__wt_free(session, fe);

			goto done;
		}

		WT_VERBOSE(session, block,
		    "allocate partial block %" PRIuMAX "/%" PRIu32
		    " from %" PRIuMAX "/%" PRIu32,
		    (uintmax_t)fe->offset, size,
		    (uintmax_t)fe->offset, fe->size);

		/*
		 * Otherwise, adjust the entry.   The address remains correctly
		 * sorted, but we have to re-insert at the appropriate location
		 * in the size-sorted queue.
		 */
		fe->offset += size;
		fe->size -= size;
		block->freelist_bytes -= size;
		TAILQ_REMOVE(&block->freeqs, fe, qs);

		new = fe;
#ifdef HAVE_BLOCK_SMARTS
		TAILQ_FOREACH(fe, &block->freeqs, qs) {
			if (new->size > fe->size)
				continue;
			if (new->size < fe->size || new->offset < fe->offset)
				break;
		}
		if (fe == NULL)
			TAILQ_INSERT_TAIL(&block->freeqs, new, qs);
		else
			TAILQ_INSERT_BEFORE(fe, new, qs);
#else
		TAILQ_INSERT_TAIL(&block->freeqs, new, qs);
#endif
		goto done;
	}

	/* No segments large enough found, extend the file. */
	ret = __block_extend(session, block, offsetp, size);

done:	__wt_spin_unlock(session, &block->freelist_lock);
	return (ret);
}

/*
 * __block_extend --
 *	Extend the file to allocate space.
 */
static int
__block_extend(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t *offsetp, uint32_t size)
{
	WT_FH *fh;

	fh = block->fh;

	/* We should never be allocating from an empty file. */
	if (fh->file_size < WT_BLOCK_DESC_SECTOR) {
		__wt_errx(session,
		    "cannot allocate from a file with no description "
		    "information");
		return (WT_ERROR);
	}

	/*
	 * Make sure we don't allocate past the maximum file size.
	 *
	 * XXX
	 * We don't know the maximum off_t on the system; for now, limit growth
	 * to a signed 64-bit value.
	 *
	 * XXX
	 * This isn't sufficient: if we grow the file to the end, there isn't
	 * enough room to write the free-list out when we close the file.  It
	 * is vanishingly unlikely to happen (we use free blocks where they're
	 * available to write the free list), but if the free-list is a bunch
	 * of small blocks, each group of which are insufficient to hole the
	 * free list, and the file has been fully populated, file close will
	 * fail because we can't write the free list.
	 */
	if (fh->file_size > (off_t)(INT64_MAX - size)) {
		__wt_errx(session,
		    "block allocation failed, file cannot grow further");
		return (WT_ERROR);
	}

	*offsetp = fh->file_size;
	fh->file_size += size;

	WT_VERBOSE(session, block,
	    "file extend %" PRIu32 "B @ %" PRIuMAX, size, (uintmax_t)*offsetp);

	WT_BSTAT_INCR(session, extend);
	return (0);
}

/*
 * __wt_block_free_buf --
 *	Free a cookie-referenced chunk of space to the underlying file.
 */
int
__wt_block_free_buf(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, uint32_t addr_size)
{
	off_t offset;
	uint32_t size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &offset, &size, NULL));

	WT_RET(__wt_block_free(session, block, offset, size));

	return (0);
}

/*
 * __wt_block_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_block_free(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t offset, uint32_t size)
{
	WT_FREE_ENTRY *fe, *new;

	new = NULL;

	WT_VERBOSE(session, block,
	    "free %" PRIuMAX "/%" PRIu32, (uintmax_t)offset, size);

	__wt_spin_lock(session, &block->freelist_lock);
	block->freelist_dirty = 1;

	WT_BSTAT_INCR(session, free);
	++block->freelist_entries;
	block->freelist_bytes += size;

	/* Allocate memory for the new entry. */
	WT_RET(__wt_calloc_def(session, 1, &new));
	new->offset = offset;
	new->size = size;

#ifdef HAVE_BLOCK_SMARTS
combine:/*
	 * Insert the entry at the appropriate place in the address list after
	 * checking to see if it adjoins adjacent entries.
	 */
	TAILQ_FOREACH(fe, &block->freeqa, qa) {
		/*
		 * If the freed entry follows (but doesn't immediate follow)
		 * the list entry, continue -- this is a fast test to get us
		 * to the right location in the list.
		 */
		if (new->offset > fe->offset + fe->size)
			continue;

		/*
		 * If the freed entry immediately precedes the list entry, fix
		 * the list entry and we're done -- no further checking needs
		 * to be done.  (We already checked to see if the freed entry
		 * immediately follows the previous list entry, and that's the
		 * only possibility.)
		 */
		if (new->offset + new->size == fe->offset) {
			fe->offset = new->offset;
			fe->size += new->size;
			TAILQ_REMOVE(&block->freeqs, fe, qs);

			--block->freelist_entries;
			__wt_free(session, new);
			new = fe;
			break;
		}

		/*
		 * If the freed entry immediately follows the list entry, fix
		 * the list entry and restart the search (restart the search
		 * because the new, extended entry may immediately precede the
		 * next entry in the list.).
		 */
		if (fe->offset + fe->size == new->offset) {
			fe->size += new->size;
			TAILQ_REMOVE(&block->freeqa, fe, qa);
			TAILQ_REMOVE(&block->freeqs, fe, qs);

			--block->freelist_entries;
			__wt_free(session, new);

			new = fe;
			goto combine;
		}

		/*
		 * The freed entry must appear before the list entry, but does
		 * not adjoin it. Insert the freed entry before the list entry.
		 */
		TAILQ_INSERT_BEFORE(fe, new, qa);
		break;
	}

	/*
	 * If we reached the end of the list, the freed entry comes after any
	 * list entry, append it.
	 */
	if (fe == NULL)
		TAILQ_INSERT_TAIL(&block->freeqa, new, qa);

#ifdef HAVE_DIAGNOSTIC
	/* Check to make sure we haven't inserted overlapping ranges. */
	if (((fe = TAILQ_PREV(new, __wt_free_qah, qa)) != NULL &&
	    fe->offset + fe->size > new->offset) ||
	    ((fe = TAILQ_NEXT(new, qa)) != NULL &&
	    new->offset + new->size > fe->offset)) {
		TAILQ_REMOVE(&block->freeqa, new, qa);
		__wt_spin_unlock(session, &block->freelist_lock);

		__wt_errx(session,
		    "block free at offset range %" PRIuMAX "-%" PRIuMAX
		    " overlaps already free block at offset range "
		    "%" PRIuMAX "-%" PRIuMAX,
		    (uintmax_t)new->offset, (uintmax_t)new->offset + new->size,
		    (uintmax_t)fe->offset, (uintmax_t)fe->offset + fe->size);
		return (WT_ERROR);
	}
#endif

#else
	WT_UNUSED(fe);

	/* No smarts: just insert at the head of the list. */
	TAILQ_INSERT_HEAD(&block->freeqa, new, qa);
#endif

#ifdef HAVE_BLOCK_SMARTS
	/*
	 * The variable new now references a WT_FREE_ENTRY structure not linked
	 * into the size list at all (if it was linked in, we unlinked it while
	 * processing the address list because the size changed).  Insert the
	 * entry into the size list, sorted first by size, and then by address
	 * (the latter so we tend to write pages at the start of the file when
	 * possible).
	 */
	TAILQ_FOREACH(fe, &block->freeqs, qs) {
		if (new->size > fe->size)
			continue;
		if (new->size < fe->size || new->offset < fe->offset)
			break;
	}
	if (fe == NULL)
		TAILQ_INSERT_TAIL(&block->freeqs, new, qs);
	else
		TAILQ_INSERT_BEFORE(fe, new, qs);
#else
	TAILQ_INSERT_HEAD(&block->freeqs, new, qs);
#endif

	__wt_spin_unlock(session, &block->freelist_lock);
	return (0);
}

/*
 * __wt_block_freelist_open --
 *	Initialize the free-list structures.
 */
void
__wt_block_freelist_open(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	__wt_spin_init(session, &block->freelist_lock);

	block->freelist_bytes = 0;
	block->freelist_entries = 0;

	block->free_offset = WT_BLOCK_INVALID_OFFSET;
	block->free_size = block->free_cksum = 0;

	TAILQ_INIT(&block->freeqa);
	TAILQ_INIT(&block->freeqs);
	block->freelist_dirty = 0;
}

/*
 * __wt_block_freelist_read --
 *	Read the free-list.
 */
int
__wt_block_freelist_read(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BUF *tmp;
	off_t offset;
	uint32_t size;
	uint8_t *p;
	int ret;

	tmp = NULL;
	ret = 0;

	/* See if there's a free-list to read. */
	if (block->free_offset == WT_BLOCK_INVALID_OFFSET)
		return (0);

	/*
	 * The free-list is read before the file is verified, which means we
	 * need to be a little paranoid.   We know the free-list chunk itself
	 * is entirely in the file because we checked when we first read the
	 * file's description structure.   Nothing here is unsafe, all we're
	 * doing is entering offset/size pairs into the in-memory free-list.
	 * The verify code will separately check every offset/size pair to make
	 * sure they're in the file.
	 */
	WT_RET(__wt_scr_alloc(session, block->free_size, &tmp));
	WT_ERR(__wt_block_read(session, block,
	    tmp, block->free_offset, block->free_size, block->free_cksum));

	/* Insert the free-list items into the linked list. */
	for (p = WT_PAGE_DISK_BYTE(tmp->mem);;) {
		offset = *(off_t *)p;
		if (offset == 0)
			break;
		p += sizeof(off_t);
		size = *(uint32_t *)p;
		p += sizeof(uint32_t);
		WT_ERR(__wt_block_free(session, block, offset, size));
	}

	/*
	 * Insert the free-list itself into the linked list, but don't clear
	 * the values, if the free-list is never modified, we don't write it.
	 */
	WT_ERR(__wt_block_free(
	    session, block, block->free_offset, block->free_size));

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_freelist_close --
 *	Write the free-list at the tail of the file.
 */
void
__wt_block_freelist_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	__wt_spin_destroy(session, &block->freelist_lock);
}

/*
 * __wt_block_freelist_write --
 *	Write the free-list at the tail of the file.
 */
int
__wt_block_freelist_write(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BUF *tmp;
	WT_FREE_ENTRY *fe;
	WT_PAGE_DISK *dsk;
	off_t offset;
	uint32_t cksum, size;
	uint8_t *p;
	size_t bytes;
	int ret;

	tmp = NULL;
	ret = 0;

	/* If the free-list hasn't changed, there's nothing to write. */
	if (block->freelist_dirty == 0)
		return (0);

	/* If there aren't any free-list entries, we're done. */
	if (block->freelist_entries == 0) {
		block->free_offset = WT_BLOCK_INVALID_OFFSET;
		block->free_size = block->free_cksum = 0;
		return (0);
	}

#ifdef HAVE_VERBOSE
	if (WT_VERBOSE_ISSET(session, block))
		__wt_block_dump(session, block);
#endif

	/* Truncate the file if possible. */
	WT_RET(__block_truncate(session, block));

	/*
	 * Get a scratch buffer, clear the page's header and data, initialize
	 * the header.  Allocate an allocation-sized aligned buffer so the
	 * block write function can zero-out unused bytes and write it without
	 * copying to something larger.
	 *
	 * We allocate room for the free-list entries, plus 1 additional (the
	 * list-terminating WT_ADDR_INVALID/0 pair).
	 */
	bytes =
	    (block->freelist_entries + 1) * (sizeof(off_t) + sizeof(uint32_t));
	WT_RET(__wt_scr_alloc(session, WT_DISK_REQUIRED(block, bytes), &tmp));
	dsk = tmp->mem;
	memset(dsk, 0, WT_PAGE_DISK_SIZE);
	dsk->u.datalen = WT_STORE_SIZE(bytes);
	dsk->type = WT_PAGE_FREELIST;
	tmp->size = WT_STORE_SIZE(WT_PAGE_DISK_SIZE + bytes);

	/*
	 * Fill the page's data.  We output the data in reverse order so we
	 * insert quickly, at least into the address queue, when we read it
	 * back in.
	 */
	p = WT_PAGE_DISK_BYTE(dsk);
	TAILQ_FOREACH_REVERSE(fe, &block->freeqa, __wt_free_qah, qa) {
		*(off_t *)p = fe->offset;
		p += sizeof(off_t);
		*(uint32_t *)p = fe->size;
		p += sizeof(uint32_t);
	}
	*(off_t *)p = 0;		/* The list terminating values. */

	/*
	 * Discard the in-memory free-list: this has to happen before writing
	 * the free-list because the underlying block write function is going
	 * to allocate file space for the free-list block(s), and allocating
	 * from the blocks on the free-list we just wrote won't work out well.
	 * A workaround would be to not compress the free-list, which implies
	 * some kind of "write but don't compress" code path, and that's more
	 * complex than ordering these operations so the eventual allocation
	 * in the write code always extends the file.
	 */
	__wt_block_discard(session, block);

	/* Write the free list to disk. */
	WT_ERR(__wt_block_write(session, block, tmp, &offset, &size, &cksum));

	/* Update the file's meta-data. */
	block->free_offset = offset;
	block->free_size = size;
	block->free_cksum = cksum;

	WT_VERBOSE(session, block,
	    "free-list written %" PRIuMAX "/%" PRIu32, (uintmax_t)offset, size);

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __block_truncate --
 *	Truncate the file if the last part of the file isn't in use.
 */
static int
__block_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FH *fh;
	WT_FREE_ENTRY *fe;
	int need_trunc;

	fh = block->fh;

	/*
	 * Repeatedly check the last element in the free-list, truncating the
	 * file if the last free-list element is also at the end of the file.
	 */
	need_trunc = 0;
	while ((fe = TAILQ_LAST(&block->freeqa, __wt_free_qah)) != NULL) {
		if (fe->offset + fe->size != fh->file_size)
			break;

		WT_VERBOSE(session, block,
		    "truncate free-list %" PRIuMAX "/%" PRIu32,
		    (uintmax_t)fe->offset, fe->size);

		fh->file_size -= fe->size;
		need_trunc = 1;

		TAILQ_REMOVE(&block->freeqa, fe, qa);
		TAILQ_REMOVE(&block->freeqs, fe, qs);

		--block->freelist_entries;
		__wt_free(session, fe);
	}

	if (need_trunc)
		WT_RET(__wt_ftruncate(session, fh, fh->file_size));

	return (0);
}

/*
 * __wt_block_discard --
 *	Discard any free-list entries.
 */
void
__wt_block_discard(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FREE_ENTRY *fe;

	while ((fe = TAILQ_FIRST(&block->freeqa)) != NULL) {
		TAILQ_REMOVE(&block->freeqa, fe, qa);
		TAILQ_REMOVE(&block->freeqs, fe, qs);
		__wt_free(session, fe);
	}
	block->freelist_entries = 0;
}

/*
 * __wt_block_stat --
 *	Free-list statistics.
 */
void
__wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BSTAT_SET(session, file_freelist_bytes, block->freelist_bytes);
	WT_BSTAT_SET(session, file_freelist_entries, block->freelist_entries);
	WT_BSTAT_SET(session, file_size, block->fh->file_size);
}

#ifdef HAVE_VERBOSE
void
__wt_block_dump(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FREE_ENTRY *fe;

	WT_VERBOSE(session, block, "freelist by offset:");
	TAILQ_FOREACH(fe, &block->freeqa, qa)
		WT_VERBOSE(session, block,
		    "\t{%" PRIuMAX "/%" PRIu32 "}",
		    (uintmax_t)fe->offset, fe->size);

	WT_VERBOSE(session, block, "freelist by size:");
	TAILQ_FOREACH(fe, &block->freeqs, qs)
		WT_VERBOSE(session, block,
		    "\t{%" PRIuMAX "/%" PRIu32 "}",
		    (uintmax_t)fe->offset, fe->size);
}
#endif
