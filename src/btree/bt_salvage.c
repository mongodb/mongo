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

	WT_BUF	   *key_start, *key_stop;	/* Temporary row-store keys */

	uint8_t  page_type;			/* Page type */

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

#define	WT_TRACK_OVFL_MISSING	0x01		/* Overflow page missing */
#define	WT_TRACK_OVFL_REFD	0x02		/* Overflow page referenced */
	uint32_t flags;

	/*
	 * The WT_TRACK structure is followed by the page's start and stop
	 * keys, that is, the beginning and end of the page's name space.
	 *
	 * In a row-store salvage, the WT_TRACK structure is followed
	 * by two key-size/key pairs.
	 *
	 * In a column-store salvage, the WT_TRACK structure is followed
	 * by two record numbers.
	 */
#define	WT_TRACK_KEY_START_SIZE(trk)					\
	(((uint32_t *)((uint8_t *)(trk) + sizeof(WT_TRACK)))[0])
#define	WT_TRACK_KEY_START(trk)						\
	((void *)((uint8_t *)(trk) + sizeof(WT_TRACK) + 2 * sizeof(uint32_t)))
#define	WT_TRACK_KEY_STOP_SIZE(trk)					\
	(((uint32_t *)((uint8_t *)(trk) + sizeof(WT_TRACK)))[1])
#define	WT_TRACK_KEY_STOP(trk)						\
	((void *)((uint8_t *)						\
	WT_TRACK_KEY_START(trk) + WT_TRACK_KEY_START_SIZE(trk)))

#define	WT_TRACK_RECNO_START(trk)					\
	(((uint64_t *)((uint8_t *)(trk) + sizeof(WT_TRACK)))[0])
#define	WT_TRACK_RECNO_STOP(trk)					\
	(((uint64_t *)((uint8_t *)(trk) + sizeof(WT_TRACK)))[1])
};

static int  __wt_salvage_discard_col(SESSION *, WT_STUFF *);
static int  __wt_salvage_discard_ovfl(SESSION *, WT_STUFF *);
static void __wt_salvage_discard_ovfl_trk(SESSION *, WT_TRACK *, WT_STUFF *);
static int  __wt_salvage_discard_row(SESSION *, WT_STUFF *);
static void __wt_salvage_free(SESSION *, WT_STUFF *);
static int  __wt_salvage_ovfl_compare(const void *, const void *);
static int  __wt_salvage_ovfl_srch_col(SESSION *, WT_PAGE_DISK *, WT_TRACK *);
static int  __wt_salvage_ovfl_srch_row(SESSION *, WT_PAGE_DISK *, WT_TRACK *);
static int  __wt_salvage_read(SESSION *, WT_STUFF *);
static int  __wt_salvage_track_compare(const void *, const void *);
static int  __wt_salvage_track_leaf(SESSION *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);
static int  __wt_salvage_track_ovfl(SESSION *, WT_PAGE_DISK *, uint32_t, WT_STUFF *);

#ifdef HAVE_DIAGNOSTIC
static void __wt_salvage_track_dump(WT_STUFF *);
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
	WT_ERR(__wt_scr_alloc(session, 0, &stuff.key_start));
	WT_ERR(__wt_scr_alloc(session, 0, &stuff.key_stop));
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
		goto err_set;
	}

	/* Build the page list. */
	WT_ERR(__wt_salvage_read(session, &stuff));

	/*
	 * If the file has no valid pages, we should probably not touch it,
	 * somebody gave us a random file.
	 */
	if (stuff.pages_next == 0) {
		__wt_errx(session,
		    "file has no valid pages and cannot be salvaged");
		goto err_set;
	}

	/*
	 * Sort the page list by key, and secondarily, by LSN.
	 *
	 * We don't have to sort the overflow array because we inserted the
	 * records into the array in file addr order.
	 */
	qsort(stuff.pages, (size_t)stuff.pages_next,
	    sizeof(WT_TRACK *), __wt_salvage_track_compare);

#ifdef HAVE_DIAGNOSTIC
	__wt_salvage_track_dump(&stuff);
#endif

	/*
	 * Discard any pages we know we don't need -- we do this in a separate
	 * pass because we want to allocate pages from the beginning of the
	 * file if possible, and the bigger the set of block from which we can
	 * allocate, the better.
	 *
	 * First, discard pages we don't need, and while we're at it, track
	 * the overflow pages they reference.
	 */
	switch (stuff.page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		WT_ERR(__wt_salvage_discard_col(session, &stuff));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_salvage_discard_row(session, &stuff));
		break;
	}

	/* Second, discard any overflow pages that aren't referenced. */
	WT_ERR(__wt_salvage_discard_ovfl(session, &stuff));

	if (0) {
err_set:	ret = WT_ERROR;
	}

