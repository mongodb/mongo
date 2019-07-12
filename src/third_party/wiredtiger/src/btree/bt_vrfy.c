/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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

#define	WT_VRFY_DUMP(vs)						\
	((vs)->dump_address ||						\
	    (vs)->dump_blocks || (vs)->dump_layout || (vs)->dump_pages)
	bool dump_address;			/* Configure: dump special */
	bool dump_blocks;
	bool dump_layout;
	bool dump_pages;
						/* Page layout information */
	uint64_t depth, depth_internal[100], depth_leaf[100];

	WT_ITEM *tmp1, *tmp2, *tmp3, *tmp4;	/* Temporary buffers */
} WT_VSTUFF;

static void __verify_checkpoint_reset(WT_VSTUFF *);
static int  __verify_page_cell(
	WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK *, WT_VSTUFF *);
static int  __verify_row_int_key_order(
	WT_SESSION_IMPL *, WT_PAGE *, WT_REF *, uint32_t, WT_VSTUFF *);
static int  __verify_row_leaf_key_order(
	WT_SESSION_IMPL *, WT_REF *, WT_VSTUFF *);
static int  __verify_tree(
	    WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK *, WT_VSTUFF *);

/*
 * __verify_config --
 *	Debugging: verification supports dumping pages in various formats.
 */
static int
__verify_config(WT_SESSION_IMPL *session, const char *cfg[], WT_VSTUFF *vs)
{
	WT_CONFIG_ITEM cval;

	WT_RET(__wt_config_gets(session, cfg, "dump_address", &cval));
	vs->dump_address = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "dump_blocks", &cval));
	vs->dump_blocks = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "dump_layout", &cval));
	vs->dump_layout = cval.val != 0;

	WT_RET(__wt_config_gets(session, cfg, "dump_pages", &cval));
	vs->dump_pages = cval.val != 0;

#if !defined(HAVE_DIAGNOSTIC)
	if (vs->dump_blocks || vs->dump_pages)
		WT_RET_MSG(session, ENOTSUP,
		    "the WiredTiger library was not built in diagnostic mode");
#endif
	return (0);
}

/*
 * __verify_config_offsets --
 *	Debugging: optionally dump specific blocks from the file.
 */
