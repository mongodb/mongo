/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"
#include "cell.i"

struct __wt_stuff; 		typedef struct __wt_stuff WT_STUFF;
struct __wt_track; 		typedef struct __wt_track WT_TRACK;

/*
 * There's a bunch of stuff we pass around during salvage, group it together
 * to make the code prettier.
 */
struct __wt_stuff {
	WT_BTREE  *btree;			/* Enclosing Btree */

	WT_TRACK **pages;			/* Pages */
	uint32_t   pages_next;			/* Next empty slot */
	uint32_t   pages_allocated;		/* Bytes allocated */

	WT_TRACK **ovfl;			/* Overflow pages */
	uint32_t   ovfl_next;			/* Next empty slot */
	uint32_t   ovfl_allocated;		/* Bytes allocated */

	uint8_t    page_type;			/* Page type */

	int	   range_merge;			/* If merged key ranges */

	uint64_t fcnt;				/* Progress counter */
};

/*
 * WT_TRACK --
 *	Structure to track validated pages, one per page.
 */
struct __wt_track {
	WT_STUFF *ss;				/* Enclosing stuff */

	uint64_t lsn;				/* LSN */

	uint32_t addr;				/* Page address */
	uint32_t size;				/* Page size */

	/*
	 * Pages that reference overflow pages contain a list of the overflow
	 * pages they reference.
	 */
	WT_OFF	*ovfl;				/* Referenced overflow pages */
	uint32_t ovfl_cnt;			/* Overflow list elements */

	union {
		struct {
			WT_BUF   range_start;	/* Row-store start range */
			WT_BUF   range_stop;	/* Row-store stop range */
		} row;
		struct {
			uint64_t range_start;	/* Col-store start range */
			uint64_t range_stop;	/* Col-store stop range */
		} col;
	} u;

#define	WT_TRACK_CHECK_START	0x001		/* Initial key updated */
#define	WT_TRACK_CHECK_STOP	0x002		/* Last key updated */
#define	WT_TRACK_MERGE		0x004		/* Page requires merging */
#define	WT_TRACK_NO_FB		0x008		/* Don't free blocks */
#define	WT_TRACK_OVFL_MISSING	0x010		/* Overflow page missing */
#define	WT_TRACK_OVFL_REFD	0x020		/* Overflow page referenced */
	uint32_t flags;
};

static int  __slvg_build_internal_col(WT_SESSION_IMPL *, uint32_t, WT_STUFF *);
static int  __slvg_build_internal_row(WT_SESSION_IMPL *, uint32_t, WT_STUFF *);
static int  __slvg_build_leaf_col(WT_SESSION_IMPL *,
		WT_TRACK *, WT_PAGE *, WT_COL_REF *, WT_STUFF *);
static int  __slvg_build_leaf_row(WT_SESSION_IMPL *,
		WT_TRACK *, WT_PAGE *, WT_ROW_REF *, WT_STUFF *, int *);
