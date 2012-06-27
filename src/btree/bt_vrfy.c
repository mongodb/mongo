/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * There's a bunch of stuff we pass around during verification, group it
 * together to make the code prettier.
 */
typedef struct {
	uint64_t record_total;			/* Total record count */

	WT_ITEM *max_key;			/* Largest key */
	WT_ITEM *max_addr;			/* Largest key page */

	uint64_t fcnt;				/* Progress counter */

	int	 dumpfile;			/* Dump file stream */

	WT_ITEM *tmp1;				/* Temporary buffer */
	WT_ITEM *tmp2;				/* Temporary buffer */
} WT_VSTUFF;

static void __verify_checkpoint_reset(WT_VSTUFF *);
static int  __verify_int(WT_SESSION_IMPL *, int);
static int  __verify_overflow(
	WT_SESSION_IMPL *, const uint8_t *, uint32_t, WT_VSTUFF *);
static int  __verify_overflow_cell(WT_SESSION_IMPL *, WT_PAGE *, WT_VSTUFF *);
static int  __verify_row_int_key_order(
	WT_SESSION_IMPL *, WT_PAGE *, WT_REF *, uint32_t, WT_VSTUFF *);
static int  __verify_row_leaf_key_order(
	WT_SESSION_IMPL *, WT_PAGE *, WT_VSTUFF *);
static int  __verify_tree(WT_SESSION_IMPL *, WT_PAGE *, uint64_t, WT_VSTUFF *);

/*
 * __wt_verify --
 *	Verify a file.
 */
int
__wt_verify(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	return (__verify_int(session, 0));
}

/*
 * __wt_dumpfile --
 *	Dump a file in debugging mode.
 */
int
__wt_dumpfile(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

#ifdef HAVE_DIAGNOSTIC
	/*
	 * We use the verification code to do debugging dumps because if we're
	 * dumping in debugging mode, we want to confirm the page is OK before
	 * walking it.
	 */
	return (__verify_int(session, 1));
#else
	WT_RET_MSG(session, ENOTSUP,
	    "the WiredTiger library was not built in diagnostic mode");
#endif
}

/*
 * __verify_int --
 *	Internal version of verify: verify a Btree, optionally dumping each
 * page in debugging mode.
 */
static int
__verify_int(WT_SESSION_IMPL *session, int dumpfile)
{
	WT_BTREE *btree;
	WT_CKPT *ckptbase, *ckpt;
	WT_DECL_RET;
	WT_ITEM dsk;
	WT_VSTUFF *vs, _vstuff;

	btree = session->btree;
	ckptbase = NULL;

	WT_CLEAR(_vstuff);
	vs = &_vstuff;
	vs->dumpfile = dumpfile;
	WT_ERR(__wt_scr_alloc(session, 0, &vs->max_key));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->max_addr));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp1));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp2));

	/* Get a list of the checkpoints for this file. */
	WT_ERR(__wt_meta_ckptlist_get(session, btree->name, &ckptbase));

	/* Inform the underlying block manager we're verifying. */
	WT_ERR(__wt_bm_verify_start(session, ckptbase));

	/* Loop through the file's checkpoints, verifying each one. */
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		WT_VERBOSE_ERR(session, verify,
		    "%s: checkpoint %s", btree->name, ckpt->name);

		/* House-keeping between checkpoints. */
		__verify_checkpoint_reset(vs);

		/*
		 * Load the checkpoint -- if the size of the root page is 0, the
		 * file is empty.
		 *
		 * Clearing the root page reference here is not an error: any
		 * root page we read will be discarded as part of calling the
		 * underlying eviction thread to discard the in-cache version
		 * of the tree.   Since our reference disappears in that call,
		 * we can't ever use it again.
		 */
		WT_CLEAR(dsk);
		WT_ERR(__wt_bm_checkpoint_load(
		    session, &dsk, ckpt->raw.data, ckpt->raw.size, 1));
		if (dsk.size != 0) {
			/* Verify then discard the checkpoint from the cache. */
			if ((ret = __wt_btree_tree_open(session, &dsk)) == 0) {
				ret = __verify_tree(
				    session, btree->root_page, (uint64_t)1, vs);
				WT_TRET(__wt_bt_cache_flush(
				    session, NULL, WT_SYNC_DISCARD, 0));
			}
		}

		/* Unload the checkpoint. */
		WT_TRET(__wt_bm_checkpoint_unload(session));
		WT_ERR(ret);
	}

	/* Discard the list of checkpoints. */
