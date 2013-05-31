/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

struct __wt_stuff; 		typedef struct __wt_stuff WT_STUFF;
struct __wt_track; 		typedef struct __wt_track WT_TRACK;

/*
 * There's a bunch of stuff we pass around during salvage, group it together
 * to make the code prettier.
 */
struct __wt_stuff {
	WT_SESSION_IMPL *session;		/* Salvage session */
	WT_BTREE  *btree;			/* Enclosing Btree */

	WT_TRACK **pages;			/* Pages */
	uint32_t   pages_next;			/* Next empty slot */
	size_t     pages_allocated;		/* Bytes allocated */

	WT_TRACK **ovfl;			/* Overflow pages */
	uint32_t   ovfl_next;			/* Next empty slot */
	size_t     ovfl_allocated;		/* Bytes allocated */

	WT_PAGE	  *root_page;			/* Created root page */

	uint8_t    page_type;			/* Page type */

	/* If need to free blocks backing merged page ranges. */
	int	   merge_free;

	WT_ITEM	  *tmp1;			/* Verbose print buffer */
	WT_ITEM	  *tmp2;			/* Verbose print buffer */

	uint64_t fcnt;				/* Progress counter */
};

/*
 * WT_TRACK --
 *	Structure to track validated pages, one per page.
 */
struct __wt_track {
	WT_STUFF *ss;				/* Enclosing stuff */

	WT_ADDR  addr;				/* Page address */
	uint32_t size;				/* Page size */
	uint64_t gen;				/* Page generation */

	/*
	 * Pages that reference overflow pages contain a list of the overflow
	 * pages they reference.
	 */
	WT_ADDR	*ovfl;				/* Referenced overflow pages */
	uint32_t ovfl_cnt;			/* Overflow list elements */