static int  __slvg_discard_ovfl(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_free(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_free_merge_block(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_free_trk(WT_SESSION_IMPL *, WT_TRACK **, int);
static int  __slvg_key_copy(WT_SESSION_IMPL *, WT_BUF *, WT_BUF *);
static void __slvg_ovfl_col_inmem_ref(WT_PAGE *, WT_STUFF *);
static int  __slvg_ovfl_compare(const void *, const void *);
static int  __slvg_ovfl_dsk_ref(WT_SESSION_IMPL *, WT_PAGE_DISK *, WT_TRACK *);
static void __slvg_ovfl_row_inmem_ref(WT_PAGE *, uint32_t, WT_STUFF *);
static int  __slvg_range_col(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_range_overlap_col(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static int  __slvg_range_overlap_row(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static int  __slvg_range_row(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_read(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_trk_compare(const void *, const void *);
static int  __slvg_trk_leaf(
		WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static int  __slvg_trk_ovfl(
		WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static void __slvg_trk_ovfl_ref(WT_TRACK *, WT_STUFF *);

#ifdef HAVE_DIAGNOSTIC
static void __slvg_trk_dump_col(WT_TRACK *);
static void __slvg_trk_dump_row(WT_TRACK *);
#endif

/*
 * __wt_salvage --
 *	Salvage a Btree.
 */
int
__wt_salvage(WT_SESSION_IMPL *session, const char *config)
{
	WT_BTREE *btree;
	WT_STUFF *ss, stuff;
	off_t len;
	uint32_t allocsize, i, leaf_cnt;
	int ret;

	WT_UNUSED(config);

	btree = session->btree;
	ret = 0;

	WT_CLEAR(stuff);
	stuff.btree = btree;
	stuff.page_type = WT_PAGE_INVALID;
	ss = &stuff;

	if (btree->fh->file_size > WT_BTREE_DESC_SECTOR) {
		/*
		 * Truncate the file to an initial sector plus N allocation size
		 * units (bytes after the last allocation size unit have to be
		 * garbage).
		 */
		allocsize = btree->allocsize;
		len = btree->fh->file_size - WT_BTREE_DESC_SECTOR;
		len = (len / allocsize) * allocsize;
		len += WT_BTREE_DESC_SECTOR;
		if (len != btree->fh->file_size)
			WT_ERR(__wt_ftruncate(session, btree->fh, len));
	}

	/* If the file has no data pages, we're done. */
	if (btree->fh->file_size <= WT_BTREE_DESC_SECTOR) {
		__wt_errx(session,
		    "the file contains no data pages and is too small to "
		    "salvage");
		goto err;
	}

	/* Read the file and build the page list. */
	WT_ERR(__slvg_read(session, ss));

	if (ss->pages_next == 0) {
		__wt_errx(session,
		    "file has no valid pages and cannot be salvaged");
		goto err;
	}

	/*
	 * Sort the page list by key, and secondarily, by LSN.
	 *
	 * We don't have to sort the overflow array because we inserted the
	 * records into the array in file addr order.
	 */
	qsort(ss->pages,
	    (size_t)ss->pages_next, sizeof(WT_TRACK *), __slvg_trk_compare);

	/*
	 * Walk the list of leaf pages looking for overlapping ranges we need
	 * to resolve.  At the same time, do a preliminary check of overflow
	 * pages -- if a valid page references an overflow page, we may need
	 * it, mark the overflow page as referenced.
	 */
	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		WT_ERR(__slvg_range_col(session, ss));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__slvg_range_row(session, ss));
		break;
	}

	/*
	 * Discard any overflow pages that aren't referenced at all, and clear
	 * the reference count.  (The reference count was just a guess: we do
	 * not know what keys will be kept vs. discarded as part of resolving
	 * key range overlaps.   When we resolve the key range overlaps we'll
	 * figure out exactly what overflow pages we're using.)
	 */
	WT_ERR(__slvg_discard_ovfl(session, ss));

	/*
	 * Build an internal page that references all of the leaf pages.
	 *
	 * Count how many internal page slots we need (we could track this
	 * during the array shuffling/splitting, but I didn't bother, it's
	 * just a pass through the array).
	 */
	for (leaf_cnt = i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			++leaf_cnt;
	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		WT_ERR(__slvg_build_internal_col(session, leaf_cnt, ss));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__slvg_build_internal_row(session, leaf_cnt, ss));
		break;
	}

	/*
	 * If we had to merge key ranges, we have to do a final pass through
	 * the leaf page array and discard file pages used during merges.
	 * We can't do it earlier: if we free'd the leaf pages we're merging as
	 * we merged them, the write of subsequent leaf pages or the internal
	 * page might allocate those free'd file blocks, and if the salvage run
	 * subsequently fails, we've overwritten pages used to construct the
	 * final key range.  In other words, if the salvage run fails, we don't
	 * want to overwrite data the next salvage run might need.
	 */
	 if (ss->range_merge)
		WT_ERR(__slvg_free_merge_block(session, ss));

	/*
	 * If we have to merge key ranges, the list of referenced overflow
	 * pages might be incorrect, that is, when we did the original pass to
	 * detect unused overflow pages, we may have flagged some pages as
	 * referenced which are no longer referenced after keys were discarded
	 * from their key range.  We reset the referenced overflow page list
	 * when merging key ranges, and so now we have a correct list.  Free
	 * any unused overflow pages.
	 */
	 if (ss->range_merge)
		WT_ERR(__slvg_discard_ovfl(session, ss));

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}

	/* Wrap up reporting. */
	__wt_progress(session, NULL, ss->fcnt);

	/* Free allocated memory. */
	WT_TRET(__slvg_free(session, ss));

	return (ret);
}

/*
 * __slvg_read --
 *	Read the file and build a table of the pages we can use.
 */
static int
__slvg_read(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_BUF *t;
	WT_FH *fh;
	WT_PAGE_DISK *dsk;
	off_t off, max;
	uint32_t addr, allocsize, checksum, size;
	int ret;

	btree = session->btree;
	fh = btree->fh;
	allocsize = btree->allocsize;

	ret = 0;
	WT_RET(__wt_scr_alloc(session, allocsize, &t));

	/*
	 * Read through the file, looking for pages we can trust -- a page
	 * we can trust consists of a valid checksum and passing the disk
	 * verification routine.
	 *
	 * The first sector of the file is the description record -- ignore
	 * it for now.
	 */
	for (off = WT_BTREE_DESC_SECTOR, max = fh->file_size; off < max;) {
		/* Report progress every 10 reads. */
		if (++ss->fcnt % 10 == 0)
			__wt_progress(session, NULL, ss->fcnt);

		addr = WT_OFF_TO_ADDR(btree, off);

		/*
		 * Read the start of a possible page (an allocation-size block),
		 * and get a page length from it.
		 */
		WT_ERR(__wt_read(session, fh, off, allocsize, t->mem));
		dsk = t->mem;

		/*
		 * The page can't be more than the min/max page size, or past
		 * the end of the file.
		 */
		size = dsk->size;
		if (size == 0 ||
		    size % allocsize != 0 ||
		    size > WT_BTREE_PAGE_SIZE_MAX ||
		    off + (off_t)size > max)
			goto skip_allocsize;

		/* The page size isn't insane, read the entire page. */
		WT_ERR(__wt_buf_initsize(session, t, size));
		WT_ERR(__wt_read(session, fh, off, size, t->mem));
		dsk = t->mem;

		/*
		 * If the checksum matches, assume we have a valid page, or, in
		 * other words, assume corruption will always fail the checksum.
		 * We could verify the page itself, but that's slow and we'd
		 * have to modify the page verification routines to be silent
		 * and to ignore off-page items, as it's reasonable for salvage
		 * to see pages that reference non-existent objects.
		 */
		checksum = dsk->checksum;
		dsk->checksum = 0;
		if (checksum != __wt_cksum(dsk, size)) {
skip_allocsize:		WT_RET(__wt_block_free(session, addr, allocsize));
			off += allocsize;
			continue;
		}

		/* Move past this page. */
		off += size;

		/*
		 * Make sure it's an expected page type for the file.
		 *
		 * We only care about leaf and overflow pages from here on out;
		 * discard all of the others.  We put them on the free list now,
		 * because we might as well overwrite them, we want the file to
		 * grow as little as possible, or shrink, and future salvage
		 * calls don't need them either.
		 */
		switch (dsk->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_FREELIST:
		case WT_PAGE_ROW_INT:
			WT_RET(__wt_block_free(session, addr, size));
			continue;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_RLE:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			if (ss->page_type == WT_PAGE_INVALID)
				ss->page_type = dsk->type;
			if (ss->page_type != dsk->type) {
				__wt_errx(session,
				    "file contains multiple file formats (both "
				    "%s and %s), and cannot be salvaged",
				    __wt_page_type_string(ss->page_type),
				    __wt_page_type_string(dsk->type));
				return (WT_ERROR);
			}

			/*
			 * After reading the file, we may write pages in order
			 * to resolve key range overlaps.   We give our newly
			 * written pages LSNs larger than any LSN found in the
			 * file in case the salvage run fails and is restarted
			 * later.  (Regardless of our LSNs, it's possible our
			 * newly written pages will have to be merged in a
			 * subsequent salvage run, at least if it's a row-store,
			 * as the key ranges are not exact.  However, having
			 * larger LSNs should make our newly written pages more
			 * likely to win over previous pages, minimizing the
			 * work done in subsequent salvage runs.)  Reset the
			 * tree's current LSN to the largest LSN we read.
			 */
			if (btree->lsn < dsk->lsn)
				btree->lsn = dsk->lsn;

			WT_ERR(__slvg_trk_leaf(session, dsk, addr, ss));
			break;
		case WT_PAGE_OVFL:
			WT_ERR(__slvg_trk_ovfl(session, dsk, addr, ss));
			break;
		}
	}

err:	__wt_scr_release(&t);

	return (ret);
}

/*
 * WT_TRACK_INIT --
 *	Update common WT_TRACK information.
 */
#define	WT_TRACK_INIT(_ss, trk, _lsn, _addr, _size) do {		\
	(trk)->ss = _ss;						\
	(trk)->lsn = _lsn;						\
	(trk)->addr = _addr;						\
	(trk)->size = _size;						\
} while (0)

/*
 * __slvg_trk_leaf --
 *	Track a leaf page.
 */
static int
__slvg_trk_leaf(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_TRACK *trk;
	uint64_t stop_recno;
	uint32_t i;
	int ret;
	uint8_t *p;

	btree = session->btree;
	page = NULL;
	ret = 0;

	/* Re-allocate the array of pages, as necessary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/* Allocate a WT_TRACK entry for this new page and fill it in. */
	WT_ERR(__wt_calloc_def(session, 1, &trk));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		/*
		 * Column-store fixed-sized format: start and stop keys can be
		 * taken from the WT_PAGE_DISK header, and they can't contain
		 * overflow items.
		 *
		 * Column-store variable-sized format: start and stop keys can
		 * be taken from the WT_PAGE_DISK header, but they can contain
		 * overflow items.
		 */
		trk->u.col.range_start = dsk->recno;
		trk->u.col.range_stop = dsk->recno + (dsk->u.entries - 1);

		if (dsk->type == WT_PAGE_COL_VAR)
			WT_ERR(__slvg_ovfl_dsk_ref(session, dsk, trk));
		break;
	case WT_PAGE_COL_RLE:
		/*
		 * Column-store RLE format: the start key can be taken from the
		 * WT_PAGE_DISK header, but the stop key requires walking the
		 * page.
		 */
		stop_recno = dsk->recno;
		WT_RLE_REPEAT_FOREACH(btree, dsk, p, i)
			stop_recno += WT_RLE_REPEAT_COUNT(p);

		trk->u.col.range_start = dsk->recno;
		trk->u.col.range_stop = stop_recno - 1;
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Row-store format: copy the first and last keys on the page.
		 * Keys are prefix-compressed, the simplest and slowest thing
		 * to do is instantiate the in-memory page, then instantiate
		 * and copy the keys, then free the page.
		 */
		WT_ERR(__wt_page_inmem(session, NULL, NULL, dsk, &page));
		WT_ERR(__wt_row_key(session,
		    page, &page->u.row_leaf.d[0], &trk->u.row.range_start));
		WT_ERR(__wt_row_key(session,
		    page, &page->u.row_leaf.d[page->entries - 1],
		    &trk->u.row.range_stop));

		/* Row-store pages can contain overflow items. */
		WT_ERR(__slvg_ovfl_dsk_ref(session, dsk, trk));
		break;
	}

	WT_TRACK_INIT(ss, trk, dsk->lsn, addr, dsk->size);
	ss->pages[ss->pages_next++] = trk;

	if (0) {
err:		if (trk != NULL)
			__wt_free(session, trk);
	}
	if (page != NULL)
		__wt_page_free(session, page, WT_PAGE_FREE_IGNORE_DISK);
	return (ret);
}

/*
 * __slvg_trk_ovfl --
 *	Track an overflow page.
 */
static int
__slvg_trk_ovfl(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, WT_STUFF *ss)
{
	WT_TRACK *trk;

	/*
	 * Reallocate the overflow page array as necessary, then save the
	 * page's location information.
	 */
	if (ss->ovfl_next * sizeof(WT_TRACK *) == ss->ovfl_allocated)
		WT_RET(__wt_realloc(session, &ss->ovfl_allocated,
		   (ss->ovfl_next + 1000) * sizeof(WT_TRACK *), &ss->ovfl));

	WT_RET(__wt_calloc_def(session, 1, &trk));

	WT_TRACK_INIT(ss, trk, dsk->lsn, addr, dsk->size);
	ss->ovfl[ss->ovfl_next++] = trk;

	return (0);
}

/*
 * __slvg_ovfl_dsk_ref --
 *	Search a page for overflow items.
 */
static int
__slvg_ovfl_dsk_ref(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
{
	WT_CELL *cell;
	WT_OFF ovfl;
	uint32_t i, ovfl_cnt;

	/*
	 * Two passes: count the overflow items, then copy them into an
	 * allocated array.
	 */
	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		if (__wt_cell_type_is_ovfl(cell))
			++ovfl_cnt;
	if (ovfl_cnt == 0)
		return (0);

	WT_RET(__wt_calloc(session, ovfl_cnt, sizeof(WT_OFF), &trk->ovfl));
	trk->ovfl_cnt = ovfl_cnt;

	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		if (__wt_cell_type_is_ovfl(cell)) {
			__wt_cell_off(cell, &ovfl);
			trk->ovfl[ovfl_cnt].addr = ovfl.addr;
			trk->ovfl[ovfl_cnt].size = ovfl.size;
			if (++ovfl_cnt == trk->ovfl_cnt)
				break;
		}
	return (0);
}

/*
 * __slvg_range_col --
 *	Figure out the leaf pages we need and discard everything else.  At the
 * same time, tag the overflow pages they reference.
 *
 * When pages split, the key range is split across multiple pages.  If not all
 * of the old versions of the page are overwritten, or not all of the new pages
 * are written, or some of the pages are corrupted, salvage will read different
 * pages with overlapping key ranges, at different LSNs.
 *
 * We salvage all of the key ranges we find, at the latest LSN value: this means
 * we may resurrect pages of deleted items, as page deletion doesn't write leaf
 * pages and salvage will read and instantiate the contents of an old version of
 * the deleted page.
 *
 * The leaf page array is sorted in key order, and secondarily on LSN: what this
 * means is that for each new key range, the first page we find is the best page
 * for that key.   The process is to walk forward from each page until we reach
 * a page with a starting key after the current page's stopping key.
 *
 * For each of page, check to see if they overlap the current page's key range.
 * If they do, resolve the overlap.  Because WiredTiger rarely splits pages,
 * overlap resolution usually means discarding a page because the key ranges
 * are the same, and one of the pages is simply an old version of the other.
 *
 * However, it's possible more complex resolution is necessary.  For example,
 * here's an improbably complex list of page ranges and LSNs:
 *
 *	Page	Range	LSN
 *	 30	 A-G	 3
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *	 33	 C-F	 6
 *	 34	 C-D	 7
 *	 35	 F-M	 8
 *	 36	 H-O	 9
 *
 * We walk forward from each page reviewing all other pages in the array that
 * overlap the range.  For each overlap, the current or the overlapping
 * page is updated so the page with the most recent information for any range
 * "owns" that range.  Here's an example for page 30.
 *
 * Review page 31: because page 31 has the range C-D and a higher LSN than page
 * 30, page 30 would "split" into two ranges, A-C and E-G, conceding the C-D
 * range to page 31.  The new track element would be inserted into array with
 * the following result:
 *
 *	Page	Range	LSN
 *	 30	 A-C	 3		<< Changed WT_TRACK element
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *	 33	 C-F	 6
 *	 34	 C-D	 7
 *	 30	 E-G	 3		<< New WT_TRACK element
 *	 35	 F-M	 8
 *	 36	 H-O	 9
 *
 * Continue the review of the first element, using its new values.
 *
 * Review page 32: because page 31 has the range B-C and a higher LSN than page
 * 30, page 30's A-C range would be truncated, conceding the B-C range to page
 * 32.
 *	 30	 A-B	 3
 *		 E-G	 3
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *	 33	 C-F	 6
 *	 34	 C-D	 7
 *
 * Review page 33: because page 33 has a starting key (C) past page 30's ending
 * key (B), we stop evaluating page 30's A-B range, as there can be no further
 * overlaps.
 *
 * This process is repeated for each page in the array.
 *
 * When page 33 is processed, we'd discover that page 33's C-F range overlaps
 * page 30's E-G range, and page 30's E-G range would be updated, conceding the
 * E-F range to page 33.
 *
 * This is not computationally expensive because we don't walk far forward in
 * the leaf array because it's sorted by starting key, and because WiredTiger
 * splits are rare, the chance of finding the kind of range overlap requiring
 * re-sorting the array is small.
 */
static int
__slvg_range_col(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	uint32_t i, j;

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 *
	 * Walk the page array looking for overlapping key ranges, adjusting
	 * the ranges based on the LSN until there are no overlaps.
	 *
	 * DO NOT USE POINTERS INTO THE ARRAY: THE ARRAY IS RE-SORTED IN PLACE
	 * AS ENTRIES ARE SPLIT, SO ARRAY REFERENCES MUST ALWAYS BE ARRAY BASE
	 * PLUS OFFSET.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;

		/*
		 * Mark the overflow pages this page references -- as soon as
		 * we're done with the page array, we'll free overflow pages
		 * that are known not to be referenced.
		 */
		if (ss->pages[i]->ovfl_cnt != 0)
			__slvg_trk_ovfl_ref(ss->pages[i], ss);

		/* Check for pages that overlap our page. */
		for (j = i + 1; j < ss->pages_next; ++j) {
			/*
			 * We're done if this page starts after our stop, no
			 * subsequent pages can overlap our page.
			 */
			if (ss->pages[j]->u.col.range_start >
			    ss->pages[i]->u.col.range_stop)
				break;

			/* There's an overlap, fix it up. */
			WT_RET(__slvg_range_overlap_col(session, i, j, ss));
			ss->range_merge = 1;
		}
	}
	return (0);
}

/*
 * __slvg_range_overlap_col --
 *	Two column-store key ranges overlap, deal with it.
 */
static int
__slvg_range_overlap_col(
    WT_SESSION_IMPL *session, uint32_t a_slot, uint32_t b_slot, WT_STUFF *ss)
{
	WT_TRACK *a_trk, *b_trk, *new;
	uint32_t i, j;

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 */

	a_trk = ss->pages[a_slot];
	b_trk = ss->pages[b_slot];

	/*
	 * The key ranges of two WT_TRACK pages in the array overlap -- choose
	 * the ranges we're going to take from each.
	 *
	 * We can think of the overlap possibilities as 11 different cases:
	 *
	 *		AAAAAAAAAAAAAAAAAA
	 * #1		BBBBBBBBBBBBBBBBBB		pages are the same
	 * #2	BBBBBBBBBBBBB				overlaps the beginning
	 * #3			BBBBBBBBBBBBBBBB	overlaps the end
	 * #4		BBBBB				B is a prefix of A
	 * #5			BBBBBB			B is middle of A
	 * #6			BBBBBBBBBB		B is a suffix of A
	 *
	 * and:
	 *
	 *		BBBBBBBBBBBBBBBBBB
	 * #7			AAAAAAAAAAAAAAAA	same as #2
	 * #8	AAAAAAAAAAAAA				same as #3
	 * #9		AAAAA				A is a prefix of B
	 * #10			AAAAAAAAAA		A is a suffix of B
	 * #11			AAAAAA			A is middle of B
	 *
	 * Because the leaf page array was sorted by record number and a_trk
	 * appears earlier in that array than b_trk, cases #2/7, #10 and #11
	 * are impossible.
	 *
	 * Finally, there's one additional complicating factor -- final ranges
	 * are assigned based on the page's LSN.
	 */
	if (a_trk->u.col.range_start == b_trk->u.col.range_start) {
		/*
		 * Case #1, #4 and #9.
		 *
		 * The secondary sort of the leaf page array was the page's LSN,
		 * in high-to-low order, which means a_trk has a higher LSN, and
		 * is more desirable, than b_trk.  In cases #1 and #4 and #9,
		 * where the start of the range is the same for the two pages,
		 * this simplifies things, it guarantees a_trk has a higher LSN
		 * than b_trk.
		 */
		if (a_trk->u.col.range_stop >= b_trk->u.col.range_stop)
			/*
			 * Case #1, #4: a_trk is a superset of b_trk, and a_trk
			 * is more desirable -- discard b_trk.
			 */
			goto delete;

		/*
		 * Case #9: b_trk is a superset of a_trk, but a_trk is more
		 * desirable: keep both but delete a_trk's key range from
		 * b_trk.
		 */
		b_trk->u.col.range_start = a_trk->u.col.range_stop + 1;
		F_SET(b_trk, WT_TRACK_MERGE);
		ss->range_merge = 1;
		return (0);
	}

	if (a_trk->u.col.range_stop == b_trk->u.col.range_stop) {
		/* Case #6. */
		if (a_trk->lsn > b_trk->lsn)
			/*
			 * Case #6: a_trk is a superset of b_trk and a_trk is
			 * more desirable -- discard b_trk.
			 */
			goto delete;

		/*
		 * Case #6: a_trk is a superset of b_trk, but b_trk is more
		 * desirable: keep both but delete b_trk's key range from a_trk.
		 */
		a_trk->u.col.range_stop = b_trk->u.col.range_start - 1;
		F_SET(a_trk, WT_TRACK_MERGE);
		ss->range_merge = 1;
		return (0);
	}

	if  (a_trk->u.col.range_stop < b_trk->u.col.range_stop) {
		/* Case #3/8. */
		if (a_trk->lsn > b_trk->lsn) {
			/*
			 * Case #3/8: a_trk is more desirable, delete a_trk's
			 * key range from b_trk;
			 */
			b_trk->u.col.range_start = a_trk->u.col.range_stop + 1;
			F_SET(b_trk, WT_TRACK_MERGE);
		} else {
			/*
			 * Case #3/8: b_trk is more desirable, delete b_trk's
			 * key range from a_trk;
			 */
			a_trk->u.col.range_stop = b_trk->u.col.range_start - 1;
			F_SET(a_trk, WT_TRACK_MERGE);
		}
		ss->range_merge = 1;
		return (0);
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->lsn > b_trk->lsn) {
delete:		WT_RET(__slvg_free_trk(session, &ss->pages[b_slot], 1));
		return (0);
	}

	/*
	 * Case #5: b_trk is more desirable and is a middle chunk of a_trk.
	 * Split a_trk into two parts, the key range before b_trk and the
	 * key range after b_trk.
	 */
	WT_RET(__wt_calloc_def(session, 1, &new));
	WT_TRACK_INIT(ss, new, a_trk->lsn, a_trk->addr, a_trk->size);

	new->u.col.range_start = b_trk->u.col.range_stop + 1;
	new->u.col.range_stop = a_trk->u.col.range_stop;
	F_SET(new, WT_TRACK_NO_FB | WT_TRACK_MERGE);

	a_trk->u.col.range_stop = b_trk->u.col.range_start - 1;
	F_SET(a_trk, WT_TRACK_MERGE);

	/* Re-allocate the array of pages, as necessary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/*
	 * Figure out where this new entry goes.  (It's going to be close by,
	 * but it could be a few slots away.)  Then shift all entries sorting
	 * greater than the new entry up by one slot, and insert the new entry.
	 */
	for (i = a_slot + 1; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;
		if (__slvg_trk_compare(&new, &ss->pages[i]) <= 0)
			break;
	}
	for (j = ss->pages_next; j > i; --j)
		ss->pages[j] = ss->pages[j - 1];
	ss->pages[i] = new;
	++ss->pages_next;
	return (0);
}

/*
 * __slvg_build_internal_col --
 *	Build a column-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_build_internal_col(
    WT_SESSION_IMPL *session, uint32_t leaf_cnt, WT_STUFF *ss)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_REF *root_page;
	WT_TRACK *trk;
	uint32_t i;
	int ret;

	root_page = &session->btree->root_page;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)leaf_cnt, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = NULL;				/* Root page */
	page->parent_ref = root_page;
	page->read_gen = 0;
	page->u.col_int.recno = 1;
	page->entries = leaf_cnt;
	page->type = WT_PAGE_COL_INT;
	WT_PAGE_SET_MODIFIED(page);

	/* Reference this page from the root of the tree. */
	root_page->state = WT_REF_MEM;
	root_page->addr = WT_ADDR_INVALID;
	root_page->size = 0;
	root_page->page = page;

	for (cref = page->u.col_int.t, i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		cref->recno = trk->u.col.range_start;
		WT_COL_REF_ADDR(cref) = trk->addr;
		WT_COL_REF_SIZE(cref) = trk->size;
		WT_COL_REF_STATE(cref) = WT_REF_DISK;

		/*
		 * If the page's key range is unmodified from when we read it
		 * (in other words, we didn't merge part of this page with
		 * another page), we can use the page without change.  If we
		 * did merge with another page, we must build a page reflecting
		 * the updated key range.
		 *
		 * If we merged pages in this salvage, the list of referenced
		 * overflow pages might be incorrect, that is, when we did the
		 * original check for unused overflow pages, it was based on
		 * the assumption that there wouldn't be any need to merge.  If
		 * we did have to merge, we may have marked some overflow pages
		 * as referenced, and they are are no longer referenced because
		 * the referencing keys were discarded from the key range.
		 *
		 * If we're not going to merge this page, do the check here;
		 * else, the build-a-page function does the check for us, based
		 * on the final list of keys included in the merged, salvaged
		 * page.
		 */
		if (F_ISSET(trk, WT_TRACK_MERGE))
			WT_ERR(__slvg_build_leaf_col(
			    session, trk, page, cref, ss));
		else
			if (ss->range_merge &&
			    ss->page_type == WT_PAGE_COL_VAR)
				__slvg_trk_ovfl_ref(trk, ss);
		++cref;
	}

	/* Write the internal page to disk. */
	return (__wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED));

err:	if (page->u.col_int.t != NULL)
		__wt_free(session, page->u.col_int.t);
	if (page != NULL)
		__wt_free(session, page);
	return (ret);
}

/*
 * __slvg_build_leaf_col --
 *	Build a column-store leaf page for a merged page.
 */
static int
__slvg_build_leaf_col(WT_SESSION_IMPL *session,
    WT_TRACK *trk, WT_PAGE *parent, WT_COL_REF *cref, WT_STUFF *ss)
{
	WT_COL *cip, *save_col_leaf;
	WT_PAGE *page;
	int ret;
	uint32_t i, n_repeat, skip, take, save_entries;
	void *cipdata;

	/* Get the original page, including the full in-memory setup. */
	WT_RET(__wt_page_in(session, parent, &cref->ref, 0));
	page = WT_COL_REF_PAGE(cref);
	save_col_leaf = page->u.col_leaf.d;
	save_entries = page->entries;

	/*
	 * Calculate the number of K/V entries we are going to skip, and
	 * the total number of K/V entries we'll take from this page.
	 */
	skip = (uint32_t)(trk->u.col.range_start - page->u.col_leaf.recno);
	take = (uint32_t)(trk->u.col.range_stop - trk->u.col.range_start) + 1;
	page->u.col_leaf.recno = trk->u.col.range_start;

	switch (page->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
		/*
		 * Adjust the page information to "see" only keys we care about.
		 *
		 * Each WT_COL entry in a variable-length or fixed-length column
		 * store maps to a single K/V pair.  Adjust the in-memory values
		 * to reference only the K/V pairs we care about.
		 */
		page->u.col_leaf.d += skip;
		page->entries = take;

		/*
		 * If we have to merge pages, the list of referenced overflow
		 * pages might be incorrect, that is, when we did the original
		 * pass to detect unused overflow pages, we may have flagged
		 * some pages as referenced which are no longer referenced as
		 * those keys were discarded from their key range.  Check again.
		 */
		if (page->type == WT_PAGE_COL_VAR)
			__slvg_ovfl_col_inmem_ref(page, ss);
		break;
	case WT_PAGE_COL_RLE:
		/*
		 * Adjust the page information to "see" only keys we care about.
		 */
		WT_COL_FOREACH(page, cip, i) {
			cipdata = WT_COL_PTR(page, cip);
			n_repeat = WT_RLE_REPEAT_COUNT(cipdata);
			if (skip > 0) {
				/*
				 * Initially, walk forward to the RLE entry that
				 * has our first record number; adjust its count
				 * count to reflect the right number of records.
				 */
				if (n_repeat <= skip) {
					++page->u.col_leaf.d;
					skip -= n_repeat;
					continue;
				}

				/*
				 * Switch to counting the records we're going
				 * to keep.
				 */
				n_repeat -= skip;
				WT_RLE_REPEAT_COUNT(cipdata) = n_repeat;
				skip = 0;
			}

			/*
			 * Next, walk forward to the RLE entry that has our last
			 * record number; adjust its count to reflect the right
			 * number of records.
			 */
			if (n_repeat <= take) {
				take -= n_repeat;
				continue;
			}
			WT_RLE_REPEAT_COUNT(cipdata) = take;
			break;
		}
		page->entries = WT_COL_SLOT(page, cip);
		break;
	}

	/*
	 * Write the new version of the leaf page to disk.
	 *
	 * We can't discard the original blocks associated with this page now.
	 * (The problem is we don't want to overwrite any original information
	 * until the salvage run succeeds -- if we free the blocks now, the next
	 * merge page we write might allocate those blocks and overwrite them,
	 * and should the salvage run eventually fail, the original information
	 * would have been lost.)  Clear the reference addr so reconciliation
	 * does not free the underlying blocks.
	 */
	WT_PAGE_SET_MODIFIED(page);
	WT_COL_REF_ADDR(cref) = WT_ADDR_INVALID;
	WT_COL_REF_SIZE(cref) = 0;
	ret = __wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED | WT_REC_SALVAGE);

	/*
	 * Reset the page.  (Don't reset the record number or RLE counts -- it
	 * doesn't matter at the moment.)
	 */
	page->u.col_leaf.d = save_col_leaf;
	page->entries = save_entries;

	/* Discard our hazard reference and the page. */
	__wt_hazard_clear(session, page);
	__wt_page_free(session, page, 0);

	return (ret);
}

/*
 * __slvg_ovfl_col_inmem_ref --
 *	Mark overflow pages referenced from this column-store page.
 */
static void
__slvg_ovfl_col_inmem_ref(WT_PAGE *page, WT_STUFF *ss)
{
	WT_CELL *cell;
	WT_COL *cip;
	WT_OFF ovfl;
	WT_TRACK **searchp;
	uint32_t i;

	WT_COL_FOREACH(page, cip, i) {
		cell = WT_COL_PTR(page, cip);
		if (__wt_cell_type(cell) == WT_CELL_DATA_OVFL) {
			__wt_cell_off(cell, &ovfl);
			searchp = bsearch(
			    &ovfl, ss->ovfl, ss->ovfl_next,
			    sizeof(WT_TRACK *), __slvg_ovfl_compare);
			F_SET(*searchp, WT_TRACK_OVFL_REFD);
		}
	}
}

/*
 * __slvg_range_row --
 *	Figure out the leaf pages we need and discard everything else.  At the
 * same time, tag the overflow pages they reference.
 */
static int
__slvg_range_row(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_BTREE *btree;
	uint32_t i, j;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	func = btree->btree_compare;

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 *
	 * Walk the page array looking for overlapping key ranges, adjusting
	 * the ranges based on the LSN until there are no overlaps.
	 *
	 * DO NOT USE POINTERS INTO THE ARRAY: THE ARRAY IS RE-SORTED IN PLACE
	 * AS ENTRIES ARE SPLIT, SO ARRAY REFERENCES MUST ALWAYS BE ARRAY BASE
	 * PLUS OFFSET.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;

		/*
		 * Mark the overflow pages this page references -- as soon as
		 * we're done with the page array, we'll free overflow pages
		 * that are known not to be referenced.
		 */
		if (ss->pages[i]->ovfl_cnt != 0)
			__slvg_trk_ovfl_ref(ss->pages[i], ss);

		/* Check for pages that overlap our page. */
		for (j = i + 1; j < ss->pages_next; ++j) {
			/*
			 * We're done if this page starts after our stop, no
			 * subsequent pages can overlap our page.
			 */
			if (func(btree,
			    (WT_ITEM *)&ss->pages[j]->u.row.range_start,
			    (WT_ITEM *)&ss->pages[i]->u.row.range_stop) > 0)
				break;

			/* There's an overlap, fix it up. */
			WT_RET(__slvg_range_overlap_row(session, i, j, ss));
			ss->range_merge = 1;
		}
	}
	return (0);
}

