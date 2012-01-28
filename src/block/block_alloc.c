/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __block_extend(WT_SESSION_IMPL *, WT_BLOCK *, off_t *, uint32_t);
static int  __block_truncate(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __block_off_srch --
 *	Search a by-offset skiplist (either the primary one, or the per-size
 * bucket offset list).
 */
static void
__block_off_srch(WT_FREE **head, off_t off, WT_FREE ***stack, int skip_off)
{
	WT_FREE **fep;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, fep = &head[i]; i >= 0;) {
		if (*fep == NULL) {
			stack[i--] = fep--;
			continue;
		}

		/* Set the stack for an exact match or the next-largest item. */
		if ((*fep)->off < off)		/* Keep going at this level */
			fep = &(*fep)->next[i + (skip_off ? (*fep)->depth : 0)];
		else				/* Drop down a level */
			stack[i--] = fep--;
	}
}

/*
 * __block_off_pair_srch --
 *	Search the primary by-offset skiplist for before/after records of the
 * specified offset.
 */
static void
__block_off_pair_srch(
    WT_FREE **head, off_t off, WT_FREE **beforep, WT_FREE **afterp)
{
	WT_FREE **fep;
	int i;

	*beforep = *afterp = NULL;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, fep = &head[i]; i >= 0;) {
		if (*fep == NULL) {
			--i;
			--fep;
			continue;
		}

		if ((*fep)->off < off) {	/* Keep going at this level */
			*beforep = *fep;
			fep = &(*fep)->next[i];
		} else {			/* Drop down a level */
			*afterp = *fep;
			--i;
			--fep;
		}
	}
}

/*
 * __block_off_last --
 *	Return the last free range in the offset skiplist.
 */
static WT_FREE *
__block_off_last(WT_FREE **head)
{
	WT_FREE *fe, **fep;
	int i;

	fe = NULL;

	for (i = WT_SKIP_MAXDEPTH - 1, fep = &head[i]; i >= 0;) {
		if (*fep == NULL) {
			--i;
			--fep;
			continue;
		}
		fe = *fep;
		fep = &(*fep)->next[i];
	}
	return (fe);
}

/*
 * __block_size_srch --
 *	Search the by-size skiplist.
 */
static void
__block_size_srch(WT_SIZE **head, uint32_t size, WT_SIZE ***stack)
{
	WT_SIZE **szp;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, szp = &head[i]; i >= 0;) {
		if (*szp == NULL) {
			stack[i--] = szp--;
			continue;
		}

		/* Set the stack for an exact match or the next-largest item. */
		if ((*szp)->size < size)	/* Keep going at this level */
			szp = &(*szp)->next[i];
		else				/* Drop down a level */
			stack[i--] = szp--;
	}
}

/*
 * __block_off_insert --
 *	Insert a record into the system.
 */
static int
__block_off_insert(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_FREE *fe)
{
	WT_FREE **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	/*
	 * If we are inserting a new size onto the size skiplist, we'll need
	 * a new WT_FREE structure for that skiplist.
	 */
	__block_size_srch(block->fsize, fe->size, sstack);
	szp = *sstack[0];
	if (szp == NULL || szp->size != fe->size) {
		WT_RET(__wt_calloc(session, 1,
		    sizeof(WT_SIZE) + fe->depth * sizeof(WT_SIZE *), &szp));
		szp->size = fe->size;
		szp->depth = fe->depth;
		for (i = 0; i < fe->depth; ++i) {
			szp->next[i] = *sstack[i];
			*sstack[i] = szp;
		}
	}

	/* Insert the new WT_FREE structure into the offset skiplist. */
	__block_off_srch(block->foff, fe->off, astack, 0);
	for (i = 0; i < fe->depth; ++i) {
		fe->next[i] = *astack[i];
		*astack[i] = fe;
	}

	/*
	 * Insert the new WT_FREE structure into the size element's offset
	 * skiplist.
	 */
	__block_off_srch(szp->foff, fe->off, astack, 1);
	for (i = 0; i < fe->depth; ++i) {
		fe->next[i + fe->depth] = *astack[i];
		*astack[i] = fe;
	}

	++block->freelist_entries;
	block->freelist_bytes += fe->size;
	block->freelist_dirty = 1;

	return (0);
}