err:	__wt_meta_ckptlist_free(session, ckptbase);

	/* Inform the underlying block manager we're done. */
	WT_TRET(__wt_bm_verify_end(session));

	if (vs != NULL) {
		/* Wrap up reporting. */
		WT_TRET(__wt_progress(session, NULL, vs->fcnt));

		/* Free allocated memory. */
		__wt_scr_free(&vs->max_key);
		__wt_scr_free(&vs->max_addr);
		__wt_scr_free(&vs->tmp1);
		__wt_scr_free(&vs->tmp2);
	}

	return (ret);
}

/*
 * __verify_checkpoint_reset --
 *	Reset anything needing to be reset for each new checkpoint verification.
 */
static void
__verify_checkpoint_reset(WT_VSTUFF *vs)
{
	/*
	 * Key order is per checkpoint, reset the data length that serves as a
	 * flag value.
	 */
	vs->max_addr->size = 0;

	/* Record total is per checkpoint, reset the record count. */
	vs->record_total = 0;
}

/*
 * __verify_tree --
 *	Verify a tree, recursively descending through it in depth-first fashion.
 * The page argument was physically verified (so we know it's correctly formed),
 * and the in-memory version built.  Our job is to check logical relationships
 * in the page and in the tree.
 */
static int
__verify_tree(WT_SESSION_IMPL *session,
    WT_PAGE *page, uint64_t parent_recno, WT_VSTUFF *vs)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_DECL_RET;
	WT_REF *ref;
	uint64_t recno;
	uint32_t entry, i, size;
	const uint8_t *addr;

	unpack = &_unpack;

	WT_VERBOSE_RET(session, verify, "%s %s",
	    __wt_page_addr_string(session, vs->tmp1, page),
	    __wt_page_type_string(page->type));

	/*
	 * The page's physical structure was verified when it was read into
	 * memory by the read server thread, and then the in-memory version
	 * of the page was built.   Now we make sure the page and tree are
	 * logically consistent.
	 *
	 * !!!
	 * The problem: (1) the read server has to build the in-memory version
	 * of the page because the read server is the thread that flags when
	 * any thread can access the page in the tree; (2) we can't build the
	 * in-memory version of the page until the physical structure is known
	 * to be OK, so the read server has to verify at least the physical
	 * structure of the page; (3) doing complete page verification requires
	 * reading additional pages (for example, overflow keys imply reading
	 * overflow pages in order to test the key's order in the page); (4)
	 * the read server cannot read additional pages because it will hang
	 * waiting on itself.  For this reason, we split page verification
	 * into a physical verification, which allows the in-memory version
	 * of the page to be built, and then a subsequent logical verification
	 * which happens here.
	 *
	 * Report progress every 10 pages.
	 */
	if (++vs->fcnt % 10 == 0)
		WT_RET(__wt_progress(session, NULL, vs->fcnt));

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the page in debugging mode. */
	if (vs->dumpfile) {
		WT_RET(__wt_debug_page(session, page, NULL));
		if (page->dsk != NULL)
			WT_RET(__wt_debug_disk(session, page->dsk, NULL));
	}
