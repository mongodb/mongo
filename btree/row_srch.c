/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_key_build(WT_TOC *, WT_PAGE *, WT_ROW *);

/*
 * __wt_bt_search_row --
 *	Search a row-store tree for a specific key.
 */
int
__wt_bt_search_row(WT_TOC *toc, DBT *key, uint32_t level, uint32_t flags)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	WT_REPL *repl;
	uint32_t addr, base, indx, limit, size;
	uint16_t write_gen;
	int cmp, isleaf, ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;
	toc->srch_repl = NULL;
	toc->srch_exp = NULL;
	toc->srch_write_gen = 0;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db,
	    "__wt_bt_search_key_row", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

restart:
	/* Search the tree. */
	for (page = idb->root_page;;) {
		/* Copy the write generation value before the read. */
		write_gen = WT_PAGE_WRITE_GEN(page);

		hdr = page->hdr;
		isleaf =
		    hdr->type == WT_PAGE_DUP_LEAF ||
		    hdr->type == WT_PAGE_ROW_LEAF;
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			rip = page->u.irow + indx;
			if (WT_KEY_PROCESS(rip))
				WT_ERR(__wt_bt_key_build(toc, page, rip));

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 *
			 * For the record, we still maintain the key at the 0th
			 * location because it means tree verification and other
			 * code that processes a level of the tree doesn't need
			 * to know about this hack.
			 */
			if (indx != 0 || isleaf) {
				cmp = db->btree_compare(db, key, (DBT *)rip);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}

		/*
		 * Reference the slot used for next step down the tree.  We do
		 * this on leaf pages too, because it's simpler to code, and we
		 * only care if there's an exact match on leaf pages; setting
		 * rip doesn't matter for leaf pages because we always return
		 * WT_NOTFOUND if there's no match.
		 *
		 * Base is the smallest index greater than key and may be the
		 * 0th index or the (last + 1) indx.  If base is not the 0th
		 * index (remember, the 0th index always sorts less than any
		 * application key), decrement it to the smallest index less
		 * than or equal to key.
		 */
		if (cmp != 0)
			rip = page->u.irow + (base == 0 ? 0 : base - 1);

		/*
		 * If we've reached the leaf page, or we've reached the level
		 * requested by our caller, we're done.
		 */
		if (isleaf || level == hdr->level)
			break;

		/*
		 * rip references the subtree containing the record; check for
		 * an update.
		 */
		if ((repl = WT_ROW_REPL(page, rip)) != NULL) {
			addr = ((WT_OFF *)WT_REPL_DATA(repl))->addr;
			size = ((WT_OFF *)WT_REPL_DATA(repl))->size;
		} else {
			addr = WT_ROW_OFF_ADDR(rip);
			size = WT_ROW_OFF_SIZE(rip);
		}

		/* Walk down to the next page. */
		if (page != idb->root_page)
			__wt_bt_page_out(toc, &page, 0);
		switch (ret = __wt_bt_page_in(toc, addr, size, 1, &page)) {
		case 0:
			break;
		case WT_RESTART:
			goto restart;
		default:
			return (ret);
		}
	}

	/*
	 * We've found the right on-page WT_ROW structure, but that's only the
	 * first step; the record may have been updated since reading the page
	 * into the cache.
	 */
	switch (hdr->type) {
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		/*
		 * If inserting a new entry, return the smallest key on the page
		 * less-than-or-equal-to the specified key.
		 */
		if (!LF_ISSET(WT_INSERT)) {
			if (cmp != 0) {			/* No match */
				ret = WT_NOTFOUND;
				goto err;
			}
							/* Deleted match. */
			if ((repl = WT_ROW_REPL(page, rip)) != NULL) {
				if (WT_REPL_DELETED_ISSET(repl)) {
					ret = WT_NOTFOUND;
					goto err;
				}
				toc->srch_repl = repl;
			}
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * When returning internal pages, set the item's WT_REPL slot
		 * if it exists, otherwise we're done.
		 */
		toc->srch_repl = WT_ROW_REPL(page, rip);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	toc->srch_page = page;
	toc->srch_ip = rip;
	toc->srch_write_gen = write_gen;
	return (0);

err:	if (page != idb->root_page)
		__wt_bt_page_out(toc, &page, 0);
	return (ret);
}

/*
 * __wt_bt_key_build --
 *	Instantiate an overflow or compressed key into a WT_ROW structure.
 */
static int
__wt_bt_key_build(WT_TOC *toc, WT_PAGE *page, WT_ROW *rip_arg)
{
	DBT *dbt, local_dbt;
	WT_ROW *rip;
	WT_ITEM *item;
	uint32_t i;

	WT_CLEAR(local_dbt);
	dbt = &local_dbt;

	item = rip_arg->key;
	WT_RET(__wt_bt_item_process(toc, item, NULL, dbt));

	/*
	 * Update the WT_ROW reference with the processed key.  If there are
	 * any duplicates of this item, update them as well.
	 */
	WT_KEY_SET(rip_arg, dbt->data, dbt->size);
	if (WT_ITEM_TYPE(rip_arg->data) == WT_ITEM_DATA_DUP ||
	    WT_ITEM_TYPE(rip_arg->data) == WT_ITEM_DATA_DUP_OVFL) {
		WT_INDX_FOREACH(page, rip, i)
			if (rip->key == item)
				WT_KEY_SET(rip, dbt->data, dbt->size);
	}

	return (0);
}
