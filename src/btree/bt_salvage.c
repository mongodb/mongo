/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

struct __wt_stuff; 		typedef struct __wt_stuff WT_STUFF;
struct __wt_track; 		typedef struct __wt_track WT_TRACK;

/*
 * There's a bunch of stuff we pass around during salvage, group it together
 * to make the code prettier.
 */
struct __wt_stuff {
	BTREE	  *btree;			/* Enclosing Btree */

	WT_TRACK **pages;			/* Pages */
	uint32_t   pages_next;			/* Next empty slot */
	uint32_t   pages_allocated;		/* Bytes allocated */

	WT_TRACK **ovfl;			/* Overflow pages */
	uint32_t   ovfl_next;			/* Next empty slot */
	uint32_t   ovfl_allocated;		/* Bytes allocated */

	uint8_t    page_type;			/* Page type */

	int	   range_merge;			/* If merged key ranges */

	void (*f)(const char *, uint64_t);	/* Progress callback */
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
	 * Pages that reference overflow pages contain a list of the pages
	 * they reference.
	 */
	WT_OVFL *ovfl;				/* Referenced overflow pages */
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

#define	WT_TRACK_FREE_BLOCKS	0x01		/* Free page's blocks */
#define	WT_TRACK_MERGE		0x02		/* Page requires merging */
#define	WT_TRACK_OVFL_MISSING	0x04		/* Overflow page missing */
#define	WT_TRACK_OVFL_REFD	0x08		/* Overflow page referenced */
	uint32_t flags;
};