static int
__verify_config_offsets(
    WT_SESSION_IMPL *session, const char *cfg[], bool *quitp)
{
	WT_CONFIG list;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_RET;
	uint64_t offset;

	*quitp = false;

	WT_RET(__wt_config_gets(session, cfg, "dump_offsets", &cval));
	__wt_config_subinit(session, &list, &cval);
	while ((ret = __wt_config_next(&list, &k, &v)) == 0) {
		/*
		 * Quit after dumping the requested blocks.  (That's hopefully
		 * what the user wanted, all of this stuff is just hooked into
		 * verify because that's where we "dump blocks" for debugging.)
		 */
		*quitp = true;
		/* NOLINTNEXTLINE(cert-err34-c) */
		if (v.len != 0 || sscanf(k.str, "%" SCNu64, &offset) != 1)
			WT_RET_MSG(session, EINVAL,
			    "unexpected dump offset format");
#if !defined(HAVE_DIAGNOSTIC)
		WT_RET_MSG(session, ENOTSUP,
		    "the WiredTiger library was not built in diagnostic mode");
#else
		WT_TRET(
		    __wt_debug_offset_blind(session, (wt_off_t)offset, NULL));
#endif
	}
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __verify_layout --
 *	Dump the tree shape.
 */
static int
__verify_layout(WT_SESSION_IMPL *session, WT_VSTUFF *vs)
{
	size_t i;
	uint64_t total;

	for (i = 0, total = 0; i < WT_ELEMENTS(vs->depth_internal); ++i)
		total += vs->depth_internal[i];
	WT_RET(__wt_msg(
	    session, "Internal page tree-depth (total %" PRIu64 "):", total));
	for (i = 0; i < WT_ELEMENTS(vs->depth_internal); ++i)
		if (vs->depth_internal[i] != 0) {
			WT_RET(__wt_msg(session,
			    "\t%03" WT_SIZET_FMT ": %" PRIu64,
			    i, vs->depth_internal[i]));
			vs->depth_internal[i] = 0;
		}

	for (i = 0, total = 0; i < WT_ELEMENTS(vs->depth_leaf); ++i)
		total += vs->depth_leaf[i];
	WT_RET(__wt_msg(
	    session, "Leaf page tree-depth (total %" PRIu64 "):", total));
	for (i = 0; i < WT_ELEMENTS(vs->depth_leaf); ++i)
		if (vs->depth_leaf[i] != 0) {
			WT_RET(__wt_msg(session,
			    "\t%03" WT_SIZET_FMT ": %" PRIu64,
			    i, vs->depth_leaf[i]));
			vs->depth_leaf[i] = 0;
		}
	return (0);
}

/*
 * __wt_verify --
 *	Verify a file.
 */
int
__wt_verify(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CELL_UNPACK addr_unpack;
	WT_CKPT *ckptbase, *ckpt;
	WT_DECL_RET;
	WT_VSTUFF *vs, _vstuff;
	size_t root_addr_size;
	uint8_t root_addr[WT_BTREE_MAX_ADDR_COOKIE];
	const char *name;
	bool bm_start, quit;

	btree = S2BT(session);
	bm = btree->bm;
	ckptbase = NULL;
	name = session->dhandle->name;
	bm_start = false;

	WT_CLEAR(_vstuff);
	vs = &_vstuff;
	WT_ERR(__wt_scr_alloc(session, 0, &vs->max_key));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->max_addr));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp1));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp2));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp3));
	WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp4));

	/* Check configuration strings. */
	WT_ERR(__verify_config(session, cfg, vs));

	/* Optionally dump specific block offsets. */
	WT_ERR(__verify_config_offsets(session, cfg, &quit));
	if (quit)
		goto done;

	/*
	 * Get a list of the checkpoints for this file. Empty objects have no
	 * checkpoints, in which case there's no work to do.
	 */
	ret = __wt_meta_ckptlist_get(session, name, false, &ckptbase);
	if (ret == WT_NOTFOUND) {
		ret = 0;
		goto done;
	}
	WT_ERR(ret);

	/* Inform the underlying block manager we're verifying. */
	WT_ERR(bm->verify_start(bm, session, ckptbase, cfg));
	bm_start = true;

	/* Loop through the file's checkpoints, verifying each one. */
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		__wt_verbose(session, WT_VERB_VERIFY,
		    "%s: checkpoint %s", name, ckpt->name);

		/* Fake checkpoints require no work. */
		if (F_ISSET(ckpt, WT_CKPT_FAKE))
			continue;

		/* House-keeping between checkpoints. */
		__verify_checkpoint_reset(vs);

		if (WT_VRFY_DUMP(vs)) {
			WT_ERR(__wt_msg(session, "%s", WT_DIVIDER));
			WT_ERR(__wt_msg(session, "%s: checkpoint %s",
			    name, ckpt->name));
		}

		/* Load the checkpoint. */
		WT_ERR(bm->checkpoint_load(bm, session,
		    ckpt->raw.data, ckpt->raw.size,
		    root_addr, &root_addr_size, true));

		/* Skip trees with no root page. */
		if (root_addr_size != 0) {
			WT_ERR(__wt_btree_tree_open(
			    session, root_addr, root_addr_size));

			if (WT_VRFY_DUMP(vs))
				WT_ERR(__wt_msg(session, "Root: %s %s",
				    __wt_addr_string(session,
				    root_addr, root_addr_size, vs->tmp1),
				    __wt_page_type_string(
				    btree->root.page->type)));

			__wt_evict_file_exclusive_off(session);

			/*
			 * Create a fake, unpacked parent cell for the tree
			 * based on the checkpoint information.
			 */
			memset(&addr_unpack, 0, sizeof(addr_unpack));
			addr_unpack.newest_durable_ts = ckpt->newest_durable_ts;
			addr_unpack.oldest_start_ts = ckpt->oldest_start_ts;
			addr_unpack.oldest_start_txn = ckpt->oldest_start_txn;
			addr_unpack.newest_stop_ts = ckpt->newest_stop_ts;
			addr_unpack.newest_stop_txn = ckpt->newest_stop_txn;
			addr_unpack.raw = WT_CELL_ADDR_INT;

			/* Verify the tree. */
			WT_WITH_PAGE_INDEX(session, ret = __verify_tree(
			    session, &btree->root, &addr_unpack, vs));

			/*
			 * We have an exclusive lock on the handle, but we're
			 * swapping root pages in-and-out of that handle, and
			 * there's a race with eviction entering the tree and
			 * seeing an invalid root page. Eviction must work on
			 * trees being verified (else we'd have to do our own
			 * eviction), lock eviction out whenever we're loading
			 * a new root page. This loops works because we are
			 * called with eviction locked out, so we release the
			 * lock at the top of the loop and re-acquire it here.
			 */
			WT_TRET(__wt_evict_file_exclusive_on(session));
			WT_TRET(__wt_evict_file(session, WT_SYNC_DISCARD));
		}

		/* Unload the checkpoint. */
		WT_TRET(bm->checkpoint_unload(bm, session));

		/*
		 * We've finished one checkpoint's verification (verification,
		 * then cache eviction and checkpoint unload): if any errors
		 * occurred, quit. Done this way because otherwise we'd need
		 * at least two more state variables on error, one to know if
		 * we need to discard the tree from the cache and one to know
		 * if we need to unload the checkpoint.
		 */
		WT_ERR(ret);

		/* Display the tree shape. */
		if (vs->dump_layout)
			WT_ERR(__verify_layout(session, vs));
	}