#endif

	/*
	 * Column-store key order checks: check the starting record number,
	 * then update the total record count.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		recno = page->u.col_fix.recno;
		goto recno_chk;
	case WT_PAGE_COL_INT:
		recno = page->u.intl.recno;
		goto recno_chk;
	case WT_PAGE_COL_VAR:
		recno = page->u.col_var.recno;
recno_chk:	if (parent_recno != recno)
			WT_RET_MSG(session, WT_ERROR,
			    "page at %s has a starting record of %" PRIu64
			    " when the expected starting record is %" PRIu64,
			    __wt_page_addr_string(session, vs->tmp1, page),
			    recno, parent_recno);
		break;
	}
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		vs->record_total += page->entries;
		break;
	case WT_PAGE_COL_VAR:
		recno = 0;
		WT_COL_FOREACH(page, cip, i)
			if ((cell = WT_COL_PTR(page, cip)) == NULL)
				++recno;
			else {
				__wt_cell_unpack(cell, unpack);
				recno += __wt_cell_rle(unpack);
			}
		vs->record_total += recno;
		break;
	}

	/*
	 * Row-store leaf page key order check: it's a depth-first traversal,
	 * the first key on this page should be larger than any key previously
	 * seen.
	 */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		WT_RET(__verify_row_leaf_key_order(session, page, vs));
		break;
	}

	/*
	 * Check overflow pages.  We check overflow cells separately from other
	 * tests that walk the page as it's simpler, and I don't care much how
	 * fast table verify runs.
	 */
	switch (page->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__verify_overflow_cell(session, page, vs));
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		WT_REF_FOREACH(page, ref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * record number should be 1 more than the total records
			 * reviewed to this point.
			 */
			if (ref->u.recno != vs->record_total + 1) {
				WT_DECL_ITEM(tmp);
				WT_RET(__wt_scr_alloc(session, 0, &tmp));
				__wt_cell_unpack(ref->addr, unpack);
				ret = __wt_bm_addr_string(
				    session, tmp, unpack->data, unpack->size);
				__wt_errx(session, "page at %s has a starting "
				    "record of %" PRIu64 " when the expected "
				    "starting record was %" PRIu64,
				    ret == 0 ?
				    (char *)tmp->data : "[Unknown address]",
				    ref->u.recno, vs->record_total + 1);
				__wt_scr_free(&tmp);
				return (WT_ERROR);
			}

			/* ref references the subtree containing the record */
			__wt_get_addr(page, ref, &addr, &size);
			WT_RET(__wt_page_in(session, page, ref));
			ret =
			    __verify_tree(session, ref->page, ref->u.recno, vs);
			__wt_page_release(session, ref->page);
			WT_RET(ret);
			WT_RET(__wt_bm_verify_addr(session, addr, size));
		}
		break;
	case WT_PAGE_ROW_INT:
		/* For each entry in an internal page, verify the subtree. */
		entry = 0;
		WT_REF_FOREACH(page, ref, i) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * key should be larger than the largest key previously
			 * reviewed.
			 *
			 * The 0th key of any internal page is magic, and we
			 * can't test against it.
			 */
			if (entry != 0)
				WT_RET(__verify_row_int_key_order(
				    session, page, ref, entry, vs));
			++entry;

			/* ref references the subtree containing the record */
			__wt_get_addr(page, ref, &addr, &size);
			WT_RET(__wt_page_in(session, page, ref));
			ret =
			    __verify_tree(session, ref->page, (uint64_t)0, vs);
			__wt_page_release(session, ref->page);
			WT_RET(ret);
			WT_RET(__wt_bm_verify_addr(session, addr, size));
		}
		break;
	}
	return (0);
}

/*
 * __verify_row_int_key_order --
 *	Compare a key on an internal page to the largest key we've seen so
 * far; update the largest key we've seen so far to that key.
 */
static int
__verify_row_int_key_order(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_REF *ref, uint32_t entry, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_IKEY *ikey;
	WT_ITEM item;
	int cmp;

	btree = session->btree;

	/* The maximum key is set, we updated it from a leaf page first. */
	WT_ASSERT(session, vs->max_addr->size != 0);

	/* Set up the key structure. */
	ikey = ref->u.key;
	item.data = WT_IKEY_DATA(ikey);
	item.size = ikey->size;

	/* Compare the key against the largest key we've seen so far. */
	WT_RET(WT_BTREE_CMP(session, btree, &item, vs->max_key, cmp));
	if (cmp <= 0)
		WT_RET_MSG(session, WT_ERROR,
		    "the internal key in entry %" PRIu32 " on the page at %s "
		    "sorts before the last key appearing on page %s, earlier "
		    "in the tree",
		    entry,
		    __wt_page_addr_string(session, vs->tmp1, page),
		    (char *)vs->max_addr->data);

	/* Update the largest key we've seen to the key just checked. */
	WT_RET(__wt_buf_set(session, vs->max_key, item.data, item.size));
	(void)__wt_page_addr_string(session, vs->max_addr, page);

	return (0);
}

