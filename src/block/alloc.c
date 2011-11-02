/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __block_discard(WT_SESSION_IMPL *);
static int  __block_extend(WT_SESSION_IMPL *, uint32_t *, uint32_t);
static int  __block_truncate(WT_SESSION_IMPL *);

/*
 * __wt_block_alloc --
 *	Alloc a chunk of space from the underlying file.
 */
int
__wt_block_alloc(WT_SESSION_IMPL *session, uint32_t *addrp, uint32_t size)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe, *new;

	btree = session->btree;

	if (size % btree->allocsize != 0) {
		__wt_errx(session,
		    "cannot allocate a block size %" PRIu32 " that is not "
		    "a multiple of the allocation size %" PRIu32,
		    size, btree->allocsize);
		return (WT_ERROR);
	}

	WT_BSTAT_INCR(session, alloc);

	TAILQ_FOREACH(fe, &btree->freeqa, qa) {
		if (fe->size < size)
			continue;

		/* Nothing fancy: first fit on the queue. */
		*addrp = fe->addr;

		/*
		 * If the size is exact, remove it from the linked lists and
		 * free the entry.
		 */
		if (fe->size == size) {
			TAILQ_REMOVE(&btree->freeqa, fe, qa);
			TAILQ_REMOVE(&btree->freeqs, fe, qs);
			--btree->freelist_entries;
			__wt_free(session, fe);

			WT_VERBOSE(session, ALLOCATE,
			    "allocate: block %" PRIu32 "/%" PRIu32,
			    *addrp, size);
			return (0);
		}

		WT_VERBOSE(session, ALLOCATE,
		    "allocate: partial block %" PRIu32 "/%" PRIu32
		    " from %" PRIu32 "/%" PRIu32,
		    *addrp, size, fe->addr, fe->size);

		/*
		 * Otherwise, adjust the entry.   The address remains correctly
		 * sorted, but we have to re-insert at the appropriate location
		 * in the size-sorted queue.
		 */
		fe->addr += size / btree->allocsize;
		fe->size -= size;
		btree->freelist_bytes -= size;
		TAILQ_REMOVE(&btree->freeqs, fe, qs);

		new = fe;
		TAILQ_FOREACH(fe, &btree->freeqs, qs) {
			if (new->size > fe->size)
				continue;
			if (new->size < fe->size || new->addr < fe->addr)
				break;
		}
		if (fe == NULL)
			TAILQ_INSERT_TAIL(&btree->freeqs, new, qs);
		else
			TAILQ_INSERT_BEFORE(fe, new, qs);

		return (0);
	}

	/* No segments large enough found, extend the file. */
	return (__block_extend(session, addrp, size));
}

/*
 * __block_extend --
 *	Extend the file to allocate space.
 */