static int  __slvg_build_internal_col(SESSION *, WT_STUFF *);
static int  __slvg_build_leaf_col(SESSION *, WT_TRACK *, WT_PAGE *, WT_COL_REF *, WT_STUFF *);
static void __slvg_col_ovfl_reference(WT_PAGE *, WT_STUFF *);
static int  __slvg_discard_ovfl(SESSION *, WT_STUFF *);
static int  __slvg_free(SESSION *, WT_STUFF *);
static int  __slvg_free_merge_block(SESSION *, WT_STUFF *);
static int  __slvg_free_trk_col(SESSION *, WT_TRACK **, int);
static int  __slvg_free_trk_ovfl(SESSION *, WT_TRACK **, int);
static int  __slvg_free_trk_row(SESSION *, WT_TRACK **, int);
static int  __slvg_ovfl_compare(const void *, const void *);
static int  __slvg_ovfl_srch_col(SESSION *, WT_PAGE_DISK *, WT_TRACK *);
static int  __slvg_ovfl_srch_row(SESSION *, WT_PAGE_DISK *, WT_TRACK *);
static int  __slvg_range_col(SESSION *, WT_STUFF *);
static int  __slvg_range_overlap_col(SESSION *, uint32_t, uint32_t, WT_STUFF *);
static int  __slvg_read(SESSION *, WT_STUFF *);
static int  __slvg_track_compare(const void *, const void *);
static int  __slvg_track_leaf(SESSION *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static int  __slvg_track_ovfl(SESSION *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static void __slvg_trk_ovfl_reference(SESSION *, WT_TRACK *, WT_STUFF *);

#ifdef HAVE_DIAGNOSTIC
static void __slvg_track_dump(const char *, WT_STUFF *);
#endif

/*
 * __wt_btree_salvage --
 *	Salvage a Btree.
 */
int
__wt_btree_salvage(SESSION *session, void (*f)(const char *, uint64_t))
{
	BTREE *btree;
	WT_STUFF stuff;
	off_t len;
	uint32_t allocsize;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_CLEAR(stuff);
	stuff.btree = btree;
	stuff.page_type = WT_PAGE_INVALID;
	stuff.f = f;

	/*
	 * Clear any existing free list -- presumably it's garbage, otherwise
	 * nobody would be calling salvage.
	 */
	__wt_block_discard(session);

	/* Truncate the file to our minimal allocation size unit. */
	allocsize = btree->allocsize;
	len = (btree->fh->file_size / allocsize) * allocsize;
	if (len != btree->fh->file_size)
		WT_ERR(__wt_ftruncate(session, btree->fh, len));

	/* If the file doesn't have any pages, we're done. */
	if (btree->fh->file_size <= WT_PAGE_DESC_SIZE) {
		__wt_errx(session, "file is too small to salvage");
		ret = WT_ERROR;
		goto err;
	}

	/* Build the page list. */
	WT_ERR(__slvg_read(session, &stuff));

	/*
	 * If the file has no valid pages, we should probably not touch it,
	 * somebody gave us a random file.
	 */
	if (stuff.pages_next == 0) {
		__wt_errx(session,
		    "file has no valid pages and cannot be salvaged");
		ret = WT_ERROR;
		goto err;
	}

#if 0
	__slvg_track_dump("after read", &stuff);
#endif

	/*
	 * Sort the page list by key, and secondarily, by LSN.
	 *
	 * We don't have to sort the overflow array because we inserted the
	 * records into the array in file addr order.
	 */
	qsort(stuff.pages, (size_t)stuff.pages_next,
	    sizeof(WT_TRACK *), __slvg_track_compare);

#if 0
	__slvg_track_dump("after sort", &stuff);
#endif

	/*
	 * Figure out the leaf pages we need and discard everything else.  At
	 * the same time, tag the overflow pages they reference.
	 */
	switch (stuff.page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		WT_ERR(__slvg_range_col(session, &stuff));
		break;
	case WT_PAGE_ROW_LEAF:
		// WT_ERR(__slvg_range_row(session, &stuff));
		break;
	}

#if 0
	__slvg_track_dump("after merge", &stuff);
#endif

	/*
	 * Discard any overflow pages that aren't referenced, and clear the
	 * reference count -- it was just a guess, we have to figure it out
	 * for real as we write the internal page.
	 */
	WT_ERR(__slvg_discard_ovfl(session, &stuff));

	/* Build the internal page that references all of these pages. */
	switch (stuff.page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		WT_ERR(__slvg_build_internal_col(session, &stuff));
		break;
	case WT_PAGE_ROW_LEAF:
		// WT_ERR(__slvg_build_internal_row(session, &stuff));
		break;
	}

	/*
	 * If we had to merge key ranges, we have to do a final pass through
	 * the leaf page array and discard file pages used during merges.
	 * We can't do it earlier: if we free'd the file pages we're merging,
	 * then the write of the newly created internal page might allocate
	 * those file blocks, and if the write of the newly created internal
	 * page partially fails, we've potentially overwritten pages that
	 * were used to construct our final key range.  In other words, if the
	 * salvage run fails, we don't want to corrupt data the next salvage
	 * run will need.
	 */
	 if (stuff.range_merge)
		WT_ERR(__slvg_free_merge_block(session, &stuff));

	/*
	 * If we have to merge key ranges, the list of referenced overflow
	 * pages might be incorrect, that is, when we did the original pass to
	 * detect unused overflow pages, we may have flagged some pages as
	 * referenced which are no longer referenced after those keys were
	 * discarded from their key range.  We reset the referenced overflow
	 * page list when merging key ranges, and so now we have a correct
	 * list.  Free any unused overflow pages.
	 */
	 if (stuff.range_merge)
		WT_ERR(__slvg_discard_ovfl(session, &stuff));

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

err:	/* Wrap up reporting. */
	if (stuff.f != NULL)
		stuff.f(session->name, stuff.fcnt);

	/* Free allocated memory. */
	WT_TRET(__slvg_free(session, &stuff));

	return (ret);
}

/*
 * __slvg_read --
 *	Read the file and build a table of the pages we can use.
 */
static int
__slvg_read(SESSION *session, WT_STUFF *ss)
{
	BTREE *btree;
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
	off = allocsize;
	max = fh->file_size;
	while (off < max) {
		/* Report progress every 10 reads. */
		if (ss->f != NULL && ++ss->fcnt % 10 == 0)
			ss->f(session->name, ss->fcnt);

		addr = (uint32_t)(off / allocsize);

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
		    size > WT_BTREE_PAGE_SIZE_MAX || off + size > max)
			goto skip_allocsize;

		/* The page size isn't insane, read the entire page. */
		if (size > t->mem_size)
			WT_ERR(__wt_buf_grow(session, t, size));
		WT_ERR(__wt_read(session, fh, off, size, t->mem));
		dsk = t->mem;

		/* Verify the checksum. */
		checksum = dsk->checksum;
		dsk->checksum = 0;
		if (checksum != __wt_cksum(dsk, size))
			goto skip_allocsize;

		/*
		 * Verify the page -- it's unexpected if the page has a valid
		 * checksum but doesn't verify, but it's technically possible.
		 */
		if (__wt_verify_dsk_page(session, dsk, addr, size)) {
skip_allocsize:		WT_RET(__wt_block_free(session, addr, allocsize));
			off += allocsize;
			continue;
		}

		/* Move past this page. */
		off += size;

		/*
		 * We have a valid page -- make sure it's an expected page type
		 * for the file.
		 *
		 * We only care about leaf and overflow pages from here on out;
		 * discard all of the others.
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
			WT_ERR(__slvg_track_leaf(session, dsk, addr, ss));
			break;
		case WT_PAGE_OVFL:
			WT_ERR(__slvg_track_ovfl(session, dsk, addr, ss));
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
 * __slvg_track_leaf --
 *	Track a leaf page.
 */
static int
__slvg_track_leaf(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, WT_STUFF *ss)
{
	BTREE *btree;
	WT_CELL *cell, *last_key_cell;
	WT_TRACK *trk;
	uint64_t stop_recno;
	uint32_t i;
	uint8_t *p;
	int ret;

	btree = session->btree;
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
			WT_ERR(__slvg_ovfl_srch_col(session, dsk, trk));
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
		 * Row-store format: instantiate and copy the first and last
		 * keys.  Additionally, row-store pages can contain overflow
		 * items.
		 */
		cell = WT_PAGE_DISK_BYTE(dsk);
		WT_ERR(__wt_cell_process(
		    session, cell, &trk->u.row.range_start));
		WT_CELL_FOREACH(dsk, cell, i)
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_KEY:
			case WT_CELL_KEY_OVFL:
				last_key_cell = cell;
				break;
			}
		WT_ERR(__wt_cell_process(
		    session, last_key_cell, &trk->u.row.range_stop));

		WT_ERR(__slvg_ovfl_srch_row(session, dsk, trk));
		break;
	}

	WT_TRACK_INIT(ss, trk, dsk->lsn, addr, dsk->size);
	ss->pages[ss->pages_next++] = trk;

	if (0) {
err:		if (trk != NULL)
			__wt_free(session, trk);
	}
	return (ret);
}

/*
 * __slvg_track_ovfl --
 *	Track an overflow page.
 */
static int
__slvg_track_ovfl(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, WT_STUFF *ss)
{
	WT_TRACK *trk;

	/*
	 * Reallocate the overflow page array as necessary, then save the
	 * page's location information.
	 */
	if (ss->ovfl_next * sizeof(WT_TRACK *) == ss->ovfl_allocated)
		WT_RET(__wt_realloc(session, &ss->ovfl_allocated,
		   (ss->ovfl_next + 1000) * sizeof(WT_TRACK *), &ss->ovfl));

	WT_RET(__wt_calloc_def(session, sizeof(WT_TRACK), &trk));

	WT_TRACK_INIT(ss, trk, dsk->lsn, addr, dsk->size);
	ss->ovfl[ss->ovfl_next++] = trk;

	return (0);
}

/*
 * __slvg_ovfl_srch_col --
 *	Search a column-store page for overflow items.
 */
static int
__slvg_ovfl_srch_col(SESSION *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
{
	WT_CELL *cell;
	WT_OVFL *ovfl;
	uint32_t i, ovfl_cnt;

	/*
	 * Two passes: count the overflow items, then copy them into an
	 * allocated array.
	 */
	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL)
			++ovfl_cnt;
	if (ovfl_cnt == 0)
		return (0);

	WT_RET(__wt_calloc(session, ovfl_cnt, sizeof(WT_OVFL), &trk->ovfl));
	trk->ovfl_cnt = ovfl_cnt;

	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL) {
			ovfl = WT_CELL_BYTE_OVFL(cell);
			trk->ovfl[ovfl_cnt].addr = ovfl->addr;
			trk->ovfl[ovfl_cnt].size = ovfl->size;
			++ovfl_cnt;
		}
	return (0);
}