	union {
		struct {
#undef	row_start
#define	row_start	u.row._row_start
			WT_ITEM   _row_start;	/* Row-store start range */
#undef	row_stop
#define	row_stop	u.row._row_stop
			WT_ITEM   _row_stop;	/* Row-store stop range */
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
static int  __slvg_col_build_internal(WT_SESSION_IMPL *, uint32_t, WT_STUFF *);
static int  __slvg_col_build_leaf(
		WT_SESSION_IMPL *, WT_TRACK *, WT_PAGE *, WT_REF *);
static int  __slvg_col_merge_ovfl(
		WT_SESSION_IMPL *, WT_TRACK *, WT_PAGE *, uint64_t, uint64_t);
static int  __slvg_col_range(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_col_range_missing(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_col_range_overlap(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static void __slvg_col_trk_update_start(uint32_t, WT_STUFF *);
static int  __slvg_merge_block_free(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_ovfl_compare(const void *, const void *);
static int  __slvg_ovfl_discard(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_ovfl_reconcile(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_read(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_row_build_internal(WT_SESSION_IMPL *, uint32_t, WT_STUFF *);
static int  __slvg_row_build_leaf(WT_SESSION_IMPL *,
		WT_TRACK *, WT_PAGE *, WT_REF *, WT_STUFF *);
static int  __slvg_row_merge_ovfl(
		WT_SESSION_IMPL *, WT_TRACK *, WT_PAGE *, uint32_t, uint32_t);
static int  __slvg_row_range(WT_SESSION_IMPL *, WT_STUFF *);
static int  __slvg_row_range_overlap(
		WT_SESSION_IMPL *, uint32_t, uint32_t, WT_STUFF *);
static int  __slvg_row_trk_update_start(
		WT_SESSION_IMPL *, WT_ITEM *, uint32_t, WT_STUFF *);
static int  __slvg_trk_compare_addr(const void *, const void *);
static int  __slvg_trk_compare_gen(const void *, const void *);
static int  __slvg_trk_compare_key(const void *, const void *);
static int  __slvg_trk_free(WT_SESSION_IMPL *, WT_TRACK **, uint32_t);
static int  __slvg_trk_init(WT_SESSION_IMPL *, uint8_t *,
		uint32_t, uint32_t, uint64_t, WT_STUFF *, WT_TRACK **);
static int  __slvg_trk_leaf(WT_SESSION_IMPL *,
		WT_PAGE_HEADER *, uint8_t *, uint32_t, WT_STUFF *);
static int  __slvg_trk_leaf_ovfl(
		WT_SESSION_IMPL *, WT_PAGE_HEADER *, WT_TRACK *);
static int  __slvg_trk_ovfl(WT_SESSION_IMPL *,
		WT_PAGE_HEADER *, uint8_t *, uint32_t, WT_STUFF *);

/*
 * __wt_bt_salvage --
 *	Salvage a Btree.
 */
int
__wt_bt_salvage(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, const char *cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_STUFF *ss, stuff;
	uint32_t i, leaf_cnt;

	WT_UNUSED(cfg);

	btree = S2BT(session);
	bm = btree->bm;

	WT_CLEAR(stuff);
	ss = &stuff;
	ss->session = session;
	ss->btree = btree;
	ss->page_type = WT_PAGE_INVALID;

	/* Allocate temporary buffers. */
	WT_ERR(__wt_scr_alloc(session, 0, &ss->tmp1));
	WT_ERR(__wt_scr_alloc(session, 0, &ss->tmp2));

	/*
	 * Step 1:
	 * Inform the underlying block manager that we're salvaging the file.
	 */
	WT_ERR(bm->salvage_start(bm, session));

	/*
	 * Step 2:
	 * Read the file and build in-memory structures that reference any leaf
	 * or overflow page.  Any pages other than leaf or overflow pages are
	 * added to the free list.
	 *
	 * Turn off read checksum and verification error messages while we're
	 * reading the file, we expect to see corrupted blocks.
	 */
	F_SET(session, WT_SESSION_SALVAGE_QUIET_ERR);
	ret = __slvg_read(session, ss);
	F_CLR(session, WT_SESSION_SALVAGE_QUIET_ERR);
	WT_ERR(ret);

	/*
	 * Step 3:
	 * Review the relationships between the pages and the overflow items.
	 *
	 * Step 4:
	 * Add unreferenced overflow page blocks to the free list.
	 */
	if (ss->ovfl_next != 0) {
		WT_ERR(__slvg_ovfl_reconcile(session, ss));
		WT_ERR(__slvg_ovfl_discard(session, ss));
	}

	/*
	 * Step 5:
	 * Walk the list of pages looking for overlapping ranges to resolve.
	 * If we find a range that needs to be resolved, set a global flag
	 * and a per WT_TRACK flag on the pages requiring modification.
	 *
	 * This requires sorting the page list by key, and secondarily by LSN.
	 *
	 * !!!
	 * It's vanishingly unlikely and probably impossible for fixed-length
	 * column-store files to have overlapping key ranges.  It's possible
	 * for an entire key range to go missing (if a page is corrupted and
	 * lost), but because pages can't split, it shouldn't be possible to
	 * find pages where the key ranges overlap.  That said, we check for
	 * it and clean up after it in reconciliation because it doesn't cost
	 * much and future column-store formats or operations might allow for
	 * fixed-length format ranges to overlap during salvage, and I don't
	 * want to have to retrofit the code later.
	 */
	qsort(ss->pages,
	    (size_t)ss->pages_next, sizeof(WT_TRACK *), __slvg_trk_compare_key);
	if (ss->page_type == WT_PAGE_ROW_LEAF)
		WT_ERR(__slvg_row_range(session, ss));
	else
		WT_ERR(__slvg_col_range(session, ss));

	/*
	 * Step 6:
	 * We may have lost key ranges in column-store databases, that is, some
	 * part of the record number space is gone.   Look for missing ranges.
	 */
	switch (ss->page_type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		WT_ERR(__slvg_col_range_missing(session, ss));
		break;
	case WT_PAGE_ROW_LEAF:
		break;
	}

	/*
	 * Step 7:
	 * Build an internal page that references all of the leaf pages,
	 * and write it, as well as any merged pages, to the file.
	 *
	 * Count how many leaf pages we have (we could track this during the
	 * array shuffling/splitting, but that's a lot harder).
	 */
	for (leaf_cnt = i = 0; i < ss->pages_next; ++i)
		if (ss->pages[i] != NULL)
			++leaf_cnt;
	if (leaf_cnt != 0)
		switch (ss->page_type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_ERR(
			    __slvg_col_build_internal(session, leaf_cnt, ss));
			break;
		case WT_PAGE_ROW_LEAF:
			WT_ERR(
			    __slvg_row_build_internal(session, leaf_cnt, ss));
			break;
		}

	/*
	 * Step 8:
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

	/*
	 * Step 9:
	 * Evict the newly created root page, creating a checkpoint.
	 */
	if (ss->root_page != NULL) {
		btree->ckpt = ckptbase;
		ret = __wt_rec_evict(session, ss->root_page, 1);
		btree->ckpt = NULL;
		ss->root_page = NULL;
	}

	/*
	 * Step 10:
	 * Inform the underlying block manager that we're done.
	 */
err:	WT_TRET(bm->salvage_end(bm, session));

	/* Discard any root page we created. */
	if (ss->root_page != NULL)
		__wt_page_out(session, &ss->root_page);

	/* Discard the leaf and overflow page memory. */
	WT_TRET(__slvg_cleanup(session, ss));

	/* Discard temporary buffers. */
	__wt_scr_free(&ss->tmp1);
	__wt_scr_free(&ss->tmp2);

	/* Wrap up reporting. */
	WT_TRET(__wt_progress(session, NULL, ss->fcnt));

	return (ret);
}

/*
 * __slvg_read --
 *	Read the file and build a table of the pages we can use.
 */
static int
__slvg_read(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_BM *bm;
	WT_DECL_ITEM(as);
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_PAGE_HEADER *dsk;
	uint32_t addrbuf_size;
	uint8_t addrbuf[WT_BTREE_MAX_ADDR_COOKIE];
	int eof;

	bm = S2BT(session)->bm;
	WT_ERR(__wt_scr_alloc(session, 0, &as));
	WT_ERR(__wt_scr_alloc(session, 0, &buf));

	for (;;) {
		/* Get the next block address from the block manager. */
		WT_ERR(bm->salvage_next(
		    bm, session, addrbuf, &addrbuf_size, &eof));
		if (eof)
			break;

		/* Report progress every 10 chunks. */
		if (++ss->fcnt % 10 == 0)
			WT_ERR(__wt_progress(session, NULL, ss->fcnt));

		/*
		 * Read (and potentially decompress) the block; the underlying
		 * block manager might only return good blocks if checksums are
		 * configured, else we may be relying on compression.  If the
		 * read fails, simply move to the next potential block.
		 */
		if (__wt_bt_read(session, buf, addrbuf, addrbuf_size) != 0)
			continue;

		/* Tell the block manager we're taking this one. */
		WT_ERR(bm->salvage_valid(bm, session, addrbuf, addrbuf_size));

		/* Create a printable version of the address. */
		WT_ERR(bm->addr_string(bm, session, as, addrbuf, addrbuf_size));

		/*
		 * Make sure it's an expected page type for the file.
		 *
		 * We only care about leaf and overflow pages from here on out;
		 * discard all of the others.  We put them on the free list now,
		 * because we might as well overwrite them, we want the file to
		 * grow as little as possible, or shrink, and future salvage
		 * calls don't need them either.
		 */
		dsk = buf->mem;
		switch (dsk->type) {
		case WT_PAGE_BLOCK_MANAGER:
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			WT_VERBOSE_ERR(session, salvage,
			    "%s page ignored %s",
			    __wt_page_type_string(dsk->type),
			    (const char *)as->data);
			WT_ERR(bm->free(bm, session, addrbuf, addrbuf_size));
			continue;
		}

		/*
		 * Verify the page.  It's unlikely a page could have a valid
		 * checksum and still be broken, but paranoia is healthy in
		 * salvage.  Regardless, verify does return failure because
		 * it detects failures we'd expect to see in a corrupted file,
		 * like overflow references past the end of the file or
		 * overflow references to non-existent pages, might as well
		 * discard these pages now.
		 */
		if (__wt_verify_dsk(session, as->data, buf) != 0) {
			WT_VERBOSE_ERR(session, salvage,
			    "%s page failed verify %s",
			    __wt_page_type_string(dsk->type),
			    (const char *)as->data);
			WT_ERR(bm->free(bm, session, addrbuf, addrbuf_size));
			continue;
		}

		WT_VERBOSE_ERR(session, salvage,
		    "tracking %s page, generation %" PRIu64 " %s",
		    __wt_page_type_string(dsk->type), dsk->write_gen,
		    (const char *)as->data);

		switch (dsk->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			if (ss->page_type == WT_PAGE_INVALID)
				ss->page_type = dsk->type;
			if (ss->page_type != dsk->type)
				WT_ERR_MSG(session, WT_ERROR,
				    "file contains multiple file formats (both "
				    "%s and %s), and cannot be salvaged",
				    __wt_page_type_string(ss->page_type),
				    __wt_page_type_string(dsk->type));

			WT_ERR(__slvg_trk_leaf(
			    session, dsk, addrbuf, addrbuf_size, ss));
			break;
		case WT_PAGE_OVFL:
			WT_ERR(__slvg_trk_ovfl(
			    session, dsk, addrbuf, addrbuf_size, ss));
			break;
		}
	}

err:	__wt_scr_free(&as);
	__wt_scr_free(&buf);

	return (ret);
}

/*
 * __slvg_trk_init --
 *	Initialize tracking information for a page.
 */
static int
__slvg_trk_init(WT_SESSION_IMPL *session,
    uint8_t *addr, uint32_t addr_size,
    uint32_t size, uint64_t gen, WT_STUFF *ss, WT_TRACK **retp)
{
	WT_DECL_RET;
	WT_TRACK *trk;

	WT_RET(__wt_calloc_def(session, 1, &trk));
	trk->ss = ss;

	WT_ERR(__wt_strndup(session, (char *)addr, addr_size, &trk->addr.addr));
	trk->addr.size = addr_size;
	trk->size = size;
	trk->gen = gen;

	*retp = trk;
	return (0);

err:	if (trk->addr.addr != NULL)
		__wt_free(session, trk->addr.addr);
	if (trk != NULL)
		__wt_free(session, trk);
	return (ret);
}

/*
 * __slvg_trk_leaf --
 *	Track a leaf page.
 */
static int
__slvg_trk_leaf(WT_SESSION_IMPL *session,
    WT_PAGE_HEADER *dsk, uint8_t *addr, uint32_t size, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_TRACK *trk;
	uint64_t stop_recno;
	uint32_t i;

	btree = S2BT(session);
	unpack = &_unpack;
	page = NULL;
	trk = NULL;

	/* Re-allocate the array of pages, as necessary. */
	WT_RET(__wt_realloc_def(
	    session, &ss->pages_allocated, ss->pages_next + 1, &ss->pages));

	/* Allocate a WT_TRACK entry for this new page and fill it in. */
	WT_RET(__slvg_trk_init(
	    session, addr, size, dsk->mem_size, dsk->write_gen, ss, &trk));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Column-store fixed-sized format: start and stop keys can be
		 * taken from the block's header, and doesn't contain overflow
		 * items.
		 */
		trk->col_start = dsk->recno;
		trk->col_stop = dsk->recno + (dsk->u.entries - 1);

		WT_VERBOSE_ERR(session, salvage,
		    "%s records %" PRIu64 "-%" PRIu64,
		    __wt_addr_string(
		    session, ss->tmp1, trk->addr.addr, trk->addr.size),
		    trk->col_start, trk->col_stop);
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Column-store variable-length format: the start key can be
		 * taken from the block's header, stop key requires walking
		 * the page.
		 */
		stop_recno = dsk->recno;
		WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
			__wt_cell_unpack(cell, unpack);
			stop_recno += __wt_cell_rle(unpack);
		}

		trk->col_start = dsk->recno;
		trk->col_stop = stop_recno - 1;

		WT_VERBOSE_ERR(session, salvage,
		    "%s records %" PRIu64 "-%" PRIu64,
		    __wt_addr_string(
		    session, ss->tmp1, trk->addr.addr, trk->addr.size),
		    trk->col_start, trk->col_stop);

		/* Column-store pages can contain overflow items. */
		WT_ERR(__slvg_trk_leaf_ovfl(session, dsk, trk));
		break;
	case WT_PAGE_ROW_LEAF:
		/*
		 * Row-store format: copy the first and last keys on the page.
		 * Keys are prefix-compressed, the simplest and slowest thing
		 * to do is instantiate the in-memory page (which instantiates
		 * the prefix keys at specific split points), then instantiate
		 * and copy the full keys, then free the page.   We do this
		 * on every leaf page, and if you need to speed up the salvage,
		 * it's probably a great place to start.
		 */
		WT_ERR(__wt_page_inmem(session, NULL, NULL, dsk, 1, &page));
		WT_ERR(__wt_row_key_copy(session,
		    page, &page->u.row.d[0], &trk->row_start));
		WT_ERR(__wt_row_key_copy(session,
		    page, &page->u.row.d[page->entries - 1], &trk->row_stop));

		if (WT_VERBOSE_ISSET(session, salvage)) {
			WT_ERR(__wt_buf_set_printable(session, ss->tmp1,
			    trk->row_start.data, trk->row_start.size));
			WT_VERBOSE_ERR(session, salvage,
			    "%s start key %.*s",
			    __wt_addr_string(session,
			    ss->tmp2, trk->addr.addr, trk->addr.size),
			    (int)ss->tmp1->size, (char *)ss->tmp1->data);
			WT_ERR(__wt_buf_set_printable(session, ss->tmp1,
			    trk->row_stop.data, trk->row_stop.size));
			WT_VERBOSE_ERR(session, salvage,
			    "%s stop key %.*s",
			    __wt_addr_string(session,
			    ss->tmp2, trk->addr.addr, trk->addr.size),
			    (int)ss->tmp1->size, (char *)ss->tmp1->data);
		}

		/* Row-store pages can contain overflow items. */
		WT_ERR(__slvg_trk_leaf_ovfl(session, dsk, trk));
		break;
	}
	ss->pages[ss->pages_next++] = trk;

	if (0) {
err:		__wt_free(session, trk);
	}
	if (page != NULL)
		__wt_page_out(session, &page);
	return (ret);
}

/*
 * __slvg_trk_ovfl --
 *	Track an overflow page.
 */
static int
__slvg_trk_ovfl(WT_SESSION_IMPL *session,
    WT_PAGE_HEADER *dsk, uint8_t *addr, uint32_t size, WT_STUFF *ss)
{
	WT_TRACK *trk;

	/*
	 * Reallocate the overflow page array as necessary, then save the
	 * page's location information.
	 */
	WT_RET(__wt_realloc_def(
	    session, &ss->ovfl_allocated, ss->ovfl_next + 1, &ss->ovfl));

	WT_RET(__slvg_trk_init(
	    session, addr, size, dsk->mem_size, dsk->write_gen, ss, &trk));
	ss->ovfl[ss->ovfl_next++] = trk;

	return (0);
}

/*
 * __slvg_trk_leaf_ovfl --
 *	Search a leaf page for overflow items.
 */
static int
__slvg_trk_leaf_ovfl(
    WT_SESSION_IMPL *session, WT_PAGE_HEADER *dsk, WT_TRACK *trk)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i, ovfl_cnt;

	btree = S2BT(session);
	unpack = &_unpack;

	/*
	 * Two passes: count the overflow items, then copy them into an
	 * allocated array.
	 */
	ovfl_cnt = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (unpack->ovfl)
			++ovfl_cnt;
	}
	if (ovfl_cnt == 0)
		return (0);

	WT_RET(__wt_calloc_def(session, ovfl_cnt, &trk->ovfl));
	trk->ovfl_cnt = ovfl_cnt;

	ovfl_cnt = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (unpack->ovfl) {
			WT_RET(__wt_strndup(session, unpack->data,
			    unpack->size, &trk->ovfl[ovfl_cnt].addr));
			trk->ovfl[ovfl_cnt].size = unpack->size;

			WT_VERBOSE_RET(session, salvage,
			    "%s overflow reference %s",
			    __wt_addr_string(session,
			    trk->ss->tmp1, trk->addr.addr, trk->addr.size),
			    __wt_addr_string(session,
			    trk->ss->tmp2, unpack->data, unpack->size));

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
	WT_TRACK *jtrk;
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
			jtrk = ss->pages[j];
			WT_RET(__slvg_col_range_overlap(session, i, j, ss));

			/*
			 * If the overlap resolution changed the entry's start
			 * key, the entry might have moved and the page array
			 * re-sorted, and pages[j] would reference a different
			 * page.  We don't move forward if that happened, we
			 * re-process the slot again (by decrementing j before
			 * the loop's increment).
			 */
			if (ss->pages[j] != NULL && jtrk != ss->pages[j])
				--j;
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

	WT_VERBOSE_RET(session, salvage,
	    "%s and %s range overlap",
	    __wt_addr_string(
	    session, ss->tmp1, a_trk->addr.addr, a_trk->addr.size),
	    __wt_addr_string(
	    session, ss->tmp2, b_trk->addr.addr, b_trk->addr.size));

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
	 * #7	AAAAAAAAAAAAA				same as #3
	 * #8			AAAAAAAAAAAAAAAA	same as #2
	 * #9		AAAAA				A is a prefix of B
	 * #10			AAAAAA			A is middle of B
	 * #11			AAAAAAAAAA		A is a suffix of B
	 *
	 * Because the leaf page array was sorted by record number and a_trk
	 * appears earlier in that array than b_trk, cases #2/8, #10 and #11
	 * are impossible.
	 *
	 * Finally, there's one additional complicating factor -- final ranges
	 * are assigned based on the page's LSN.
	 */
	if (a_trk->col_start == b_trk->col_start) {	/* Case #1, #4 and #9 */
		/*
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

	if (a_trk->col_stop == b_trk->col_stop) {	/* Case #6 */
		if (a_trk->gen > b_trk->gen)
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

	if  (a_trk->col_stop < b_trk->col_stop) {	/* Case #3/7 */
		if (a_trk->gen > b_trk->gen) {
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
	if (a_trk->gen > b_trk->gen) {
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
	WT_RET(__slvg_trk_init(session, a_trk->addr.addr,
	    a_trk->addr.size, a_trk->size, a_trk->gen, ss, &new));

	/*
	 * Second, reallocate the array of pages if necessary, and then insert
	 * the new element into the array after the existing element (that's
	 * probably wrong, but we'll fix it up in a second).
	 */
	WT_RET(__wt_realloc_def(
	    session, &ss->pages_allocated, ss->pages_next + 1, &ss->pages));
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

merge:	WT_VERBOSE_RET(session, salvage,
	    "%s and %s require merge",
	    __wt_addr_string(
	    session, ss->tmp1, a_trk->addr.addr, a_trk->addr.size),
	    __wt_addr_string(
	    session, ss->tmp2, b_trk->addr.addr, b_trk->addr.size));
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
static int
__slvg_col_range_missing(WT_SESSION_IMPL *session, WT_STUFF *ss)
{
	WT_TRACK *trk;
	uint64_t r;
	uint32_t i;

	for (i = 0, r = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;
		if (trk->col_start != r + 1) {
			WT_VERBOSE_RET(session, salvage,
			    "%s column-store missing range from %"
			    PRIu64 " to %" PRIu64 " inclusive",
			    __wt_addr_string(session,
			    ss->tmp1, trk->addr.addr, trk->addr.size),
			    r + 1, trk->col_start - 1);

			/*
			 * We need to instantiate deleted items for the missing
			 * record range.
			 */
			trk->col_missing = r + 1;
			F_SET(trk, WT_TRACK_MERGE);
		}
		r = trk->col_stop;
	}
	return (0);
}

/*
 * __slvg_modify_init --
 *	Initialize a salvage page's modification information.
 */
static int
__slvg_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/* The tree is dirty. */
	btree->modified = 1;

	/* The page is dirty. */
	WT_RET(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	return (0);
}

/*
 * __slvg_col_build_internal --
 *	Build a column-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_col_build_internal(
    WT_SESSION_IMPL *session, uint32_t leaf_cnt, WT_STUFF *ss)
{
	WT_ADDR *addr;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;
	WT_TRACK *trk;
	uint32_t i;

	/* Allocate a column-store root (internal) page and fill it in. */
	WT_RET(__wt_page_alloc(session, WT_PAGE_COL_INT, leaf_cnt, &page));
	page->parent = NULL;				/* Root page */
	page->ref = NULL;
	page->read_gen = WT_READ_GEN_NOTSET;
	page->u.intl.recno = 1;
	page->entries = leaf_cnt;
	WT_ERR(__slvg_modify_init(session, page));

	for (ref = page->u.intl.t, i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		WT_ERR(__wt_calloc(session, 1, sizeof(WT_ADDR), &addr));
		WT_ERR(__wt_strndup(session,
		    (char *)trk->addr.addr, trk->addr.size, &addr->addr));
		addr->size = trk->addr.size;
		addr->leaf_no_overflow = trk->ovfl_cnt == 0 ? 1 : 0;

		ref->page = NULL;
		ref->addr = addr;
		ref->u.recno = trk->col_start;
		ref->state = WT_REF_DISK;

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

			WT_ERR(__slvg_col_build_leaf(session, trk, page, ref));
		}
		++ref;
	}

	ss->root_page = page;

	if (0) {
err:		__wt_page_out(session, &page);
	}
	return (ret);
}

/*
 * __slvg_col_build_leaf --
 *	Build a column-store leaf page for a merged page.
 */
static int
__slvg_col_build_leaf(
    WT_SESSION_IMPL *session, WT_TRACK *trk, WT_PAGE *parent, WT_REF *ref)
{
	WT_COL *save_col_var;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_SALVAGE_COOKIE *cookie, _cookie;
	uint64_t skip, take;
	uint32_t save_entries;

	cookie = &_cookie;
	WT_CLEAR(*cookie);

	/* Get the original page, including the full in-memory setup. */
	WT_RET(__wt_page_in(session, parent, ref));
	page = ref->page;
	save_col_var = page->u.col_var.d;
	save_entries = page->entries;

	/*
	 * Calculate the number of K/V entries we are going to skip, and
	 * the total number of K/V entries we'll take from this page.
	 */
	cookie->skip = skip = trk->col_start - page->u.col_var.recno;
	cookie->take = take = (trk->col_stop - trk->col_start) + 1;

	WT_VERBOSE_ERR(session, salvage,
	    "%s merge discarding first %" PRIu64 " records, "
	    "then taking %" PRIu64 " records",
	    __wt_addr_string(
	    session, trk->ss->tmp1, trk->addr.addr, trk->addr.size),
	    skip, take);

	/*
	 * Discard backing overflow pages for any items being discarded that
	 * reference overflow pages.
	 */
	if (page->type == WT_PAGE_COL_VAR)
		WT_ERR(__slvg_col_merge_ovfl(session, trk, page, skip, take));

	/*
	 * If we're missing some part of the range, the real start range is in
	 * trk->col_missing, else, it's in trk->col_start.  Update the parent's
	 * reference as well as the page itself.
	 */
	if (trk->col_missing == 0)
		page->u.col_var.recno = trk->col_start;
	else {
		page->u.col_var.recno = trk->col_missing;
		cookie->missing = trk->col_start - trk->col_missing;

		WT_VERBOSE_ERR(session, salvage,
		    "%s merge inserting %" PRIu64 " missing records",
		    __wt_addr_string(
		    session, trk->ss->tmp1, trk->addr.addr, trk->addr.size),
		    cookie->missing);
	}
	ref->u.recno = page->u.col_var.recno;

	/*
	 * We can't discard the original blocks associated with this page now.
	 * (The problem is we don't want to overwrite any original information
	 * until the salvage run succeeds -- if we free the blocks now, the next
	 * merge page we write might allocate those blocks and overwrite them,
	 * and should the salvage run eventually fail, the original information
	 * would have been lost.)  Clear the reference addr so eviction doesn't
	 * free the underlying blocks.
	 */
	__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
	__wt_free(session, ref->addr);
	ref->addr = NULL;

	/* Write the new version of the leaf page to disk. */
	WT_ERR(__slvg_modify_init(session, page));
	WT_ERR(__wt_rec_write(session, page, cookie, WT_SKIP_UPDATE_ERR));

	/* Reset the page. */
	page->u.col_var.d = save_col_var;
	page->entries = save_entries;

	ret = __wt_page_release(session, page);
	if (ret == 0)
		ret = __wt_rec_evict(session, page, 1);

	if (0) {
err:		WT_TRET(__wt_page_release(session, page));
	}

	return (ret);
}

/*
 * __slvg_col_merge_ovfl --
 *	Free file blocks referenced from keys discarded from merged pages.
 */
static int
__slvg_col_merge_ovfl(WT_SESSION_IMPL *session,
    WT_TRACK *trk, WT_PAGE *page, uint64_t skip, uint64_t take)
{
	WT_BM *bm;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_CELL *cell;
	WT_COL *cip;
	uint64_t recno, start, stop;
	uint32_t i;

	bm = S2BT(session)->bm;
	unpack = &_unpack;

	recno = page->u.col_var.recno;
	start = recno + skip;
	stop = (recno + skip + take) - 1;

	WT_COL_FOREACH(page, cip, i) {
		cell = WT_COL_PTR(page, cip);
		__wt_cell_unpack(cell, unpack);
		recno += __wt_cell_rle(unpack);

		if (unpack->type != WT_CELL_VALUE_OVFL)
			continue;
		if (recno >= start && recno <= stop)
			continue;

		WT_VERBOSE_RET(session, salvage,
		    "%s merge discard freed overflow reference %s",
		    __wt_addr_string(session,
			trk->ss->tmp1, trk->addr.addr, trk->addr.size),
		    __wt_addr_string(session,
			trk->ss->tmp2, unpack->data, unpack->size));

		WT_RET(bm->free(bm, session, unpack->data, unpack->size));
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
	WT_TRACK *jtrk;
	WT_BTREE *btree;
	uint32_t i, j;
	int cmp;

	btree = S2BT(session);

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
			WT_RET(WT_BTREE_CMP(session, btree,
			    &ss->pages[j]->row_start,
			    &ss->pages[i]->row_stop, cmp));
			if (cmp > 0)
				break;

			/* There's an overlap, fix it up. */
			jtrk = ss->pages[j];
			WT_RET(__slvg_row_range_overlap(session, i, j, ss));

			/*
			 * If the overlap resolution changed the entry's start
			 * key, the entry might have moved and the page array
			 * re-sorted, and pages[j] would reference a different
			 * page.  We don't move forward if that happened, we
			 * re-process the slot again (by decrementing j before
			 * the loop's increment).
			 */
			if (ss->pages[j] != NULL && jtrk != ss->pages[j])
				--j;
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
	int cmp;

	/*
	 * DO NOT MODIFY THIS CODE WITHOUT REVIEWING THE CORRESPONDING ROW- OR
	 * COLUMN-STORE CODE: THEY ARE IDENTICAL OTHER THAN THE PAGES THAT ARE
	 * BEING HANDLED.
	 */
	btree = S2BT(session);

	a_trk = ss->pages[a_slot];
	b_trk = ss->pages[b_slot];

	WT_VERBOSE_RET(session, salvage,
	    "%s and %s range overlap",
	    __wt_addr_string(
	    session, ss->tmp1, a_trk->addr.addr, a_trk->addr.size),
	    __wt_addr_string(
	    session, ss->tmp2, b_trk->addr.addr, b_trk->addr.size));

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
	 * #7	AAAAAAAAAAAAA				same as #3
	 * #8			AAAAAAAAAAAAAAAA	same as #2
	 * #9		AAAAA				A is a prefix of B
	 * #10			AAAAAA			A is middle of B
	 * #11			AAAAAAAAAA		A is a suffix of B
	 *
	 * Because the leaf page array was sorted by record number and a_trk
	 * appears earlier in that array than b_trk, cases #2/8, #10 and #11
	 * are impossible.
	 *
	 * Finally, there's one additional complicating factor -- final ranges
	 * are assigned based on the page's LSN.
	 */
#define	A_TRK_START	(&a_trk->row_start)
#define	A_TRK_STOP	(&a_trk->row_stop)
#define	B_TRK_START	(&b_trk->row_start)
#define	B_TRK_STOP	(&b_trk->row_stop)
#define	SLOT_START(i)	(&ss->pages[i]->row_start)
#define	__slvg_key_copy(session, dst, src)				\
	__wt_buf_set(session, dst, (src)->data, (src)->size)

	WT_RET(WT_BTREE_CMP(session, btree, A_TRK_START, B_TRK_START, cmp));
	if (cmp == 0) {					/* Case #1, #4, #9 */
		/*
		 * The secondary sort of the leaf page array was the page's LSN,
		 * in high-to-low order, which means a_trk has a higher LSN, and
		 * is more desirable, than b_trk.  In cases #1 and #4 and #9,
		 * where the start of the range is the same for the two pages,
		 * this simplifies things, it guarantees a_trk has a higher LSN
		 * than b_trk.
		 */
		WT_RET(
		    WT_BTREE_CMP(session, btree, A_TRK_STOP, B_TRK_STOP, cmp));
		if (cmp >= 0)
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

	WT_RET(WT_BTREE_CMP(session, btree, A_TRK_STOP, B_TRK_STOP, cmp));
	if (cmp == 0) {					/* Case #6 */
		if (a_trk->gen > b_trk->gen)
			/*
			 * Case #6: a_trk is a superset of b_trk and a_trk is
			 * more desirable -- discard b_trk.
			 */
			goto delete;

		/*
		 * Case #6: a_trk is a superset of b_trk, but b_trk is more
		 * desirable: keep both but delete b_trk's key range from a_trk.
		 */
		WT_RET(__slvg_key_copy(session, A_TRK_STOP, B_TRK_START));
		F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);
		goto merge;
	}

	WT_RET(WT_BTREE_CMP(session, btree, A_TRK_STOP, B_TRK_STOP, cmp));
	if (cmp < 0) {					/* Case #3/7 */
		if (a_trk->gen > b_trk->gen) {
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
			    session, A_TRK_STOP, B_TRK_START));
			F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);
		}
		goto merge;
	}

	/*
	 * Case #5: a_trk is a superset of b_trk and a_trk is more desirable --
	 * discard b_trk.
	 */
	if (a_trk->gen > b_trk->gen) {
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
	WT_RET(__slvg_trk_init(session, a_trk->addr.addr,
	    a_trk->addr.size, a_trk->size, a_trk->gen, ss, &new));

	/*
	 * Second, reallocate the array of pages if necessary, and then insert
	 * the new element into the array after the existing element (that's
	 * probably wrong, but we'll fix it up in a second).
	 */
	WT_RET(__wt_realloc_def(
	    session, &ss->pages_allocated, ss->pages_next + 1, &ss->pages));
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
	WT_RET(__slvg_key_copy(session, &new->row_stop, A_TRK_STOP));
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
	WT_RET(__slvg_key_copy(session, A_TRK_STOP, B_TRK_START));
	F_SET(a_trk, WT_TRACK_CHECK_STOP | WT_TRACK_MERGE);

merge:	WT_VERBOSE_RET(session, salvage,
	    "%s and %s require merge",
	    __wt_addr_string(
	    session, ss->tmp1, a_trk->addr.addr, a_trk->addr.size),
	    __wt_addr_string(
	    session, ss->tmp2, b_trk->addr.addr, b_trk->addr.size));
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
	WT_DECL_ITEM(dsk);
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_TRACK *trk;
	uint32_t i;
	int cmp, found;

	btree = S2BT(session);
	page = NULL;
	found = 0;

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
	 * Read and instantiate the WT_TRACK page (we don't have to verify the
	 * page, nor do we have to be quiet on error, we've already read this
	 * page successfully).
	 */
	WT_RET(__wt_scr_alloc(session, trk->size, &dsk));
	WT_ERR(__wt_bt_read(session, dsk, trk->addr.addr, trk->addr.size));
	WT_ERR(__wt_page_inmem(session, NULL, NULL, dsk->mem, 1, &page));

	/*
	 * Walk the page, looking for a key sorting greater than the specified
	 * stop key -- that's our new start key.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &key));
	WT_ROW_FOREACH(page, rip, i) {
		WT_ERR(__wt_row_key(session, page, rip, key, 0));
		WT_ERR(WT_BTREE_CMP(session, btree, key, stop, cmp));
		if (cmp > 0) {
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
	WT_ERR_TEST(!found, WT_ERROR);
	WT_ERR(__slvg_key_copy(session, &trk->row_start, key));

	/*
	 * We may need to re-sort some number of elements in the list.  Walk
	 * forward in the list until reaching an entry which cannot overlap
	 * the adjusted entry.  If it's more than a single slot, re-sort the
	 * entries.
	 */
	for (i = slot + 1; i < ss->pages_next; ++i) {
		if (ss->pages[i] == NULL)
			continue;
		WT_ERR(WT_BTREE_CMP(session, btree,
		    SLOT_START(i), &trk->row_stop, cmp));
		if (cmp > 0)
			break;
	}
	i -= slot;
	if (i > 1)
		qsort(ss->pages + slot, (size_t)i,
		    sizeof(WT_TRACK *), __slvg_trk_compare_key);

	if (page != NULL)
		__wt_page_out(session, &page);

err:	__wt_scr_free(&dsk);
	__wt_scr_free(&key);

	return (ret);
}

/*
 * __slvg_row_build_internal --
 *	Build a row-store in-memory page that references all of the leaf
 *	pages we've found.
 */
static int
__slvg_row_build_internal(
    WT_SESSION_IMPL *session, uint32_t leaf_cnt,  WT_STUFF *ss)
{
	WT_ADDR *addr;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *ref;
	WT_TRACK *trk;
	uint32_t i;

	/* Allocate a row-store root (internal) page and fill it in. */
	WT_RET(__wt_page_alloc(session, WT_PAGE_ROW_INT, leaf_cnt, &page));
	page->parent = NULL;
	page->ref = NULL;
	page->read_gen = WT_READ_GEN_NOTSET;
	page->entries = leaf_cnt;
	WT_ERR(__slvg_modify_init(session, page));

	for (ref = page->u.intl.t, i = 0; i < ss->pages_next; ++i) {
		if ((trk = ss->pages[i]) == NULL)
			continue;

		WT_ERR(__wt_calloc(session, 1, sizeof(WT_ADDR), &addr));
		WT_ERR(__wt_strndup(session,
		    (char *)trk->addr.addr, trk->addr.size, &addr->addr));
		addr->size = trk->addr.size;
		addr->leaf_no_overflow = trk->ovfl_cnt == 0 ? 1 : 0;

		ref->page = NULL;
		ref->addr = addr;
		ref->u.key = NULL;
		ref->state = WT_REF_DISK;

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
			    session, trk, page, ref, ss));
		} else
			WT_ERR(__wt_row_ikey_incr(session, page, 0,
			    trk->row_start.data, trk->row_start.size,
			    &ref->u.key));
		++ref;
	}

	ss->root_page = page;

	if (0) {
err:		__wt_page_out(session, &page);
	}
	return (ret);
}

/*
 * __slvg_row_build_leaf --
 *	Build a row-store leaf page for a merged page.
 */
static int
__slvg_row_build_leaf(WT_SESSION_IMPL *session,
    WT_TRACK *trk, WT_PAGE *parent, WT_REF *ref, WT_STUFF *ss)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_SALVAGE_COOKIE *cookie, _cookie;
	uint32_t i, skip_start, skip_stop;
	int cmp;

	btree = S2BT(session);
	page = NULL;

	cookie = &_cookie;
	WT_CLEAR(*cookie);

	/* Allocate temporary space in which to instantiate the keys. */
	WT_RET(__wt_scr_alloc(session, 0, &key));

	/* Get the original page, including the full in-memory setup. */
	WT_ERR(__wt_page_in(session, parent, ref));
	page = ref->page;

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
			WT_ERR(__wt_row_key(session, page, rip, key, 0));

			/*
			 * >= is correct: see the comment above.
			 */
			WT_ERR(WT_BTREE_CMP(
			    session, btree, key, &trk->row_start, cmp));
			if (cmp >= 0)
				break;
			if (WT_VERBOSE_ISSET(session, salvage)) {
				WT_ERR(__wt_buf_set_printable(session,
				    ss->tmp1, key->data, key->size));
				WT_VERBOSE_ERR(session, salvage,
				    "%s merge discarding leading key %.*s",
				    __wt_addr_string(session,
				    ss->tmp2, trk->addr.addr, trk->addr.size),
				    (int)ss->tmp1->size,
				    (char *)ss->tmp1->data);
			}
			++skip_start;
		}
	if (F_ISSET(trk, WT_TRACK_CHECK_STOP))
		WT_ROW_FOREACH_REVERSE(page, rip, i) {
			WT_ERR(__wt_row_key(session, page, rip, key, 0));

			/*
			 * < is correct: see the comment above.
			 */
			WT_ERR(WT_BTREE_CMP(
			    session, btree, key, &trk->row_stop, cmp));
			if (cmp < 0)
				break;
			if (WT_VERBOSE_ISSET(session, salvage)) {
				WT_ERR(__wt_buf_set_printable(session,
				    ss->tmp1, key->data, key->size));
				WT_VERBOSE_ERR(session, salvage,
				    "%s merge discarding trailing key %.*s",
				    __wt_addr_string(session,
				    ss->tmp2, trk->addr.addr, trk->addr.size),
				    (int)ss->tmp1->size,
				    (char *)ss->tmp1->data);
			}
			++skip_stop;
		}

	/* We should have selected some entries, but not the entire page. */
	WT_ASSERT(session,
	    skip_start + skip_stop > 0 &&
	    skip_start + skip_stop < page->entries);

	/*
	 * Take a copy of this page's first key to define the start of
	 * its range.  The key may require processing, otherwise, it's
	 * a copy from the page.
	 */
	rip = page->u.row.d + skip_start;
	WT_ERR(__wt_row_key(session, page, rip, key, 0));
	WT_ERR(__wt_row_ikey_incr(
	    session, parent, 0, key->data, key->size, &ref->u.key));

	/*
	 * Discard backing overflow pages for any items being discarded that
	 * reference overflow pages.
	 */
	WT_ERR(__slvg_row_merge_ovfl(session, trk, page, 0, skip_start));
	WT_ERR(__slvg_row_merge_ovfl(
	    session, trk, page, page->entries - skip_stop, page->entries));

	/*
	 * If we take all of the keys, we don't write the page and we clear the
	 * merge flags so that the underlying blocks are not later freed (for
	 * merge pages re-written into the file, the underlying blocks have to
	 * be freed, but if this page never gets written, we shouldn't free the
	 * blocks).
	 */
	if (skip_start == 0 && skip_stop == 0)
		F_CLR(trk, WT_TRACK_MERGE);
	else {
		/*
		 * Change the page to reflect the correct record count: there
		 * is no need to copy anything on the page itself, the entries
		 * value limits the number of page items.
		 */
		page->entries -= skip_stop;
		cookie->skip = skip_start;

		/*
		 * We can't discard the original blocks associated with the page
		 * now.  (The problem is we don't want to overwrite any original
		 * information until the salvage run succeeds -- if we free the
		 * blocks now, the next merge page we write might allocate those
		 * blocks and overwrite them, and should the salvage run fail,
		 * the original information would have been lost to subsequent
		 * salvage runs.)  Clear the reference addr so eviction doesn't
		 * free the underlying blocks.
		 */
		__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
		__wt_free(session, ref->addr);
		ref->addr = NULL;

		/* Write the new version of the leaf page to disk. */
		WT_ERR(__slvg_modify_init(session, page));
		WT_ERR(__wt_rec_write(
		    session, page, cookie, WT_SKIP_UPDATE_ERR));

		/* Reset the page. */
		page->entries += skip_stop;
	}

	/*
	 * Discard our hazard pointer and evict the page, updating the
	 * parent's reference.
	 */
	ret = __wt_page_release(session, page);
	if (ret == 0)
		ret = __wt_rec_evict(session, page, 1);

	if (0) {
err:		WT_TRET(__wt_page_release(session, page));
	}
	__wt_scr_free(&key);

	return (ret);
}

/*
 * __slvg_row_merge_ovfl --
 *	Free file blocks referenced from keys discarded from merged pages.
 */
static int
__slvg_row_merge_ovfl(WT_SESSION_IMPL *session,
   WT_TRACK *trk, WT_PAGE *page, uint32_t start, uint32_t stop)
{
	WT_BM *bm;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_ROW *rip;

	bm = S2BT(session)->bm;
	unpack = &_unpack;

	for (rip = page->u.row.d + start; start < stop; ++start) {
		ikey = WT_ROW_KEY_COPY(rip);
		if (__wt_off_page(page, ikey))
			cell = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
		else
			cell = (WT_CELL *)ikey;
		__wt_cell_unpack(cell, unpack);
		if (unpack->type == WT_CELL_KEY_OVFL) {
			WT_VERBOSE_RET(session, salvage,
			    "%s merge discard freed overflow reference %s",
			    __wt_addr_string(session,
			    trk->ss->tmp1, trk->addr.addr, trk->addr.size),
			    __wt_addr_string(session,
			    trk->ss->tmp2, unpack->data, unpack->size));

			WT_RET(bm->free(
			    bm, session, unpack->data, unpack->size));
		}

		if ((cell = __wt_row_value(page, rip)) == NULL)
			continue;
		__wt_cell_unpack(cell, unpack);
		if (unpack->type == WT_CELL_VALUE_OVFL) {
			WT_VERBOSE_RET(session, salvage,
			    "%s merge discard freed overflow reference %s",
			    __wt_addr_string(session,
			    trk->ss->tmp1, trk->addr.addr, trk->addr.size),
			    __wt_addr_string(session,
			    trk->ss->tmp2, unpack->data, unpack->size));

			WT_RET(bm->free(
			    bm, session, unpack->data, unpack->size));
		}
	}
	return (0);
}

/*
 * __slvg_trk_compare_addr --
 *	Compare two WT_TRACK array entries by address cookie.
 */
static int
__slvg_trk_compare_addr(const void *a, const void *b)
{
	WT_DECL_RET;
	WT_TRACK *a_trk, *b_trk;
	uint32_t len;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	/*
	 * We don't care about the order because these are opaque cookies --
	 * we're just sorting them so we can binary search instead of linear
	 * search.
	 */
	len = WT_MIN(a_trk->addr.size, b_trk->addr.size);
	ret = memcmp(a_trk->addr.addr, b_trk->addr.addr, len);
	if (ret == 0)
		ret = a_trk->addr.size > b_trk->addr.size ? -1 : 1;
	return (ret);
}

/*
 * __slvg_ovfl_compare --
 *	Bsearch comparison routine for the overflow array.
 */
static int
__slvg_ovfl_compare(const void *a, const void *b)
{
	WT_ADDR *addr;
	WT_DECL_RET;
	WT_TRACK *trk;
	uint32_t len;

	addr = (WT_ADDR *)a;
	trk = *(WT_TRACK **)b;

	len = WT_MIN(trk->addr.size, addr->size);
	ret = memcmp(addr->addr, trk->addr.addr, len);
	if (ret == 0 && addr->size != trk->addr.size)
		ret = addr->size < trk->addr.size ? -1 : 1;
	return (ret);
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
	WT_ADDR *addr;
	WT_TRACK **searchp, *trk;
	uint32_t i, j;

	/*
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
	 * This requires sorting the page list by LSN, and the overflow array
	 * by address cookie.
	 */
	qsort(ss->pages,
	    (size_t)ss->pages_next, sizeof(WT_TRACK *), __slvg_trk_compare_gen);
	qsort(ss->ovfl,
	    (size_t)ss->ovfl_next, sizeof(WT_TRACK *), __slvg_trk_compare_addr);

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
			addr = &trk->ovfl[j];
			searchp = bsearch(addr, ss->ovfl, ss->ovfl_next,
			    sizeof(WT_TRACK *), __slvg_ovfl_compare);

			/*
			 * If the overflow page doesn't exist or its size does
			 * not match, or if another page has already claimed the
			 * overflow page, discard the leaf page.
			 */
			if (searchp != NULL &&
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
				addr = &trk->ovfl[--j];
				searchp =
				    bsearch(addr, ss->ovfl, ss->ovfl_next,
				    sizeof(WT_TRACK *), __slvg_ovfl_compare);
				F_CLR(*searchp, WT_TRACK_OVFL_REFD);
			}
			WT_VERBOSE_RET(session, salvage,
			    "%s references unavailable overflow page %s",
			    __wt_addr_string(session,
			    ss->tmp1, trk->addr.addr, trk->addr.size),
			    __wt_addr_string(session,
			    ss->tmp2, addr->addr, addr->size));
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
	uint64_t a_gen, a_recno, b_gen, b_recno;
	int cmp;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	if (a_trk == NULL)
		return (b_trk == NULL ? 0 : 1);
	if (b_trk == NULL)
		return (-1);

	switch (a_trk->ss->page_type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
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
		/*
		 * XXX
		 * WT_BTREE_CMP can potentially fail, and we're ignoring that
		 * error because this routine is called as an underlying qsort
		 * routine.
		 */
		(void)WT_BTREE_CMP(a_trk->ss->session, btree,
		    &a_trk->row_start, &b_trk->row_start, cmp);
		if (cmp != 0)
			return (cmp);
		break;
	}

	/*
	 * If the primary keys compare equally, differentiate based on LSN.
	 * Sort from highest LSN to lowest, that is, the earlier pages in
	 * the array are more desirable.
	 */
	a_gen = a_trk->gen;
	b_gen = b_trk->gen;
	return (a_gen > b_gen ? -1 : (a_gen < b_gen ? 1 : 0));
}

/*
 * __slvg_trk_compare_gen --
 *	Compare two WT_TRACK array entries by LSN.
 */
static int
__slvg_trk_compare_gen(const void *a, const void *b)
{
	WT_TRACK *a_trk, *b_trk;
	uint64_t a_gen, b_gen;

	a_trk = *(WT_TRACK **)a;
	b_trk = *(WT_TRACK **)b;

	/*
	 * Sort from highest LSN to lowest, that is, the earlier pages in the
	 * array are more desirable.
	 */
	a_gen = a_trk->gen;
	b_gen = b_trk->gen;
	return (a_gen > b_gen ? -1 : (a_gen < b_gen ? 1 : 0));
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
		WT_VERBOSE_RET(session, salvage,
		    "%s unused overflow page",
		    __wt_addr_string(session,
		    ss->tmp1, ss->ovfl[i]->addr.addr, ss->ovfl[i]->addr.size));
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
	WT_BM *bm;
	WT_ADDR *addr;
	WT_TRACK *trk;
	uint32_t i;

	bm = S2BT(session)->bm;
	trk = *trkp;
	*trkp = NULL;

	/*
	 * If freeing underlying file blocks or overflow pages, this is a page
	 * we were tracking but eventually decided not to use.  That merits a
	 * verbose description.
	 */
	if (LF_ISSET(WT_TRK_FREE_BLOCKS)) {
		WT_VERBOSE_RET(session, salvage,
		    "%s page discarded: discard freed file bytes %" PRIu32,
		    __wt_addr_string(
		    session, trk->ss->tmp1, trk->addr.addr, trk->addr.size),
		    trk->size);
		WT_RET(bm->free(bm, session, trk->addr.addr, trk->addr.size));
	}
	__wt_free(session, trk->addr.addr);

	for (i = 0; i < trk->ovfl_cnt; ++i) {
		addr = &trk->ovfl[i];
		if (LF_ISSET(WT_TRK_FREE_OVFL)) {
			WT_VERBOSE_RET(session, salvage,
			    "%s page discarded: discard freed overflow page %s",
			    __wt_addr_string(session,
			    trk->ss->tmp1, trk->addr.addr, trk->addr.size),
			    __wt_addr_string(session,
			    trk->ss->tmp2, addr->addr, addr->size));
			WT_RET(bm->free(bm, session, addr->addr, addr->size));
		}
		__wt_free(session, addr->addr);
	}
	__wt_free(session, trk->ovfl);

	if (trk->ss->page_type == WT_PAGE_ROW_LEAF) {
		__wt_buf_free(session, &trk->row_start);
		__wt_buf_free(session, &trk->row_stop);
	}
	__wt_free(session, trk);

	return (0);
}
