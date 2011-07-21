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
	WT_BTREE  *btree;			/* Enclosing Btree */

	WT_TRACK **pages;			/* Pages */
	uint32_t   pages_next;			/* Next empty slot */
	uint32_t   pages_allocated;		/* Bytes allocated */

	WT_TRACK **ovfl;			/* Overflow pages */
	uint32_t   ovfl_next;			/* Next empty slot */
	uint32_t   ovfl_allocated;		/* Bytes allocated */

	uint8_t    page_type;			/* Page type */

	/* If need to free blocks backing merged page ranges. */
	int	   merge_free;

	int	   verbose;			/* If WT_VERB_SALVAGE set */
	WT_BUF	  *vbuf;			/* Verbose print buffer */

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
#undef	row_start
#define	row_start	u.row._row_start
			WT_BUF   _row_start;	/* Row-store start range */
#undef	row_stop
#define	row_stop	u.row._row_stop
			WT_BUF   _row_stop;	/* Row-store stop range */
		} row;

		struct {
#undef	col_start
#define	col_start	u.col._col_start
			uint64_t _col_start;	/* Col-store start range */
#undef	col_stop
#define	col_stop	u.col._col_stop
			uint64_t _col_stop;	/* Col-store stop range */
#undef	col_missing
#define	col_missing	u.col._col_missing
			uint64_t _col_missing;	/* Col-store missing range */
		} col;
	} u;

#define	WT_TRACK_CHECK_START	0x001		/* Initial key updated */
#define	WT_TRACK_CHECK_STOP	0x002		/* Last key updated */
#define	WT_TRACK_MERGE		0x004		/* Page requires merging */
#define	WT_TRACK_NO_FILE_BLOCKS	0x008		/* WT_TRACK w/o file blocks */
#define	WT_TRACK_OVFL_REFD	0x010		/* Overflow page referenced */
	uint32_t flags;
};

						/* Flags to __slvg_trk_free() */
#define	WT_TRK_FREE_BLOCKS	0x01		/* Free any blocks */
#define	WT_TRK_FREE_OVFL	0x02		/* Free any overflow pages */