/*
 * __slvg_ovfl_srch_row --
 *	Search a row-store page for overflow items.
 */
static int
__slvg_ovfl_srch_row(SESSION *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
{
	WT_CELL *cell;
	WT_OVFL *ovfl;
	uint32_t i, ovfl_cnt;

	/*
	 * Two passes: count the overflow items, then copy them into an
	 * allocated array.
	 */
	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_DATA_OVFL:
			++ovfl_cnt;
			break;
		}
	if (ovfl_cnt == 0)
		return (0);

	WT_RET(__wt_calloc(session, ovfl_cnt, sizeof(WT_OVFL), &trk->ovfl));
	trk->ovfl_cnt = ovfl_cnt;

	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, i)
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_DATA_OVFL:
			ovfl = WT_CELL_BYTE_OVFL(cell);
			trk->ovfl[ovfl_cnt].addr = ovfl->addr;
			trk->ovfl[ovfl_cnt].size = ovfl->size;
			++ovfl_cnt;
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
 * range to page 31.
 *
 *	 30	 A-C	 3
 *		 E-G	 3	[new WT_TRACK element]
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *
 * Review page 32: because page 31 has the range B-C and a higher LSN than page
 * 30, page 30's A-C range would be truncated, conceding the B-C range to page
 * 32.
 *	 30	 A-B	 3
 *		 E-G	 3
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *	 33	 C-F	 6
 *
 * Review page 33: because page 33 has a starting key (C) past page 30's ending
 * key (B), we stop evaluating page 30's A-B range, because there can be no
 * further overlaps.
 *
 * This process is repeated for each page through the array.  If new elements
 * are created, they are sorted into the correct spot in the array, at the time
 * they are created, and then we proceed.  In this example, page 30's newly
 * created E-G range would be sorted into the array:
 *
 *	 30	 A-C	 3
 *	 31	 C-D	 4
 *	 32	 B-C	 5
 *	 33	 C-F	 6
 *	 34	 C-D	 7
 *	 30	 E-G	 3
 *	 35	 F-M	 8
 *	 36	 H-O	 9
 *
 * When page 33 was processed, we'd discover that page 33's C-F range overlaps
 * page 30's E-G range, and page 30's E-G range would be updated, conceding the
 * E-F range to page 33.
 *
 * And so on, and so forth.
 *
 * This is not computationally expensive because we don't walk far forward in
 * the leaf array because it's sorted by starting key, and because WiredTiger
 * splits are rare, the chance of finding the kind of range overlap requiring
 * re-sorting the array is small.
 *
 * Finally, for each page from which we're going to salvage a key, mark the
 * overflow pages it references -- as soon as we're done with the leaf array,
 * we're going to free any overflow pages that are known not to be referenced.
 */