/*
 * __slvg_range_overlap_row --
 *	Two row-store key ranges overlap, deal with it.
 */
static int
__slvg_range_overlap_row(
    WT_SESSION_IMPL *session, uint32_t a_slot, uint32_t b_slot, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_TRACK *a_trk, *b_trk, *new;
	uint32_t i, j;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 */

	btree = session->btree;
	func = btree->btree_compare;

	a_trk = ss->pages[a_slot];
	b_trk = ss->pages[b_slot];

	/*
	 * The key ranges of two WT_TRACK pages in the array overlap -- choose
	 * the ranges we're going to take from each.
	 *
	 * We can think of the overlap possibilities as 11 different cases:
	 *
	 *		AAAAAAAAAAAAAAAAAA
	 * #1		BBBBBBBBBBBBBBBBBB		pages are the same
	 * #2	BBBBBBBBBBBBB				overlaps the beginning
	 * #3			BBBBBBBBBBBBBBBB	overlaps the end
	 * #4		BBBBB				B is a prefix of A
	 * #5			BBBBBB			B is middle of A
	 * #6			BBBBBBBBBB		B is a suffix of A
	 *
	 * and:
	 *
	 *		BBBBBBBBBBBBBBBBBB
	 * #7			AAAAAAAAAAAAAAAA	same as #2
	 * #8	AAAAAAAAAAAAA				same as #3
	 * #9		AAAAA				A is a prefix of B
	 * #10			AAAAAAAAAA		A is a suffix of B
	 * #11			AAAAAA			A is middle of B
	 *
	 * Because the leaf page array was sorted by record number and a_trk
	 * appears earlier in that array than b_trk, cases #2/7, #10 and #11
	 * are impossible.
	 *
	 * Finally, there's one additional complicating factor -- final ranges
	 * are assigned based on the page's LSN.
	 */
#define	A_TRK_START	((WT_ITEM *)&a_trk->u.row.range_start)
#define	A_TRK_STOP	((WT_ITEM *)&a_trk->u.row.range_stop)
#define	B_TRK_START	((WT_ITEM *)&b_trk->u.row.range_start)
#define	B_TRK_STOP	((WT_ITEM *)&b_trk->u.row.range_stop)
#define	A_TRK_STOP_BUF	(&a_trk->u.row.range_stop)
#define	B_TRK_START_BUF	(&b_trk->u.row.range_start)
#define	B_TRK_STOP_BUF	(&b_trk->u.row.range_stop)
	if (func(btree, A_TRK_START, B_TRK_START) == 0) {
		/*
		 * Case #1, #4 and #9.
		 *
		 * The secondary sort of the leaf page array was the page's LSN,
		 * in high-to-low order, which means a_trk has a higher LSN, and
		 * is more desirable, than b_trk.  In cases #1 and #4 and #9,
		 * where the start of the range is the same for the two pages,
		 * this simplifies things, it guarantees a_trk has a higher LSN
		 * than b_trk.
		 */
		if (func(btree, A_TRK_STOP, B_TRK_STOP) >= 0)
			/*
			 * Case #1, #4: a_trk is a superset of b_trk, and a_trk
			 * is more desirable -- discard b_trk.
			 */
			goto delete;

		/*
		 * Case #9: b_trk is a superset of a_trk, but a_trk is more
		 * desirable: keep both but delete a_trk's key range from
		 * b_trk.
		 */
		WT_RET(
		    __slvg_key_copy(session, B_TRK_START_BUF, A_TRK_STOP_BUF));
		F_SET(b_trk, WT_TRACK_CHECK_START | WT_TRACK_MERGE);
		ss->range_merge = 1;
		return (0);
	}

	if (func(btree, A_TRK_STOP, B_TRK_STOP) == 0) {
		/* Case #6. */
		if (a_trk->lsn > b_trk->lsn)
			/*
			 * Case #6: a_trk is a superset of b_trk and a_trk is
			 * more desirable -- discard b_trk.
			 */
			goto delete;

		/*
		 * Case #6: a_trk is a superset of b_trk, but b_trk is more
		 * desirable: keep both but delete b_trk's key range from a_trk.
		 */
		WT_RET(
		    __slvg_key_copy(session, A_TRK_STOP_BUF, B_TRK_START_BUF));
		F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);
		ss->range_merge = 1;
		return (0);
	}

	if  (func(btree, A_TRK_STOP, B_TRK_STOP) < 0) {
		/* Case #3/8. */
		if (a_trk->lsn > b_trk->lsn) {
			/*
			 * Case #3/8: a_trk is more desirable, delete a_trk's
			 * key range from b_trk;
			 */
			WT_RET(__slvg_key_copy(
			    session, B_TRK_START_BUF, A_TRK_STOP_BUF));
			F_SET(b_trk, WT_TRACK_CHECK_START | WT_TRACK_MERGE);
		} else {
			/*
			 * Case #3/8: b_trk is more desirable, delete b_trk's
			 * key range from a_trk;
			 */
			WT_RET(__slvg_key_copy(
			    session, A_TRK_STOP_BUF, B_TRK_START_BUF));
			F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);
		}
		ss->range_merge = 1;
		return (0);
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->lsn > b_trk->lsn) {
delete:		WT_RET(__slvg_free_trk(session, &ss->pages[b_slot], 1));
		return (0);
	}

	/*
	 * Case #5: b_trk is more desirable and is a middle chunk of a_trk.
	 * Split a_trk into two parts, the key range before b_trk and the
	 * key range after b_trk.
	 */
	WT_RET(__wt_calloc_def(session, 1, &new));
	WT_TRACK_INIT(ss, new, a_trk->lsn, a_trk->addr, a_trk->size);
	WT_RET(
	    __slvg_key_copy(session, &new->u.row.range_start, B_TRK_STOP_BUF));
	WT_RET(
	    __slvg_key_copy(session, &new->u.row.range_stop, A_TRK_STOP_BUF));
	F_SET(new, WT_TRACK_CHECK_START | WT_TRACK_NO_FB | WT_TRACK_MERGE);

	WT_RET(__slvg_key_copy(session, A_TRK_STOP_BUF, B_TRK_START_BUF));
	F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);

	/* Re-allocate the array of pages, as necessary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/*
	 * Figure out where this new entry goes.  (It's going to be close by,
	 * but it could be a few slots away.)  Then shift all entries sorting
	 * greater than the new entry up by one slot, and insert the new entry.
	 */
	for (i = a_slot + 1; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;
		if (__slvg_trk_compare(&new, &ss->pages[i]) <= 0)
			break;
	}
	for (j = ss->pages_next; j > i; --j)
		ss->pages[j] = ss->pages[j - 1];
	ss->pages[i] = new;
	++ss->pages_next;
	return (0);
}

