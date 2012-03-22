/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __block_extend(WT_SESSION_IMPL *, WT_BLOCK *, off_t *, off_t);
static int __block_merge(WT_SESSION_IMPL *, WT_EXTLIST *, off_t, off_t);

#ifdef HAVE_VERBOSE
static void __block_extlist_dump(WT_SESSION_IMPL *, WT_EXTLIST *);
#endif

/*
 * __block_off_srch --
 *	Search a by-offset skiplist (either the primary by-offset list, or the
 * by-offset list referenced by a size entry), for the specified offset.
 */
static void
__block_off_srch(WT_EXT **head, off_t off, WT_EXT ***stack, int skip_off)
{
	WT_EXT **extp;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 *
	 * Return a stack for an exact match or the next-largest item.
	 *
	 * The WT_EXT structure contains two skiplists, the primary one and the
	 * per-size bucket one: if the skip_off flag is set, offset the skiplist
	 * array by the depth specified in this particular structure.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;)
		if (*extp != NULL && (*extp)->off < off)
			extp =
			    &(*extp)->next[i + (skip_off ? (*extp)->depth : 0)];
		else
			stack[i--] = extp--;
}

/*
 * __block_size_srch --
 *	Search the by-size skiplist for the specified size.
 */
static void
__block_size_srch(WT_SIZE **head, off_t size, WT_SIZE ***stack)
{
	WT_SIZE **szp;
	int i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 *
	 * Return a stack for an exact match or the next-largest item.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, szp = &head[i]; i >= 0;)
		if (*szp != NULL && (*szp)->size < size)
			szp = &(*szp)->next[i];
		else
			stack[i--] = szp--;
}

/*
 * __block_off_pair_srch --
 *	Search a by-offset skiplist for before/after records of the specified
 * offset.
 */
static void
__block_off_pair_srch(
    WT_EXTLIST *el, off_t off, WT_EXT **beforep, WT_EXT **afterp)
{
	WT_EXT **head, **extp;
	int i;

	*beforep = *afterp = NULL;

	head = el->off;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;) {
		if (*extp == NULL) {
			--i;
			--extp;
			continue;
		}

		if ((*extp)->off < off) {	/* Keep going at this level */
			*beforep = *extp;
			extp = &(*extp)->next[i];
		} else {			/* Drop down a level */
			*afterp = *extp;
			--i;
			--extp;
		}
	}
}

/*
 * __block_extlist_last --
 *	Return the last extent range in the skiplist.
 */
static WT_EXT *
__block_extlist_last(WT_EXT **head)
{
	WT_EXT *ext, **extp;
	int i;

	ext = NULL;

	for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;) {
		if (*extp == NULL) {
			--i;
			--extp;
			continue;
		}
		ext = *extp;
		extp = &(*extp)->next[i];
	}
	return (ext);
}

/*
 * __block_off_insert --
 *	Insert a record into an extent list.
 */
static int
__block_off_insert(WT_SESSION_IMPL *session, WT_EXTLIST *el, WT_EXT *ext)
{
	WT_EXT **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	/*
	 * If we are inserting a new size onto the size skiplist, we'll need
	 * a new WT_EXT structure for that skiplist.
	 */
	__block_size_srch(el->size, ext->size, sstack);
	szp = *sstack[0];
	if (szp == NULL || szp->size != ext->size) {
		WT_RET(__wt_calloc(session, 1,
		    sizeof(WT_SIZE) + ext->depth * sizeof(WT_SIZE *), &szp));
		szp->size = ext->size;
		szp->depth = ext->depth;
		for (i = 0; i < ext->depth; ++i) {
			szp->next[i] = *sstack[i];
			*sstack[i] = szp;
		}
	}

	/* Insert the new WT_EXT structure into the offset skiplist. */
	__block_off_srch(el->off, ext->off, astack, 0);
	for (i = 0; i < ext->depth; ++i) {
		ext->next[i] = *astack[i];
		*astack[i] = ext;
	}

	/*
	 * Insert the new WT_EXT structure into the size element's offset
	 * skiplist.
	 */
	__block_off_srch(szp->off, ext->off, astack, 1);
	for (i = 0; i < ext->depth; ++i) {
		ext->next[i + ext->depth] = *astack[i];
		*astack[i] = ext;
	}

	++el->entries;
	el->bytes += (uint64_t)ext->size;

	return (0);
}

