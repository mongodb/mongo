/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static int  __wt_rec_col_fix(SESSION *, WT_PAGE *);
static int  __wt_rec_col_int(SESSION *, WT_PAGE *);
static int  __wt_rec_col_merge(SESSION *,
		WT_PAGE *, uint64_t *, uint32_t *, uint8_t **, uint32_t *);
static int  __wt_rec_col_rle(SESSION *, WT_PAGE *);
static int  __wt_rec_col_split(SESSION *, WT_PAGE **, WT_PAGE *);
static int  __wt_rec_col_var(SESSION *, WT_PAGE *);
static void __wt_rec_parent_update_clean(WT_PAGE *);
static int  __wt_rec_parent_update_dirty(
		SESSION *, WT_PAGE *, WT_PAGE *, uint32_t, uint32_t, uint32_t);
static int  __wt_rec_row_leaf_insert(
		SESSION *, WT_INSERT *, uint32_t *, uint8_t **, uint32_t *);
static int  __wt_rec_row_int(SESSION *, WT_PAGE *);
static int  __wt_rec_row_leaf(SESSION *, WT_PAGE *, uint32_t);
static int  __wt_rec_row_merge(
		SESSION *, WT_PAGE *, uint32_t *, uint8_t **, uint32_t *);
static int  __wt_rec_row_split(SESSION *, WT_PAGE **, WT_PAGE *);
static int  __wt_rec_wrapup(SESSION *, WT_PAGE *, int *);

static int __wt_split(
	SESSION *, uint64_t *, uint32_t *, uint8_t **, uint32_t *, int);
static int __wt_split_fixup(
	SESSION *, uint64_t *, uint32_t *, uint8_t **, uint32_t *);
static int __wt_split_init(SESSION *,
	WT_PAGE *, uint32_t, uint32_t, uint64_t *, uint8_t **, uint32_t *);
static int __wt_split_write(SESSION *, int, WT_PAGE_DISK *, void *);

static inline uint32_t	__wt_allocation_size(SESSION *, void *, uint8_t *);
static inline int	__wt_block_free_ovfl(SESSION *, WT_OVFL *);

/*
 * __wt_allocation_size --
 *	Return the size to the minimum number of allocation units needed
 * (the page size can either grow or shrink), and zero out unused bytes.
 */
static inline uint32_t
__wt_allocation_size(SESSION *session, void *begin, uint8_t *end)
{
	BTREE *btree;
	uint32_t len, alloc_len;

	btree = session->btree;

	len = WT_PTRDIFF32(end, begin);
	alloc_len = WT_ALIGN(len, btree->allocsize);
	if (alloc_len > len)
		memset(end, 0, alloc_len - len);
	return (alloc_len);
}

/*
 * __wt_block_free_ovfl --
 *	Free an chunk of space, referenced by an overflow structure, to the
 *	underlying file.
 */
static inline int
__wt_block_free_ovfl(SESSION *session, WT_OVFL *ovfl)
{
	BTREE *btree;

	btree = session->btree;
	return (__wt_block_free(
	    session, ovfl->addr, WT_HDR_BYTES_TO_ALLOC(btree, ovfl->size)));
}

/*
 * __wt_page_reconcile --
 *	Format an in-memory page to its on-disk format, and write it.
 */
int
__wt_page_reconcile(
    SESSION *session, WT_PAGE *page, uint32_t slvg_skip, int discard)
{
	BTREE *btree;

	btree = session->btree;

	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "reconcile %s page addr %lu (type %s)",
	    WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean",
	    (u_long)page->addr, __wt_page_type_string(page->type)));

	/*
	 * Clean pages are simple: update the parent's state and discard the
	 * page.  (It makes on sense to do reconciliation on a clean page if
	 * you're not going to discard it.)
	 */
	if (!WT_PAGE_IS_MODIFIED(page)) {
		if (discard) {
			__wt_rec_parent_update_clean(page);
			__wt_page_discard(session, page);
		}
		return (0);
	}

	/*
	 * Update the disk generation before reading the page.  The workQ will
	 * update the write generation after it makes a change, and if we have
	 * different disk and write generation numbers, the page may be dirty.
	 * We technically require a flush (the eviction server might run on a
	 * different core before a flush naturally occurred).
	 */
	WT_PAGE_DISK_WRITE(page);
	WT_MEMORY_FLUSH;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_rec_col_fix(session, page));
		break;
	case WT_PAGE_COL_RLE:
		WT_RET(__wt_rec_col_rle(session, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_rec_col_var(session, page));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_rec_col_int(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_rec_row_int(session, page));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_rec_row_leaf(session, page, slvg_skip));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * Resolve the WT_REC_LIST information and update the parent -- note,
	 * the wrapup routine may clear the discard flag, for deleted pages
	 * that cannot be discarded.
	 */
	WT_RET(__wt_rec_wrapup(session, page, &discard));

	/*
	 * Free the original disk blocks: we've either written a new disk block
	 * referenced by the parent, or we're discarding the page entirely, and
	 * in either case, the page's address has changed.
	 */
	if (page->addr != WT_ADDR_INVALID) {
		WT_RET(__wt_block_free(session, page->addr, page->size));
		page->addr = WT_ADDR_INVALID;
		page->size = 0;
	}

	/* Optionally discard the in-memory page. */
	if (discard) {
		/*
		 * If we discard the in-memory root page, kill the reference
		 * to catch mistakes.
		 */
		if (WT_PAGE_IS_ROOT(page))
			btree->root_page.page = NULL;
		__wt_page_discard(session, page);
	}

	/*
	 * Newly created internal pages are normally merged into their parents
	 * when said parent is reconciled.  Newly split root pages can't be
	 * merged (as they have no parent), the new root page must be written.
	 *
	 * We detect root splits when the tree's root page is flagged to merge.
	 * We do the check here because I don't want the reconciliation code to
	 * handle two pages at once, and we've just finished with the original
	 * page.
	 *
	 * Reconcile the new root page explicitly rather than waiting for a
	 * natural reconcile, because root splits result from walking the tree
	 * during a sync or close call, and the new root page won't be visited
	 * as part of that walk.
	 */
	return (
	    FLD_ISSET(btree->root_page.state, WT_REF_MERGE) ?
	    __wt_page_reconcile(
	    session, btree->root_page.page, 0, discard) : 0);
}

/*
 * __wt_split_init --
 *	Initialization for the reconciliation split functions.
 */