/*
 * __slvg_build_internal_row --
 *	Build a row-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_build_internal_row(
    WT_SESSION_IMPL *session, uint32_t leaf_cnt, WT_STUFF *ss)
{
	WT_PAGE *page;
	WT_ROW_REF *rref;
	WT_REF *root_page;
	WT_TRACK *trk;

	uint32_t i;
	int deleted, ret;

	root_page = &session->btree->root_page;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)leaf_cnt, &page->u.row_int.t));

	/* Fill it in. */
	page->parent = NULL;				/* Root page */
	page->parent_ref = root_page;
	page->read_gen = 0;
	page->entries = leaf_cnt;
	page->type = WT_PAGE_ROW_INT;
	WT_PAGE_SET_MODIFIED(page);

	/* Reference this page from the root of the tree. */
	root_page->state = WT_REF_MEM;
	root_page->addr = WT_ADDR_INVALID;
	root_page->size = 0;
	root_page->page = page;

	for (rref = page->u.row_int.t, i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		WT_ROW_REF_ADDR(rref) = trk->addr;
		WT_ROW_REF_SIZE(rref) = trk->size;
		WT_ROW_REF_STATE(rref) = WT_REF_DISK;

		/*
		 * If the page's key range is unmodified from when we read it
		 * (in other words, we didn't merge part of this page with
		 * another page), we can use the page without change.  If we
		 * did merge with another page, we must build a page reflecting
		 * the updated key range.
		 *
		 * If we merged pages in this salvage, the list of referenced
		 * overflow pages might be incorrect, that is, when we did the
		 * original check for unused overflow pages, it was based on
		 * the assumption that there wouldn't be any need to merge.  If
		 * we did have to merge, we may have marked some overflow pages
		 * as referenced, and they are are no longer referenced because
		 * the referencing keys were discarded from the key range.
		 *
		 * If we're not going to merge this page, do the check here;
		 * else, the build-a-page function does the check for us, based
		 * on the final list of keys included in the merged, salvaged
		 * page.
		 */
		if (F_ISSET(trk, WT_TRACK_MERGE)) {
			WT_ERR(__slvg_build_leaf_row(
			    session, trk, page, rref, ss, &deleted));

			/*
			 * If we take none of the keys, we don't need the page;
			 * fix up the count of entries we're creating.
			 */
			if (deleted) {
				--page->entries;
				continue;
			}
		} else {
			WT_ERR(__wt_row_ikey_alloc(session, 0,
			    trk->u.row.range_start.data,
			    trk->u.row.range_start.size,
			    (WT_IKEY **)&rref->key));
			if (ss->range_merge)
				__slvg_trk_ovfl_ref(trk, ss);
		}
		++rref;
	}

	/* Write the internal page to disk. */
	return (__wt_page_reconcile(
	    session, page, 0, WT_REC_EVICT | WT_REC_LOCKED));