/*
 * __block_off_remove --
 *	Remove a record from the system.
 */
static int
__block_off_remove(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t off, WT_FREE **fep)
{
	WT_FREE *fe, **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	/* Find and remove the record from the by-offset skiplist. */
	__block_off_srch(block->foff, off, astack, 0);
	fe = *astack[0];
	if (fe == NULL || fe->off != off)
		return (EINVAL);
	for (i = 0; i < fe->depth; ++i)
		*astack[i] = fe->next[i];

	/*
	 * Find and remove the record from the size's offset skiplist; if that
	 * empties the by-size skiplist entry, remove it as well.
	 */
	__block_size_srch(block->fsize, fe->size, sstack);
	szp = *sstack[0];
	if (szp == NULL || szp->size != fe->size)
		return (EINVAL);
	__block_off_srch(szp->foff, off, astack, 1);
	fe = *astack[0];
	if (fe == NULL || fe->off != off)
		return (EINVAL);
	for (i = 0; i < fe->depth; ++i)
		*astack[i] = fe->next[i + fe->depth];
	if (szp->foff[0] == NULL) {
		for (i = 0; i < szp->depth; ++i)
			*sstack[i] = szp->next[i];
		__wt_free(session, szp);
	}

	--block->freelist_entries;
	block->freelist_bytes -= fe->size;
	block->freelist_dirty = 1;

	/* Return the record if our caller wants it, otherwise free it. */
	if (fep == NULL)
		__wt_free(session, fe);
	else
		*fep = fe;

	return (0);
}

/*
 * __wt_block_alloc --
 *	Alloc a chunk of space from the underlying file.
 */