done:
err:
	/* Inform the underlying block manager we're done. */
	if (bm_start)
		WT_TRET(bm->verify_end(bm, session));

	/* Discard the list of checkpoints. */
	if (ckptbase != NULL)
		__wt_meta_ckptlist_free(session, &ckptbase);

	/* Free allocated memory. */
	__wt_scr_free(session, &vs->max_key);
	__wt_scr_free(session, &vs->max_addr);
	__wt_scr_free(session, &vs->tmp1);
	__wt_scr_free(session, &vs->tmp2);
	__wt_scr_free(session, &vs->tmp3);
	__wt_scr_free(session, &vs->tmp4);

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

	/* Tree depth. */
	vs->depth = 1;
}

/*
 * __verify_addr_ts --
 *	Check an address block's timestamps.
 */
static int
__verify_addr_ts(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_CELL_UNPACK *unpack, WT_VSTUFF *vs)
{
	char ts_string[2][WT_TS_INT_STRING_SIZE];

	if (unpack->newest_stop_ts == WT_TS_NONE)
		WT_RET_MSG(session, WT_ERROR,
		    "internal page reference at %s has a newest stop "
		    "timestamp of 0",
		    __wt_page_addr_string(session, ref, vs->tmp1));
	if (unpack->oldest_start_ts > unpack->newest_stop_ts)
		WT_RET_MSG(session, WT_ERROR,
		    "internal page reference at %s has an oldest start "
		    "timestamp %s newer than its newest stop timestamp %s",
		    __wt_page_addr_string(session, ref, vs->tmp1),
		    __wt_timestamp_to_string(
		    unpack->oldest_start_ts, ts_string[0]),
		    __wt_timestamp_to_string(
		    unpack->newest_stop_ts, ts_string[1]));
	if (unpack->newest_stop_txn == WT_TXN_NONE)
		WT_RET_MSG(session, WT_ERROR,
		    "internal page reference at %s has a newest stop "
		    "transaction of 0",
		    __wt_page_addr_string(session, ref, vs->tmp1));
	if (unpack->oldest_start_txn > unpack->newest_stop_txn)
		WT_RET_MSG(session, WT_ERROR,
		    "internal page reference at %s has an oldest start "
		    "transaction (%" PRIu64 ") newer than its newest stop "
		    "transaction (%" PRIu64 ")",
		    __wt_page_addr_string(session, ref, vs->tmp1),
		    unpack->oldest_start_txn, unpack->newest_stop_txn);
	return (0);
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
    WT_REF *ref, WT_CELL_UNPACK *addr_unpack, WT_VSTUFF *vs)
{
	WT_BM *bm;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_REF *child_ref;
	uint64_t recno;
	uint32_t entry, i;

	bm = S2BT(session)->bm;
	page = ref->page;

	unpack = &_unpack;

	__wt_verbose(session, WT_VERB_VERIFY, "%s %s",
	    __wt_page_addr_string(session, ref, vs->tmp1),
	    __wt_page_type_string(page->type));

	/* Optionally dump the address. */
	if (vs->dump_address)
		WT_RET(__wt_msg(session, "%s %s",
		    __wt_page_addr_string(session, ref, vs->tmp1),
		    __wt_page_type_string(page->type)));

	/* Track the shape of the tree. */
	if (WT_PAGE_IS_INTERNAL(page))
		++vs->depth_internal[
		    WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1)];
	else
		++vs->depth_leaf[
		    WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1)];

	/*
	 * The page's physical structure was verified when it was read into
	 * memory by the read server thread, and then the in-memory version
	 * of the page was built. Now we make sure the page and tree are
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
	 * Report progress occasionally.
	 */