/*
 * __block_off_remove --
 *	Remove a record from an extent list.
 */
static int
__block_off_remove(
    WT_SESSION_IMPL *session, WT_EXTLIST *el, off_t off, WT_EXT **extp)
{
	WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	u_int i;

	/* Find and remove the record from the by-offset skiplist. */
	__block_off_srch(el->off, off, astack, 0);
	ext = *astack[0];
	if (ext == NULL || ext->off != off)
		return (EINVAL);
	for (i = 0; i < ext->depth; ++i)
		*astack[i] = ext->next[i];

	/*
	 * Find and remove the record from the size's offset skiplist; if that
	 * empties the by-size skiplist entry, remove it as well.
	 */
	__block_size_srch(el->size, ext->size, sstack);
	szp = *sstack[0];
	if (szp == NULL || szp->size != ext->size)
		return (EINVAL);
	__block_off_srch(szp->off, off, astack, 1);
	ext = *astack[0];
	if (ext == NULL || ext->off != off)
		return (EINVAL);
	for (i = 0; i < ext->depth; ++i)
		*astack[i] = ext->next[i + ext->depth];
	if (szp->off[0] == NULL) {
		for (i = 0; i < szp->depth; ++i)
			*sstack[i] = szp->next[i];
		__wt_free(session, szp);
	}

	--el->entries;
	el->bytes -= (uint64_t)ext->size;

	/* Return the record if our caller wants it, otherwise free it. */
	if (extp == NULL)
		__wt_free(session, ext);
	else
		*extp = ext;

	return (0);
}

/*
 * __wt_block_alloc --
 *	Alloc a chunk of space from the underlying file.
 */
int
__wt_block_alloc(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t *offp, off_t size)
{
	WT_EXT *ext;
	WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
	int ret;

	ret = 0;

	WT_BSTAT_INCR(session, alloc);
	if (size % block->allocsize != 0)
		WT_RET_MSG(session, EINVAL,
		    "cannot allocate a block size %" PRIdMAX " that is not "
		    "a multiple of the allocation size %" PRIu32,
		    (intmax_t)size, block->allocsize);

	__wt_spin_lock(session, &block->live_lock);

	/*
	 * Allocation is first-fit by size: search the by-size skiplist for the
	 * requested size and take the first entry on the by-size offset list.
	 * If we don't have anything large enough, extend the file.
	 */
	__block_size_srch(block->live.avail.size, size, sstack);
	szp = *sstack[0];
	if (szp == NULL) {
		WT_ERR(__block_extend(session, block, offp, size));
		goto done;
	}

	/* Remove the first record, and set the returned offset. */
	ext = szp->off[0];
	WT_ERR(__block_off_remove(session, &block->live.avail, ext->off, &ext));
	*offp = ext->off;

	/* If doing a partial allocation, adjust the record and put it back. */
	if (ext->size > size) {
		WT_VERBOSE(session, block,
		    "allocate %" PRIdMAX " from range %" PRIdMAX "-%"
		    PRIdMAX ", range shrinks to %" PRIdMAX "-%" PRIdMAX,
		    (intmax_t)size,
		    (intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
		    (intmax_t)(ext->off + size),
		    (intmax_t)(ext->off + size + ext->size - size));

		ext->off += size;
		ext->size -= size;
		WT_ERR(__block_off_insert(session, &block->live.avail, ext));
	} else {
		WT_VERBOSE(session, block,
		    "allocate range %" PRIdMAX "-%" PRIdMAX,
		    (intmax_t)ext->off, (intmax_t)(ext->off + ext->size));

		__wt_free(session, ext);
	}

done: err:
	__wt_spin_unlock(session, &block->live_lock);
	return (ret);
}

/*
 * __block_extend --
 *	Extend the file to allocate space.
 */
static int
__block_extend(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t *offp, off_t size)
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
	 * easy way to know the maximum off_t on a system, limit growth to 8B
	 * bits (we currently check an off_t is 8B in verify_build.h).  I don't
	 * think we're likely to see anything bigger for awhile.
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
	if (fh->file_size > (off_t)INT64_MAX - size)
		WT_RET_MSG(session, WT_ERROR,
		    "block allocation failed, file cannot grow further");

	*offp = fh->file_size;
	fh->file_size += size;

	WT_BSTAT_INCR(session, extend);
	WT_VERBOSE(session, block,
	    "file extend %" PRIdMAX "B @ %" PRIdMAX,
	    (intmax_t)size, (intmax_t)*offp);

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
	off_t off;
	uint32_t size;

	WT_UNUSED(addr_size);

	/* Crack the cookie. */
	WT_RET(__wt_block_buffer_to_addr(block, addr, &off, &size, NULL));
	WT_RET(__wt_block_free(session, block, off, size));

	return (0);
}