static int  __slvg_cleanup(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_col_build_internal(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_col_build_leaf(
		WT_SESSION_IMPL *, WT_TRACK *, WT_PAGE *, WT_COL_REF *);
static int  __slvg_col_merge_ovfl(
		WT_SESSION_IMPL *, uint32_t, WT_PAGE *, uint64_t, uint64_t);
static int  __slvg_col_range(WT_SESSION_IMPL *, WT_STUFF *);
static void __slvg_col_range_missing(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_col_range_overlap(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static void __slvg_col_trk_update_start(uint32_t, WT_STUFF *);
static int  __slvg_load_byte_string(
		WT_SESSION_IMPL *, const uint8_t *, uint32_t, WT_BUF *);
static int  __slvg_merge_block_free(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_ovfl_compare(const void *, const void *);
static int  __slvg_ovfl_discard(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_ovfl_reconcile(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_read(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_row_build_internal(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_row_build_leaf(WT_SESSION_IMPL *,
		WT_TRACK *, WT_PAGE *, WT_ROW_REF *, WT_STUFF *, int *);
static int  __slvg_row_merge_ovfl(
		WT_SESSION_IMPL *, uint32_t, WT_PAGE *, uint32_t, uint32_t);
static int  __slvg_row_range(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_row_range_overlap(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static int  __slvg_row_trk_update_start(
		WT_SESSION_IMPL *, WT_ITEM *, uint32_t, WT_STUFF *);
static int  __slvg_trk_compare_key(const void *, const void *);
static int  __slvg_trk_compare_lsn(const void *, const void *);
static int  __slvg_trk_free(WT_SESSION_IMPL *, WT_TRACK **, uint32_t);
static int  __slvg_trk_leaf(
		WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static int  __slvg_trk_leaf_ovfl(WT_SESSION_IMPL *, WT_PAGE_DISK *, WT_TRACK *);
static int  __slvg_trk_ovfl(
		WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);

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
	uint32_t allocsize;
	int ret;

	WT_UNUSED(config);

	btree = session->btree;
	ret = 0;

	WT_CLEAR(stuff);
	ss = &stuff;
	ss->btree = btree;
	ss->page_type = WT_PAGE_INVALID;

	/* Allocate a buffer for printing strings. */
	if (FLD_ISSET(S2C(session)->verbose, WT_VERB_SALVAGE)) {
		ss->verbose = 1;
		WT_ERR(__wt_scr_alloc(session, 0, &ss->vbuf));
	}

	if (btree->fh->file_size > WT_BTREE_DESC_SECTOR) {
		/*
		 * Truncate the file to an initial sector plus N allocation size
		 * units (bytes trailing the last multiple of an allocation size
		 * unit must be garbage, by definition).
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
		    "the file contains no data pages and cannot be salvaged");
		goto err;
	}

	/*
	 * Step 1:
	 * Read the file and build in-memory structures that reference any leaf
	 * or overflow page that checksums correctly.  Any pages other than leaf
	 * or overflow pages are added to the free list.
	 */
	WT_ERR(__slvg_read(session, ss));
	if (ss->pages_next == 0) {
		__wt_errx(session,
		    "file has no valid pages and cannot be salvaged");
		goto err;
	}

	/*
	 * Step 2:
	 * Review the relationships between the pages and the overflow items.
	 *
	 * Discard any page referencing a non-existent overflow page.  We do
	 * this before checking overlapping key ranges on the grounds that a
	 * bad key range we can use is better than a terrific key range that
	 * references pages we don't have.
	 *
	 * An alternative would be to discard only the on-page item referencing
	 * the missing overflow item.  We're not doing that because: (1) absent
	 * corruption, a missing overflow item is a strong argument the page was
	 * replaced (but admittedly, corruption is probably why we're here); (2)
	 * it's a lot of work, and as WiredTiger supports very large page sizes,
	 * overflow items simply shouldn't be common.
	 *
	 * If an overflow page is referenced more than once, discard leaf pages
	 * with the lowest LSNs until overflow pages are only referenced once.
	 *
	 * This requires sorting the page list by LSN; we don't have to sort the
	 * overflow array because we inserted the records into the array in file
	 * addr order, which is the sort key.
	 */
	if (ss->ovfl_next != 0) {
		qsort(ss->pages, (size_t)ss->pages_next,
		    sizeof(WT_TRACK *), __slvg_trk_compare_lsn);
		WT_ERR(__slvg_ovfl_reconcile(session, ss));
	}

	/*
	 * Step 3:
	 * Add unreferenced overflow page blocks to the free list.
	 */
	WT_ERR(__slvg_ovfl_discard(session, ss));

	/*
	 * Step 4:
	 * Walk the list of pages looking for overlapping ranges to resolve.
	 * If we find a range that needs to be resolved, set a global flag
	 * and a per WT_TRACK flag on the pages requiring modification.
	 *
	 * This requires sorting the page list by key, and secondarily by LSN.
	 */
	qsort(ss->pages,
	    (size_t)ss->pages_next, sizeof(WT_TRACK *), __slvg_trk_compare_key);
	if (ss->page_type == WT_PAGE_ROW_LEAF)
		WT_ERR(__slvg_row_range(session, ss));
	else
		WT_ERR(__slvg_col_range(session, ss));

	/*
	 * Step 5:
	 * We may have lost key ranges in column-store databases, that is, some
	 * part of the record number space is gone.   Look for missing ranges.
	 */
	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
		__slvg_col_range_missing(session, ss);
		break;
	case WT_PAGE_ROW_LEAF:
		break;
	}

	/*
	 * Step 6:
	 * Build an internal page that references all of the leaf pages, and
	 * write it, as well as any merged pages, to the file.
	 */
	switch (ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
		WT_ERR(__slvg_col_build_internal(session, ss));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__slvg_row_build_internal(session, ss));
		break;
	}

	/*
	 * Step 7:
	 * If we had to merge key ranges, we have to do a final pass through
	 * the leaf page array and discard file pages used during key merges.
	 * We can't do it earlier: if we free'd the leaf pages we're merging as
	 * we merged them, the write of subsequent leaf pages or the internal
	 * page might allocate those free'd file blocks, and if the salvage run
	 * subsequently fails, we'd have overwritten pages used to construct the
	 * final key range.  In other words, if the salvage run fails, we don't
	 * want to overwrite data the next salvage run might need.
	 */
	 if (ss->merge_free)
		WT_ERR(__slvg_merge_block_free(session, ss));

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}

	/* Discard the leaf and overflow page memory. */
	WT_TRET(__slvg_cleanup(session, ss));

	/* Discard verbose print buffer. */
	if (ss->vbuf != NULL)
		__wt_scr_release(&ss->vbuf);

	/* Wrap up reporting. */
	__wt_progress(session, NULL, ss->fcnt);

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
			goto skip;

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
		if (checksum != __wt_cksum(dsk, size))
			goto skip;

		/*
		 * Verify the page: it's vanishingly unlikely a page could pass
		 * checksum and still be broken, but a degree of paranoia is
		 * healthy in salvage.  Regardless, verify does return failure
		 * here because it detects some failures we'd expect to see in
		 * a corrupted file, like overflow references past the end of
		 * the file.
		 */
		if (__wt_verify_dsk(session, dsk, addr, size, 1)) {
skip:			WT_VERBOSE(session, SALVAGE,
			    "skipping %" PRIu32 "B at file offset %" PRIu64,
			    allocsize, (uint64_t)off);

			WT_RET(__wt_block_free(session, addr, allocsize));
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
			WT_VERBOSE(session, SALVAGE,
			    "ignoring %s page [%" PRIu32 "/%" PRIu32 "]",
			    __wt_page_type_string(dsk->type), addr, size);

			WT_RET(__wt_block_free(session, addr, size));
			continue;
		}

		WT_VERBOSE(session, SALVAGE,
		    "tracking %s page [%" PRIu32 "/%" PRIu32 " @ %" PRIu64 "]",
		    __wt_page_type_string(dsk->type), addr, size, dsk->lsn);

		switch (dsk->type) {
		case WT_PAGE_COL_FIX:
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
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_PAGE *page;
	WT_TRACK *trk;
	uint64_t stop_recno;
	uint32_t i;
	int ret;

	unpack = &_unpack;
	page = NULL;
	trk = NULL;
	ret = 0;

	/* Re-allocate the array of pages, as necessary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/* Allocate a WT_TRACK entry for this new page and fill it in. */
	WT_ERR(__wt_calloc_def(session, 1, &trk));
	WT_TRACK_INIT(ss, trk, dsk->lsn, addr, dsk->size);

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Column-store fixed-sized format: start and stop keys can be
		 * taken from the WT_PAGE_DISK header, and they can't contain
		 * overflow items.
		 *
		 * Column-store variable-sized format: start and stop keys can
		 * be taken from the WT_PAGE_DISK header, but they can contain
		 * overflow items.
		 */
		trk->col_start = dsk->recno;
		trk->col_stop = dsk->recno + (dsk->u.entries - 1);

		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] records %" PRIu64 "-%" PRIu64,
		    addr, trk->col_start, trk->col_stop);
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Column-store RLE format: the start key can be taken from the
		 * WT_PAGE_DISK header, but the stop key requires walking the
		 * page.
		 */
		stop_recno = dsk->recno;
		WT_CELL_FOREACH(dsk, cell, unpack, i) {
			__wt_cell_unpack(cell, unpack);
			stop_recno += unpack->rle;
		}

		trk->col_start = dsk->recno;
		trk->col_stop = stop_recno - 1;

		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] records %" PRIu64 "-%" PRIu64,
		    addr, trk->col_start, trk->col_stop);

		/* Column-store pages can contain overflow items. */
		WT_ERR(__slvg_trk_leaf_ovfl(session, dsk, trk));
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
		    page, &page->u.row_leaf.d[0], &trk->row_start));
		WT_ERR(__wt_row_key(session,
		    page, &page->u.row_leaf.d[page->entries - 1],
		    &trk->row_stop));

		if (ss->verbose) {
			WT_ERR(__slvg_load_byte_string(session,
			    trk->row_start.data,
			    trk->row_start.size, ss->vbuf));
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] start key %.*s",
			    addr, (int)ss->vbuf->size, (char *)ss->vbuf->data);
			WT_ERR(__slvg_load_byte_string(session,
			    trk->row_stop.data,
			    trk->row_stop.size, ss->vbuf));
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] stop key %.*s",
			    addr, (int)ss->vbuf->size, (char *)ss->vbuf->data);
		}

		/* Row-store pages can contain overflow items. */
		WT_ERR(__slvg_trk_leaf_ovfl(session, dsk, trk));
		break;
	}
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
 * __slvg_trk_leaf_ovfl --
 *	Search a leaf page for overflow items.
 */