err:	if (page->u.row_int.t != NULL)
		__wt_free(session, page->u.row_int.t);
	if (page != NULL)
		__wt_free(session, page);
	return (ret);
}

/*
 * __slvg_build_leaf_row --
 *	Build a row-store leaf page for a merged page.
 */
static int
__slvg_build_leaf_row(WT_SESSION_IMPL *session, WT_TRACK *trk,
    WT_PAGE *parent, WT_ROW_REF *rref, WT_STUFF *ss, int *deletedp)
{
	WT_BTREE *btree;
	WT_BUF *key;
	WT_IKEY *ikey;
	WT_ITEM *item, _item;
	WT_PAGE *page;
	WT_ROW *rip;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *), ret;
	uint32_t i, skip_start, skip_stop;

	btree = session->btree;
	func = btree->btree_compare;
	*deletedp = 0;

	/* Allocate temporary space in which to instantiate the keys. */
	WT_RET(__wt_scr_alloc(session, 0, &key));

	/* Get the original page, including the full in-memory setup. */
	WT_ERR(__wt_page_in(session, parent, &rref->ref, 0));
	page = WT_ROW_REF_PAGE(rref);

	/*
	 * Figure out how many entries we want to take and how many we want to
	 * skip.  If we're checking the starting range key, keys on the page
	 * must be greater-than the range key; if we're checking the stopping
	 * range key, keys on the page must be less-than the range key.  This
	 * is because we took a key from another page to define the the start
	 * or stop of this page's range, and so that page owns the "equal to"
	 * range space.
	 *
	 * If we're checking the starting range key, we may need to skip leading
	 * records on the page.  In the column-store code we set the in-memory
	 * WT_COL reference and we're done because the column-store leaf page
	 * reconciliation doesn't care about the on-disk information.  Row-store
	 * is harder because row-store leaf page reconciliation copies the key
	 * values from the original on-disk page.  Building a copy of the page
	 * and reconciling it is going to be a lot of work -- for now, drill a
	 * hole into reconciliation and pass in a value that skips some number
	 * of initial page records.
	 */
	skip_start = skip_stop = 0;
	if (F_ISSET(trk, WT_TRACK_CHECK_START))
		WT_ROW_FOREACH(page, rip, i) {
			if (__wt_off_page(page, rip->key)) {
				ikey = rip->key;
				_item.data = WT_IKEY_DATA(ikey);
				_item.size = ikey->size;
				item = &_item;
			} else {
				WT_ERR(__wt_row_key(session, page, rip, key));
				item = (WT_ITEM *)key;
			}

			if  (func(btree,
			    item, (WT_ITEM *)&trk->u.row.range_start) > 0)
				break;
			++skip_start;
		}
	if (F_ISSET(trk, WT_TRACK_CHECK_STOP))
		WT_ROW_FOREACH_REVERSE(page, rip, i) {
			if (__wt_off_page(page, rip->key)) {
				ikey = rip->key;
				_item.data = WT_IKEY_DATA(ikey);
				_item.size = ikey->size;
				item = &_item;
			} else {
				WT_ERR(__wt_row_key(session, page, rip, key));
				item = (WT_ITEM *)key;
			}
			if  (func(btree,
			    item, (WT_ITEM *)&trk->u.row.range_stop) < 0)
				break;
			++skip_stop;
		}

	/*
	 * Because the starting/stopping keys are boundaries, not exact matches,
	 * we may have just decided not to take all of the keys on the page, or
	 * none of them.
	 *
	 * If we take none of the keys, all we have to do is tell our caller to
	 * not include this leaf page in the internal page it's building.
	 */
	if (skip_start + skip_stop >= page->entries)
		*deletedp = 1;
	else {
		/*
		 * Change the page to reflect the new record count: there is no
		 * need to copy anything on the page itself, the entries value
		 * limits the number of items on the page.
		 */
		page->entries -= skip_stop;

		/*
		 * If we have to merge pages, the list of referenced overflow
		 * pages might be incorrect, that is, when we did the original
		 * pass to detect unused overflow pages, we may have flagged
		 * some pages as referenced which are no longer referenced as
		 * those keys were discarded from their key range.  Check again.
		 */
		__slvg_ovfl_row_inmem_ref(page, skip_start, ss);

		/*
		 * If we take all of the keys, we don't write the page and we
		 * clear the merge flags so that the underlying blocks are not
		 * later freed (for merge pages re-written into the file, the
		 * underlying blocks have to be freed -- well, this page never
		 * gets written, so don't free the blocks).
		 */
		if (skip_start == 0 && skip_stop == 0)
			F_CLR(trk, WT_TRACK_MERGE);
		else {
			/*
			 * Write the new version of the leaf page to disk.
			 *
			 * We can't discard the original blocks associated with
			 * this page now.  (The problem is we don't want to
			 * overwrite any original information until the salvage
			 * run succeeds -- if we free the blocks now, the next
			 * merge page we write might allocate those blocks and
			 * overwrite them, and should the salvage run eventually
			 * fail, the original information would have been lost.)
			 * Clear the reference addr so reconciliation does not
			 * free the underlying blocks.
			 */
			WT_PAGE_SET_MODIFIED(page);
			WT_ROW_REF_ADDR(rref) = WT_ADDR_INVALID;
			WT_ROW_REF_SIZE(rref) = 0;
			ret = __wt_page_reconcile(session,
			    page, skip_start,
			    WT_REC_EVICT | WT_REC_LOCKED | WT_REC_SALVAGE);
			page->entries += skip_stop;
		}

		/*
		 * Take a copy of this page's first key to define the start of
		 * its range.  The key may require processing, otherwise, it's
		 * a copy from the page.
		 */
		rip = page->u.row_leaf.d + skip_start;
		if (__wt_off_page(page, rip->key)) {
			ikey = rip->key;
			WT_ERR(__wt_row_ikey_alloc(session, 0,
			    WT_IKEY_DATA(ikey), ikey->size,
			    (WT_IKEY **)&rref->key));
		} else {
			WT_ERR(__wt_row_key(session, page, rip, key));
			WT_ERR(__wt_row_ikey_alloc(session, 0,
			    key->data, key->size, (WT_IKEY **)&rref->key));
		}
	}

	/* Discard our hazard reference and the page. */
	__wt_hazard_clear(session, page);
	__wt_page_free(session, page, 0);