static int
__wt_split_init(SESSION *session,
    WT_PAGE *page, uint32_t max, uint32_t min,
    uint64_t *recnop, uint8_t **first_freep, uint32_t *space_availp)
{
	BTREE *btree;
	WT_PAGE_DISK *dsk;
	WT_REC_LIST *r;
	uint32_t space_avail;

	btree = session->btree;

	r = &S2C(session)->cache->reclist;

	/* Allocate a scratch buffer to hold the new disk image. */
	WT_RET(__wt_scr_alloc(session, max, &r->dsk_tmp));

	/*
	 * Some fields of the disk image are fixed based on the original page,
	 * set them.
	 */
	dsk = r->dsk_tmp->mem;
	WT_CLEAR(*dsk);
	dsk->type = page->type;
	dsk->recno = *recnop;

	/*
	 * If we have to split, we want to choose a smaller page size for the
	 * split pages, because otherwise we could end up splitting one large
	 * packed page over and over.   We don't want to pick the minimum size
	 * either, because that penalizes an application that did a bulk load
	 * and subsequently inserted a few items into packed pages.  Currently,
	 * I'm using 75%, but I have no empirical evidence that's a good value.
	 * We should leave this as a tuning variable, but probably undocumented.
	 *
	 * The maximum page size may be a multiple of the split page size (for
	 * example, there's a maximum page size of 128KB, but because the table
	 * is active and we don't want to split a lot, the split size is 20KB).
	 * The maximum page size may NOT be an exact multiple of the split page
	 * size.
	 *
	 * The problem is we do lots of work to build these pages and don't want
	 * to start over when we reach the maximum page size (it's painful to
	 * start over after creating overflow items and compacted data).  So,
	 * the loop calls the helper function when it approaches a split-page
	 * boundary, and we save the information at that point.  That allows us
	 * to go back and split the page up when we eventually overflow the
	 * maximum page size.
	 */
	r->page_size = max;
	r->split_page_size = WT_ALIGN((max / 4) * 3, btree->allocsize);
#ifdef HAVE_DIAGNOSTIC
	/*
	 * This won't get tested enough if we don't force the code to create
	 * lots of splits.
	 */
	r->split_page_size = min;
#else
	WT_UNUSED(min);
#endif
	/*
	 * If the maximum page size is the same as the split page size, there
	 * is no need to maintain split boundaries within a larger page.
	 */
	if (max == r->split_page_size) {
		space_avail = max - WT_PAGE_DISK_SIZE;
		r->split_count = 0;
	} else {
		/*
		 * Pre-calculate the bytes available in a split-sized page and
		 * how many split pages there are in the full-sized page.
		 */
		space_avail =
		    r->split_avail = r->split_page_size - WT_PAGE_DISK_SIZE;
		r->split_count = (max / r->split_page_size) - 1;

		/*
		 * We know the maximum number of items we'll have to save, make
		 * sure there's enough room.
		 *
		 * The calculation is actually +1, because we save the start
		 * point one past the current entry -- make it +10 so we don't
		 * grow slot-by-slot.
		 */
		if (r->s_entries < r->split_count + 10) {
			if (r->save != NULL)
				__wt_free(session, r->save);
			r->s_entries = 0;
			WT_RET(__wt_calloc_def(session,
			    r->split_count + 10, &r->save));
			r->s_entries = r->split_count + 10;
		}
	}

	/* Initialize the total entries in split chunks. */
	r->total_split_entries = 0;

	/*
	 * Initialize the arrays of saved and written page entries to reference
	 * the next slot.
	 */
	r->l_next = r->s_next = 0;

	/*
	 * Set the caller's information and configure so the loop calls us
	 * when approaching the split boundary.
	 */
	*first_freep = WT_PAGE_DISK_BYTE(dsk);
	*space_availp = space_avail;
	return (0);
}

/*
 * __wt_split --
 *	Handle the page reconciliation bookkeeping.  (Did you know "bookkeeper"
 * has 3 doubled letters in a row?  Sweet-tooth does, too.)
 */
static int
__wt_split(SESSION *session, uint64_t *recnop,
    uint32_t *entriesp, uint8_t **first_freep, uint32_t *space_availp, int done)
{
	WT_PAGE_DISK *dsk;
	WT_REC_LIST *r;
	struct rec_save *r_save;
	int ret, which;

	ret = 0;

	/*
	 * Handle page-buffer size tracking; we have to do this work in every
	 * reconciliation loop, and I don't want to repeat the code that many
	 * times.
	 */
	r = &S2C(session)->cache->reclist;
	dsk = r->dsk_tmp->mem;

	/*
	 * There are 4 cases we have to handle.
	 *
	 * #1
	 * We're done reconciling a page, in which case we can ignore any split
	 * information we've accumulated to that point and write whatever we
	 * have in the current buffer.
	 *
	 * #2
	 * Not done, and about to cross a split boundary, in which case we save
	 * away the current boundary information and return.
	 *
	 * #3
	 * Not done, and about to cross the max boundary, in which case we have
	 * to physically split the page -- use the saved split information to
	 * write all the split pages.
	 *
	 * #4
	 * Not done, and about to cross the split boundary, but we've already
	 * done the split thing when we approached the max boundary, in which
	 * case we write the page and keep going.
	 *
	 * Cases #2 and #3 are the hard ones: we're called when we're about to
	 * cross each split boundary, and we save information away so we can
	 * split if we have to.  We're also called when we're about to cross
	 * the maximum page boundary: in that case, we do the actual split,
	 * clean things up, then keep going.
	 */
	if (done)					/* Case #1 */
		which = 1;
	else if (r->split_count > 0)			/* Case #2 */
		which = 2;
	else if (r->s_next != 0)			/* Case #3 */
		which = 3;
	else						/* Case #4 */
		which = 4;

	switch (which) {
	case 1:
		/* We're done, write the remaining information. */
		dsk->u.entries = *entriesp;
		ret = __wt_split_write(
		    session, *entriesp == 0 ? 1 : 0, dsk, *first_freep);
		break;
	case 2:
		/*
		 * Save the information about where we are when the split would
		 * have happened.
		 */
		r_save = &r->save[r->s_next];
		if (r->s_next == 0) {
			r_save->recno = dsk->recno;
			r_save->start = WT_PAGE_DISK_BYTE(dsk);
		}
		r_save->entries = *entriesp - r->total_split_entries;
		r->total_split_entries = *entriesp;
		++r_save;
		r_save->recno = *recnop;
		r_save->start = *first_freep;

		++r->s_next;

		/*
		 * Give our caller more space, hopefully it will all fit!
		 *
		 * Notice we're not increasing the space our caller has, instead
		 * setting it to a maximum, that's because we want our caller to
		 * call again when it approaches another split-size boundary.
		 *
		 * However, the maximum page size may not be a multiple of the
		 * split page size -- on the last part of the page, whatever
		 * space left is what we have.
		 */
		if (--r->split_count == 0)
			*space_availp =
			    r->page_size - WT_PTRDIFF32(*first_freep, dsk);
		else
			*space_availp = r->split_avail;
		break;
	case 3:
		/*
		 * It didn't all fit, but we just noticed that.
		 *
		 * Cycle through the saved split-point information, writing any
		 * split page pieces we have tracked, and reset our caller's
		 * information with any remnant we don't write.
		 */
		ret = __wt_split_fixup(
		    session, recnop, entriesp, first_freep, space_availp);

		/* Set the starting record number for the next set of items. */
		dsk->recno = *recnop;

		/* We're done saving split-points. */
		r->s_next = 0;
		break;
	case 4:
		/*
		 * It didn't all fit, but either we've already noticed it and
		 * are now processing the rest of the page at the split-size
		 * boundaries, or, the split size was the same as the page size,
		 * so we never bothered with saving split-point information.
		 *
		 * Write the current disk image.
		 */
		dsk->u.entries = *entriesp;
		WT_RET(__wt_split_write(session, 0, dsk, *first_freep));

		/*
		 * Set the starting record number for the next set of items,
		 * reset the caller's information -- we only get here if we
		 * had to split, so we're using split-size chunks.
		 */
		dsk->recno = *recnop;
		*entriesp = 0;
		*first_freep = WT_PAGE_DISK_BYTE(dsk);
		*space_availp = r->split_avail;
		break;
	}

	return (ret);
}