static int
__slvg_trk_leaf_ovfl(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i, ovfl_cnt;

	unpack = &_unpack;

	/*
	 * Two passes: count the overflow items, then copy them into an
	 * allocated array.
	 */
	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (unpack->ovfl)
			++ovfl_cnt;
	}
	if (ovfl_cnt == 0)
		return (0);

	WT_RET(__wt_calloc(session, ovfl_cnt, sizeof(WT_OFF), &trk->ovfl));
	trk->ovfl_cnt = ovfl_cnt;

	ovfl_cnt = 0;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (unpack->ovfl) {
			trk->ovfl[ovfl_cnt].addr = unpack->off.addr;
			trk->ovfl[ovfl_cnt].size = unpack->off.size;

			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] overflow reference [%" PRIu32
			    "/%" PRIu32 "]",
			    trk->addr, unpack->off.addr, unpack->off.size);

			if (++ovfl_cnt == trk->ovfl_cnt)
				break;
		}
	}

	return (0);
}

/*
 * __slvg_col_range --
 *	Figure out the leaf pages we need and free the leaf pages we don't.
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
__slvg_col_range(WT_SESSION_IMPL *session, WT_STUFF *ss)
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

		/* Check for pages that overlap our page. */
		for (j = i + 1; j < ss->pages_next; ++j) {
			if (ss->pages[j] == NULL)
				continue;
			/*
			 * We're done if this page starts after our stop, no
			 * subsequent pages can overlap our page.
			 */
			if (ss->pages[j]->col_start >
			    ss->pages[i]->col_stop)
				break;

			/* There's an overlap, fix it up. */
			WT_RET(__slvg_col_range_overlap(session, i, j, ss));
		}
	}
	return (0);
}

/*
 * __slvg_col_range_overlap --
 *	Two column-store key ranges overlap, deal with it.
 */
static int
__slvg_col_range_overlap(
    WT_SESSION_IMPL *session, uint32_t a_slot, uint32_t b_slot, WT_STUFF *ss)
{
	WT_TRACK *a_trk, *b_trk, *new;

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 */
	a_trk = ss->pages[a_slot];
	b_trk = ss->pages[b_slot];

	WT_VERBOSE(session, SALVAGE,
	    "[%" PRIu32 "] and [%" PRIu32 "] range overlap",
	    a_trk->addr, b_trk->addr);

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
	if (a_trk->col_start == b_trk->col_start) {
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
		if (a_trk->col_stop >= b_trk->col_stop)
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
		b_trk->col_start = a_trk->col_stop + 1;
		__slvg_col_trk_update_start(b_slot, ss);
		F_SET(b_trk, WT_TRACK_MERGE);
		goto merge;
	}

	if (a_trk->col_stop == b_trk->col_stop) {
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
		a_trk->col_stop = b_trk->col_start - 1;
		F_SET(a_trk, WT_TRACK_MERGE);
		goto merge;
	}

	if  (a_trk->col_stop < b_trk->col_stop) {
		/* Case #3/8. */
		if (a_trk->lsn > b_trk->lsn) {
			/*
			 * Case #3/8: a_trk is more desirable, delete a_trk's
			 * key range from b_trk;
			 */
			b_trk->col_start = a_trk->col_stop + 1;
			__slvg_col_trk_update_start(b_slot, ss);
			F_SET(b_trk, WT_TRACK_MERGE);
		} else {
			/*
			 * Case #3/8: b_trk is more desirable, delete b_trk's
			 * key range from a_trk;
			 */
			a_trk->col_stop = b_trk->col_start - 1;
			F_SET(a_trk, WT_TRACK_MERGE);
		}
		goto merge;
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->lsn > b_trk->lsn) {
delete:		WT_RET(__slvg_trk_free(session,
		    &ss->pages[b_slot], WT_TRK_FREE_BLOCKS | WT_TRK_FREE_OVFL));
		return (0);
	}

	/*
	 * Case #5: b_trk is more desirable and is a middle chunk of a_trk.
	 * Split a_trk into two parts, the key range before b_trk and the
	 * key range after b_trk.
	 *
	 * First, create a copy of the original page's WT_TRACK information
	 * (same LSN, addr and size), that we'll use to reference the key
	 * range at the end of a_trk.
	 */
	WT_RET(__wt_calloc_def(session, 1, &new));
	WT_TRACK_INIT(ss, new, a_trk->lsn, a_trk->addr, a_trk->size);

	/*
	 * Second, reallocate the array of pages if necessary, and then insert
	 * the new element into the array after the existing element (that's
	 * probably wrong, but we'll fix it up in a second).
	 */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));
	memmove(ss->pages + a_slot + 1, ss->pages + a_slot,
	    (ss->pages_next - a_slot) * sizeof(*ss->pages));
	ss->pages[a_slot + 1] = new;
	++ss->pages_next;

	/*
	 * Third, set its start key to be the first key after the stop key of
	 * the middle chunk (that's b_trk), and its stop key to be the stop key
	 * of the original chunk, and call __slvg_col_trk_update_start.  That
	 * function will re-sort the WT_TRACK array as necessary to move our
	 * new entry into the right sorted location.
	 */
	new->col_start = b_trk->col_stop + 1;
	new->col_stop = a_trk->col_stop;
	__slvg_col_trk_update_start(a_slot + 1, ss);

	/*
	 * Fourth, the new WT_TRACK information doesn't reference any file
	 * blocks (let the original a_trk structure reference file blocks).
	 */
	F_SET(new, WT_TRACK_MERGE | WT_TRACK_NO_FILE_BLOCKS);

	/*
	 * Finally, set the original WT_TRACK information to reference only
	 * the initial key space in the page, that is, everything up to the
	 * starting key of the middle chunk (that's b_trk).
	 */
	a_trk->col_stop = b_trk->col_start - 1;
	F_SET(a_trk, WT_TRACK_MERGE);

merge:	WT_VERBOSE(session, SALVAGE,
	    "[%" PRIu32 "] and [%" PRIu32 "] require merge",
	    a_trk->addr, b_trk->addr);
	return (0);
}

/*
 * __slvg_col_trk_update_start --
 *	Update a column-store page's start key after an overlap.
 */
static void
__slvg_col_trk_update_start(uint32_t slot, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	trk = ss->pages[slot];

	/*
	 * If we deleted an initial piece of the WT_TRACK name space, it may no
	 * longer be in the right location.
	 *
	 * For example, imagine page #1 has the key range 30-50, it split, and
	 * we wrote page #2 with key range 30-40, and page #3 key range with
	 * 40-50, where pages #2 and #3 have larger LSNs than page #1.  When the
	 * key ranges were sorted, page #2 came first, then page #1 (because of
	 * their earlier start keys than page #3), and page #2 came before page
	 * #1 because of its LSN.  When we resolve the overlap between page #2
	 * and page #1, we truncate the initial key range of page #1, and it now
	 * sorts after page #3, because it has the same starting key of 40, and
	 * a lower LSN.
	 *
	 * We have already updated b_trk's start key; what we may have to do is
	 * re-sort some number of elements in the list.
	 */
	for (i = slot + 1; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;
		if (ss->pages[i]->col_start > trk->col_stop)
			break;
	}
	i -= slot;
	if (i > 1)
		qsort(ss->pages + slot, (size_t)i,
		    sizeof(WT_TRACK *), __slvg_trk_compare_key);
}