#define	WT_VERIFY_PROGRESS_INTERVAL	100
	if (++vs->fcnt % WT_VERIFY_PROGRESS_INTERVAL == 0)
		WT_RET(__wt_progress(session, NULL, vs->fcnt));

#ifdef HAVE_DIAGNOSTIC
	/* Optionally dump the blocks or page in debugging mode. */
	if (vs->dump_blocks)
		WT_RET(__wt_debug_disk(session, page->dsk, NULL));
	if (vs->dump_pages)
		WT_RET(__wt_debug_page(session, NULL, ref, NULL));
#endif

	/*
	 * Column-store key order checks: check the page's record number and
	 * then update the total record count.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
		recno = ref->ref_recno;
		goto recno_chk;
	case WT_PAGE_COL_VAR:
		recno = ref->ref_recno;
recno_chk:	if (recno != vs->record_total + 1)
			WT_RET_MSG(session, WT_ERROR,
			    "page at %s has a starting record of %" PRIu64
			    " when the expected starting record is %" PRIu64,
			    __wt_page_addr_string(session, ref, vs->tmp1),
			    recno, vs->record_total + 1);
		break;
	}
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		vs->record_total += page->entries;
		break;
	case WT_PAGE_COL_VAR:
		recno = 0;
		WT_COL_FOREACH(page, cip, i) {
			cell = WT_COL_PTR(page, cip);
			__wt_cell_unpack(session, page, cell, unpack);
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
		WT_RET(__verify_row_leaf_key_order(session, ref, vs));
		break;
	}

	/* Compare the address type against the page type. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
			goto celltype_err;
		break;
	case WT_PAGE_COL_VAR:
		if (addr_unpack->raw != WT_CELL_ADDR_LEAF &&
		    addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
			goto celltype_err;
		break;
	case WT_PAGE_ROW_LEAF:
		if (addr_unpack->raw != WT_CELL_ADDR_DEL &&
		    addr_unpack->raw != WT_CELL_ADDR_LEAF &&
		    addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
			goto celltype_err;
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		if (addr_unpack->raw != WT_CELL_ADDR_INT)
celltype_err:		WT_RET_MSG(session, WT_ERROR,
			    "page at %s, of type %s, is referenced in "
			    "its parent by a cell of type %s",
			    __wt_page_addr_string(session, ref, vs->tmp1),
			    __wt_page_type_string(page->type),
			    __wt_cell_type_string(addr_unpack->raw));
		break;
	}

	/*
	 * Check overflow pages and timestamps. Done in one function as both
	 * checks require walking the page cells and we don't want to do it
	 * twice.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__verify_page_cell(session, ref, addr_unpack, vs));
		break;
	}

	/* Check tree connections and recursively descend the tree. */
	switch (page->type) {
	case WT_PAGE_COL_INT:
		/* For each entry in an internal page, verify the subtree. */
		entry = 0;
		WT_INTL_FOREACH_BEGIN(session, page, child_ref) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * record number should be 1 more than the total records
			 * reviewed to this point.
			 */
			++entry;
			if (child_ref->ref_recno != vs->record_total + 1) {
				WT_RET_MSG(session, WT_ERROR,
				    "the starting record number in entry %"
				    PRIu32 " of the column internal page at "
				    "%s is %" PRIu64 " and the expected "
				    "starting record number is %" PRIu64,
				    entry,
				    __wt_page_addr_string(
				    session, child_ref, vs->tmp1),
				    child_ref->ref_recno, vs->record_total + 1);
			}

			/* Unpack the address block and check timestamps */
			__wt_cell_unpack(
			    session, child_ref->home, child_ref->addr, unpack);
			WT_RET(__verify_addr_ts(
			    session, child_ref, unpack, vs));

			/* Verify the subtree. */
			++vs->depth;
			WT_RET(__wt_page_in(session, child_ref, 0));
			ret = __verify_tree(session, child_ref, unpack, vs);
			WT_TRET(__wt_page_release(session, child_ref, 0));
			--vs->depth;
			WT_RET(ret);

			WT_RET(bm->verify_addr(
			    bm, session, unpack->data, unpack->size));
		} WT_INTL_FOREACH_END;
		break;
	case WT_PAGE_ROW_INT:
		/* For each entry in an internal page, verify the subtree. */
		entry = 0;
		WT_INTL_FOREACH_BEGIN(session, page, child_ref) {
			/*
			 * It's a depth-first traversal: this entry's starting
			 * key should be larger than the largest key previously
			 * reviewed.
			 *
			 * The 0th key of any internal page is magic, and we
			 * can't test against it.
			 */
			++entry;
			if (entry != 1)
				WT_RET(__verify_row_int_key_order(
				    session, page, child_ref, entry, vs));

			/* Unpack the address block and check timestamps */
			__wt_cell_unpack(
			    session, child_ref->home, child_ref->addr, unpack);
			WT_RET(__verify_addr_ts(
			    session, child_ref, unpack, vs));

			/* Verify the subtree. */
			++vs->depth;
			WT_RET(__wt_page_in(session, child_ref, 0));
			ret = __verify_tree(session, child_ref, unpack, vs);
			WT_TRET(__wt_page_release(session, child_ref, 0));
			--vs->depth;
			WT_RET(ret);

			WT_RET(bm->verify_addr(
			    bm, session, unpack->data, unpack->size));
		} WT_INTL_FOREACH_END;
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
    WT_PAGE *parent, WT_REF *ref, uint32_t entry, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_ITEM item;
	int cmp;

	btree = S2BT(session);

	/* The maximum key is set, we updated it from a leaf page first. */
	WT_ASSERT(session, vs->max_addr->size != 0);

	/* Get the parent page's internal key. */
	__wt_ref_key(parent, ref, &item.data, &item.size);

	/* Compare the key against the largest key we've seen so far. */
	WT_RET(__wt_compare(
	    session, btree->collator, &item, vs->max_key, &cmp));
	if (cmp <= 0)
		WT_RET_MSG(session, WT_ERROR,
		    "the internal key in entry %" PRIu32 " on the page at %s "
		    "sorts before the last key appearing on page %s, earlier "
		    "in the tree: %s, %s",
		    entry,
		    __wt_page_addr_string(session, ref, vs->tmp1),
		    (char *)vs->max_addr->data,
		    __wt_buf_set_printable(session,
		    item.data, item.size, vs->tmp2),
		    __wt_buf_set_printable(session,
		    vs->max_key->data, vs->max_key->size, vs->tmp3));

	/* Update the largest key we've seen to the key just checked. */
	WT_RET(__wt_buf_set(session, vs->max_key, item.data, item.size));
	WT_IGNORE_RET_PTR(__wt_page_addr_string(session, ref, vs->max_addr));

	return (0);
}

