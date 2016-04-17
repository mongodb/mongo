/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Shared rebalance information.
 */
typedef struct {
	WT_REF **leaf;				/* List of leaf pages */
	size_t	 leaf_next;			/* Next entry */
	size_t   leaf_allocated;		/* Allocated bytes */

	WT_ADDR *fl;				/* List of objects to free */
	size_t	 fl_next;			/* Next entry */
	size_t   fl_allocated;			/* Allocated bytes */

	WT_PAGE *root;				/* Created root page */

	uint8_t	 type;				/* Internal page type */

#define	WT_REBALANCE_PROGRESS_INTERVAL	100
	uint64_t progress;			/* Progress counter */

	WT_ITEM *tmp1;				/* Temporary buffers */
	WT_ITEM *tmp2;
} WT_REBALANCE_STUFF;

/*
 * __rebalance_discard --
 *	Free the allocated information.
 */
static void
__rebalance_discard(WT_SESSION_IMPL *session, WT_REBALANCE_STUFF *rs)
{
	while (rs->leaf_next > 0) {
		--rs->leaf_next;
		__wt_free_ref(
		    session, rs->leaf[rs->leaf_next], rs->type, false);
	}
	__wt_free(session, rs->leaf);

	while (rs->fl_next > 0) {
		--rs->fl_next;
		__wt_free(session, rs->fl[rs->fl_next].addr);
	}
	__wt_free(session, rs->fl);
}

/*
 * __rebalance_leaf_append --
 *	Add a new entry to the list of leaf pages.
 */
static int
__rebalance_leaf_append(WT_SESSION_IMPL *session,
    const uint8_t *key, size_t key_len, uint64_t recno,
    const uint8_t *addr, size_t addr_len, u_int addr_type,
    WT_REBALANCE_STUFF *rs)
{
	WT_ADDR *copy_addr;
	WT_REF *copy;

	WT_RET(__wt_verbose(session, WT_VERB_REBALANCE,
	    "rebalance leaf-list append %s, %s",
	    __wt_buf_set_printable(session, key, key_len, rs->tmp2),
	    __wt_addr_string(session, addr, addr_len, rs->tmp1)));

	/* Allocate and initialize a new leaf page reference. */
	WT_RET(__wt_realloc_def(
	    session, &rs->leaf_allocated, rs->leaf_next + 1, &rs->leaf));
	WT_RET(__wt_calloc_one(session, &copy));
	rs->leaf[rs->leaf_next++] = copy;

	copy->page = NULL;
	copy->home = NULL;
	copy->pindex_hint = 0;
	copy->state = WT_REF_DISK;

	WT_RET(__wt_calloc_one(session, &copy_addr));
	copy->addr = copy_addr;
	WT_RET(__wt_strndup(session, addr, addr_len, &copy_addr->addr));
	copy_addr->size = (uint8_t)addr_len;
	copy_addr->type = (uint8_t)addr_type;

	if (recno == WT_RECNO_OOB)
		WT_RET(__wt_row_ikey(session, 0, key, key_len, copy));
	else
		copy->ref_recno = recno;

	copy->page_del = NULL;
	return (0);
}

/*
 * __rebalance_fl_append --
 *	Add a new entry to the free list.
 */
static int
__rebalance_fl_append(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_len, WT_REBALANCE_STUFF *rs)
{
	WT_ADDR *copy;

	WT_RET(__wt_realloc_def(
	    session, &rs->fl_allocated, rs->fl_next + 1, &rs->fl));
	copy = &rs->fl[rs->fl_next++];

	WT_RET(__wt_strndup(session, addr, addr_len, &copy->addr));
	copy->size = (uint8_t)addr_len;
	copy->type = 0;

	return (0);
}

/*
 * __rebalance_internal --
 *	Build an in-memory page that references all of the leaf pages we've
 * found.
 */
static int
__rebalance_internal(WT_SESSION_IMPL *session, WT_REBALANCE_STUFF *rs)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF **refp;
	uint32_t i, leaf_next;

	btree = S2BT(session);

	/*
	 * There's a limit to the number of pages we can rebalance: the number
	 * of elements on a page is a 4B quantity and it's technically possible
	 * there could be more pages than that in a tree.
	 */
	if (rs->leaf_next > UINT32_MAX)
		WT_RET_MSG(session, ENOTSUP,
		    "too many leaf pages to rebalance, %" WT_SIZET_FMT " pages "
		    "exceeds the maximum of %" PRIu32,
		    rs->leaf_next, UINT32_MAX);
	leaf_next = (uint32_t)rs->leaf_next;

	/* Allocate a row-store root (internal) page and fill it in. */
	WT_RET(__wt_page_alloc(session, rs->type, leaf_next, false, &page));
	page->pg_intl_parent_ref = &btree->root;
	WT_ERR(__wt_page_modify_init(session, page));
	__wt_page_modify_set(session, page);

	pindex = WT_INTL_INDEX_GET_SAFE(page);
	for (refp = pindex->index, i = 0; i < leaf_next; ++i) {
		rs->leaf[i]->home = page;
		*refp++ = rs->leaf[i];
		rs->leaf[i] = NULL;
	}

	rs->root = page;
	return (0);