/*
 * __wt_block_free --
 *	Free a chunk of space to the underlying file.
 */
int
__wt_block_free(
    WT_SESSION_IMPL *session, WT_BLOCK *block, off_t off, off_t size)
{
	int ret;

	WT_BSTAT_INCR(session, free);
	WT_VERBOSE(session, block,
	    "free %" PRIdMAX "/%" PRIdMAX, (intmax_t)off, (intmax_t)size);

	__wt_spin_lock(session, &block->live_lock);
	ret = __block_merge(session, &block->live.avail, off, (off_t)size);
	__wt_spin_unlock(session, &block->live_lock);

	return (ret);
}

/*
 * __block_merge --
 *	Insert an extent into an extent list, merging if possible.
 */
static int
__block_merge(WT_SESSION_IMPL *session, WT_EXTLIST *el, off_t off, off_t size)
{
	WT_EXT *ext, *after, *before;
	u_int skipdepth;

	/*
	 * Retrieve the records preceding/following the offset.  If the records
	 * are contiguous with the free'd offset, combine records.
	 */
	__block_off_pair_srch(el, off, &before, &after);
	if (before != NULL) {
		if (before->off + before->size > off)
			WT_RET_MSG(session, EINVAL,
			    "%s: existing range %" PRIdMAX "-%" PRIdMAX
			    " overlaps with merge range %" PRIdMAX "-%" PRIdMAX,
			    el->name,
			    (intmax_t)before->off,
			    (intmax_t)(before->off + before->size),
			    (intmax_t)off, (intmax_t)(off + size));
		if (before->off + before->size != off)
			before = NULL;
	}
	if (after != NULL) {
		if (off + size > after->off)
			WT_RET_MSG(session, EINVAL,
			    "%s: merge range %" PRIdMAX "-%" PRIdMAX
			    " overlaps with existing range %" PRIdMAX
			    "-%" PRIdMAX,
			    el->name,
			    (intmax_t)off, (intmax_t)(off + size),
			    (intmax_t)after->off,
			    (intmax_t)(after->off + after->size));
		if (off + size != after->off)
			after = NULL;
	}
	if (before == NULL && after == NULL) {
		/* Allocate a new WT_EXT structure. */
		skipdepth = __wt_skip_choose_depth();
		WT_RET(__wt_calloc(session, 1,
		    sizeof(WT_EXT) + skipdepth * 2 * sizeof(WT_EXT *), &ext));
		ext->off = off;
		ext->size = size;
		ext->depth = (uint8_t)skipdepth;
		return (__block_off_insert(session, el, ext));
	}

	/*
	 * If the "before" offset range abuts, we'll use it as our new record;
	 * if the "after" offset range also abuts, include its size and remove
	 * it from the system.  Else, only the "after" offset range abuts, use
	 * the "after" offset range as our new record.  In either case, remove
	 * the record we're going to use, adjust it and re-insert it.
	 */
	if (before == NULL) {
		WT_RET(__block_off_remove(session, el, after->off, &ext));

		WT_VERBOSE(session, block,
		    "%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %"
		    PRIdMAX "-%" PRIdMAX,
		    el->name,
		    (intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
		    (intmax_t)off, (intmax_t)(off + ext->size + size));

		ext->off = off;
		ext->size += size;
	} else {
		if (after != NULL) {
			size += after->size;
			WT_RET(
			    __block_off_remove(session, el, after->off, NULL));
		}
		WT_RET(__block_off_remove(session, el, before->off, &ext));

		WT_VERBOSE(session, block,
		    "%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %"
		    PRIdMAX "-%" PRIdMAX,
		    el->name,
		    (intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
		    (intmax_t)ext->off,
		    (intmax_t)(ext->off + ext->size + size));

		ext->size += size;
	}
	return (__block_off_insert(session, el, ext));
}

/*
 * __wt_block_extlist_read --
 *	Read an extent list.
 */
int
__wt_block_extlist_read(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_EXTLIST *el, off_t off, uint32_t size, uint32_t cksum)
{
	WT_ITEM *tmp;
	off_t loff, lsize;
	uint8_t *p;
	int ret;

	tmp = NULL;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, size, &tmp));
	WT_ERR(__wt_block_read(session, block, tmp, off, size, cksum));

#define	WT_EXTLIST_READ(p, v) do {					\
	(v) = *(off_t *)(p);						\
	(p) += sizeof(off_t);						\
} while (0)

	p = WT_BLOCK_HEADER_BYTE(tmp->mem);
	WT_EXTLIST_READ(p, loff);
	WT_EXTLIST_READ(p, lsize);
	if (loff != WT_BLOCK_EXTLIST_MAGIC || lsize != 0)
		goto corrupted;
	for (;;) {
		WT_EXTLIST_READ(p, loff);
		WT_EXTLIST_READ(p, lsize);
		if (loff == WT_BLOCK_INVALID_OFFSET)
			break;

		/*
		 * We check the offset/size pairs represent valid file ranges,
		 * then insert them into the list.  We don't necessarily have
		 * to check for offsets past the end-of-file, but it's a cheap
		 * and easy test to do here and we'd have to do the check as
		 * part of file verification, regardless.
		 */
		if ((loff - WT_BLOCK_DESC_SECTOR) % block->allocsize != 0 ||
		    lsize % block->allocsize != 0 ||
		    loff + lsize > block->fh->file_size)
corrupted:		WT_ERR_MSG(session, WT_ERROR,
			    "file contains a corrupted %s extent list, range %"
			    PRIdMAX "-%" PRIdMAX " past end-of-file",
			    el->name,
			    (intmax_t)loff, (intmax_t)(loff + lsize));

		/*
		 * We could insert instead of merge, because ranges shouldn't
		 * overlap, but merge knows how to allocate WT_EXT structures,
		 * and a little paranoia is a good thing.
		 */
		WT_ERR(__block_merge(session, el, loff, lsize));
	}

	WT_VERBOSE_CALL(session, block, __block_extlist_dump(session, el));

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_extlist_write --
 *	Write an extent list at the tail of the file.
 */
