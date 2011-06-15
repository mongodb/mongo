/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_block_discard(WT_SESSION_IMPL *);
static void __wt_block_extend(WT_SESSION_IMPL *, uint32_t *, uint32_t);
static int  __wt_block_truncate(WT_SESSION_IMPL *);

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

	WT_ASSERT(session, size % btree->allocsize == 0);

	WT_STAT_INCR(btree->stats, alloc);

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
			return (0);
		}

		/*
		 * Otherwise, adjust the entry.   The address remains correctly
		 * sorted, but we have to re-insert at the appropriate location
		 * in the size-sorted queue.
		 */
		fe->addr += size / btree->allocsize;
		fe->size -= size;
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
	__wt_block_extend(session, addrp, size);

	return (0);
}

/*
 * __wt_block_extend --
 *	Extend the file to allocate space.
 */
static void
__wt_block_extend(WT_SESSION_IMPL *session, uint32_t *addrp, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;

	btree = session->btree;
	fh = btree->fh;

	/* We should never be allocating from an empty file. */
	WT_ASSERT(session, fh->file_size >= WT_BTREE_DESC_SECTOR);

	*addrp = WT_OFF_TO_ADDR(btree, fh->file_size);
	fh->file_size += size;

	WT_STAT_INCR(btree->stats, extend);
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

	WT_ASSERT(session, addr != WT_ADDR_INVALID);
	WT_ASSERT(session, size % btree->allocsize == 0);

	btree->freelist_dirty = 1;

	WT_STAT_INCR(btree->stats, free);
	++btree->freelist_entries;

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
		__wt_errx(session, "block free at addr range "
		    "%" PRIu32 "-%" PRIu32
		    " overlaps already free block at addr range "
		    "%" PRIu32 "-%" PRIu32,
		    new->addr, new->addr + (new->size / btree->allocsize),
		    fe->addr, fe->addr + (fe->size / btree->allocsize));
		TAILQ_REMOVE(&btree->freeqa, new, qa);
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
 * __wt_block_read --
 *	Read the free-list at the tail of the file.
 */
int
__wt_block_read(WT_SESSION_IMPL *session)
{
	WT_BUF *tmp;
	WT_BTREE *btree;
	uint32_t *p;
	int ret;

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

	/* Get a scratch buffer and make it look like our work page. */
	WT_RET(__wt_scr_alloc(session, btree->free_size, &tmp));

	/* Read in the free-list. */
	WT_ERR(__wt_disk_read(session,
	    tmp->mem, btree->free_addr, btree->free_size));

	/* Insert the free-list items into the linked list. */
	for (p = (uint32_t *)WT_PAGE_DISK_BYTE(tmp->mem);
	    *p != WT_ADDR_INVALID; p += 2)
		WT_ERR(__wt_block_free(session, p[0], p[1]));

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_block_write --
 *	Write the free-list at the tail of the file.
 */
int
__wt_block_write(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_FREE_ENTRY *fe;
	WT_PAGE_DISK *dsk;
	uint32_t addr, size, *p, total_entries;
	int ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	/* If the free-list hasn't changed, there's nothing to write. */
	if (btree->freelist_dirty == 0)
		return (0);

	/* If there aren't any free-list entries, we're done. */
	if (btree->freelist_entries == 0) {
		addr = WT_ADDR_INVALID;
		size = 0;
		goto done;
	}

	/* Truncate the file if possible. */
	WT_RET(__wt_block_truncate(session));

	/*
	 * We allocate room for all of the free-list entries, plus 2 more.  The
	 * first additional entry is for the free-list pages themselves, and the
	 * second is for a list-terminating WT_ADDR_INVALID entry.
	 */
	total_entries = btree->freelist_entries + 2;
	size = WT_DISK_REQUIRED(session, total_entries * 2 * sizeof(uint32_t));

	/* Allocate room at the end of the file. */
	__wt_block_extend(session, &addr, size);

	/* Get a scratch buffer and make it look like our work page. */
	WT_RET(__wt_scr_alloc(session, size, &tmp));

	/* Clear the page's header and data, initialize the header. */
	dsk = tmp->mem;
	memset(dsk, 0, size);
	dsk->u.datalen = total_entries * 2 * WT_SIZEOF32(uint32_t);
	dsk->type = WT_PAGE_FREELIST;

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
	*p++ = addr;			/* The free-list chunk itself. */
	*p++ = size;
	*p++ = WT_ADDR_INVALID;		/* The list terminating values. */
	*p = 0;

	/* Write the free list to disk. */
	WT_ERR(__wt_disk_write(session, dsk, addr, size));

done:	/* Update the file's meta-data. */
	btree->free_addr = addr;
	btree->free_size = size;
	WT_ERR(__wt_desc_update(session));

	/* Discard the in-memory free-list. */
	__wt_block_discard(session);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_block_truncate --
 *	Truncate the file if the last part of the file isn't in use.
 */
static int
__wt_block_truncate(WT_SESSION_IMPL *session)
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
 * __wt_block_discard --
 *	Discard any free-list entries.
 */
static void
__wt_block_discard(WT_SESSION_IMPL *session)
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

#ifdef HAVE_DIAGNOSTIC
void
__wt_block_dump(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_FREE_ENTRY *fe;

	btree = session->btree;

	fprintf(stderr, "Freelist by addr:");
	TAILQ_FOREACH(fe, &btree->freeqa, qa)
		fprintf(stderr, " {%" PRIu32 "/%" PRIu32 "}",
		    fe->addr, fe->size);
	fprintf(stderr, "\n");
	fprintf(stderr, "Freelist by size:");
	TAILQ_FOREACH(fe, &btree->freeqs, qs)
		fprintf(stderr, " {%" PRIu32 "/%" PRIu32 "}",
		    fe->addr, fe->size);
	fprintf(stderr, "\n");
}
#endif