err:	__wt_scr_release(&key);

	return (ret);
}

/*
 * __slvg_ovfl_row_inmem_ref --
 *	Mark overflow pages referenced from this row-store page.
 */
static void
__slvg_ovfl_row_inmem_ref(WT_PAGE *page, uint32_t skip_start, WT_STUFF *ss)
{
	WT_CELL *cell;
	WT_ROW *rip;
	WT_OFF ovfl;
	WT_TRACK **searchp;
	uint32_t i;

	WT_ROW_FOREACH(page, rip, i) {
		/* Skip any leading keys on the page we're not keeping. */
		if (skip_start != 0) {
			--skip_start;
			continue;
		}

		if (__wt_off_page(page, rip->key))
			cell = WT_REF_OFFSET(
			    page, ((WT_IKEY *)rip->key)->cell_offset);
		else
			cell = rip->key;
		if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL) {
			__wt_cell_off(cell, &ovfl);
			searchp =
			    bsearch(&ovfl, ss->ovfl, ss->ovfl_next,
			    sizeof(WT_TRACK *), __slvg_ovfl_compare);
			F_SET(*searchp, WT_TRACK_OVFL_REFD);
		}
		if ((cell = __wt_row_value(page, rip)) != NULL &&
		    __wt_cell_type(cell) == WT_CELL_DATA_OVFL) {
			__wt_cell_off(cell, &ovfl);
			searchp =
			    bsearch(&ovfl, ss->ovfl, ss->ovfl_next,
			    sizeof(WT_TRACK *), __slvg_ovfl_compare);
			F_SET(*searchp, WT_TRACK_OVFL_REFD);
		}
	}
}