int
__wt_block_alloc(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t *offsetp, uint32_t size)
{
	WT_FREE *fe;
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	int ret;

	ret = 0;

	WT_BSTAT_INCR(session, alloc);
	if (size % block->allocsize != 0)
		WT_RET_MSG(session, EINVAL,
		    "cannot allocate a block size %" PRIu32 " that is not "
		    "a multiple of the allocation size %" PRIu32,
		    size, block->allocsize);

	__wt_spin_lock(session, &block->freelist_lock);

	/*
	 * Allocation is first-fit by size: search the by-size skiplist for the
	 * requested size and take the first entry on the by-size offset list.
	 * If we don't have anything large enough, extend the file.
	 */
	__block_size_srch(block->fsize, size, sstack);
	szp = *sstack[0];
	if (szp == NULL) {
		WT_ERR(__block_extend(session, block, offsetp, size));
		goto done;
	}

	/* Remove the first record, and set the returned offset. */
	fe = szp->foff[0];
	WT_ERR(__block_off_remove(session, block, fe->off, &fe));
	*offsetp = fe->off;

	/* If doing a partial allocation, adjust the record and put it back. */
	if (fe->size > size) {
		WT_VERBOSE(session, block,
		    "allocate %" PRIu32 " from range %" PRIuMAX "-%"
		    PRIuMAX ", range shrinks to %" PRIuMAX "-%" PRIuMAX,
		    size,
		    (uintmax_t)fe->off, (uintmax_t)fe->off + fe->size,
		    (uintmax_t)fe->off + size,
		    (uintmax_t)fe->off + size + (fe->size - size));

		fe->off += size;
		fe->size -= size;
		WT_ERR(__block_off_insert(session, block,fe));
	} else {
		WT_VERBOSE(session, block,
		    "allocate range %" PRIuMAX "-%" PRIuMAX,
		    (uintmax_t)fe->off, (uintmax_t)fe->off + fe->size);

		__wt_free(session, fe);
	}

done: err:
	__wt_spin_unlock(session, &block->freelist_lock);
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
	if (fh->file_size < WT_BLOCK_DESC_SECTOR)
		WT_RET_MSG(session, EINVAL,
		    "cannot allocate from a file with no description "
		    "information");

	/*
	 * Make sure we don't allocate past the maximum file size.  There's no
	 * easy way to know the maximum off_t on a system, limit growth to 64
	 * bits, that should be big enough.
	 *
	 * XXX
	 * This isn't sufficient: if we grow the file to the end, there isn't
	 * enough room to write the free-list out when we close the file.  It
	 * is vanishingly unlikely to happen (we use free blocks where they're
	 * available to write the free list), but if the free-list is a bunch
	 * of small blocks, each group of which are insufficient to hold the
	 * free list, and the file has been fully populated, file close will
	 * fail because we can't write the free list.
	 */
	if (fh->file_size > (off_t)(INT64_MAX - size))
		WT_RET_MSG(session, WT_ERROR,
		    "block allocation failed, file cannot grow further");

	*offsetp = fh->file_size;
	fh->file_size += size;

	WT_BSTAT_INCR(session, extend);
	WT_VERBOSE(session, block,
	    "file extend %" PRIu32 "B @ %" PRIuMAX, size, (uintmax_t)*offsetp);

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
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t off, uint32_t size)
{
	WT_FREE *fe, *after, *before;
	u_int skipdepth;
	int ret;

	ret = 0;

	WT_BSTAT_INCR(session, free);
	WT_VERBOSE(session, block,
	    "free %" PRIuMAX "/%" PRIu32, (uintmax_t)off, size);

	__wt_spin_lock(session, &block->freelist_lock);

	/*
	 * Retrieve the records preceding/following the offset.  If the records
	 * are contiguous with the free'd offset, combine records.
	 */
	__block_off_pair_srch(block->foff, off, &before, &after);
	if (before != NULL) {
		if (before->off + before->size > off)
			WT_ERR_MSG(session, EINVAL,
			    "existing range %" PRIuMAX "-%" PRIuMAX " overlaps "
			    "with free'd range %" PRIuMAX "-%" PRIuMAX,
			    (uintmax_t)before->off,
			    (uintmax_t)before->off + before->size,
			    (uintmax_t)off,
			    (uintmax_t)off + size);
		if (before->off + before->size != off)
			before = NULL;
	}
	if (after != NULL) {
		if (off + size > after->off)
			WT_ERR_MSG(session, EINVAL,
			    "free'd range %" PRIuMAX "-%" PRIuMAX " overlaps "
			    "with existing range %" PRIuMAX "-%" PRIuMAX,
			    (uintmax_t)off,
			    (uintmax_t)off + size,
			    (uintmax_t)after->off,
			    (uintmax_t)after->off + after->size);
		if (off + size != after->off)
			after = NULL;
	}
	if (before == NULL && after == NULL) {
		/* Allocate a new WT_FREE structure. */
		skipdepth = __wt_skip_choose_depth();
		WT_ERR(__wt_calloc(session, 1,
		    sizeof(WT_FREE) + skipdepth * 2 * sizeof(WT_FREE *), &fe));
		fe->off = off;
		fe->size = size;
		fe->depth = (uint8_t)skipdepth;
		goto done;
	}

	/*
	 * If the "before" offset range abuts, we'll use it as our new record;
	 * if the "after" offset range also abuts, include its size and remove
	 * it from the system.  Else, only the "after" offset range abuts, use
	 * the "after" offset range as our new record.  In either case, remove
	 * the record we're going to use, adjust it and re-insert it.
	 */
	if (before == NULL) {
		WT_ERR(__block_off_remove(session, block, after->off, &fe));

		WT_VERBOSE(session, block,
		    "free range grows from %" PRIuMAX "-%" PRIuMAX ", to %"
		    PRIuMAX "-%" PRIuMAX,
		    (uintmax_t)fe->off, (uintmax_t)fe->off + fe->size,
		    (uintmax_t)off, (uintmax_t)off + fe->size + size);

		fe->off = off;
		fe->size += size;
	} else {
		if (after != NULL) {
			size += after->size;
			WT_ERR(__block_off_remove(
			    session, block, after->off, NULL));
		}
		WT_ERR(__block_off_remove(session, block, before->off, &fe));

		WT_VERBOSE(session, block,
		    "free range grows from %" PRIuMAX "-%" PRIuMAX ", to %"
		    PRIuMAX "-%" PRIuMAX,
		    (uintmax_t)fe->off, (uintmax_t)fe->off + fe->size,
		    (uintmax_t)fe->off, (uintmax_t)fe->off + fe->size + size);

		fe->size += size;
	}

done:	WT_ERR(__block_off_insert(session, block,fe));

err:	__wt_spin_unlock(session, &block->freelist_lock);
	return (ret);
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
	block->freelist_dirty = 0;

	block->free_offset = WT_BLOCK_INVALID_OFFSET;
	block->free_size = block->free_cksum = 0;
}