/*
 * __wt_split_fixup --
 *	Physically split the already-written page data.
 */
static int
__wt_split_fixup(SESSION *session, uint64_t *recnop,
    uint32_t *entriesp, uint8_t **first_freep, uint32_t *space_availp)
{
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	WT_REC_LIST *r;
	struct rec_save *r_save;
	uint32_t i, len;
	uint8_t *dsk_start;
	int ret;

	ret = 0;

	/*
	 * When we overflow physical limits of the page, we walk the list of
	 * split chunks we've created and write those pages out, then update
	 * the caller's information.
	 */
	r = &S2C(session)->cache->reclist;

	/*
	 * The data isn't laid out on page-boundaries, so we have to copy it
	 * into another buffer before writing it.
	 *
	 * Allocate a scratch buffer to hold the new disk image.  Copy the
	 * WT_PAGE_DISK header onto the scratch buffer, most of the header
	 * information remains unchanged between the pages.
	 */
	WT_RET(__wt_scr_alloc(session, r->split_page_size, &tmp));
	dsk = tmp->mem;
	memcpy(dsk, r->dsk_tmp->mem, WT_PAGE_DISK_SIZE);

	/*
	 * For each split chunk we've created, update the disk image and copy
	 * it into place.
	 */
	dsk_start = WT_PAGE_DISK_BYTE(dsk);
	for (i = 0, r_save = r->save; i < r->s_next; ++i, ++r_save) {
		/* Copy out the starting record number. */
		dsk->recno = r_save->recno;

		/*
		 * Copy out the number of entries, and deduct that from the
		 * main loops count of entries.
		 */
		*entriesp -= dsk->u.entries = r_save->entries;

		/* Copy out the page contents, and write it. */
		len = WT_PTRDIFF32((r_save + 1)->start, r_save->start);
		memcpy(dsk_start, r_save->start, len);
		WT_ERR(__wt_split_write(session, 0, dsk, dsk_start + len));
	}

	/*
	 * There is probably a remnant that didn't get written, copy it down to
	 * the beginning of the working buffer.
	 */
	dsk_start = WT_PAGE_DISK_BYTE(r->dsk_tmp->mem);
	len = WT_PTRDIFF32(*first_freep, r_save->start);
	memcpy(dsk_start, r_save->start, len);

	/*
	 * Fix up our caller's information -- we corrected the entry count as
	 * part of looping through the split page chunks.   Set the starting
	 * record number, we have that saved.
	 */
	*recnop = r_save->recno;
	*first_freep = dsk_start + len;
	*space_availp = r->split_avail - len;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_split_write --
 *	Write a disk block out for the helper functions.
 */
static int
__wt_split_write(SESSION *session, int deleted, WT_PAGE_DISK *dsk, void *end)
{
	WT_REC_LIST *r;
	struct rec_list *r_list;
	uint32_t addr, size;

	r = &S2C(session)->cache->reclist;

	if (deleted) {
		size = 0;
		addr = WT_ADDR_INVALID;
	} else {
		/*
		 * Set the disk block size and clear trailing bytes.
		 * Allocate file space.
		 * Write the disk block.
		 */
		size = __wt_allocation_size(session, dsk, end);
		WT_RET(__wt_block_alloc(session, &addr, size));
		WT_RET(__wt_disk_write(session, dsk, addr, size));
	}

	/* Save the key and addr/size pairs to update the parent. */
	if (r->l_next == r->l_entries) {
		WT_RET(__wt_realloc(session, &r->l_allocated,
		    (r->l_entries + 20) * sizeof(*r->list), &r->list));
		r->l_entries += 20;
	}
	r_list = &r->list[r->l_next++];
	r_list->off.addr = addr;
	r_list->off.size = size;
	r_list->deleted = 0;

	/* Deletes are easy -- just flag the fact and we're done. */
	if (deleted) {
		r_list->deleted = 1;
		return (0);
	}

	/*
	 * For a column-store, the key is the recno, for a row-store, it's the
	 * first key on the page, a variable-length byte string.
	 */
	switch (dsk->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_cell_process(
		    session, WT_PAGE_DISK_BYTE(dsk), &r_list->key));
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		WT_RECNO(&r_list->off) = dsk->recno;
		break;
	}

	return (0);
}

/*
 * __wt_rec_col_int --
 *	Reconcile a column-store internal page.
 */
static int
__wt_rec_col_int(SESSION *session, WT_PAGE *page)
{
	uint64_t recno;
	uint32_t entries, space_avail;
	uint8_t *first_free;

	recno = page->u.col_int.recno;
	entries = 0;
	WT_RET(__wt_split_init(session, page, session->btree->intlmax,
	    session->btree->intlmin, &recno, &first_free, &space_avail));

	/*
	 * Walking the row-store internal pages is complicated by the fact that
	 * we're taking keys from the underlying disk image for the top-level
	 * page and we're taking keys from in-memory structures for merge pages.
	 * Column-store is simpler because the only information we copy is the
	 * WT_OFF_RECORD structure, and it comes from in-memory structures in
	 * both the top-level and merge cases.  In short, both the top-level
	 * and merge page walks look the same, and we just call the merge page
	 * function on the top-level page.
	 */
	WT_RET(__wt_rec_col_merge(
	    session, page, &recno, &entries, &first_free, &space_avail));

	/* Write the remnant page. */
	return (__wt_split(
	    session, &recno, &entries, &first_free, &space_avail, 1));
}