static int
__block_extend(WT_SESSION_IMPL *session, uint32_t *addrp, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;

	btree = session->btree;
	fh = btree->fh;

	/* We should never be allocating from an empty file. */
	if (fh->file_size < WT_BTREE_DESC_SECTOR) {
		__wt_errx(session,
		    "cannot allocate from a file with no description "
		    "information");
		return (WT_ERROR);
	}

	/*
	 * Make sure we don't allocate past the maximum file size.
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
	if (fh->file_size > WT_FILE_OFF_MAX(btree)) {
		__wt_errx(session,
		    "block allocation failed, file cannot grow further");
		return (WT_ERROR);
	}

	*addrp = WT_OFF_TO_ADDR(btree, fh->file_size);
	fh->file_size += size;

	WT_VERBOSE(session, ALLOCATE,
	    "allocate: file extend %" PRIu32 "/%" PRIu32, *addrp, size);

	WT_BSTAT_INCR(session, extend);
	return (0);
}

/*
 * __wt_block_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_block_free(WT_SESSION_IMPL *session, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe, *new;

	btree = session->btree;
	new = NULL;

	if (addr == WT_ADDR_INVALID) {
		__wt_errx(session,
		    "attempt to free an invalid file address");
		return (WT_ERROR);
	}
	if (size % btree->allocsize != 0) {
		__wt_errx(session,
		    "cannot free a block size %" PRIu32 " that is not a "
		    "multiple of the allocation size %" PRIu32,
		    size, btree->allocsize);
		return (WT_ERROR);
	}

	WT_VERBOSE(session, ALLOCATE,
	    "allocate: free %" PRIu32 "/%" PRIu32, addr, size);

	btree->freelist_dirty = 1;

	WT_BSTAT_INCR(session, free);
	++btree->freelist_entries;
	btree->freelist_bytes += size;

	/* Allocate memory for the new entry. */
	WT_RET(__wt_calloc_def(session, 1, &new));
	new->addr = addr;
	new->size = size;

combine:/*
	 * Insert the entry at the appropriate place in the address list after
	 * checking to see if it adjoins adjacent entries.
	 */
	TAILQ_FOREACH(fe, &btree->freeqa, qa) {
		/*
		 * If the freed entry follows (but doesn't immediate follow)
		 * the list entry, continue -- this is a fast test to get us
		 * to the right location in the list.
		 */
		if (new->addr > fe->addr + (fe->size / btree->allocsize))
			continue;

		/*
		 * If the freed entry immediately precedes the list entry, fix
		 * the list entry and we're done -- no further checking needs
		 * to be done.  (We already checked to see if the freed entry
		 * immediately follows the previous list entry, and that's the
		 * only possibility.)
		 */
		if (new->addr + (new->size / btree->allocsize) == fe->addr) {
			fe->addr = new->addr;
			fe->size += new->size;
			TAILQ_REMOVE(&btree->freeqs, fe, qs);

			--btree->freelist_entries;
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
		if (fe->addr + (fe->size / btree->allocsize) == new->addr) {
			fe->size += new->size;
			TAILQ_REMOVE(&btree->freeqa, fe, qa);
			TAILQ_REMOVE(&btree->freeqs, fe, qs);

			--btree->freelist_entries;
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
		TAILQ_INSERT_TAIL(&btree->freeqa, new, qa);

#ifdef HAVE_DIAGNOSTIC
	/* Check to make sure we haven't inserted overlapping ranges. */
	if (((fe = TAILQ_PREV(new, __wt_free_qah, qa)) != NULL &&
	    fe->addr + (fe->size / btree->allocsize) > new->addr) ||
	    ((fe = TAILQ_NEXT(new, qa)) != NULL &&
	    new->addr + (new->size / btree->allocsize) > fe->addr)) {
		TAILQ_REMOVE(&btree->freeqa, new, qa);

		__wt_errx(session,
		    "block free at addr range %" PRIu32 "-%" PRIu32
		    " overlaps already free block at addr range "
		    "%" PRIu32 "-%" PRIu32,
		    new->addr, new->addr + (new->size / btree->allocsize),
		    fe->addr, fe->addr + (fe->size / btree->allocsize));
		return (WT_ERROR);
	}
#endif

	/*
	 * The variable new now references a WT_FREE_ENTRY structure not linked
	 * into the size list at all (if it was linked in, we unlinked it while
	 * processing the address list because the size changed).  Insert the
	 * entry into the size list, sorted first by size, and then by address
	 * (the latter so we tend to write pages at the start of the file when
	 * possible).
	 */
	TAILQ_FOREACH(fe, &btree->freeqs, qs) {
		if (new->size > fe->size)
			continue;
		if (new->size < fe->size || new->addr < fe->addr)
			break;
	}
	if (fe == NULL)
		TAILQ_INSERT_TAIL(&btree->freeqs, new, qs);
	else
		TAILQ_INSERT_BEFORE(fe, new, qs);

	return (0);
}

/*
 * __wt_block_freelist_read --
 *	Read the free-list at the tail of the file.
 */
int
__wt_block_freelist_read(WT_SESSION_IMPL *session)
{
	WT_BUF *tmp;
	WT_BTREE *btree;
	uint32_t *p;
	int ret;

	tmp = NULL;
	btree = session->btree;
	ret = 0;

	/*
	 * The free-list is read before the file is verified, which means we
	 * need to be a little paranoid.   We know the free-list chunk itself
	 * is entirely in the file because we checked when we first read the
	 * file's description structure.   Nothing here is unsafe, all we're
	 * doing is entering addr/size pairs into the in-memory free-list.
	 * The verify code will separately check every addr/size pair to make
	 * sure they're in the file.
	 *
	 * Make sure there's a free-list to read.
	 */
	if (btree->free_addr == WT_ADDR_INVALID)
		return (0);

	WT_RET(__wt_scr_alloc(session, btree->free_size, &tmp));
	WT_ERR(__wt_block_read(
	    session, tmp, btree->free_addr, btree->free_size, 0));

	/* Insert the free-list items into the linked list. */
	for (p = (uint32_t *)WT_PAGE_DISK_BYTE(tmp->mem);
	    *p != WT_ADDR_INVALID; p += 2)
		WT_ERR(__wt_block_free(session, p[0], p[1]));

	/*
	 * Insert the free-list itself into the linked list, but don't clear
	 * the values, if the free-list is never modified, we don't write it.
	 */
	WT_ERR(__wt_block_free(session, btree->free_addr, btree->free_size));

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_freelist_write --
 *	Write the free-list at the tail of the file.
 */
int
__wt_block_freelist_write(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_FREE_ENTRY *fe;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size, *p;
	size_t bytes;
	int ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	addr = WT_ADDR_INVALID;
	size = 0;

	/* If the free-list hasn't changed, there's nothing to write. */
	if (btree->freelist_dirty == 0)
		return (0);

	/* If there aren't any free-list entries, we're done. */
	if (btree->freelist_entries == 0)
		goto done;

#ifdef HAVE_VERBOSE
	if (WT_VERBOSE_ISSET(session, ALLOCATE))
		__wt_block_dump(session);
#endif

	/* Truncate the file if possible. */
	WT_RET(__block_truncate(session));

	/*
	 * Get a scratch buffer, clear the page's header and data, initialize
	 * the header.  Allocate an allocation-sized aligned buffer so the
	 * block write function can zero-out unused bytes and write it without
	 * copying to something larger.
	 *
	 * We allocate room for the free-list entries, plus 1 additional (the
	 * list-terminating WT_ADDR_INVALID/0 pair).
	 */
	bytes = (btree->freelist_entries + 1) * 2 * sizeof(uint32_t);
	WT_RET(__wt_scr_alloc(session, WT_DISK_REQUIRED(session, bytes), &tmp));
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
	TAILQ_FOREACH_REVERSE(fe, &btree->freeqa, __wt_free_qah, qa) {
		*p++ = fe->addr;
		*p++ = fe->size;
	}
	*p++ = WT_ADDR_INVALID;		/* The list terminating values. */
	*p = 0;

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
	__block_discard(session);

	/* Write the free list to disk. */
	WT_ERR(__wt_block_write(session, tmp, &addr, &size));

done:	/* Update the file's meta-data. */
	btree->free_addr = addr;
	btree->free_size = size;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __block_truncate --
 *	Truncate the file if the last part of the file isn't in use.
 */
static int
__block_truncate(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_FH *fh;
	WT_FREE_ENTRY *fe;
	int need_trunc;

	btree = session->btree;
	fh = btree->fh;

	/*
	 * Repeatedly check the last element in the free-list, truncating the
	 * file if the last free-list element is also at the end of the file.
	 */
	need_trunc = 0;
	while ((fe = TAILQ_LAST(&btree->freeqa, __wt_free_qah)) != NULL) {
		if (WT_ADDR_TO_OFF(btree, fe->addr) + (off_t)fe->size !=
		    fh->file_size)
			break;

		WT_VERBOSE(session, ALLOCATE,
		    "allocate: truncate free-list %" PRIu32 "/%" PRIu32,
		    fe->addr, fe->size);

		fh->file_size -= fe->size;
		need_trunc = 1;

		TAILQ_REMOVE(&btree->freeqa, fe, qa);
		TAILQ_REMOVE(&btree->freeqs, fe, qs);

		--btree->freelist_entries;
		__wt_free(session, fe);
	}

	if (need_trunc)
		WT_RET(__wt_ftruncate(session, fh, fh->file_size));

	return (0);
}

/*
 * __block_discard --
 *	Discard any free-list entries.
 */
static void
__block_discard(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe;

	btree = session->btree;

	while ((fe = TAILQ_FIRST(&btree->freeqa)) != NULL) {
		TAILQ_REMOVE(&btree->freeqa, fe, qa);
		TAILQ_REMOVE(&btree->freeqs, fe, qs);

		--btree->freelist_entries;
		__wt_free(session, fe);
	}
}

/*
 * __wt_block_stat --
 *	Free-list statistics.
 */
void
__wt_block_stat(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_BSTAT_SET(session, file_freelist_bytes, btree->freelist_bytes);
	WT_BSTAT_SET(session, file_freelist_entries, btree->freelist_entries);
}

#ifdef HAVE_VERBOSE
void
__wt_block_dump(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe;

	btree = session->btree;

	WT_VERBOSE(session, ALLOCATE, "allocate: freelist by addr:");
	TAILQ_FOREACH(fe, &btree->freeqa, qa)
		WT_VERBOSE(session, ALLOCATE,
		    "\t{%" PRIu32 "/%" PRIu32 "}", fe->addr, fe->size);

	WT_VERBOSE(session, ALLOCATE, "allocate: freelist by size:");
	TAILQ_FOREACH(fe, &btree->freeqs, qs)
		WT_VERBOSE(session, ALLOCATE,
		    "\t{%" PRIu32 "/%" PRIu32 "}", fe->addr, fe->size);
}
#endif