err:	/* Wrap up reporting. */
	if (stuff.f != NULL)
		stuff.f(session->name, stuff.fcnt);

	/* Free allocated memory. */
	__wt_salvage_free(session, &stuff);

	if (stuff.key_start != NULL)
		__wt_scr_release(&stuff.key_start);
	if (stuff.key_stop != NULL)
		__wt_scr_release(&stuff.key_stop);

	return (ret);
}

/*
 * __wt_salvage_read --
 *	Read the file and build a table of the pages we can use.
 */
static int
__wt_salvage_read(SESSION *session, WT_STUFF *ss)
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
			WT_ERR(__wt_salvage_track_leaf(session, dsk, addr, ss));
			break;
		case WT_PAGE_OVFL:
			WT_ERR(__wt_salvage_track_ovfl(session, dsk, addr, ss));
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
#define	WT_TRACK_INIT(_ss, trk, dsk, _addr) do {			\
	(trk)->ss = _ss;						\
	(trk)->lsn = (dsk)->lsn;					\
	(trk)->addr = _addr;						\
	(trk)->size = (dsk)->size;					\
} while (0)

/*
 * __wt_salvage_track_leaf --
 *	Track a leaf page.
 */
static int
__wt_salvage_track_leaf(
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

	/* Re-allocate the array of pages, as necesary. */
	if (ss->pages_next * sizeof(WT_TRACK *) == ss->pages_allocated)
		WT_RET(__wt_realloc(session, &ss->pages_allocated,
		   (ss->pages_next + 1000) * sizeof(WT_TRACK *), &ss->pages));

	/* Allocate a WT_TRACK entry for this new page and fill it in. */
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
		WT_ERR(__wt_calloc(
		    session, 1, sizeof(WT_TRACK) + 2 * sizeof(uint64_t), &trk));
		WT_TRACK_RECNO_START(trk) = dsk->recno;
		WT_TRACK_RECNO_STOP(trk) = dsk->recno + dsk->u.entries;

		if (dsk->type == WT_PAGE_COL_VAR)
			WT_ERR(__wt_salvage_ovfl_srch_col(session, dsk, trk));
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

		WT_ERR(__wt_calloc(
		    session, 1, sizeof(WT_TRACK) + 2 * sizeof(uint64_t), &trk));
		WT_TRACK_RECNO_START(trk) = dsk->recno;
		WT_TRACK_RECNO_STOP(trk) = stop_recno;
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Row-store format: instantiate and copy the first and last
		 * keys.  Additionally, row-store pages can contain overflow
		 * items.
		 */
		cell = WT_PAGE_DISK_BYTE(dsk);
		WT_ERR(__wt_cell_process(session, cell, ss->key_start));
		WT_CELL_FOREACH(dsk, cell, i)
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_KEY:
			case WT_CELL_KEY_OVFL:
				last_key_cell = cell;
				break;
			}
		WT_ERR(__wt_cell_process(session, last_key_cell, ss->key_stop));

		WT_ERR(__wt_calloc(session, 1, sizeof(WT_TRACK) +
		    sizeof(uint32_t) + ss->key_start->item.size +
		    sizeof(uint32_t) + ss->key_stop->item.size,
		    &trk));

		WT_TRACK_KEY_START_SIZE(trk) = ss->key_start->item.size;
		memcpy(WT_TRACK_KEY_START(trk),
		    ss->key_start->item.data, ss->key_start->item.size);
		WT_TRACK_KEY_STOP_SIZE(trk) = ss->key_stop->item.size;
		memcpy(WT_TRACK_KEY_STOP(trk),
		    ss->key_stop->item.data, ss->key_stop->item.size);

		WT_ERR(__wt_salvage_ovfl_srch_row(session, dsk, trk));
		break;
	}

	WT_TRACK_INIT(ss, trk, dsk, addr);
	ss->pages[ss->pages_next++] = trk;

	if (0) {
err:		if (trk != NULL)
			__wt_free(session, trk);
	}
	return (ret);
}

/*
 * __wt_salvage_track_ovfl --
 *	Track an overflow page.
 */
static int
__wt_salvage_track_ovfl(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, WT_STUFF *ss)
{
	WT_TRACK *trk;

	/*
	 * Reallocate the overflow page array as necessary, then save the
	 * page's location information.
	 */
	/* Re-allocate the array of overflow pages, as necesary. */
	if (ss->ovfl_next * sizeof(WT_TRACK *) == ss->ovfl_allocated)
		WT_RET(__wt_realloc(session, &ss->ovfl_allocated,
		   (ss->ovfl_next + 1000) * sizeof(WT_TRACK *), &ss->ovfl));

	WT_RET(__wt_calloc_def(session, sizeof(WT_TRACK), &trk));

	WT_TRACK_INIT(ss, trk, dsk, addr);
	ss->ovfl[ss->ovfl_next++] = trk;

	return (0);
}