err:	__wt_page_out(session, &page);
	return (ret);
}

/*
 * __rebalance_free_original --
 *	Free the tracked internal pages and overflow keys.
 */
static int
__rebalance_free_original(WT_SESSION_IMPL *session, WT_REBALANCE_STUFF *rs)
{
	WT_ADDR *addr;
	uint64_t i;

	for (i = 0; i < rs->fl_next; ++i) {
		addr = &rs->fl[i];

		WT_RET(__wt_verbose(session, WT_VERB_REBALANCE,
		    "rebalance discarding %s",
		    __wt_addr_string(
		    session, addr->addr, addr->size, rs->tmp1)));

		WT_RET(__wt_btree_block_free(session, addr->addr, addr->size));
	}
	return (0);
}

/*
 * __rebalance_col_walk --
 *	Walk a column-store page and its descendants.
 */
static int
__rebalance_col_walk(
    WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_REBALANCE_STUFF *rs)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	uint32_t i;

	btree = S2BT(session);

	WT_ERR(__wt_scr_alloc(session, 0, &buf));

	/* Report progress periodically. */
	if (++rs->progress % WT_REBALANCE_PROGRESS_INTERVAL == 0)
		WT_ERR(__wt_progress(session, NULL, rs->progress));

	/*
	 * Walk the page, instantiating keys: the page contains sorted key and
	 * location cookie pairs.  Keys are on-page/overflow items and location
	 * cookies are WT_CELL_ADDR_XXX items.
	 */
	WT_CELL_FOREACH(btree, dsk, cell, &unpack, i) {
		__wt_cell_unpack(cell, &unpack);
		switch (unpack.type) {
		case WT_CELL_ADDR_INT:
			/* An internal page: read it and recursively walk it. */
			WT_ERR(__wt_bt_read(
			    session, buf, unpack.data, unpack.size));
			WT_ERR(__rebalance_col_walk(session, buf->data, rs));
			WT_ERR(__wt_verbose(session, WT_VERB_REBALANCE,
			    "free-list append internal page: %s",
			    __wt_addr_string(
			    session, unpack.data, unpack.size, rs->tmp1)));
			WT_ERR(__rebalance_fl_append(
			    session, unpack.data, unpack.size, rs));
			break;
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
			WT_ERR(__rebalance_leaf_append(session,
			    NULL, 0, unpack.v, unpack.data, unpack.size,
			    unpack.type == WT_CELL_ADDR_LEAF ?
			    WT_ADDR_LEAF : WT_ADDR_LEAF_NO, rs));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __rebalance_row_leaf_key --
 *	Acquire a copy of the key for a leaf page.
 */
static int
__rebalance_row_leaf_key(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_len, WT_ITEM *key, WT_REBALANCE_STUFF *rs)
{
	WT_PAGE *page;
	WT_DECL_RET;

	/*
	 * We need the first key from a leaf page. Leaf pages are relatively
	 * complex (Huffman encoding, prefix compression, and so on), do the
	 * work to instantiate the page and copy the first key to the buffer.
	 */
	WT_RET(__wt_bt_read(session, rs->tmp1, addr, addr_len));
	WT_RET(__wt_page_inmem(session, NULL, rs->tmp1->data, 0, 0, &page));
	ret = __wt_row_leaf_key_copy(session, page, &page->pg_row_d[0], key);
	__wt_page_out(session, &page);
	return (ret);
}

/*
 * __rebalance_row_walk --
 *	Walk a row-store page and its descendants.
 */
static int
__rebalance_row_walk(
    WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_REBALANCE_STUFF *rs)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK key, unpack;
	WT_DECL_ITEM(buf);
	WT_DECL_ITEM(leafkey);
	WT_DECL_RET;
	size_t len;
	uint32_t i;
	bool first_cell;
	const void *p;

	btree = S2BT(session);
	WT_CLEAR(key);			/* [-Werror=maybe-uninitialized] */

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_scr_alloc(session, 0, &leafkey));

	/* Report progress periodically. */
	if (++rs->progress % WT_REBALANCE_PROGRESS_INTERVAL == 0)
		WT_ERR(__wt_progress(session, NULL, rs->progress));

	/*
	 * Walk the page, instantiating keys: the page contains sorted key and
	 * location cookie pairs.  Keys are on-page/overflow items and location
	 * cookies are WT_CELL_ADDR_XXX items.
	 */
	first_cell = true;
	WT_CELL_FOREACH(btree, dsk, cell, &unpack, i) {
		__wt_cell_unpack(cell, &unpack);
		switch (unpack.type) {
		case WT_CELL_KEY:
			key = unpack;
			break;
		case WT_CELL_KEY_OVFL:
			/*
			 * Any overflow key that references an internal page is
			 * of no further use, schedule its blocks to be freed.
			 *
			 * We could potentially use the same overflow key being
			 * freed here for the internal page we're creating, but
			 * that's more work to get reconciliation to understand
			 * and overflow keys are (well, should be), uncommon.
			 */
			WT_ERR(__wt_verbose(session, WT_VERB_REBALANCE,
			    "free-list append overflow key: %s",
			    __wt_addr_string(
			    session, unpack.data, unpack.size, rs->tmp1)));

			WT_ERR(__rebalance_fl_append(
			    session, unpack.data, unpack.size, rs));

			key = unpack;
			break;
		case WT_CELL_ADDR_DEL:
			/*
			 * A deleted leaf page: we're rebalancing this tree,
			 * which means no transaction can be active in it,
			 * which means no deleted leaf page is interesting,
			 * ignore it.
			 */
			first_cell = false;
			break;
		case WT_CELL_ADDR_INT:
			/* An internal page, schedule its blocks to be freed. */
			WT_ERR(__wt_verbose(session, WT_VERB_REBALANCE,
			    "free-list append internal page: %s",
			    __wt_addr_string(
			    session, unpack.data, unpack.size, rs->tmp1)));
			WT_ERR(__rebalance_fl_append(
			    session, unpack.data, unpack.size, rs));

			/* Read and recursively walk the page. */
			WT_ERR(__wt_bt_read(
			    session, buf, unpack.data, unpack.size));
			WT_ERR(__rebalance_row_walk(session, buf->data, rs));
			break;
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
			/*
			 * A leaf page.
			 * We can't trust the 0th key on an internal page (we
			 * often don't store them in reconciliation because it
			 * saves space), get it from the underlying leaf page.
			 * Else, if the internal page key is an overflow key,
			 * instantiate it and use it.
			 * Else, we can use the internal page's key as is, it's
			 * sufficient for the page.
			 */
			if (first_cell) {
				WT_ERR(__rebalance_row_leaf_key(session,
				    unpack.data, unpack.size, leafkey, rs));
				p = leafkey->data;
				len = leafkey->size;
			} else if (key.type == WT_CELL_KEY_OVFL) {
				WT_ERR(__wt_dsk_cell_data_ref(
				    session, WT_PAGE_ROW_INT, &key, leafkey));
				p = leafkey->data;
				len = leafkey->size;
			} else {
				p = key.data;
				len = key.size;
			}
			WT_ERR(__rebalance_leaf_append(session,
			    p, len, WT_RECNO_OOB, unpack.data, unpack.size,
			    unpack.type == WT_CELL_ADDR_LEAF ?
			    WT_ADDR_LEAF : WT_ADDR_LEAF_NO, rs));

			first_cell = false;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

err:	__wt_scr_free(session, &buf);
	__wt_scr_free(session, &leafkey);
	return (ret);
}

/*
 * __wt_bt_rebalance --
 *	Rebalance the last checkpoint in the file.
 */
int
__wt_bt_rebalance(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_REBALANCE_STUFF *rs, _rstuff;
	bool evict_reset;

	WT_UNUSED(cfg);

	btree = S2BT(session);
	evict_reset = false;

	/*
	 * If the tree has never been written to disk, we're done, rebalance
	 * walks disk images, not in-memory pages. For the same reason, the
	 * tree has to be clean.
	 */
	if (btree->root.page->dsk == NULL)
		return (0);
	if (btree->modified)
		WT_RET_MSG(session, EINVAL,
		    "tree is modified, only clean trees may be rebalanced");

	WT_CLEAR(_rstuff);
	rs = &_rstuff;

	WT_ERR(__wt_scr_alloc(session, 0, &rs->tmp1));
	WT_ERR(__wt_scr_alloc(session, 0, &rs->tmp2));

	/* Set the internal page tree type. */
	rs->type = btree->root.page->type;

	/*
	 * Get exclusive access to the file. (Not required, the only page in the
	 * cache is the root page, and that cannot be evicted; however, this way
	 * eviction ignores the tree entirely.)
	 */
	WT_ERR(__wt_evict_file_exclusive_on(session));
	evict_reset = true;

	/* Recursively walk the tree. */
	switch (rs->type) {
	case WT_PAGE_ROW_INT:
		WT_ERR(
		    __rebalance_row_walk(session, btree->root.page->dsk, rs));
		break;
	case WT_PAGE_COL_INT:
		WT_ERR(
		    __rebalance_col_walk(session, btree->root.page->dsk, rs));
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	/* Build a new root page. */
	WT_ERR(__rebalance_internal(session, rs));

	/*
	 * Schedule the free of the original blocks (they shouldn't actually be
	 * freed until the next checkpoint completes).
	 */
	WT_ERR(__rebalance_free_original(session, rs));

	/*
	 * Swap the old root page for our newly built root page, writing the new
	 * root page as part of a checkpoint will finish the rebalance.
	 */
	__wt_page_out(session, &btree->root.page);
	btree->root.page = rs->root;
	rs->root = NULL;

err:	if (evict_reset)
	    __wt_evict_file_exclusive_off(session);

	/* Discard any leftover root page we created. */
	if (rs->root != NULL) {
		__wt_page_modify_clear(session, rs->root);
		__wt_page_out(session, &rs->root);
	}

	/* Discard any leftover leaf and internal page information. */
	__rebalance_discard(session, rs);

	__wt_scr_free(session, &rs->tmp1);
	__wt_scr_free(session, &rs->tmp2);

	return (ret);
}