static int
__slvg_range_col(SESSION *session, WT_STUFF *ss)
{
	uint32_t i, j;

	/*
	 * Walk the page array looking for overlapping key ranges, adjusting
	 * the ranges based on the LSN until there are no overlaps.
	 *
	 * !!!
	 * DO NOT USE POINTERS into the array: the array is re-sorted in place
	 * as entries are split, so array references must always be array base
	 * plus offset.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;

		/* Mark the overflow pages this page references. */
		if (ss->pages[i]->ovfl_cnt != 0)
			__slvg_trk_ovfl_reference(session, ss->pages[i], ss);

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
    SESSION *session, uint32_t a_slot, uint32_t b_slot, WT_STUFF *ss)
{
	WT_TRACK *a_trk, *b_trk, *new;
	uint32_t i, j;

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
		F_SET(b_trk, WT_TRACK_FREE_BLOCKS | WT_TRACK_MERGE);
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
		a_trk->u.col.range_stop = b_trk->u.col.range_stop - 1;
		F_SET(a_trk, WT_TRACK_FREE_BLOCKS | WT_TRACK_MERGE);
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
			F_SET(b_trk, WT_TRACK_FREE_BLOCKS | WT_TRACK_MERGE);
		} else {
			/*
			 * Case #3/8: b_trk is more desirable, delete b_trk's
			 * key range from a_trk;
			 */
			a_trk->u.col.range_stop = b_trk->u.col.range_start - 1;
			F_SET(a_trk, WT_TRACK_FREE_BLOCKS | WT_TRACK_MERGE);
		}
		ss->range_merge = 1;
		return (0);
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->lsn > b_trk->lsn) {
delete:		F_SET(b_trk, WT_TRACK_FREE_BLOCKS);
		WT_RET(__slvg_free_trk_col(session, &ss->pages[b_slot], 1));
		return (0);
	}

	/*
	 * Case #5: b_trk is more desirable and is a middle chunk of a_trk.
	 * Split a_trk into two parts, the key range before b_trk and the
	 * key range after b_trk.  We have to sort the new element into the
	 * correct place in our tree -- it would be pretty easy to bubble it
	 * into place, but I'm not bothering.
	 */
	WT_RET(__wt_calloc_def(session, 1, &new));
	WT_TRACK_INIT(ss, new, a_trk->lsn, a_trk->addr, a_trk->size);

	new->u.col.range_start = b_trk->u.col.range_stop + 1;
	new->u.col.range_stop = a_trk->u.col.range_stop;
	F_SET(new, WT_TRACK_MERGE);
	a_trk->u.col.range_stop = b_trk->u.col.range_start - 1;
	F_SET(a_trk, WT_TRACK_FREE_BLOCKS | WT_TRACK_MERGE);

	/* Re-allocate the array of pages, as necessary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/*
	 * Figure out where this new entry goes.  (It's going to be close by,
	 * but it could be a few slots away.)  Then shift all entries sorting
	 * greater than the new entry up by one slot, and insert the new entry.
	 */
	for (i = a_slot + 1; i < ss->pages_next; ++i)
		if (__slvg_track_compare(&new, &ss->pages[i]) <= 0)
			break;
	for (j = ss->pages_next; j > i; --j)
		ss->pages[j] = ss->pages[j - 1];
	ss->pages[i] = new;
	++ss->pages_next;

	ss->range_merge = 1;
	return (0);
}