/*
 * __wt_salvage_ovfl_srch_col --
 *	Search a column-store page for overflow items.
 */
static int
__wt_salvage_ovfl_srch_col(SESSION *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
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
 * __wt_salvage_ovfl_srch_row --
 *	Search a row-store page for overflow items.
 */
static int
__wt_salvage_ovfl_srch_row(SESSION *session, WT_PAGE_DISK *dsk, WT_TRACK *trk)
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
 * __wt_salvage_discard_col --
 *	Discard old copies of column-store pages.
 */
static int
__wt_salvage_discard_col(SESSION *session, WT_STUFF *ss)
{
	WT_TRACK *next, *trk;
	uint64_t stop;
	uint32_t i, j;

	/*
	 * Walk the page array, reviewing each page to decide if we care about
	 * it -- the goal is to put as many pages on the free list as possible.
	 *
	 * The page array is sorted in key order, and secondarily on LSN: this
	 * means that for each new starting key, the first page we find is the
	 * right page for that starting key.   Walk forward from each page until
	 * we reach a page with a starting key after our page's stopping key.
	 * For each of those pages, check to see if they are an older generation
	 * of our page, that is, if their starting/stopping key is within our
	 * starting/stopping key, and they have a lower LSN than our page.  If
	 * so, they can be discarded.
	 *
	 * For each page we're going to keep, mark overflow pages it references,
	 * then do a pass to discard overflow pages not referenced by any useful
	 * page.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		/* Check for pages that are subsets of our page. */
		stop = WT_TRACK_RECNO_STOP(ss->pages[i]);
		for (j = i; ++j < ss->pages_next;) {
			next = ss->pages[j];
			/*
			 * If this page starts after our stop, we don't have
			 * to go any further forward in the list, no further
			 * pages can be subsets of our page.
			 */
			if (WT_TRACK_RECNO_START(next) > stop)
				break;

			/*
			 * If this page has a higher LSN than our page, or is
			 * not a subset of our page, we're going to need data
			 * from it.
			 */
			if (next->lsn > trk->lsn)
				continue;
			if (WT_TRACK_RECNO_STOP(next) > stop)
				continue;

			/* The page is useless, discard it. */
			WT_RET(
			    __wt_block_free(session, next->addr, next->size));
			__wt_free(session, next->ovfl);
			__wt_free(session, next);
			ss->pages[j] = NULL;
		}

		/* Review the overflow pages this page references. */
		if (trk->ovfl_cnt != 0)
			__wt_salvage_discard_ovfl_trk(session, trk, ss);
	}

	return (0);
}

/*
 * __wt_salvage_discard_row --
 *	Discard old copies of row-store pages.
 */
static int
__wt_salvage_discard_row(SESSION *session, WT_STUFF *ss)
{
	BTREE *btree;
	WT_TRACK *next, *trk;
	WT_ITEM trk_stop, next_start, next_stop;
	uint32_t i, j;

	btree = session->btree;

	/*
	 * Walk the page array, reviewing each page to decide if we care about
	 * it -- the goal is to put as many pages on the free list as possible.
	 *
	 * The page array is sorted in key order, and secondarily on LSN: this
	 * means that for each new starting key, the first page we find is the
	 * right page for that starting key.   Walk forward from each page until
	 * we reach a page with a starting key after our page's stopping key.
	 * For each of those pages, check to see if they are an older generation
	 * of our page, that is, if their starting/stopping key is within our
	 * starting/stopping key, and they have a lower LSN than our page.  If
	 * so, they can be discarded.
	 *
	 * For each page we're going to keep, mark overflow pages it references,
	 * then do a pass to discard overflow pages not referenced by any useful
	 * page.
	 */
	for (i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		/* Check for pages that are subsets of our page. */
		trk_stop.data = WT_TRACK_KEY_STOP(trk);
		trk_stop.size = WT_TRACK_KEY_STOP_SIZE(trk);
		for (j = i; ++j < ss->pages_next;) {
			next = ss->pages[j];
			/*
			 * If this page starts after our stop, we don't have
			 * to go any further forward in the list, no further
			 * pages can be subsets of our page.
			 */
			next_start.data = WT_TRACK_KEY_START(next);
			next_start.size = WT_TRACK_KEY_START_SIZE(next);
			if (btree->btree_compare(
			    btree, &next_start, &trk_stop) > 0)
				break;

			/*
			 * If this page has a higher LSN than our page, or is
			 * not a subset of our page, we're going to need data
			 * from it.
			 */
			if (next->lsn > trk->lsn)
				continue;
			next_stop.data = WT_TRACK_KEY_STOP(next);
			next_stop.size = WT_TRACK_KEY_STOP_SIZE(next);
			if (btree->btree_compare(
			    btree, &next_stop, &trk_stop) > 0)
				continue;

			/* The page is useless, discard it. */
			WT_RET(
			    __wt_block_free(session, next->addr, next->size));
			__wt_free(session, next->ovfl);
			__wt_free(session, next);
			ss->pages[j] = NULL;
		}

		/* Review the overflow pages this page references. */
		if (trk->ovfl_cnt != 0)
			__wt_salvage_discard_ovfl_trk(session, trk, ss);
	}

	return (0);
}