/*
 * __wt_block_freelist_read --
 *	Read the free-list.
 */
int
__wt_block_freelist_read(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_ITEM *tmp;
	off_t offset;
	uint32_t size;
	uint8_t *p;
	int ret;

	tmp = NULL;
	ret = 0;

	/* See if there's a free-list to read. */
	if (block->free_offset == WT_BLOCK_INVALID_OFFSET)
		return (0);

	WT_RET(__wt_scr_alloc(session, block->free_size, &tmp));
	WT_ERR(__wt_block_read(session, block,
	    tmp, block->free_offset, block->free_size, block->free_cksum));

	/*
	 * The free-list is read before the file is verified, which means we
	 * need to be a little paranoid.   We know the free-list chunk itself
	 * is entirely in the file because we checked when we first read the
	 * file's description structure.  Confirm the offset/size pairs are
	 * valid, then insert them into the linked list.
	 */
	p = WT_BLOCK_HEADER_BYTE(tmp->mem);
	offset = *(off_t *)p;
	p += sizeof(off_t);
	size = *(uint32_t *)p;
	p += sizeof(uint32_t);
	if (offset != WT_BLOCK_FREELIST_MAGIC || size != 0)
		goto corrupted;
	for (;;) {
		offset = *(off_t *)p;
		if (offset == WT_BLOCK_INVALID_OFFSET)
			break;
		p += sizeof(off_t);
		size = *(uint32_t *)p;
		p += sizeof(uint32_t);

		if ((offset - WT_BLOCK_DESC_SECTOR) % block->allocsize != 0 ||
		    size % block->allocsize != 0 ||
		    offset + size > block->fh->file_size)
corrupted:		WT_ERR_MSG(session, WT_ERROR,
			    "file contains a corrupted free-list, range %"
			    PRIuMAX "-%" PRIuMAX " past end-of-file",
			    (uintmax_t)offset, (uintmax_t)offset + size);

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
	WT_ITEM *tmp;
	WT_FREE *fe;
	WT_PAGE_HEADER *dsk;
	uint32_t datasize, size;
	uint8_t *p;
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

	/* Truncate the file if possible. */
	WT_RET(__block_truncate(session, block));

	WT_VERBOSE_CALL(session, block, __wt_block_dump(session, block));

	/*
	 * Get a scratch buffer, clear the page's header and data, initialize
	 * the header.  Allocate an allocation-sized aligned buffer so the
	 * block write function can zero-out unused bytes and write it without
	 * copying to something larger.
	 *
	 * Allocate room for the free-list entries, plus 2 additional entries:
	 * the initial WT_BLOCK_FREELIST_MAGIC/0 pair and the list-terminating
	 * WT_BLOCK_INVALID_OFFSET/0 pair.
	 */
	datasize = size = (block->freelist_entries + 2) *
	    WT_STORE_SIZE(sizeof(off_t) + sizeof(uint32_t));
	WT_RET(__wt_block_write_size(session, block, &size));
	WT_RET(__wt_scr_alloc(session, size, &tmp));
	dsk = tmp->mem;
	memset(dsk, 0, WT_BLOCK_HEADER_BYTE_SIZE);
	dsk->u.datalen = WT_STORE_SIZE(datasize);
	dsk->type = WT_PAGE_FREELIST;
	tmp->size = WT_STORE_SIZE(WT_BLOCK_HEADER_BYTE_SIZE + datasize);

	/* Fill the page's data. */
	p = WT_BLOCK_HEADER_BYTE(dsk);
	*(off_t *)p = WT_BLOCK_FREELIST_MAGIC;		/* Initial value */
	p += sizeof(off_t);
	*(uint32_t *)p = 0;
	p += sizeof(uint32_t);
	WT_FREE_FOREACH(fe, block->foff) {		/* Free ranges */
		*(off_t *)p = fe->off;
		p += sizeof(off_t);
		*(uint32_t *)p = fe->size;
		p += sizeof(uint32_t);
	}
	*(off_t *)p = WT_BLOCK_INVALID_OFFSET;		/* Ending value */
	p += sizeof(off_t);
	*(uint32_t *)p = 0;
	p += sizeof(uint32_t);

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

	/* Write the free list to disk and update the file's meta-data. */
	WT_ERR(__wt_block_write(session, block, tmp,
	    &block->free_offset, &block->free_size, &block->free_cksum));

	WT_VERBOSE(session, block,
	    "free-list written %" PRIuMAX "/%" PRIu32,
	    (uintmax_t)block->free_offset, block->free_size);

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
	WT_FREE *fe;

	fh = block->fh;

	/*
	 * Check if the last free range is at the end of the file, and if so,
	 * truncate the file and discard the range.
	 */
	if ((fe = __block_off_last(block->foff)) == NULL)
		return (0);
	if (fe->off + fe->size != fh->file_size)
		return (0);

	WT_VERBOSE(session, block,
	    "truncate file from %" PRIuMAX " to %" PRIuMAX,
	    (uintmax_t)fh->file_size, (uintmax_t)fe->off);

	fh->file_size = fe->off;
	WT_RET(__wt_ftruncate(session, fh, fh->file_size));

	WT_RET(__block_off_remove(session, block, fe->off, NULL));

	return (0);
}

/*
 * __wt_block_discard --
 *	Discard any free-list entries.
 */
void
__wt_block_discard(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FREE *fe, *nfe;
	WT_SIZE *szp, *nszp;

	for (fe = block->foff[0]; fe != NULL; fe = nfe) {
		nfe = fe->next[0];
		__wt_free(session, fe);
	}
	memset(block->foff, 0, sizeof(block->foff));
	for (szp = block->fsize[0]; szp != NULL; szp = nszp) {
		nszp = szp->next[0];
		__wt_free(session, szp);
	}
	memset(block->fsize, 0, sizeof(block->fsize));

	block->freelist_bytes = 0;
	block->freelist_entries = 0;
	block->freelist_dirty = 0;
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
	WT_BSTAT_SET(session, file_magic, WT_BLOCK_MAGIC);
	WT_BSTAT_SET(session, file_major, WT_BLOCK_MAJOR_VERSION);
	WT_BSTAT_SET(session, file_minor, WT_BLOCK_MINOR_VERSION);
}

#ifdef HAVE_VERBOSE
void
__wt_block_dump(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_FREE *fe;
	WT_SIZE *szp;

	WT_VERBOSE(session, block, "freelist by offset:");
	WT_FREE_FOREACH(fe, block->foff)
		WT_VERBOSE(session, block,
		    "\t{%" PRIuMAX "/%" PRIu32 "}",
		    (uintmax_t)fe->off, fe->size);

	WT_VERBOSE(session, block, "freelist by size:");
	WT_FREE_FOREACH(szp, block->fsize) {
		WT_VERBOSE(session, block,
		    "\t{%" PRIu32 "}",
		    szp->size);
		WT_FREE_FOREACH_OFF(fe, szp->foff)
			WT_VERBOSE(session, block,
			    "\t\t{%" PRIuMAX "/%" PRIu32 "}",
			    (uintmax_t)fe->off, fe->size);
	}
}
#endif