/*
 * __slvg_build_internal_col --
 *	Build a column-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_build_internal_col(SESSION *session, WT_STUFF *ss)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_TRACK *trk;
	uint32_t i, leaf_cnt;
	int ret;

	/*
	 * Count how many slots we need (we could track this during the array
	 * shuffling/splitting, but I didn't bother.
	 */
	for (leaf_cnt = i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			++leaf_cnt;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)leaf_cnt, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = NULL;				/* Root page */
	page->parent_ref = NULL;
	page->read_gen = 0;
	page->u.col_int.recno = 1;
	page->addr = WT_ADDR_INVALID;
	page->size = 0;
	page->indx_count = leaf_cnt;
	page->type = WT_PAGE_COL_INT;
	WT_PAGE_SET_MODIFIED(page);

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
				__slvg_trk_ovfl_reference(session, trk, ss);
		++cref;
	}

	/* Write the internal page to disk. */
	return (__wt_page_reconcile(session, page, 1));

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
__slvg_build_leaf_col(SESSION *session,
    WT_TRACK *trk, WT_PAGE *parent, WT_COL_REF *cref, WT_STUFF *ss)
{
	WT_PAGE *page;
	uint32_t r_take, r_skip;
	int ret;

	/* Get the original page, including the full in-memory setup. */
	WT_RET(__wt_page_in(session, parent, &cref->ref, 0));
	page = WT_COL_REF_PAGE(cref);

	/*
	 * Create a new set of entries that only reference the keys we
	 * care about.
	 */
	r_take = (uint32_t)(trk->u.col.range_stop - trk->u.col.range_start) + 1;
	r_skip = (uint32_t)(trk->u.col.range_start - page->u.col_leaf.recno);

	switch (page->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
		/*
		 * Variable-length and fixed-length column stores are easy
		 * because each WT_COL entry maps to a single record -- we
		 * can calculate how many references to copy.
		 */
		memcpy(page->u.col_leaf.d,
		    page->u.col_leaf.d + r_skip, r_take * sizeof(WT_COL));
		page->indx_count = r_take;
		page->u.col_leaf.recno = trk->u.col.range_start;
		WT_PAGE_SET_MODIFIED(page);

		/*
		 * If we have to merge pages, the list of referenced overflow
		 * pages might be incorrect, that is, when we did the original
		 * pass to detect unused overflow pages, we may have flagged
		 * some pages as referenced which are no longer referenced as
		 * those keys were discarded from their key range.  Check again.
		 */
		if (page->type == WT_PAGE_COL_VAR)
			__slvg_col_ovfl_reference(page, ss);
		break;
	case WT_PAGE_COL_RLE:
		/* Harder... have to walk the records... */
		/* CAN THIS OVERFLOW A uint32_t? */
		break;
	}

	/*
	 * Write the leaf page to disk, and discard our hazard reference.
	 * We don't want to discard the original blocks associated with
	 * this page, so we set the addr so the reconciliation code will
	 * ignore it.
	 */
	page->addr = WT_ADDR_INVALID;
	ret = __wt_page_reconcile(session, page, 1);

	__wt_hazard_clear(session, page);

	return (ret);
}