/*
 * __wt_rec_col_merge --
 *	Recursively walk a column-store internal tree of merge pages.
 */
static int
__wt_rec_col_merge(SESSION *session, WT_PAGE *page, uint64_t *recnop,
    uint32_t *entriesp, uint8_t **first_freep, uint32_t *space_availp)
{
	WT_COL_REF *cref;
	WT_OFF_RECORD off;
	WT_PAGE *ref_page;
	uint64_t recno;
	uint32_t entries, i, space_avail;
	uint8_t *first_free;

	recno = *recnop;
	entries = *entriesp;
	first_free = *first_freep;
	space_avail = *space_availp;

	/* For each entry in the page... */
	WT_COL_REF_FOREACH(page, cref, i) {
		/* Update the starting record number in case we split. */
		recno = cref->recno;

		/*
		 * If this is a reference to a merge page, check the page type.
		 * A leaf page must be an empty page, we're done.  An internal
		 * page must have resulted from a page split, recursively call
		 * ourselves and walk that page.
		 */
		if (FLD_ISSET(WT_COL_REF_STATE(cref), WT_REF_MERGE)) {
			ref_page = WT_COL_REF_PAGE(cref);
			if (ref_page->type == WT_PAGE_COL_INT)
				WT_RET(__wt_rec_col_merge(
				    session, ref_page, &recno,
				    &entries, &first_free, &space_avail));
			__wt_page_discard(session, ref_page);
			continue;
		}

		/* Boundary: allocate, split or write the page. */
		if (sizeof(WT_OFF_RECORD) > space_avail)
			WT_RET(__wt_split(session,
			    &recno, &entries, &first_free, &space_avail, 0));

		/* Copy a new WT_OFF_RECORD structure into place. */
		off.addr = WT_COL_REF_ADDR(cref);
		off.size = WT_COL_REF_SIZE(cref);
		WT_RECNO(&off) = cref->recno;
		memcpy(first_free, &off, sizeof(WT_OFF_RECORD));
		first_free += sizeof(WT_OFF_RECORD);
		space_avail -= WT_SIZEOF32(WT_OFF_RECORD);
		++entries;
	}

	*recnop = recno;
	*entriesp = entries;
	*first_freep = first_free;
	*space_availp = space_avail;
	return (0);
}

/*
 * __wt_rec_col_fix --
 *	Reconcile a fixed-width, column-store leaf page (does not handle
 *	run-length encoding).
 */
static int
__wt_rec_col_fix(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_UPDATE *upd;
	uint64_t unused;
	uint32_t entries, i, len, space_avail;
	uint8_t *data, *first_free;
	void *cipdata;
	int ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page; get a scratch
	 * buffer, clear the contents and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	/*
	 * Fixed-size pages can't split, but we use the underlying helper
	 * functions because they don't add much overhead, and it's better
	 * if all the reconciliation functions look the same.
	 */
	unused = page->u.col_leaf.recno;
	entries = 0;
	WT_ERR(__wt_split_init(session, page, session->btree->leafmax,
	    session->btree->leafmin, &unused, &first_free, &space_avail));

	/* For each entry in the in-memory page... */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);

		/*
		 * Get a reference to the data, on- or off- page, and see if
		 * it's been deleted.
		 */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				data = tmp->mem;	/* Deleted */
			else				/* Updated */
				data = WT_UPDATE_DATA(upd);
		} else if (WT_FIX_DELETE_ISSET(cipdata))
			data = tmp->mem;		/* On-disk deleted */
		else					/* On-disk data */
			data = WT_COL_PTR(page, cip);

		/*
		 * When reconciling a fixed-width page that doesn't support
		 * run-length encoding, the on-page information can't change
		 * size -- there's no reason to ever split such a page.
		 */
		memcpy(first_free, data, len);
		first_free += len;
		space_avail -= len;
		++entries;
	}

	/* Write the remnant page. */
	ret = __wt_split(
	    session, &unused, &entries, &first_free, &space_avail, 1);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_rec_col_rle --
 *	Reconcile a fixed-width, run-length encoded, column-store leaf page.
 */
static int
__wt_rec_col_rle(SESSION *session, WT_PAGE *page)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_INSERT *ins;
	uint64_t recno;
	uint32_t entries, i, len, space_avail;
	uint16_t n, nrepeat, repeat_count;
	uint8_t *data, *first_free, *last_data;
	int ret;
	void *cipdata;

	btree = session->btree;
	tmp = NULL;
	last_data = NULL;
	ret = 0;

	/*
	 * We need a "deleted" data item to store on the page.  Make sure the
	 * SESSION's scratch buffer is big enough.  Clear the buffer's contents
	 * and set the delete flag.
	 */
	len = btree->fixed_len;
	WT_ERR(__wt_scr_alloc(session, len, &tmp));
	memset(tmp->mem, 0, len);
	WT_FIX_DELETE_SET(tmp->mem);

	recno = page->u.col_leaf.recno;
	entries = 0;
	WT_RET(__wt_split_init(session, page, session->btree->leafmax,
	    session->btree->leafmin, &recno, &first_free, &space_avail));

	/* For each entry in the in-memory page... */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		/*
		 * Generate entries for the new page: loop through the repeat
		 * records, checking for WT_INSERT entries matching the record
		 * number.
		 *
		 * Note the increment of recno in the for loop to update the
		 * starting record number in case we split.
		 */
		ins = WT_COL_INSERT(page, cip),
		nrepeat = WT_RLE_REPEAT_COUNT(cipdata);
		for (n = 0;
		    n < nrepeat; n += repeat_count, recno += repeat_count) {
			if (ins != NULL && WT_INSERT_RECNO(ins) == recno) {
				/* Use the WT_INSERT's WT_UPDATE field. */
				if (WT_UPDATE_DELETED_ISSET(ins->upd))
					data = tmp->mem;
				else
					data = WT_UPDATE_DATA(ins->upd);
				repeat_count = 1;

				ins = ins->next;
			} else {
				if (WT_FIX_DELETE_ISSET(cipdata))
					data = tmp->mem;
				else
					data = WT_RLE_REPEAT_DATA(cipdata);
				/*
				 * The repeat count is the number of records
				 * up to the next WT_INSERT record, or up to
				 * the end of this entry if we have no more
				 * WT_INSERT records.
				 */
				if (ins == NULL)
					repeat_count = nrepeat - n;
				else
					repeat_count = (uint16_t)
					    (WT_INSERT_RECNO(ins) - recno);
			}

			/*
			 * In all cases, check the last entry written on the
			 * page to see if it's identical, and increment its
			 * repeat count where possible.
			 */
			if (last_data != NULL && memcmp(
			    WT_RLE_REPEAT_DATA(last_data), data, len) == 0 &&
			    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
				WT_RLE_REPEAT_COUNT(last_data) += repeat_count;
				continue;
			}

			/* Boundary: allocate, split or write the page. */
			if (len > space_avail)
				WT_ERR(__wt_split(session, &recno,
				    &entries, &first_free, &space_avail, 0));

			last_data = first_free;
			WT_RLE_REPEAT_COUNT(last_data) = repeat_count;
			memcpy(WT_RLE_REPEAT_DATA(last_data), data, len);

			first_free += len + sizeof(uint16_t);
			space_avail -= len + WT_SIZEOF32(uint16_t);
			++entries;
		}
	}

	/* Write the remnant page. */
	ret = __wt_split(
	    session, &recno, &entries, &first_free, &space_avail, 1);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_rec_col_var --
 *	Reconcile a variable-width column-store leaf page.
 */