/*
 * __slvg_trk_ovfl_ref --
 *	Review the overflow pages a WT_TRACK entry references.
 */
static void
__slvg_trk_ovfl_ref(WT_TRACK *trk, WT_STUFF *ss)
{
	WT_TRACK **searchp;
	uint32_t i;

	/*
	 * Review the overflow pages referenced by the argument page: if we find
	 * the overflow page in the overflow page array and it's the right size,
	 * mark it as referenced.
	 *
	 * If we don't find the overflow page, or we find it and it's the wrong
	 * size, it means there is corruption: mark the page for cleanup.
	 *
	 * Fasttrack the corruption where we reference overflow pages but didn't
	 * find them.
	 */
	if (ss->ovfl_next == 0) {
		for (i = 0; i < trk->ovfl_cnt; ++i)
			F_SET(trk, WT_TRACK_OVFL_MISSING);
		return;
	}
	for (i = 0; i < trk->ovfl_cnt; ++i) {
		searchp =
		    bsearch(&trk->ovfl[i], ss->ovfl, ss->ovfl_next,
		    sizeof(WT_TRACK *), __slvg_ovfl_compare);
		if (searchp == NULL || (*searchp)->size != trk->ovfl[i].size)
			F_SET(trk, WT_TRACK_OVFL_MISSING);
		else
			F_SET(*searchp, WT_TRACK_OVFL_REFD);
	}
}