/*
 * __verify_row_leaf_key_order --
 *	Compare the first key on a leaf page to the largest key we've seen so
 * far; update the largest key we've seen so far to the last key on the page.
 */
static int
__verify_row_leaf_key_order(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	int cmp;

	btree = session->btree;

	/*
	 * If a tree is empty (just created), it won't have keys; if there
	 * are no keys, we're done.
	 */
	if (page->entries == 0)
		return (0);

	/*
	 * We visit our first leaf page before setting the maximum key (the 0th
	 * keys on the internal pages leading to the smallest leaf in the tree
	 * are all empty entries).
	 */
	if (vs->max_addr->size != 0) {
		WT_RET(
		    __wt_row_key_copy(session, page, page->u.row.d, vs->tmp1));

		/*
		 * Compare the key against the largest key we've seen so far.
		 *
		 * If we're comparing against a key taken from an internal page,
		 * we can compare equal (which is an expected path, the internal
		 * page key is often a copy of the leaf page's first key).  But,
		 * in the case of the 0th slot on an internal page, the last key
		 * we've seen was a key from a previous leaf page, and it's not
		 * OK to compare equally in that case.
		 */
		WT_RET(WT_BTREE_CMP(session,
		    btree, vs->tmp1, (WT_ITEM *)vs->max_key, cmp));
		if (cmp < 0)
			WT_RET_MSG(session, WT_ERROR,
			    "the first key on the page at %s sorts equal to or "
			    "less than a key appearing on the page at %s, "
			    "earlier in the tree",
			    __wt_page_addr_string(session, vs->tmp1, page),
				(char *)vs->max_addr->data);
	}

	/* Update the largest key we've seen to the last key on this page. */
	WT_RET(__wt_row_key_copy(session,
	    page, page->u.row.d + (page->entries - 1), vs->max_key));
	(void)__wt_page_addr_string(session, vs->max_addr, page);

	return (0);
}

/*
 * __verify_overflow_cell --
 *	Verify any overflow cells on the page.
 */
static int
__verify_overflow_cell(WT_SESSION_IMPL *session, WT_PAGE *page, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_RET;
	WT_PAGE_HEADER *dsk;
	uint32_t cell_num, i;

	btree = session->btree;
	unpack = &_unpack;

	/*
	 * If a tree is empty (just created), it won't have a disk image;
	 * if there is no disk image, we're done.
	 */
	if ((dsk = page->dsk) == NULL)
		return (0);

	/* Walk the disk page, verifying pages referenced by overflow cells. */
	cell_num = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		++cell_num;
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_VALUE_OVFL:
			WT_ERR(__verify_overflow(
			    session, unpack->data, unpack->size, vs));
			break;
		}
	}
	return (0);

err:	WT_RET_MSG(session, ret,
	    "cell %" PRIu32 " on page at %s references an overflow item at %s "
	    "that failed verification",
	    cell_num - 1,
	    __wt_page_addr_string(session, vs->tmp1, page),
	    __wt_addr_string(session, vs->tmp2, unpack->data, unpack->size));
}

/*
 * __verify_overflow --
 *	Read in an overflow page and check it.
 */
static int
__verify_overflow(WT_SESSION_IMPL *session,
    const uint8_t *addr, uint32_t addr_size, WT_VSTUFF *vs)
{
	WT_PAGE_HEADER *dsk;

	/* Read and verify the overflow item. */
	WT_RET(__wt_bm_read(session, vs->tmp1, addr, addr_size));

	/*
	 * The physical page has already been verified, but we haven't confirmed
	 * it was an overflow page, only that it was a valid page.  Confirm it's
	 * the type of page we expected.
	 */
	dsk = vs->tmp1->mem;
	if (dsk->type != WT_PAGE_OVFL)
		WT_RET_MSG(session, WT_ERROR,
		    "overflow referenced page at %s is not an overflow page",
		    __wt_addr_string(session, vs->tmp1, addr, addr_size));

	WT_RET(__wt_bm_verify_addr(session, addr, addr_size));
	return (0);
}