/*
 * __slvg_col_range_missing --
 *	Detect missing ranges from column-store files.
 */
static void
__slvg_col_range_missing(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint64_t r;
	uint32_t i;

	for (i = 0, r = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;
		if (trk->col_start != r + 1) {
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] column-store missing range from %"
			    PRIu64 " to %" PRIu64 " inclusive",
			    trk->addr, r + 1, trk->col_start - 1);

			/*
			 * We need to instantiate deleted items for the missing
			 * record range.
			 */
			trk->col_missing = r + 1;
			F_SET(trk, WT_TRACK_MERGE);
		}
		r = trk->col_stop;
	}
}

/*
 * __slvg_col_build_internal --
 *	Build a column-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_col_build_internal(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_COL_REF *cref;
	WT_PAGE *page;
	WT_REF *root_page;
	WT_TRACK *trk;
	uint32_t i, leaf_cnt;
	int ret;

	root_page = &session->btree->root_page;

	/*
	 * Count how many internal page slots we need (we could track this
	 * during the array shuffling/splitting, but that's a lot harder).
	 */
	for (leaf_cnt = i = 0; i < ss->pages_next; ++i)
		if ((trk = ss->pages[i]) != NULL)
			++leaf_cnt;

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

		cref->recno = trk->col_start;
		WT_COL_REF_ADDR(cref) = trk->addr;
		WT_COL_REF_SIZE(cref) = trk->size;
		WT_COL_REF_STATE(cref) = WT_REF_DISK;

		/*
		 * If the page's key range is unmodified from when we read it
		 * (in other words, we didn't merge part of this page with
		 * another page), we can use the page without change.  If we
		 * did merge with another page, we must build a page reflecting
		 * the updated key range, and that requires an additional pass
		 * to free its backing blocks.
		 */
		if (F_ISSET(trk, WT_TRACK_MERGE)) {
			ss->merge_free = 1;

			WT_ERR(__slvg_col_build_leaf(session, trk, page, cref));
		}
		++cref;
	}

	/* Write the internal page to disk. */
	return (__wt_page_reconcile(
	    session, page, WT_REC_EVICT | WT_REC_LOCKED));

err:	if (page->u.col_int.t != NULL)
		__wt_free(session, page->u.col_int.t);
	if (page != NULL)
		__wt_free(session, page);
	return (ret);
}

/*
 * __slvg_col_build_leaf --
 *	Build a column-store leaf page for a merged page.
 */
static int
__slvg_col_build_leaf(WT_SESSION_IMPL *session,
    WT_TRACK *trk, WT_PAGE *parent, WT_COL_REF *cref)
{
	WT_COL *save_col_leaf;
	WT_PAGE *page;
	WT_SALVAGE_COOKIE *cookie, _cookie;
	uint64_t skip, take;
	uint32_t save_entries;
	int ret;

	cookie = &_cookie;
	WT_CLEAR(*cookie);

	/* Get the original page, including the full in-memory setup. */
	WT_RET(__wt_page_in(session, parent, &cref->ref, 0));
	page = WT_COL_REF_PAGE(cref);
	save_col_leaf = page->u.col_leaf.d;
	save_entries = page->entries;

	/*
	 * Calculate the number of K/V entries we are going to skip, and
	 * the total number of K/V entries we'll take from this page.
	 */
	skip = trk->col_start - page->u.col_leaf.recno;
	take = (trk->col_stop - trk->col_start) + 1;

	WT_VERBOSE(session, SALVAGE,
	    "[%" PRIu32 "] merge discarding first %" PRIu64 " records, "
	    "then taking %" PRIu64 " records",
	    trk->addr, skip, take);

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Adjust the page information to "see" only keys we care about.
		 *
		 * Each WT_COL entry in a variable-length or fixed-length column
		 * store maps to a single K/V pair.  Adjust the in-memory values
		 * to reference only the K/V pairs we care about.
		 */
		page->u.col_leaf.d += skip;
		page->entries = (uint32_t)take;
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Discard backing overflow pages for any items being discarded
		 * that reference overflow pages.
		 */
		WT_ERR(__slvg_col_merge_ovfl(
		    session, trk->addr, page, skip, take));

		cookie->skip = skip;
		cookie->take = take;
		break;
	}

	/*
	 * If we're missing some part of the range, the real start range is in
	 * trk->col_missing, else, it's in trk->col_start.  Update the parent's
	 * reference as well as the page itself.
	 */
	if (trk->col_missing == 0)
		page->u.col_leaf.recno = trk->col_start;
	else {
		page->u.col_leaf.recno = trk->col_missing;
		cookie->missing = trk->col_start - trk->col_missing;

		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] merge inserting %" PRIu64 " missing records",
		    trk->addr, cookie->missing);
	}
	cref->recno = page->u.col_leaf.recno;

	/*
	 * We can't discard the original blocks associated with this page now.
	 * (The problem is we don't want to overwrite any original information
	 * until the salvage run succeeds -- if we free the blocks now, the next
	 * merge page we write might allocate those blocks and overwrite them,
	 * and should the salvage run eventually fail, the original information
	 * would have been lost.)  Clear the reference addr so reconciliation
	 * does not free the underlying blocks.
	 */
	WT_COL_REF_ADDR(cref) = WT_ADDR_INVALID;
	WT_COL_REF_SIZE(cref) = 0;

	/* Write the new version of the leaf page to disk. */
	WT_PAGE_SET_MODIFIED(page);

	ret = __wt_page_reconcile_int(session,
	    page, cookie, WT_REC_EVICT | WT_REC_LOCKED | WT_REC_SALVAGE);

err:	/* Reset the page. */
	page->u.col_leaf.d = save_col_leaf;
	page->entries = save_entries;

	/* Discard our hazard reference and the page. */
	__wt_hazard_clear(session, page);
	__wt_page_free(session, page, 0);

	return (ret);
}

/*
 * __slvg_col_merge_ovfl --
 *	Free file blocks referenced from keys discarded from merged pages.
 */