/*
 * __slvg_col_ovfl_reference --
 *	Mark overflow pages referenced from this column-store page.
 */
static void
__slvg_col_ovfl_reference(WT_PAGE *page, WT_STUFF *ss)
{
	WT_CELL *cell;
	WT_COL *cip;
	WT_OVFL *ovfl;
	WT_TRACK **searchp;
	uint32_t i;

	WT_COL_INDX_FOREACH(page, cip, i) {
		cell = WT_COL_PTR(page, cip);
		if (WT_CELL_TYPE(cell) != WT_CELL_DATA_OVFL)
			continue;
		ovfl = WT_CELL_BYTE_OVFL(cell);
		searchp =
		    bsearch(&ovfl, ss->ovfl, ss->ovfl_next,
		    sizeof(WT_TRACK *), __slvg_ovfl_compare);
		F_SET(*searchp, WT_TRACK_OVFL_REFD);
	}
}

/*
 * __slvg_trk_ovfl_reference --
 *	Review the overflow pages a WT_TRACK entry references.
 */
static void
__slvg_trk_ovfl_reference(SESSION *session, WT_TRACK *trk, WT_STUFF *ss)
{
	BTREE *btree;
	WT_TRACK **searchp;
	uint32_t i;

	btree = session->btree;

	/*
	 * Review the overflow pages referenced by the argument page: if we find
	 * the overflow page in the overflow page array and it's the right size,
	 * mark it as referenced.
	 *
	 * If we don't find the overflow page, or we find it and it's the wrong
	 * size, it means there is corruption: mark the page for cleanup.
	 *
	 * Fasttrack the corruption where we reference overflow pages but didn't
	 * find them -- not that I care about performance, but I don't trust the
	 * library search function to get a search space of 0 correct.
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
		if (searchp == NULL || (*searchp)->size !=
		    WT_HDR_BYTES_TO_ALLOC(btree, trk->ovfl[i].size))
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
	WT_OVFL *ovfl;
	WT_TRACK *entry;

	ovfl = (WT_OVFL *)a;
	entry = *(WT_TRACK **)b;

	return (ovfl->addr > entry->addr ? 1 :
	    ((ovfl->addr < entry->addr) ? -1 : 0));
}

/*
 * __slvg_track_compare --
 *	Sort a WT_TRACK array by key, and secondarily, by LSN.
 */
static int
__slvg_track_compare(const void *a, const void *b)
{
	BTREE *btree;
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
		    &a_trk->u.row.range_start.item,
		    &b_trk->u.row.range_start.item)) != 0)
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
 * __slvg_discard_ovfl --
 *	Discard any overflow pages that are never referenced.
 */
static int
__slvg_discard_ovfl(SESSION *session, WT_STUFF *ss)
{
	WT_TRACK *trk, **p, **t;
	uint32_t i;

	/*
	 * Walk the overflow page array -- if an overflow page isn't referenced,
	 * we have no use for it.   Collapse the array as we go, we search it
	 * again when we create the internal pages, and the smaller it is, the
	 * faster that search will go.
	 */
	for (i = 0, p = t = ss->ovfl; i < ss->ovfl_next; ++i, ++t) {
		trk = *t;
		if (F_ISSET(trk, WT_TRACK_OVFL_REFD)) {
			F_CLR(trk, WT_TRACK_OVFL_REFD);
			*p++ = trk;
		} else
			WT_RET(__slvg_free_trk_ovfl(session, t, 1));
	}

	ss->ovfl_next = WT_PTRDIFF32(p, ss->ovfl);
	return (0);
}

/*
 * __slvg_free_merge_block --
 *	Free any file blocks that had to be merged back into the free-list.
 */
static int
__slvg_free_merge_block(SESSION *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		for (i = 0; i < ss->pages_next; ++i) {
			if ((trk = ss->pages[i]) == NULL)
				continue;
			if (F_ISSET(trk, WT_TRACK_FREE_BLOCKS))
				WT_RET(__slvg_free_trk_col(
				    session, &ss->pages[i], 1));
		}
		break;
	case WT_PAGE_ROW_LEAF:
		for (i = 0; i < ss->pages_next; ++i) {
			if ((trk = ss->pages[i]) == NULL)
				continue;
			if (F_ISSET(trk, WT_TRACK_FREE_BLOCKS))
				WT_RET(__slvg_free_trk_row(
				    session, &ss->pages[i], 1));
		}
	}

	return (0);
}