/*
 * __verify_row_leaf_key_order --
 *	Compare the first key on a leaf page to the largest key we've seen so
 * far; update the largest key we've seen so far to the last key on the page.
 */
static int
__verify_row_leaf_key_order(
    WT_SESSION_IMPL *session, WT_REF *ref, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	int cmp;

	btree = S2BT(session);
	page = ref->page;

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
		WT_RET(__wt_row_leaf_key_copy(
		    session, page, page->pg_row, vs->tmp1));

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
		WT_RET(__wt_compare(session,
		    btree->collator, vs->tmp1, (WT_ITEM *)vs->max_key, &cmp));
		if (cmp < 0)
			WT_RET_MSG(session, WT_ERROR,
			    "the first key on the page at %s sorts equal to "
			    "or less than the last key appearing on the page "
			    "at %s, earlier in the tree: %s, %s",
			    __wt_page_addr_string(session, ref, vs->tmp2),
			    (char *)vs->max_addr->data,
			    __wt_buf_set_printable(session,
			    vs->tmp1->data, vs->tmp1->size, vs->tmp3),
			    __wt_buf_set_printable(session,
			    vs->max_key->data, vs->max_key->size, vs->tmp4));
	}

	/* Update the largest key we've seen to the last key on this page. */
	WT_RET(__wt_row_leaf_key_copy(session, page,
	    page->pg_row + (page->entries - 1), vs->max_key));
	WT_IGNORE_RET_PTR(__wt_page_addr_string(session, ref, vs->max_addr));

	return (0);
}