static int
__slvg_col_merge_ovfl(WT_SESSION_IMPL *session,
    uint32_t addr, WT_PAGE *page, uint64_t skip, uint64_t take)
{
	WT_CELL_UNPACK *unpack, _unpack;
	WT_CELL *cell;
	WT_COL *cip;
	uint64_t recno, start, stop;
	uint32_t i;

	unpack = &_unpack;

	recno = page->u.col_leaf.recno;
	start = recno + skip;
	stop = (recno + skip + take) - 1;

	WT_COL_FOREACH(page, cip, i) {
		cell = WT_COL_PTR(page, cip);
		__wt_cell_unpack(cell, unpack);
		recno += unpack->rle;

		if (unpack->type != WT_CELL_DATA_OVFL)
			continue;
		if (recno >= start && recno <= stop)
			continue;

		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] merge discard freed overflow "
		    "reference [%" PRIu32 "/%" PRIu32 "]",
		    addr, unpack->off.addr, unpack->off.size);

		WT_RET(__wt_block_free(
		    session, unpack->off.addr, unpack->off.size));
	}
	return (0);
}

/*
 * __slvg_row_range --
 *	Figure out the leaf pages we need and discard everything else.  At the
 * same time, tag the overflow pages they reference.
 */
static int
__slvg_row_range(WT_SESSION_IMPL *session, WT_STUFF *ss)
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

		/* Check for pages that overlap our page. */
		for (j = i + 1; j < ss->pages_next; ++j) {
			if (ss->pages[j] == NULL)
				continue;
			/*
			 * We're done if this page starts after our stop, no
			 * subsequent pages can overlap our page.
			 */
			if (func(btree,
			    (WT_ITEM *)&ss->pages[j]->row_start,
			    (WT_ITEM *)&ss->pages[i]->row_stop) > 0)
				break;

			/* There's an overlap, fix it up. */
			WT_RET(__slvg_row_range_overlap(session, i, j, ss));
		}
	}
	return (0);
}

/*
 * __slvg_row_range_overlap --
 *	Two row-store key ranges overlap, deal with it.
 */
static int
__slvg_row_range_overlap(
    WT_SESSION_IMPL *session, uint32_t a_slot, uint32_t b_slot, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_TRACK *a_trk, *b_trk, *new;
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

	WT_VERBOSE(session, SALVAGE,
	    "[%" PRIu32 "] and [%" PRIu32 "] range overlap",
	    a_trk->addr, b_trk->addr);

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
#define	A_TRK_START	((WT_ITEM *)A_TRK_START_BUF)
#define	A_TRK_START_BUF	(&a_trk->row_start)
#define	A_TRK_STOP	((WT_ITEM *)A_TRK_STOP_BUF)
#define	A_TRK_STOP_BUF	(&a_trk->row_stop)
#define	B_TRK_START_BUF	(&b_trk->row_start)
#define	B_TRK_START	((WT_ITEM *)B_TRK_START_BUF)
#define	B_TRK_STOP_BUF	(&b_trk->row_stop)
#define	B_TRK_STOP	((WT_ITEM *)B_TRK_STOP_BUF)
#define	SLOT_START(i)	((WT_ITEM *)&ss->pages[i]->row_start)
#define	__slvg_key_copy(session, dst, src)				\
	__wt_buf_set(session, dst, (src)->data, (src)->size)

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
		WT_RET(__slvg_row_trk_update_start(
		    session, A_TRK_STOP, b_slot, ss));
		F_SET(b_trk, WT_TRACK_CHECK_START | WT_TRACK_MERGE);
		goto merge;
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
		goto merge;
	}

	if  (func(btree, A_TRK_STOP, B_TRK_STOP) < 0) {
		/* Case #3/8. */
		if (a_trk->lsn > b_trk->lsn) {
			/*
			 * Case #3/8: a_trk is more desirable, delete a_trk's
			 * key range from b_trk;
			 */
			WT_RET(__slvg_row_trk_update_start(
			    session, A_TRK_STOP, b_slot, ss));
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
		goto merge;
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->lsn > b_trk->lsn) {
delete:		WT_RET(__slvg_trk_free(session,
		    &ss->pages[b_slot], WT_TRK_FREE_BLOCKS | WT_TRK_FREE_OVFL));
		return (0);
	}

	/*
	 * Case #5: b_trk is more desirable and is a middle chunk of a_trk.
	 * Split a_trk into two parts, the key range before b_trk and the
	 * key range after b_trk.
	 *
	 * First, create a copy of the original page's WT_TRACK information
	 * (same LSN, addr and size), that we'll use to reference the key
	 * range at the end of a_trk.
	 */
	WT_RET(__wt_calloc_def(session, 1, &new));
	WT_TRACK_INIT(ss, new, a_trk->lsn, a_trk->addr, a_trk->size);

	/*
	 * Second, reallocate the array of pages if necessary, and then insert
	 * the new element into the array after the existing element (that's
	 * probably wrong, but we'll fix it up in a second).
	 */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));
	memmove(ss->pages + a_slot + 1, ss->pages + a_slot,
	    (ss->pages_next - a_slot) * sizeof(*ss->pages));
	ss->pages[a_slot + 1] = new;
	++ss->pages_next;

	/*
	 * Third, set its its stop key to be the stop key of the original chunk,
	 * and call __slvg_row_trk_update_start.   That function will both set
	 * the start key to be the first key after the stop key of the middle
	 * chunk (that's b_trk), and re-sort the WT_TRACK array as necessary to
	 * move our new entry into the right sorted location.
	 */
	WT_RET(__slvg_key_copy(session, &new->row_stop, A_TRK_STOP_BUF));
	WT_RET(
	    __slvg_row_trk_update_start(session, B_TRK_STOP, a_slot + 1, ss));

	/*
	 * Fourth, the new WT_TRACK information doesn't reference any file
	 * blocks (let the original a_trk structure reference file blocks).
	 */
	F_SET(new,
	    WT_TRACK_CHECK_START | WT_TRACK_MERGE | WT_TRACK_NO_FILE_BLOCKS);

	/*
	 * Finally, set the original WT_TRACK information to reference only
	 * the initial key space in the page, that is, everything up to the
	 * starting key of the middle chunk (that's b_trk).
	 */
	WT_RET(__slvg_key_copy(session, A_TRK_STOP_BUF, B_TRK_START_BUF));
	F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);

merge:	WT_VERBOSE(session, SALVAGE,
	    "[%" PRIu32 "] and [%" PRIu32 "] require merge",
	    a_trk->addr, b_trk->addr);
	return (0);
}

/*
 * __slvg_row_trk_update_start --
 *	Update a row-store page's start key after an overlap.
 */