/*
 * __slvg_ovfl_compare --
 *	Bsearch comparison routine for the overflow array.
 */
static int
__slvg_ovfl_compare(const void *a, const void *b)
{
	WT_OFF *ovfl;
	WT_TRACK *entry;

	ovfl = (WT_OFF *)a;
	entry = *(WT_TRACK **)b;

	return (ovfl->addr > entry->addr ? 1 :
	    ((ovfl->addr < entry->addr) ? -1 : 0));
}

/*
 * __slvg_trk_compare --
 *	Sort a WT_TRACK array by key, and secondarily, by LSN.
 */
static int
__slvg_trk_compare(const void *a, const void *b)
{
	WT_BTREE *btree;
	WT_TRACK *a_trk, *b_trk;
	uint64_t a_lsn, a_recno, b_lsn, b_recno;
	int cmp;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	switch (a_trk->ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		a_recno = a_trk->u.col.range_start;
		b_recno = b_trk->u.col.range_start;
		if (a_recno == b_recno)
			break;
		if (a_recno > b_recno)
			return (1);
		if (a_recno < b_recno)
			return (-1);
		break;
	case WT_PAGE_ROW_LEAF:
		btree = a_trk->ss->btree;
		if ((cmp = btree->btree_compare(btree,
		    (WT_ITEM *)&a_trk->u.row.range_start,
		    (WT_ITEM *)&b_trk->u.row.range_start)) != 0)
			return (cmp);
		break;
	}

	/*
	 * If the primary keys compare equally, differentiate based on LSN.
	 * We sort from highest LSN to lowest, that is, the earlier pages in
	 * the array are more desirable.
	 */
	a_lsn = a_trk->lsn;
	b_lsn = b_trk->lsn;
	return (a_lsn > b_lsn ? -1 : (a_lsn < b_lsn ? 1 : 0));
}

/*
 * __slvg_key_copy --
 *	Copy a WT_TRACK start/stop key to another WT_TRACK start/stop key.
 */
static int
__slvg_key_copy(WT_SESSION_IMPL *session, WT_BUF *dst, WT_BUF *src)
{
	return (__wt_buf_set(session, dst, src->data, src->size));
}

/*
 * __slvg_discard_ovfl --
 *	Discard any overflow pages that are never referenced.
 */
static int
__slvg_discard_ovfl(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_TRACK *trk, **p, **t;
	uint32_t i, discard;

	/*
	 * Walk the overflow page array -- if an overflow page isn't referenced,
	 * we have no use for it.   Collapse the array as we go, we search it
	 * again when we create the internal pages, and the smaller it is, the
	 * faster that search will go.
	 */
	discard = 0;
	for (i = 0, p = t = ss->ovfl; i < ss->ovfl_next; ++i, ++t) {
		trk = *t;
		if (F_ISSET(trk, WT_TRACK_OVFL_REFD)) {
			F_CLR(trk, WT_TRACK_OVFL_REFD);
			*p++ = trk;
		} else {
			WT_RET(__slvg_free_trk(session, t, 1));
			++discard;
		}
	}

	ss->ovfl_next -= discard;
	return (0);
}

/*
 * __slvg_free_merge_block --
 *	Free any file blocks that had to be merged back into the free-list.
 */
static int
__slvg_free_merge_block(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	for (i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;
		if (F_ISSET(trk, WT_TRACK_MERGE) &&
		    !F_ISSET(trk, WT_TRACK_NO_FB))
			WT_RET(__slvg_free_trk(session, &ss->pages[i], 1));
	}

	return (0);
}

/*
 * __slvg_free --
 *	Discard memory allocated to the page and overflow arrays.
 */
static int
__slvg_free(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	uint32_t i;

	/* Discard the leaf page array. */
	for (i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			WT_RET(__slvg_free_trk(session, &ss->pages[i], 0));
	__wt_free(session, ss->pages);

	/* Discard the overflow page array. */
	for (i = 0; i < ss->ovfl_next; ++i)
		if (ss->ovfl[i] != NULL)
			WT_RET(__slvg_free_trk(session, &ss->ovfl[i], 0));
	__wt_free(session, ss->ovfl);

	return (0);
}

/*
 * __slvg_free_trk --
 *	Discard a WT_TRACK structure and (optionally) its underlying blocks.
 */
static int
__slvg_free_trk(WT_SESSION_IMPL *session, WT_TRACK **trkp, int blocks)
{
	WT_TRACK *trk;

	trk = *trkp;
	*trkp = NULL;

	if (blocks)
		WT_RET(__wt_block_free(session, trk->addr, trk->size));

	if (trk->ss->page_type == WT_PAGE_ROW_LEAF) {
		__wt_buf_free(session, &trk->u.row.range_start);
		__wt_buf_free(session, &trk->u.row.range_stop);
	}

	__wt_free(session, trk->ovfl);
	__wt_free(session, trk);

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_trk_dump --
 *	Dump out the sorted track information.
 */
void
__wt_trk_dump(const char *l, void *ss_arg)
{
	WT_STUFF *ss;
	WT_TRACK *trk;
	uint32_t i;
	void (*func)(WT_TRACK *);

	ss = ss_arg;

	if (l != NULL)
		fprintf(stderr, "%s: ", l);
	fprintf(stderr, "salvage page track list\n");
	func = ss->page_type == WT_PAGE_ROW_LEAF ?
	    __slvg_trk_dump_row : __slvg_trk_dump_col;
	for (i = 0; i < ss->pages_next; ++i)
		if ((trk = ss->pages[i]) != NULL)
			func(trk);

	fprintf(stderr, "overflow page track list: ");
	for (i = 0; i < ss->ovfl_next; ++i) {
		trk = ss->ovfl[i];
		fprintf(stderr,
		    "%" PRIu32 "/%" PRIu32 " ", trk->addr, trk->size);
	}
	fprintf(stderr, "\n");
}

/*
 * __slvg_trk_dump_col --
 *	Dump a column-store WT_TRACK structure.
 */
static void
__slvg_trk_dump_col(WT_TRACK *trk)
{
	fprintf(stderr, "%6" PRIu32 "/%-6" PRIu32 " (%" PRIu64 ")"
	    "\t%" PRIu64 "-%" PRIu64 "\n",
	    trk->addr, trk->size, trk->lsn,
	    trk->u.col.range_start, trk->u.col.range_stop);
}

/*
 * __slvg_trk_dump_row --
 *	Dump a row-store WT_TRACK structure.
 */
static void
__slvg_trk_dump_row(WT_TRACK *trk)
{
	fprintf(stderr, "%6" PRIu32 "/%-6" PRIu32 " (%" PRIu64 ")\n"
	    "\t%.*s\n\t%.*s\n",
	    trk->addr, trk->size, trk->lsn,
	    trk->u.row.range_start.size, (char *)trk->u.row.range_start.data,
	    trk->u.row.range_stop.size, (char *)trk->u.row.range_stop.data);
}
#endif