int
__wt_block_extlist_write(WT_SESSION_IMPL *session, WT_BLOCK *block,
    WT_EXTLIST *el, off_t *offp, uint32_t *sizep, uint32_t *cksump)
{
	WT_EXT *ext;
	WT_ITEM *tmp;
	WT_PAGE_HEADER *dsk;
	uint32_t datasize, size;
	uint8_t *p;
	int ret;
	const char *name;

	tmp = NULL;
	ret = 0;

	WT_VERBOSE_CALL(session, block, __block_extlist_dump(session, el));

	/* If there aren't any entries, we're done. */
	if (el->entries == 0) {
		*offp = WT_BLOCK_INVALID_OFFSET;
		*sizep = *cksump = 0;
		return (0);
	}

	/*
	 * Get a scratch buffer, clear the page's header and data, initialize
	 * the header.  Allocate an allocation-sized aligned buffer so the
	 * block write function can zero-out unused bytes and write it without
	 * copying to something larger.
	 *
	 * Allocate room for the free-list entries, plus 2 additional entries:
	 * the initial WT_BLOCK_EXTLIST_MAGIC/0 pair and the list-terminating
	 * WT_BLOCK_INVALID_OFFSET/0 pair.
	 */
	datasize = size = (el->entries + 2) * WT_STORE_SIZE(sizeof(off_t)  * 2);
	WT_RET(__wt_block_write_size(session, block, &size));
	WT_RET(__wt_scr_alloc(session, size, &tmp));
	dsk = tmp->mem;
	memset(dsk, 0, WT_BLOCK_HEADER_BYTE_SIZE);
	dsk->u.datalen = WT_STORE_SIZE(datasize);
	dsk->type = WT_PAGE_FREELIST;
	tmp->size = WT_STORE_SIZE(WT_BLOCK_HEADER_BYTE_SIZE + datasize);

#define	WT_EXTLIST_WRITE(p, v) do {					\
	*(off_t *)(p) = (v);						\
	(p) += sizeof(off_t);						\
} while (0)

	/* Fill the page's data. */
	p = WT_BLOCK_HEADER_BYTE(dsk);
	WT_EXTLIST_WRITE(p, WT_BLOCK_EXTLIST_MAGIC);	/* Initial value */
	WT_EXTLIST_WRITE(p, 0);
	WT_EXT_FOREACH(ext, el->off) {		/* Free ranges */
		WT_EXTLIST_WRITE(p, ext->off);
		WT_EXTLIST_WRITE(p, ext->size);
	}
	WT_EXTLIST_WRITE(p, WT_BLOCK_INVALID_OFFSET);	/* Ending value */
	WT_EXTLIST_WRITE(p, 0);

	/*
	 * XXX
	 * Discard the in-memory free-list: this has to happen before writing
	 * the free-list because the underlying block write function is going
	 * to allocate file space for the free-list block(s), and allocating
	 * from the blocks on the free-list we just wrote won't work out well.
	 * A workaround would be to not compress the free-list, which implies
	 * some kind of "write but don't compress" code path, and that's more
	 * complex than ordering these operations so the eventual allocation
	 * in the write code always extends the file.
	 */
	name = el->name;
	__wt_block_extlist_free(session, el);
	el = NULL;

	/* Write the extent list to disk. */
	WT_ERR(__wt_block_write(session, block, tmp, offp, sizep, cksump));

	WT_VERBOSE(session, block,
	    "%s written %" PRIdMAX "/%" PRIu32, name, (intmax_t)*offp, *sizep);

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_extlist_truncate --
 *	Truncate the file based on the last available extent in the list.
 */