static int
__slvg_row_trk_update_start(
    WT_SESSION_IMPL *session, WT_ITEM *stop, uint32_t slot, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_BUF *key, *dsk;
	WT_IKEY *ikey;
	WT_ITEM *item, _item;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_TRACK *trk;
	uint32_t i;
	int found, ret, (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	key = dsk = NULL;
	page = NULL;
	func = btree->btree_compare;
	found = ret = 0;

	trk = ss->pages[slot];

	/*
	 * If we deleted an initial piece of the WT_TRACK name space, it may no
	 * longer be in the right location.
	 *
	 * For example, imagine page #1 has the key range 30-50, it split, and
	 * we wrote page #2 with key range 30-40, and page #3 key range with
	 * 40-50, where pages #2 and #3 have larger LSNs than page #1.  When the
	 * key ranges were sorted, page #2 came first, then page #1 (because of
	 * their earlier start keys than page #3), and page #2 came before page
	 * #1 because of its LSN.  When we resolve the overlap between page #2
	 * and page #1, we truncate the initial key range of page #1, and it now
	 * sorts after page #3, because it has the same starting key of 40, and
	 * a lower LSN.
	 *
	 * First, update the WT_TRACK start key based on the specified stop key.
	 *
	 * Read and instantiate the WT_TRACK page.
	 */
	WT_RET(__wt_scr_alloc(session, trk->size, &dsk));
	WT_ERR(__wt_disk_read(session, dsk->mem, trk->addr, trk->size));
	WT_ERR(__wt_page_inmem(session, NULL, NULL, dsk->mem, &page));

	/*
	 * Walk the page, looking for a key sorting greater than the specified
	 * stop key -- that's our new start key.
	 */
	WT_RET(__wt_scr_alloc(session, 0, &key));
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
		if  (func(btree, item, stop) > 0) {
			found = 1;
			break;
		}
	}

	/*
	 * We know that at least one key on the page sorts after the specified
	 * stop key, otherwise the page would have entirely overlapped and we
	 * would have discarded it, we wouldn't be here.  Therefore, this test
	 * is safe.  (But, it never hurts to check.)
	 */
	WT_RET_TEST(!found, WT_ERROR);
	WT_RET(__slvg_key_copy(session, &trk->row_start, item));

	/*
	 * We may need to re-sort some number of elements in the list.  Walk
	 * forward in the list until reaching an entry which cannot overlap
	 * the adjusted entry.  If it's more than a single slot, re-sort the
	 * entries.
	 */
	for (i = slot + 1; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;
		if  (func(btree, SLOT_START(i), (WT_ITEM *)&trk->row_stop) > 0)
			break;
	}
	i -= slot;
	if (i > 1)
		qsort(ss->pages + slot, (size_t)i,
		    sizeof(WT_TRACK *), __slvg_trk_compare_key);

	if (page != NULL)
		__wt_page_free(session, page, WT_PAGE_FREE_IGNORE_DISK);

err:	__wt_scr_release(&dsk);
	__wt_scr_release(&key);

	return (ret);
}

/*
 * __slvg_row_build_internal --
 *	Build a row-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_row_build_internal(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_PAGE *page;
	WT_REF *root_page;
	WT_ROW_REF *rref;
	WT_TRACK *trk;
	uint32_t i, leaf_cnt;
	int deleted, ret;

	root_page = &session->btree->root_page;

	/*
	 * Count how many internal page slots we need (we could track this
	 * during the array shuffling/splitting, but that's a lot harder).
	 */
	for (leaf_cnt = i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			++leaf_cnt;

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
		 * the updated key range, and that requires an additional pass
		 * to free its backing blocks.
		 */
		if (F_ISSET(trk, WT_TRACK_MERGE)) {
			ss->merge_free = 1;

			WT_ERR(__slvg_row_build_leaf(
			    session, trk, page, rref, ss, &deleted));

			/*
			 * If we took none of the keys from the merged page,
			 * we don't need the page; fix the count of entries
			 * we're creating.
			 */
			if (deleted) {
				--page->entries;
				continue;
			}
		} else
			WT_ERR(__wt_row_ikey_alloc(session, 0,
			    trk->row_start.data,
			    trk->row_start.size,
			    (WT_IKEY **)&rref->key));
		++rref;
	}

	/* Write the internal page to disk. */
	return (__wt_page_reconcile(
	    session, page, WT_REC_EVICT | WT_REC_LOCKED));

err:	if (page->u.row_int.t != NULL)
		__wt_free(session, page->u.row_int.t);
	if (page != NULL)
		__wt_free(session, page);
	return (ret);
}

/*
 * __slvg_row_build_leaf --
 *	Build a row-store leaf page for a merged page.
 */