/*
 * __wt_salvage_discard_ovfl --
 *	Discard any overflow pages that are never referenced.
 */
static int
__wt_salvage_discard_ovfl(SESSION *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	/*
	 * Walk the overflow page array -- if an overflow page isn't referenced,
	 * we have no use for it.
	 */
	for (i = 0; i < ss->ovfl_next; ++i) {
		trk = ss->ovfl[i];
		if (!F_ISSET(trk, WT_TRACK_OVFL_REFD)) {
			WT_RET(__wt_block_free(session, trk->addr, trk->size));
			__wt_free(session, trk);
			ss->ovfl[i] = NULL;
		}
	}
	return (0);
}

/*
 * __wt_salvage_discard_ovfl_trk --
 *	Review the overflow pages a WT_TRACK entry references.
 */
static void
__wt_salvage_discard_ovfl_trk(SESSION *session, WT_TRACK *trk, WT_STUFF *ss)
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
		    sizeof(WT_TRACK *), __wt_salvage_ovfl_compare);
		if (searchp == NULL || (*searchp)->size !=
		    WT_HDR_BYTES_TO_ALLOC(btree, trk->ovfl[i].size))
			F_SET(trk, WT_TRACK_OVFL_MISSING);
		else
			F_SET(*searchp, WT_TRACK_OVFL_REFD);
	}
}

/*
 * __wt_salvage_ovfl_compare --
 *	Bsearch comparison routine for the overflow array.
 */
static int
__wt_salvage_ovfl_compare(const void *a, const void *b)
{
	WT_OVFL *ovfl;
	WT_TRACK *entry;

	ovfl = (WT_OVFL *)a;
	entry = *(WT_TRACK **)b;

	return (ovfl->addr > entry->addr ? 1 :
	    ((ovfl->addr < entry->addr) ? -1 : 0));
}

/*
 * __wt_salvage_track_compare --
 *	Sort a WT_TRACK array by key, and secondarily, by LSN.
 */
static int
__wt_salvage_track_compare(const void *a, const void *b)
{
	BTREE *btree;
	WT_ITEM a_item, b_item;
	WT_TRACK *a_trk, *b_trk;
	uint64_t a_recno, a_lsn, b_recno, b_lsn;
	int cmp;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	switch (a_trk->ss->page_type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
		a_recno = WT_TRACK_RECNO_START(a_trk);
		b_recno = WT_TRACK_RECNO_START(b_trk);
		if (a_recno == b_recno)
			break;
		if (a_recno > b_recno)
			return (1);
		if (a_recno < b_recno)
			return (-1);
		break;
	case WT_PAGE_ROW_LEAF:
		a_item.data = WT_TRACK_KEY_START(a_trk);
		a_item.size = WT_TRACK_KEY_START_SIZE(a_trk);
		b_item.data = WT_TRACK_KEY_START(b_trk);
		b_item.size = WT_TRACK_KEY_START_SIZE(b_trk);
		btree = a_trk->ss->btree;
		cmp = btree->btree_compare(btree, &a_item, &b_item);
		if (cmp != 0)
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
 * __wt_salvage_free --
 *	Discard memory allocated to the track buffers.
 */
static void
__wt_salvage_free(SESSION *session, WT_STUFF *ss)
{
	uint32_t i;

	for (i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			__wt_free(session, ss->pages[i]);
	__wt_free(session, ss->pages);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_salvage_track_dump --
 *	Dump out the sorted track information.
 */
static void
__wt_salvage_track_dump(WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint32_t i;

	fprintf(stderr, "salvage page track list:\n");

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
			    (unsigned long long)WT_TRACK_RECNO_START(trk),
			    (unsigned long long)WT_TRACK_RECNO_STOP(trk));
		}
		break;
	case WT_PAGE_ROW_LEAF:
		for (i = 0; i < ss->pages_next; ++i) {
			if ((trk = ss->pages[i]) == NULL)
				continue;
			fprintf(stderr, "%6lu/%-6lu (%llu)\n\t%.*s\n\t%.*s\n",
			    (u_long)trk->addr, (u_long)trk->size,
			    (unsigned long long)trk->lsn,
			    WT_TRACK_KEY_START_SIZE(trk),
			    (char *)WT_TRACK_KEY_START(trk),
			    WT_TRACK_KEY_STOP_SIZE(trk),
			    (char *)WT_TRACK_KEY_STOP(trk));
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