int
__wt_block_extlist_truncate(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
	WT_EXT *ext;
	WT_FH *fh;

	fh = block->fh;

	/*
	 * Check if the last available extent is at the end of the file, and if
	 * so, truncate the file and discard the extent.
	 */
	if ((ext = __block_extlist_last(el->off)) == NULL)
		return (0);
	if (ext->off + ext->size != fh->file_size)
		return (0);

	WT_VERBOSE(session, block,
	    "truncate file from %" PRIdMAX " to %" PRIdMAX,
	    (intmax_t)fh->file_size, (intmax_t)ext->off);

	fh->file_size = ext->off;
	WT_RET(__wt_ftruncate(session, fh, fh->file_size));

	WT_RET(__block_off_remove(session, el, ext->off, NULL));

	return (0);
}
/*
 * __wt_block_extlist_free --
 *	Discard an extent list.
 */
void
__wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el)
{
	WT_EXT *ext, *next;
	WT_SIZE *szp, *nszp;

	for (ext = el->off[0]; ext != NULL; ext = next) {
		next = ext->next[0];
		__wt_free(session, ext);
	}
	memset(el->off, 0, sizeof(el->off));
	for (szp = el->size[0]; szp != NULL; szp = nszp) {
		nszp = szp->next[0];
		__wt_free(session, szp);
	}
	memset(el->size, 0, sizeof(el->size));

	el->bytes = 0;
	el->entries = 0;
}

#ifdef HAVE_VERBOSE
static void
__block_extlist_dump(WT_SESSION_IMPL *session, WT_EXTLIST *el)
{
	WT_EXT *ext;
	WT_SIZE *szp;

	if (el->entries == 0) {
		WT_VERBOSE(session, block, "%s: [Empty]", el->name);
		return;
	}

	WT_VERBOSE(session, block, "%s: list by offset:", el->name);
	WT_EXT_FOREACH(ext, el->off)
		WT_VERBOSE(session, block,
		    "\t{%" PRIuMAX "/%" PRIuMAX "}",
		    (uintmax_t)ext->off, (uintmax_t)ext->size);

	WT_VERBOSE(session, block, "%s: list by size:", el->name);
	WT_EXT_FOREACH(szp, el->size) {
		WT_VERBOSE(session, block,
		    "\t{%" PRIuMAX "}",
		    (uintmax_t)szp->size);
		WT_EXT_FOREACH_OFF(ext, szp->off)
			WT_VERBOSE(session, block,
			    "\t\t{%" PRIuMAX "/%" PRIuMAX "}",
			    (uintmax_t)ext->off, (uintmax_t)ext->size);
	}
}
#endif