/*
 * __verify_overflow --
 *	Read in an overflow page and check it.
 */
static int
__verify_overflow(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_size, WT_VSTUFF *vs)
{
	WT_BM *bm;
	const WT_PAGE_HEADER *dsk;

	bm = S2BT(session)->bm;

	/* Read and verify the overflow item. */
	WT_RET(__wt_bt_read(session, vs->tmp1, addr, addr_size));

	/*
	 * The physical page has already been verified, but we haven't confirmed
	 * it was an overflow page, only that it was a valid page.  Confirm it's
	 * the type of page we expected.
	 */
	dsk = vs->tmp1->data;
	if (dsk->type != WT_PAGE_OVFL)
		WT_RET_MSG(session, WT_ERROR,
		    "overflow referenced page at %s is not an overflow page",
		    __wt_addr_string(session, addr, addr_size, vs->tmp1));

	WT_RET(bm->verify_addr(bm, session, addr, addr_size));
	return (0);
}

/*
 * __verify_ts_addr_cmp --
 *	Do a cell timestamp check against the parent.
 */
static int
__verify_ts_addr_cmp(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t cell_num,
    const char *ts1_name, wt_timestamp_t ts1,
    const char *ts2_name, wt_timestamp_t ts2,
    bool gt, WT_VSTUFF *vs)
{
	const char *ts1_bp, *ts2_bp;
	char ts_string[2][WT_TS_INT_STRING_SIZE];

	if (gt && ts1 >= ts2)
		return (0);
	if (!gt && ts1 <= ts2)
		return (0);

	switch (ts1) {
	case WT_TS_MAX:
		ts1_bp = "WT_TS_MAX";
		break;
	case WT_TS_NONE:
		ts1_bp = "WT_TS_NONE";
		break;
	default:
		ts1_bp = __wt_timestamp_to_string(ts1, ts_string[0]);
		break;
	}
	switch (ts2) {
	case WT_TS_MAX:
		ts2_bp = "WT_TS_MAX";
		break;
	case WT_TS_NONE:
		ts2_bp = "WT_TS_NONE";
		break;
	default:
		ts2_bp = __wt_timestamp_to_string(ts2, ts_string[1]);
		break;
	}
	WT_RET_MSG(session, WT_ERROR,
	    "cell %" PRIu32 " on page at %s failed verification with %s "
	    "timestamp of %s, %s the parent's %s timestamp of %s",
	    cell_num,
	    __wt_page_addr_string(session, ref, vs->tmp1),
	    ts1_name, ts1_bp,
	    gt ? "less than" : "greater than",
	    ts2_name, ts2_bp);
}