static int
__wt_rec_col_var(SESSION *session, WT_PAGE *page)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE } data_loc;
	WT_COL *cip;
	WT_BUF *value, value_buf;
	WT_CELL value_cell, *cell;
	WT_OVFL value_ovfl;
	WT_UPDATE *upd;
	uint32_t entries, i, len, space_avail;
	uint64_t recno;
	uint8_t *first_free;

	WT_CLEAR(value_cell);
	WT_CLEAR(value_buf);
	value = &value_buf;

	recno = page->u.col_leaf.recno;
	entries = 0;
	WT_RET(__wt_split_init(session, page, session->btree->leafmax,
	    session->btree->leafmin, &recno, &first_free, &space_avail));

	/* For each entry in the in-memory page... */
	WT_COL_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a reference to the value: it's either an update or the
		 * original on-page item.
		 */
		cell = WT_COL_PTR(page, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			/*
			 * If we update an overflow value, free the underlying
			 * file space.
			 */
			if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL)
				WT_RET(__wt_block_free_ovfl(
				    session, WT_CELL_BYTE_OVFL(cell)));

			/*
			 * Check for deletion, else build the value's WT_CELL
			 * chunk from the most recent update value.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				WT_CLEAR(value_cell);
				WT_CELL_SET(&value_cell, WT_CELL_DEL, 0);
				len = WT_CELL_SPACE_REQ(0);
			} else {
				value->data = WT_UPDATE_DATA(upd);
				value->size = upd->size;
				WT_RET(__wt_item_build_value(
				    session, value, &value_cell, &value_ovfl));
				len = WT_CELL_SPACE_REQ(value->size);
			}
			data_loc = DATA_OFF_PAGE;
		} else {
			value->data = cell;
			value->size = WT_CELL_LEN(cell);
			len = WT_CELL_SPACE_REQ(value->size);
			data_loc = DATA_ON_PAGE;
		}

		/* Boundary: allocate, split or write the page. */
		if (len > space_avail)
			WT_RET(__wt_split(session,
			    &recno, &entries, &first_free, &space_avail, 0));

		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(first_free, value->data, len);
			break;
		case DATA_OFF_PAGE:
			memcpy(first_free, &value_cell, sizeof(value_cell));
			memcpy(first_free +
			    sizeof(value_cell), value->data, value->size);
			break;
		}
		first_free += len;
		space_avail -= len;
		++entries;

		/* Update the starting record number in case we split. */
		++recno;
	}

	/* Write the remnant page. */
	return (__wt_split(
	    session, &recno, &entries, &first_free, &space_avail, 1));
}

/*
 * __wt_rec_row_int --
 *	Reconcile a row-store internal page.
 */
static int
__wt_rec_row_int(SESSION *session, WT_PAGE *page)
{
	WT_CELL *key_cell, *value_cell;
	WT_OFF *from;
	WT_PAGE *ref_page;
	WT_ROW_REF *rref;
	uint64_t unused;
	uint32_t entries, i, len, space_avail;
	uint8_t *first_free;

	unused = 0;
	entries = 0;
	WT_RET(__wt_split_init(session, page, session->btree->intlmax,
	    session->btree->intlmin, &unused, &first_free, &space_avail));

	/*
	 * There are two kinds of row-store internal pages we walk: the first
	 * is a page created in-memory, in which case there's no underlying
	 * disk image.  The second is a page read from disk, in which case we
	 * take the keys from the underlying disk image.
	 */
	if (page->XXdsk == NULL) {
		WT_RET(__wt_rec_row_merge(
		    session, page, &entries, &first_free, &space_avail));
		return (__wt_split(
		    session, &unused, &entries, &first_free, &space_avail, 1));
	}

	/*
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_INDX_AND_KEY_FOREACH for details.
	 *
	 * For each entry in the in-memory page...
	 */
	WT_ROW_REF_AND_KEY_FOREACH(page, rref, key_cell, i) {
		/*
		 * If this is a reference to a merge page, check the page type.
		 * A leaf page must be an empty page, we're done.  An internal
		 * page must have resulted from a page split: we may be merging
		 * subtrees into this page, and they may be multiple levels deep
		 * -- it's not likely, but it's possible.  Call a routine for
		 * each such subtree.
		 */
		if (FLD_ISSET(WT_ROW_REF_STATE(rref), WT_REF_MERGE)) {
			ref_page = WT_ROW_REF_PAGE(rref);
			if (ref_page->type == WT_PAGE_ROW_INT)
				WT_RET(__wt_rec_row_merge(session, ref_page,
				    &entries, &first_free, &space_avail));
			__wt_page_discard(session, ref_page);

			/* Delete any underlying overflow key. */
			if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
				WT_RET(__wt_block_free_ovfl(
				    session, WT_CELL_BYTE_OVFL(key_cell)));
			continue;
		}

		value_cell = WT_CELL_NEXT(key_cell);
		len = WT_PTRDIFF32(WT_CELL_NEXT(value_cell), key_cell);

		/* Boundary: allocate, split or write the page. */
		if (len + sizeof(WT_OFF) > space_avail)
			WT_RET(__wt_split(session,
			    &unused, &entries, &first_free, &space_avail, 0));

		/*
		 * XXX
		 * For now, we just punch the new page locations into the old
		 * on-page information, that will eventually change.
		 */
		from = WT_CELL_BYTE_OFF(value_cell);
		from->addr = WT_ROW_REF_ADDR(rref);
		from->size = WT_ROW_REF_SIZE(rref);

		/* Copy the key and re-written WT_OFF structure into place. */
		memcpy(first_free, key_cell, len);
		first_free += len;
		space_avail -= len;
		entries += 2;
	}

	/* Write the remnant page. */
	return (__wt_split(
	    session, &unused, &entries, &first_free, &space_avail, 1));
}