static int
__slvg_row_build_leaf(WT_SESSION_IMPL *session, WT_TRACK *trk,
    WT_PAGE *parent, WT_ROW_REF *rref, WT_STUFF *ss, int *deletedp)
{
	WT_BTREE *btree;
	WT_BUF *key;
	WT_IKEY *ikey;
	WT_ITEM *item, _item;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_SALVAGE_COOKIE *cookie, _cookie;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *), ret;
	uint32_t i, skip_start, skip_stop;

	*deletedp = 0;

	btree = session->btree;
	page = NULL;
	func = btree->btree_compare;

	cookie = &_cookie;
	WT_CLEAR(*cookie);

	/* Allocate temporary space in which to instantiate the keys. */
	WT_RET(__wt_scr_alloc(session, 0, &key));

	/* Get the original page, including the full in-memory setup. */
	WT_ERR(__wt_page_in(session, parent, &rref->ref, 0));
	page = WT_ROW_REF_PAGE(rref);

	/*
	 * Figure out how many page keys we want to take and how many we want
	 * to skip.
	 *
	 * If checking the starting range key, the key we're searching for will
	 * be equal to the starting range key.  This is because we figured out
	 * the true merged-page start key as part of discarding initial keys
	 * from the page (see the __slvg_row_range_overlap function, and its
	 * calls to __slvg_row_trk_update_start for more information).
	 *
	 * If checking the stopping range key, we want the keys on the page that
	 * are less-than the stopping range key.  This is because we copied a
	 * key from another page to define this page's stop range: that page is
	 * the page that owns the "equal to" range space.
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

			/*
			 * >= is correct: see the comment above.
			 */
			if  (func(btree,
			    item, (WT_ITEM *)&trk->row_start) >= 0)
				break;
			if (ss->verbose) {
				WT_ERR(__slvg_load_byte_string(session,
				    item->data, item->size, ss->vbuf));
				WT_VERBOSE(session, SALVAGE,
				    "[%" PRIu32 "] merge discarding leading "
				    "key %.*s",
				    trk->addr, (int)ss->vbuf->size,
				    (char *)ss->vbuf->data);
			}
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

			/*
			 * < is correct: see the comment above.
			 */
			if  (func(btree,
			    item, (WT_ITEM *)&trk->row_stop) < 0)
				break;
			if (ss->verbose) {
				WT_ERR(__slvg_load_byte_string(session,
				    item->data, item->size, ss->vbuf));
				WT_VERBOSE(session, SALVAGE,
				    "[%" PRIu32 "] merge discarding trailing "
				    "key %.*s",
				    trk->addr, (int)ss->vbuf->size,
				    (char *)ss->vbuf->data);
			}
			++skip_stop;
		}

	/*
	 * Because the stopping key range is a boundary, not an exact match,
	 * we may have just decided not to take all of the keys on the page, or
	 * none of them.
	 *
	 * If we take none of the keys, all we have to do is tell our caller to
	 * not include this leaf page in the internal page it's building.
	 */
	if (skip_start + skip_stop >= page->entries) {
		*deletedp = 1;
		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] merge required no records, deleting instead",
		    trk->addr);
	} else {
		/*
		 * Discard backing overflow pages for any items being discarded
		 * that reference overflow pages.
		 */
		WT_ERR(__slvg_row_merge_ovfl(
		    session, trk->addr, page, 0, skip_start));
		WT_ERR(__slvg_row_merge_ovfl(session,
		    trk->addr, page, page->entries - skip_stop, page->entries));

		/*
		 * If we take all of the keys, we don't write the page and we
		 * clear the merge flags so that the underlying blocks are not
		 * later freed (for merge pages re-written into the file, the
		 * underlying blocks have to be freed, but if this page never
		 * gets written, we shouldn't free the blocks).
		 */
		if (skip_start == 0 && skip_stop == 0)
			F_CLR(trk, WT_TRACK_MERGE);
		else {
			/*
			 * Change the page to reflect the correct record count:
			 * there is no need to copy anything on the page itself,
			 * the entries value limits the number of page items.
			 */
			page->entries -= skip_stop;

			/*
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
			WT_ROW_REF_ADDR(rref) = WT_ADDR_INVALID;
			WT_ROW_REF_SIZE(rref) = 0;

			/* Write the new version of the leaf page to disk. */
			WT_PAGE_SET_MODIFIED(page);
			cookie->skip = skip_start;
			ret = __wt_page_reconcile_int(session, page, cookie,
			    WT_REC_EVICT | WT_REC_LOCKED | WT_REC_SALVAGE);

			page->entries += skip_stop;
			if (ret != 0)
				goto err;
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
err:	__wt_hazard_clear(session, page);
	__wt_page_free(session, page, 0);

	__wt_scr_release(&key);

	return (ret);
}

/*
 * __slvg_row_merge_ovfl --
 *	Free file blocks referenced from keys discarded from merged pages.
 */
static int
__slvg_row_merge_ovfl(WT_SESSION_IMPL *session,
   uint32_t addr, WT_PAGE *page, uint32_t start, uint32_t stop)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_ROW *rip;

	unpack = &_unpack;

	for (rip = page->u.row_leaf.d + start; start < stop; ++start) {
		if (__wt_off_page(page, rip->key))
			cell = WT_REF_OFFSET(
			    page, ((WT_IKEY *)rip->key)->cell_offset);
		else
			cell = rip->key;
		__wt_cell_unpack(cell, unpack);
		if (unpack->type == WT_CELL_KEY_OVFL) {
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] merge discard freed overflow "
			    "reference [%" PRIu32 "/%" PRIu32 "]",
			    addr, unpack->off.addr, unpack->off.size);

			WT_RET(__wt_block_free(
			    session, unpack->off.addr, unpack->off.size));
		}

		if ((cell = __wt_row_value(page, rip)) == NULL)
			continue;
		__wt_cell_unpack(cell, unpack);
		if (unpack->type == WT_CELL_DATA_OVFL) {
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] merge discard freed overflow "
			    "reference [%" PRIu32 "/%" PRIu32 "]",
			    addr, unpack->off.addr, unpack->off.size);

			WT_RET(__wt_block_free(
			    session, unpack->off.addr, unpack->off.size));
		}
	}
	return (0);
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
 * __slvg_ovfl_reconcile --
 *	Review relationships between leaf pages and the overflow pages, delete
 * leaf pages until there's a one-to-one relationship between leaf and overflow
 * pages.
 */
static int
__slvg_ovfl_reconcile(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_OFF *off;
	WT_TRACK **searchp, *trk;
	uint32_t i, j;

	/*
	 * Walk the list of pages and discard any pages referencing non-existent
	 * overflow pages or referencing overflow pages also referenced by pages
	 * with higher LSNs.  Our caller sorted the page list by LSN, high to
	 * low, so we don't have to do explicit testing of the page LSNs, the
	 * first page to reference an overflow page is the best page to own it.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;
		for (j = 0; j < trk->ovfl_cnt; ++j) {
			off = &trk->ovfl[j];
			searchp = bsearch(off, ss->ovfl, ss->ovfl_next,
			    sizeof(WT_TRACK *), __slvg_ovfl_compare);

			/*
			 * If the overflow page doesn't exist or its size does
			 * not match, or if another page has already claimed the
			 * overflow page, discard the leaf page.
			 */
			if (searchp != NULL &&
			    (*searchp)->size == off->size &&
			    !F_ISSET(*searchp, WT_TRACK_OVFL_REFD)) {
				F_SET(*searchp, WT_TRACK_OVFL_REFD);
				continue;
			}

			/*
			 * This leaf page isn't usable.  Discard the leaf page
			 * and clear the "referenced" flag for overflow pages
			 * already claimed by this page.  I hate to repeat the
			 * searches, but the alternative is a pointer for each
			 * overflow page referenced by the leaf page and this
			 * is the only thing we'd use it for.
			 */
			while (j > 0) {
				off = &trk->ovfl[--j];
				searchp =
				    bsearch(off, ss->ovfl, ss->ovfl_next,
				    sizeof(WT_TRACK *), __slvg_ovfl_compare);
				F_CLR(*searchp, WT_TRACK_OVFL_REFD);
			}
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] references unavailable "
			    "overflow page [%" PRIu32 "/%" PRIu32 "]",
			    trk->addr, off->addr, off->size);
			WT_RET(__slvg_trk_free(
			    session, &ss->pages[i], WT_TRK_FREE_BLOCKS));
			break;
		}
	}
	return (0);
}

/*
 * __slvg_trk_compare_key --
 *	Compare two WT_TRACK array entries by key, and secondarily, by LSN.
 */