/*
 * __slvg_free --
 *	Discard memory allocated to the page and overflow arrays.
 */
static int
__slvg_free(SESSION *session, WT_STUFF *ss)
{
	uint32_t i;

	/* Discard the leaf page array. */
	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		for (i = 0; i < ss->pages_next; ++i)
			if (ss->pages[i] != NULL)
				WT_RET(__slvg_free_trk_col(
				    session, &ss->pages[i], 0));
		break;
	case WT_PAGE_ROW_LEAF:
		for (i = 0; i < ss->pages_next; ++i)
			if (ss->pages[i] != NULL)
				WT_RET(__slvg_free_trk_row(
				    session, &ss->pages[i], 0));
	}

	/* Discard the overflow page array. */
	for (i = 0; i < ss->ovfl_next; ++i)
		if (ss->ovfl[i] != NULL)
			WT_RET(__slvg_free_trk_ovfl(session, &ss->ovfl[i], 0));

	__wt_free(session, ss->pages);
	return (0);
}

/*
 * __slvg_free_trk_col --
 *	Discard a column-store WT_TRACK structure and all its resources.
 */
static int
__slvg_free_trk_col(SESSION *session, WT_TRACK **trkp, int blocks)
{
	WT_TRACK *trk;

	trk = *trkp;
	*trkp = NULL;

	if (blocks)
		WT_RET(__wt_block_free(session, trk->addr, trk->size));

	__wt_free(session, trk->ovfl);
	__wt_free(session, trk);

	return (0);
}

/*
 * __slvg_free_trk_row --
 *	Discard a row-store WT_TRACK structure and all its resources.
 */
static int
__slvg_free_trk_row(SESSION *session, WT_TRACK **trkp, int blocks)
{
	WT_TRACK *trk;

	trk = *trkp;
	*trkp = NULL;

	if (blocks)
		WT_RET(__wt_block_free(session, trk->addr, trk->size));

	__wt_buf_free(session, &trk->u.row.range_start);
	__wt_buf_free(session, &trk->u.row.range_stop);

	__wt_free(session, trk->ovfl);
	__wt_free(session, trk);

	return (0);
}

/*
 * __slvg_free_trk_ovfl --
 *	Discard a overflow WT_TRACK structure and all its resources.
 */
static int
__slvg_free_trk_ovfl(SESSION *session, WT_TRACK **trkp, int blocks)
{
	WT_TRACK *trk;

	trk = *trkp;
	*trkp = NULL;

	if (blocks)
		WT_RET(__wt_block_free(session, trk->addr, trk->size));

	__wt_free(session, trk->ovfl);
	__wt_free(session, trk);

	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __slvg_track_dump --
 *	Dump out the sorted track information.
 */
static void
__slvg_track_dump(const char *l, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	fprintf(stderr, "salvage page track list (%s):\n", l);

	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		for (i = 0; i < ss->pages_next; ++i) {
			if ((trk = ss->pages[i]) == NULL)
				continue;
			fprintf(stderr, "%6lu/%-6lu (%llu)\t%llu-%llu\n",
			    (u_long)trk->addr, (u_long)trk->size,
			    (unsigned long long)trk->lsn,
			    (unsigned long long)trk->u.col.range_start,
			    (unsigned long long)trk->u.col.range_stop);
		}
		break;
	case WT_PAGE_ROW_LEAF:
		for (i = 0; i < ss->pages_next; ++i) {
			if ((trk = ss->pages[i]) == NULL)
				continue;
			fprintf(stderr, "%6lu/%-6lu (%llu)\n\t%.*s\n\t%.*s\n",
			    (u_long)trk->addr, (u_long)trk->size,
			    (unsigned long long)trk->lsn,
			    trk->u.row.range_start.item.size,
			    (char *)trk->u.row.range_start.item.data,
			    trk->u.row.range_stop.item.size,
			    (char *)trk->u.row.range_stop.item.data);
		}
		break;
	}

	fprintf(stderr, "overflow page track list: ");
	for (i = 0; i < ss->ovfl_next; ++i) {
		trk = ss->ovfl[i];
		fprintf(stderr, "%lu/%lu ",
		    (u_long)trk->addr, (u_long)trk->size);
	}
	fprintf(stderr, "\n");
}
#endif