/*
 * __verify_txn_addr_cmp --
 *	Do a cell transaction check against the parent.
 */
static int
__verify_txn_addr_cmp(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t cell_num,
    const char *txn1_name, uint64_t txn1,
    const char *txn2_name, uint64_t txn2,
    bool gt, WT_VSTUFF *vs)
{
	if (gt && txn1 >= txn2)
		return (0);
	if (!gt && txn1 <= txn2)
		return (0);

	WT_RET_MSG(session, WT_ERROR,
	    "cell %" PRIu32 " on page at %s failed verification with %s "
	    "transaction of %" PRIu64 ", %s the parent's %s transaction of "
	    "%" PRIu64,
	    cell_num,
	    __wt_page_addr_string(session, ref, vs->tmp1),
	    txn1_name, txn1,
	    gt ? "less than" : "greater than",
	    txn2_name, txn2);
}

/*
 * __verify_page_cell --
 *	Verify the cells on the page.
 */
static int
__verify_page_cell(WT_SESSION_IMPL *session,
    WT_REF *ref, WT_CELL_UNPACK *addr_unpack, WT_VSTUFF *vs)
{
	WT_BTREE *btree;
	WT_CELL_UNPACK unpack;
	WT_DECL_RET;
	const WT_PAGE_HEADER *dsk;
	uint32_t cell_num;
	char ts_string[2][WT_TS_INT_STRING_SIZE];
	bool found_ovfl;

	/*
	 * If a tree is empty (just created), it won't have a disk image;
	 * if there is no disk image, we're done.
	 */
	if ((dsk = ref->page->dsk) == NULL)
		return (0);

	btree = S2BT(session);
	found_ovfl = false;

	/* Walk the page, tracking timestamps and verifying overflow pages. */
	cell_num = 0;
	WT_CELL_FOREACH_BEGIN(session, btree, dsk, unpack) {
		++cell_num;
		switch (unpack.type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_VALUE_OVFL:
			found_ovfl = true;
			if ((ret = __verify_overflow(
			    session, unpack.data, unpack.size, vs)) != 0)
				WT_RET_MSG(session, ret,
				    "cell %" PRIu32 " on page at %s references "
				    "an overflow item at %s that failed "
				    "verification",
				    cell_num - 1,
				    __wt_page_addr_string(session,
				    ref, vs->tmp1),
				    __wt_addr_string(session,
				    unpack.data, unpack.size, vs->tmp2));
			break;
		}

		/*
		 * Timestamps aren't necessarily an exact match, but should be
		 * within the boundaries of the parent reference.
		 */
		switch (unpack.type) {
		case WT_CELL_ADDR_DEL:
		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
			if (unpack.newest_stop_ts == WT_TS_NONE)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a "
				    "newest stop timestamp of 0",
				    cell_num - 1,
				    __wt_page_addr_string(
				    session, ref, vs->tmp1));
			if (unpack.newest_stop_txn == WT_TXN_NONE)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a "
				    "newest stop transaction of 0",
				    cell_num - 1,
				    __wt_page_addr_string(
				    session, ref, vs->tmp1));
			if (unpack.oldest_start_ts > unpack.newest_stop_ts)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has an "
				    "oldest start timestamp %s newer than "
				    "its newest stop timestamp %s",
				    cell_num - 1,
				    __wt_page_addr_string(session,
				    ref, vs->tmp1),
				    __wt_timestamp_to_string(
					unpack.oldest_start_ts, ts_string[0]),
				    __wt_timestamp_to_string(
					unpack.newest_stop_ts, ts_string[1]));
			if (unpack.oldest_start_txn > unpack.newest_stop_txn) {
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has an "
				    "oldest start transaction (%" PRIu64 ") "
				    "newer than its newest stop transaction "
				    "(%" PRIu64 ")",
				    cell_num - 1,
				    __wt_page_addr_string(session,
				    ref, vs->tmp1), unpack.oldest_start_txn,
				    unpack.newest_stop_txn);
			}

			WT_RET(__verify_ts_addr_cmp(session, ref, cell_num - 1,
			    "newest durable", unpack.newest_durable_ts,
			    "newest durable", addr_unpack->newest_durable_ts,
			    false, vs));
			WT_RET(__verify_ts_addr_cmp(session, ref, cell_num - 1,
			    "oldest start", unpack.oldest_start_ts,
			    "oldest start", addr_unpack->oldest_start_ts,
			    true, vs));
			WT_RET(__verify_txn_addr_cmp(session, ref, cell_num - 1,
			    "oldest start", unpack.oldest_start_txn,
			    "oldest start", addr_unpack->oldest_start_txn,
			    true, vs));
			WT_RET(__verify_ts_addr_cmp(session, ref, cell_num - 1,
			    "newest stop", unpack.newest_stop_ts,
			    "newest stop", addr_unpack->newest_stop_ts,
			    false, vs));
			WT_RET(__verify_txn_addr_cmp(session, ref, cell_num - 1,
			    "newest stop", unpack.newest_stop_txn,
			    "newest stop", addr_unpack->newest_stop_txn,
			    false, vs));
			break;
		case WT_CELL_DEL:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_COPY:
		case WT_CELL_VALUE_OVFL:
		case WT_CELL_VALUE_SHORT:
			if (unpack.stop_ts == WT_TS_NONE)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a stop "
				    "timestamp of 0",
				    cell_num - 1,
				    __wt_page_addr_string(
				    session, ref, vs->tmp1));
			if (unpack.start_ts > unpack.stop_ts)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a "
				    "start timestamp %s newer than its stop "
				    "timestamp %s",
				    cell_num - 1,
				    __wt_page_addr_string(session,
				    ref, vs->tmp1),
				    __wt_timestamp_to_string(
				    unpack.start_ts, ts_string[0]),
				    __wt_timestamp_to_string(
				    unpack.stop_ts, ts_string[1]));
			if (unpack.stop_txn == WT_TXN_NONE)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a stop "
				    "transaction of 0",
				    cell_num - 1,
				    __wt_page_addr_string(
				    session, ref, vs->tmp1));
			if (unpack.start_txn > unpack.stop_txn)
				WT_RET_MSG(session, WT_ERROR,
				    "cell %" PRIu32 " on page at %s has a "
				    "start transaction %" PRIu64 "newer than "
				    "its stop transaction %" PRIu64,
				    cell_num - 1,
				    __wt_page_addr_string(session,
				    ref, vs->tmp1),
				    unpack.start_txn, unpack.stop_txn);

			WT_RET(__verify_ts_addr_cmp(session, ref, cell_num - 1,
			    "start", unpack.start_ts,
			    "oldest start", addr_unpack->oldest_start_ts,
			    true, vs));
			WT_RET(__verify_txn_addr_cmp(session, ref, cell_num - 1,
			    "start", unpack.start_txn,
			    "oldest start", addr_unpack->oldest_start_txn,
			    true, vs));
			WT_RET(__verify_ts_addr_cmp(session, ref, cell_num - 1,
			    "stop", unpack.stop_ts,
			    "newest stop", addr_unpack->newest_stop_ts,
			    false, vs));
			WT_RET(__verify_txn_addr_cmp(session, ref, cell_num - 1,
			    "stop", unpack.stop_txn,
			    "newest stop", addr_unpack->newest_stop_txn,
			    false, vs));
			break;
		}
	} WT_CELL_FOREACH_END;

	/*
	 * Object if a leaf-no-overflow address cell references a page with
	 * overflow keys, but don't object if a leaf address cell references
	 * a page without overflow keys.  Reconciliation doesn't guarantee
	 * every leaf page without overflow items will be a leaf-no-overflow
	 * type.
	 */
	if (found_ovfl && addr_unpack->raw == WT_CELL_ADDR_LEAF_NO)
		WT_RET_MSG(session, WT_ERROR,
		    "page at %s, of type %s and referenced in its parent by a "
		    "cell of type %s, contains overflow items",
		    __wt_page_addr_string(session, ref, vs->tmp1),
		    __wt_page_type_string(ref->page->type),
		    __wt_cell_type_string(addr_unpack->raw));

	return (0);
}