static int
__slvg_trk_compare_key(const void *a, const void *b)
{
	WT_BTREE *btree;
	WT_TRACK *a_trk, *b_trk;
	uint64_t a_lsn, a_recno, b_lsn, b_recno;
	int cmp;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	if (a_trk == NULL)
		return (b_trk == NULL ? 0 : 1);
	if (b_trk == NULL)
		return (-1);

	switch (a_trk->ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
		a_recno = a_trk->col_start;
		b_recno = b_trk->col_start;
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
		    (WT_ITEM *)&a_trk->row_start,
		    (WT_ITEM *)&b_trk->row_start)) != 0)
			return (cmp);
		break;
	}

	/*
	 * If the primary keys compare equally, differentiate based on LSN.
	 * Sort from highest LSN to lowest, that is, the earlier pages in
	 * the array are more desirable.
	 */
	a_lsn = a_trk->lsn;
	b_lsn = b_trk->lsn;
	return (a_lsn > b_lsn ? -1 : (a_lsn < b_lsn ? 1 : 0));
}

/*
 * __slvg_trk_compare_lsn --
 *	Compare two WT_TRACK array entries by LSN.
 */
static int
__slvg_trk_compare_lsn(const void *a, const void *b)
{
	WT_TRACK *a_trk, *b_trk;
	uint64_t a_lsn, b_lsn;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	/*
	 * Sort from highest LSN to lowest, that is, the earlier pages in the
	 * array are more desirable.
	 */
	a_lsn = a_trk->lsn;
	b_lsn = b_trk->lsn;
	return (a_lsn > b_lsn ? -1 : (a_lsn < b_lsn ? 1 : 0));
}

/*
 * __slvg_merge_block_free --
 *	Free file blocks for pages that had to be merged.
 */
static int
__slvg_merge_block_free(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	/*
	 * Free any underlying file blocks for merged pages.  We do not free
	 * referenced overflow pages: that had to be done when creating the
	 * merged pages because we chose the overflow pages to free based on
	 * the keys we retained or discarded.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;
		if (F_ISSET(trk, WT_TRACK_MERGE) &&
		    !F_ISSET(trk, WT_TRACK_NO_FILE_BLOCKS))
			WT_RET(__slvg_trk_free(
			    session, &ss->pages[i], WT_TRK_FREE_BLOCKS));
	}

	return (0);
}

/*
 * __slvg_ovfl_discard --
 *	Discard unused overflow pages.
 */
static int
__slvg_ovfl_discard(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	uint32_t i;

	/*
	 * Walk the overflow page array: if an overflow page isn't referenced,
	 * add its file blocks to the free list.
	 */
	for (i = 0; i < ss->ovfl_next; ++i) {
		if (F_ISSET(ss->ovfl[i], WT_TRACK_OVFL_REFD))
			continue;
		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] unused overflow page",
		    ss->ovfl[i]->addr);
		WT_RET(__slvg_trk_free(
		    session, &ss->ovfl[i], WT_TRK_FREE_BLOCKS));
	}

	return (0);
}

/*
 * __slvg_free --
 *	Discard memory allocated to the page and overflow arrays.
 */
static int
__slvg_cleanup(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	uint32_t i;

	/* Discard the leaf page array. */
	for (i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			WT_RET(__slvg_trk_free(session, &ss->pages[i], 0));
	__wt_free(session, ss->pages);

	/* Discard the ovfl page array. */
	for (i = 0; i < ss->ovfl_next; ++i)
		if (ss->ovfl[i] != NULL)
			WT_RET(__slvg_trk_free(session, &ss->ovfl[i], 0));
	__wt_free(session, ss->ovfl);

	return (0);
}

/*
 * __slvg_trk_free --
 *	Discard a WT_TRACK structure and (optionally) its underlying blocks.
 */
static int
__slvg_trk_free(WT_SESSION_IMPL *session, WT_TRACK **trkp, uint32_t flags)
{
	WT_OFF *off;
	WT_TRACK *trk;
	uint32_t i;

	trk = *trkp;
	*trkp = NULL;

	/*
	 * If freeing underlying file blocks or overflow pages, this is a page
	 * we were tracking but eventually decided not to use.  That merits a
	 * verbose description.
	 */
	if (LF_ISSET(WT_TRK_FREE_BLOCKS)) {
		WT_VERBOSE(session, SALVAGE,
		    "[%" PRIu32 "] page discarded: discard freed file blocks [%"
		    PRIu32 "/%" PRIu32 "]",
		    trk->addr, trk->addr, trk->size);
		WT_RET(__wt_block_free(session, trk->addr, trk->size));
	}
	if (LF_ISSET(WT_TRK_FREE_OVFL))
		for (i = 0; i < trk->ovfl_cnt; ++i) {
			off = &trk->ovfl[i];
			WT_VERBOSE(session, SALVAGE,
			    "[%" PRIu32 "] page discarded: discard freed "
			    "overflow page [%" PRIu32 "/%" PRIu32 "]",
			    trk->addr, off->addr, off->size);
			WT_RET(__wt_block_free(session, off->addr, off->size));
		}

	if (trk->ss->page_type == WT_PAGE_ROW_LEAF) {
		__wt_buf_free(session, &trk->row_start);
		__wt_buf_free(session, &trk->row_stop);
	}

	__wt_free(session, trk->ovfl);
	__wt_free(session, trk);

	return (0);
}

/*
 * __slvg_load_byte_string --
 *	Load a single byte string into a buffer, in printable characters,
 * where possible.
 */
static int
__slvg_load_byte_string(
    WT_SESSION_IMPL *session, const uint8_t *data, uint32_t size, WT_BUF *buf)
{
	static const char hex[] = "0123456789abcdef";
	uint32_t avail;
	int ch, len;
	char *p;

	/*
	 * The maximum size is the byte-string length, all hex characters, plus
	 * a trailing nul byte.  Throw in a few extra bytes for fun.
	 *
	 * The underlying functions use type int, not uint32_t, check we're
	 * not in trouble, just out of sheer, raving paranoia.
	 */
	if ((uint64_t)size * 4 + 20 >= UINT32_MAX)
		return (ENOMEM);
	avail = size * 4 + 20;
	WT_RET(__wt_buf_init(session, buf, avail));

	for (p = buf->mem; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			len = snprintf(p, avail, "%c", ch);
		else
			len = snprintf(p, avail, "%x%x",
			    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
		/*
		 * Be paranoid about buffer overflow: even if our calculation
		 * is off, or snprintf(3) returns large length values, don't
		 * overflow the end of the buffer.
		 */
		if ((u_int)len >= avail)
			break;
		p += len;
		buf->size += (u_int)len;
		avail -= (u_int)len;
	}
	return (0);
}