/*
 * __wt_rec_row_merge --
 *	Recursively walk a row-store internal tree of merge pages.
 */
static int
__wt_rec_row_merge(SESSION *session, WT_PAGE *page,
    uint32_t *entriesp, uint8_t **first_freep, uint32_t *space_availp)
{
	WT_CELL cell;
	WT_OFF off;
	WT_BUF key;
	WT_OVFL key_ovfl;
	WT_PAGE *ref_page;
	WT_ROW_REF *rref;
	uint64_t unused;
	uint32_t entries, i, space_avail;
	uint8_t *first_free;

	WT_CLEAR(key);
	unused = 0;

	entries = *entriesp;
	first_free = *first_freep;
	space_avail = *space_availp;

	/*
	 * For each entry in the in-memory page...
	 */
	WT_ROW_REF_FOREACH(page, rref, i) {
		/*
		 * If this is a reference to a merge page, check the page type.
		 * A leaf page must be an empty page, we're done.  An internal
		 * page must have resulted from a page split, recursively call
		 * ourselves and walk that page.
		 */
		if (FLD_ISSET(WT_ROW_REF_STATE(rref), WT_REF_MERGE)) {
			ref_page = WT_ROW_REF_PAGE(rref);
			if (ref_page->type == WT_PAGE_ROW_INT)
				WT_RET(__wt_rec_row_merge(session, ref_page,
				    &entries, &first_free, &space_avail));
			__wt_page_discard(session, ref_page);
			continue;
		}

		/* Build a key to store on the page. */
		key.data = rref->key;
		key.size = rref->size;
		WT_RET(__wt_item_build_key(session, &key, &cell, &key_ovfl));

		/* Boundary: allocate, split or write the page. */
		if (WT_CELL_SPACE_REQ(key.size) +
		    WT_CELL_SPACE_REQ(sizeof(WT_OFF)) > space_avail)
			WT_RET(__wt_split(session,
			    &unused, &entries, &first_free, &space_avail, 0));

		/* Copy the key into place. */
		memcpy(first_free, &cell, sizeof(WT_CELL));
		memcpy(first_free + sizeof(WT_CELL), key.data, key.size);
		first_free += WT_CELL_SPACE_REQ(key.size);
		space_avail -= WT_CELL_SPACE_REQ(key.size);

		/* Copy the off-page reference into place. */
		off.addr = WT_ROW_REF_ADDR(rref);
		off.size = WT_ROW_REF_SIZE(rref);
		WT_CELL_SET(&cell, WT_CELL_OFF, sizeof(WT_OFF));
		memcpy(first_free, &cell, sizeof(WT_CELL));
		memcpy(first_free + sizeof(WT_CELL), &off, sizeof(WT_OFF));
		first_free += WT_CELL_SPACE_REQ(sizeof(WT_OFF));
		space_avail -= WT_CELL_SPACE_REQ(sizeof(WT_OFF));

		entries += 2;
	}

	*entriesp = entries;
	*first_freep = first_free;
	*space_availp = space_avail;

	if (key.mem != NULL)
		__wt_buf_free(session, &key);

	return (0);
}

/*
 * __wt_rec_row_leaf --
 *	Reconcile a row-store leaf page.
 */
static int
__wt_rec_row_leaf(SESSION *session, WT_PAGE *page, uint32_t slvg_skip)
{
	enum { DATA_ON_PAGE, DATA_OFF_PAGE, EMPTY_DATA } data_loc;
	WT_INSERT *ins;
	WT_BUF value_buf;
	WT_CELL value_cell, *key_cell;
	WT_OVFL value_ovfl;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint64_t unused;
	uint32_t entries, i, key_len, space_avail, value_len;
	uint8_t *first_free;
	void *ripvalue;

	WT_CLEAR(value_buf);
	unused = 0;
	entries = 0;

	WT_RET(__wt_split_init(session, page, session->btree->leafmax,
	    session->btree->leafmin, &unused, &first_free, &space_avail));

	/*
	 * Write any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((ins = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		WT_RET(__wt_rec_row_leaf_insert(
		    session, ins, &entries, &first_free, &space_avail));

	/*
	 * Walk the page, writing key/value pairs.
	 *
	 * We have to walk both the WT_ROW structures and the original page --
	 * see the comment at WT_INDX_AND_KEY_FOREACH for details.
	 */
	WT_ROW_INDX_AND_KEY_FOREACH(page, rip, key_cell, i) {
		/*
		 * The salvage code, on some rare occasions, wants to reconcile
		 * a page but skip some leading records on the page.  Because
		 * the row-store leaf reconciliation function copies keys from
		 * the original disk page, this is non-trivial -- just changing
		 * the in-memory pointers isn't sufficient, we have to change
		 * the WT_CELL structures on the disk page, too.  It's ugly, but
		 * we pass in a value that tells us how many records to skip in
		 * this case.
		 */
		if (slvg_skip != 0) {
			--slvg_skip;
			continue;
		}
		/*
		 * Get a reference to the value.  We get the value first because
		 * it may have been deleted, in which case we ignore the pair.
		 */
		value_len = 0;
		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
			/*
			 * If we update an overflow value, free the underlying
			 * file space.
			 */
			if (!WT_ROW_EMPTY_ISSET(rip)) {
				ripvalue = WT_ROW_PTR(page, rip);
				if (WT_CELL_TYPE(ripvalue) == WT_CELL_DATA_OVFL)
					WT_RET(__wt_block_free_ovfl(
					    session,
					    WT_CELL_BYTE_OVFL(ripvalue)));
			}

			/*
			 * If this key/value pair was deleted, we're done.  If
			 * the key was an overflow item, free the underlying
			 * file space.
			 */
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (WT_CELL_TYPE(key_cell) == WT_CELL_KEY_OVFL)
					WT_RET(__wt_block_free_ovfl(session,
					    WT_CELL_BYTE_OVFL(key_cell)));
				goto leaf_insert;
			}

			/*
			 * If no value, nothing needs to be copied.  Otherwise,
			 * build the value's WT_CELL chunk from the most recent
			 * update value.
			 */
			if (upd->size == 0)
				data_loc = EMPTY_DATA;
			else {
				value_buf.data = WT_UPDATE_DATA(upd);
				value_buf.size = upd->size;
				WT_RET(__wt_item_build_value(session,
				    &value_buf, &value_cell, &value_ovfl));
				value_len = WT_CELL_SPACE_REQ(value_buf.size);
				data_loc = DATA_OFF_PAGE;
			}
		} else {
			/*
			 * Copy the item off the page -- however, when the page
			 * was read into memory, there may not have been a value
			 * item, that is, it may have been zero length.  Catch
			 * that case.
			 */
			if (WT_ROW_EMPTY_ISSET(rip))
				data_loc = EMPTY_DATA;
			else {
				ripvalue = WT_ROW_PTR(page, rip);
				value_buf.data = ripvalue;
				value_buf.size =
				    WT_CELL_SPACE_REQ(WT_CELL_LEN(ripvalue));
				value_len = value_buf.size;
				data_loc = DATA_ON_PAGE;
			}
		}

		/* Take the key's WT_CELL from the original page. */
		key_len = WT_CELL_SPACE_REQ(WT_CELL_LEN(key_cell));

		/* Boundary: allocate, split or write the page. */
		if (key_len + value_len > space_avail)
			WT_RET(__wt_split(session,
			    &unused, &entries, &first_free, &space_avail, 0));

		/* Copy the key onto the page. */
		memcpy(first_free, key_cell, key_len);
		first_free += key_len;
		space_avail -= key_len;
		++entries;

		/* Copy the value onto the page. */
		switch (data_loc) {
		case DATA_ON_PAGE:
			memcpy(first_free, value_buf.data, value_buf.size);
			first_free += value_buf.size;
			space_avail -= value_buf.size;
			++entries;
			break;
		case DATA_OFF_PAGE:
			memcpy(first_free, &value_cell, sizeof(value_cell));
			memcpy(first_free +
			    sizeof(WT_CELL), value_buf.data, value_buf.size);
			first_free += value_len;
			space_avail -= value_len;
			++entries;
			break;
		case EMPTY_DATA:
			break;
		}

leaf_insert:	/* Write any K/V pairs inserted into the page after this key. */
		if ((ins = WT_ROW_INSERT(page, rip)) != NULL)
			WT_RET(__wt_rec_row_leaf_insert(
			    session, ins, &entries, &first_free, &space_avail));
	}

	if (value_buf.mem != NULL)
		__wt_buf_free(session, &value_buf);

	/* Write the remnant page. */
	return (__wt_split(
	    session, &unused, &entries, &first_free, &space_avail, 1));
}

/*
 * __wt_rec_row_leaf_insert --
 *	Walk an insert chain, writing K/V pairs.
 */
static int
__wt_rec_row_leaf_insert(SESSION *session, WT_INSERT *ins,
    uint32_t *entriesp, uint8_t **first_freep, uint32_t *space_availp)
{
	WT_CELL key_cell, value_cell;
	WT_BUF key_buf, value_buf;
	WT_OVFL key_ovfl, value_ovfl;
	WT_UPDATE *upd;
	uint64_t unused;
	uint32_t entries, key_len, space_avail, value_len;
	uint8_t *first_free;

	WT_CLEAR(key_buf);
	WT_CLEAR(value_buf);
	unused = 0;

	entries = *entriesp;
	first_free = *first_freep;
	space_avail = *space_availp;

	for (; ins != NULL; ins = ins->next) {
		/* Build a value to store on the page. */
		upd = ins->upd;
		if (WT_UPDATE_DELETED_ISSET(upd))
			continue;
		if (upd->size == 0)
			value_len = 0;
		else {
			value_buf.data = WT_UPDATE_DATA(upd);
			value_buf.size = upd->size;
			WT_RET(__wt_item_build_value(
			    session, &value_buf, &value_cell, &value_ovfl));
			value_len = WT_CELL_SPACE_REQ(value_buf.size);
		}

		/* Build a key to store on the page. */
		key_buf.data = WT_INSERT_KEY(ins);
		key_buf.size = WT_INSERT_KEY_SIZE(ins);
		WT_RET(__wt_item_build_key(
		    session, &key_buf, &key_cell, &key_ovfl));
		key_len = WT_CELL_SPACE_REQ(key_buf.size);

		/* Boundary: allocate, split or write the page. */
		if (key_len + value_len > space_avail)
			WT_RET(__wt_split(session,
			    &unused, &entries, &first_free, &space_avail, 0));

		/* Copy the key cell into place. */
		memcpy(first_free, &key_cell, sizeof(WT_CELL));
		memcpy(
		    first_free + sizeof(WT_CELL), key_buf.data, key_buf.size);
		first_free += key_len;
		space_avail -= key_len;
		++entries;

		/* Copy the value cell into place. */
		if (value_len == 0)
			continue;
		memcpy(first_free, &value_cell, sizeof(WT_CELL));
		memcpy(first_free + sizeof(WT_CELL),
		    value_buf.data, value_buf.size);
		first_free += value_len;
		space_avail -= value_len;
		++entries;
	}

	if (key_buf.mem != NULL)
		__wt_buf_free(session, &key_buf);
	if (value_buf.mem != NULL)
		__wt_buf_free(session, &value_buf);

	*entriesp = entries;
	*first_freep = first_free;
	*space_availp = space_avail;
	return (0);
}

/*
 * __wt_rec_finish  --
 *	Resolve the WT_REC_LIST information.
 */
static int
__wt_rec_wrapup(SESSION *session, WT_PAGE *page, int *discardp)
{
	BTREE *btree;
	WT_PAGE *new;
	WT_REC_LIST *r;
	int discard;

	btree = session->btree;
	new = NULL;
	r = &S2C(session)->cache->reclist;
	discard = *discardp;

	/*
	 * If all entries on the page were deleted, mark the page to be merged
	 * into its parent when the parent is evicted.
	 */
	if (r->list[0].deleted) {
		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "reconcile: delete page %lu (%luB)",
		    (u_long)page->addr, (u_long)page->size));

		/*
		 * Deleted pages cannot be discarded because they're no longer
		 * backed by file blocks -- clear our callers discard flag, the
		 * page will be discarded when the parent is reconciled.
		 */
		*discardp = 0;

		return (__wt_rec_parent_update_dirty(session, page,
		    NULL, WT_ADDR_INVALID, 0, WT_REF_EVICTED | WT_REF_MERGE));
	}

	/*
	 * Because WiredTiger's pages grow without splitting, we're replacing a
	 * single page with another single page most of the time.
	 */
	if (r->l_next == 1) {
		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "reconcile: move %lu to %lu, (%luB to %luB)",
		    (u_long)page->addr, r->list[0].off.addr,
		    (u_long)page->size, r->list[0].off.size));

		return (__wt_rec_parent_update_dirty(
		    session, page, NULL,
		    r->list[0].off.addr, r->list[0].off.size,
		    discard ? WT_REF_DISK : WT_REF_MEM));
	}

	/*
	 * A page grew so large we had to divide it into two or more physical
	 * pages -- create a new internal page.
	 */
	WT_VERBOSE(S2C(session), WT_VERB_EVICT,
	    (session, "reconcile: %lu (%luB) splitting",
	    (u_long)page->addr, (u_long)page->size));
	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(btree->stats, split_intl);
		break;
	case WT_PAGE_ROW_LEAF:
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(btree->stats, split_leaf);
		break;
	}
	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_rec_row_split(session, &new, page));
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_rec_col_split(session, &new, page));
		break;
	}

	WT_ASSERT(session, new != NULL);

	/*
	 * Update the parent to reference the new internal page, and flag the
	 * page to be merged into the parent when the parent is reconciled.
	 */
	return (__wt_rec_parent_update_dirty(session,
	    page, new, WT_ADDR_INVALID, 0, WT_REF_EVICTED | WT_REF_MERGE));
}

/*
 * __wt_rec_parent_update_clean  --
 *	Update a parent page's reference for a discarded, clean page.
 */
static void
__wt_rec_parent_update_clean(WT_PAGE *page)
{
	/*
	 * Update the relevant WT_REF structure; no memory flush is needed,
	 * the state field is declared volatile.
	 */
	page->parent_ref->state = WT_REF_DISK;
}

/*
 * __wt_rec_parent_update_dirty  --
 *	Update a parent page's reference to a reconciled page.
 */
static int
__wt_rec_parent_update_dirty(SESSION *session,
    WT_PAGE *page, WT_PAGE *split, uint32_t addr, uint32_t size, uint32_t state)
{
	WT_REF *parent_ref;
	BTREE *btree;

	btree = session->btree;

	/*
	 * Update the relevant parent WT_REF structure, flush memory, and then
	 * update the state of the parent reference.  No further memory flush
	 * needed, the state field is declared volatile.
	 */
	if (WT_PAGE_IS_ROOT(page))
		parent_ref = &btree->root_page;
	else
		parent_ref = page->parent_ref;
	parent_ref->addr = addr;
	parent_ref->size = size;
	if (split != NULL)
		parent_ref->page = split;
	WT_MEMORY_FLUSH;
	parent_ref->state = state;

	/* If we're reconciling the root page, update the descriptor record. */
	if (WT_PAGE_IS_ROOT(page))
		return (__wt_desc_write(session));

	/*
	 * Mark the parent page dirty.
	 *
	 * There's no chance we need to flush this write -- the eviction thread
	 * is the only thread that eventually cares if the page is dirty or not,
	 * and it's our update that's making it dirty.   (The workQ thread does
	 * have to flush its set-modified update, of course).
	 *
	 * We don't care if we race with the workQ; if the workQ thread races
	 * with us, the page will still be marked dirty and that's all we care
	 * about.
	 */
	WT_PAGE_SET_MODIFIED(page->parent);

	return (0);
}

/*
 * __wt_rec_row_split --
 *	Update a row-store parent page's reference when a page is split.
 */
static int
__wt_rec_row_split(SESSION *session, WT_PAGE **splitp, WT_PAGE *orig)
{
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_REC_LIST *r;
	WT_ROW_REF *rref;
	struct rec_list *r_list;
	uint32_t i;
	int ret;

	cache = S2C(session)->cache;
	r = &cache->reclist;
	ret = 0;

	/* Allocate a row-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)r->l_next, &page->u.row_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = ++cache->read_gen;
	page->addr = WT_ADDR_INVALID;
	page->size = 0;
	page->indx_count = r->l_next;
	page->type = WT_PAGE_ROW_INT;
	WT_PAGE_SET_MODIFIED(page);

	for (rref = page->u.row_int.t,
	    r_list = r->list, i = 0; i < r->l_next; ++rref, ++r_list, ++i) {
		/*
		 * Steal the split buffer's pointer -- we could allocate and
		 * copy here, but that means split buffers would potentially
		 * grow without bound, this way we do the same number of
		 * memory allocations and the split buffers don't just keep
		 * getting bigger.
		 */
		__wt_key_set(rref, r_list->key.data, r_list->key.size);
		__wt_buf_clear(&r_list->key);
		WT_ROW_REF_ADDR(rref) = r_list->off.addr;
		WT_ROW_REF_SIZE(rref) = r_list->off.size;
	}

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}

/*
 * __wt_rec_col_split --
 *	Update a column-store parent page's reference when a page is split.
 */
static int
__wt_rec_col_split(SESSION *session, WT_PAGE **splitp, WT_PAGE *orig)
{
	WT_CACHE *cache;
	WT_COL_REF *cref;
	WT_OFF_RECORD *off;
	WT_PAGE *page;
	WT_REC_LIST *r;
	struct rec_list *r_list;
	uint32_t i;
	int ret;

	cache = S2C(session)->cache;
	r = &cache->reclist;
	ret = 0;

	/* Allocate a column-store internal page. */
	WT_RET(__wt_calloc_def(session, 1, &page));
	WT_ERR(__wt_calloc_def(session, (size_t)r->l_next, &page->u.col_int.t));

	/* Fill it in. */
	page->parent = orig->parent;
	page->parent_ref = orig->parent_ref;
	page->read_gen = ++cache->read_gen;
	page->u.col_int.recno = WT_RECNO(&r->list->off);
	page->addr = WT_ADDR_INVALID;
	page->size = 0;
	page->indx_count = r->l_next;
	page->type = WT_PAGE_COL_INT;
	WT_PAGE_SET_MODIFIED(page);

	for (cref = page->u.col_int.t,
	    r_list = r->list, i = 0; i < r->l_next; ++cref, ++r_list, ++i) {
		off = &r_list->off;
		WT_COL_REF_ADDR(cref) = off->addr;
		WT_COL_REF_SIZE(cref) = off->size;
		cref->recno = WT_RECNO(off);

		WT_VERBOSE(S2C(session), WT_VERB_EVICT, (session,
		    "split: %lu (%luB), starting record %llu",
		    (u_long)off->addr, (u_long)off->size,
		    (unsigned long long)WT_RECNO(&r_list->off)));
	}

	*splitp = page;
	return (0);

err:	__wt_free(session, page);
	return (ret);
}
