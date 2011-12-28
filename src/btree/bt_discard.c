/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __free_insert(WT_SESSION_IMPL *, WT_INSERT_HEAD **, uint32_t);
static void __free_insert_list(WT_SESSION_IMPL *, WT_INSERT *);
static void __free_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_update(WT_SESSION_IMPL *, WT_UPDATE **, uint32_t);
static void __free_update_list(WT_SESSION_IMPL *, WT_UPDATE *);

/*
 * __wt_page_out --
 *	Discard an in-memory page, freeing all memory associated with it.
 */
void
__wt_page_out(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	/*
	 * When a page is discarded, it's been disconnected from its parent and
	 * parent's WT_COL_REF/WT_ROW_REF structure may now point to a different
	 * page.   Make sure we don't use any of that information by accident.
	 */
	page->parent = NULL;
	page->parent_ref.ref = NULL;

	/* If not a split merged into its parent, the page must be clean. */
	WT_ASSERT(session,
	    !__wt_page_is_modified(page) ||
	    F_ISSET(page, WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE) != 0);

#ifdef HAVE_DIAGNOSTIC
	__wt_hazard_validate(session, page);
#endif

	/*
	 * If this page has a memory footprint associated with it, update
	 * the cache information.
	 */
	if (page->memory_footprint != 0)
		__wt_cache_page_evict(session, page);

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		__free_page_col_fix(session, page);
		break;
	case WT_PAGE_COL_INT:
		__free_page_col_int(session, page);
		break;
	case WT_PAGE_COL_VAR:
		__free_page_col_var(session, page);
		break;
	case WT_PAGE_ROW_INT:
		__free_page_row_int(session, page);
		break;
	case WT_PAGE_ROW_LEAF:
		__free_page_row_leaf(session, page);
		break;
	}

	if (!LF_ISSET(WT_PAGE_FREE_IGNORE_DISK))	/* Disk image */
		__wt_free(session, page->dsk);

	if (page->modify != NULL) {			/* WT_PAGE_MODIFY */
		__wt_free(session, page->modify->track);
		__wt_free(session, page->modify);
	}

	__wt_free(session, page);
}

/*
 * __free_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__free_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT_HEAD *ins_head;

	btree = session->btree;

	/* Free the append array. */
	if ((ins_head = WT_COL_APPEND(btree, page)) != NULL) {
		__free_insert_list(session, WT_SKIP_FIRST(ins_head));
		__wt_free(session, ins_head);
		__wt_free(session, btree->append);
	}

	/* Free the update array. */
	if (page->modify != NULL && page->modify->update != NULL)
		__free_insert(session, page->modify->update, 1);
}

/*
 * __free_page_col_int --
 *	Discard a WT_PAGE_COL_INT page.
 */
static void
__free_page_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL_REF *cref;
	uint32_t i;

	/*
	 * For each referenced addr, see if the addr was an allocation, and if
	 * so, free it.
	 */
	WT_COL_REF_FOREACH(page, cref, i)
		if (cref->ref.addr != NULL &&
		    __wt_off_page(page, cref->ref.addr)) {
			__wt_free(session, ((WT_ADDR *)cref->ref.addr)->addr);
			__wt_free(session, cref->ref.addr);
		}

	/* Free the subtree-reference array. */
	__wt_free(session, page->u.col_int.t);
}

/*
 * __free_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__free_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT_HEAD *ins_head;

	btree = session->btree;

	/* Free the in-memory index array. */
	__wt_free(session, page->u.col_var.d);

	/* Free the RLE lookup array. */
	__wt_free(session, page->u.col_var.repeats);

	/* Free the append array. */
	if ((ins_head = WT_COL_APPEND(btree, page)) != NULL) {
		__free_insert_list(session, WT_SKIP_FIRST(ins_head));
		__wt_free(session, ins_head);
		__wt_free(session, btree->append);
	}

	/* Free the insert array. */
	if (page->modify != NULL && page->modify->update != NULL)
		__free_insert(session, page->modify->update, page->entries);
}

/*
 * __free_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__free_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_ROW_REF *rref;
	uint32_t i;

	/*
	 * Free any allocated keys.
	 *
	 * For each referenced addr, see if the addr was an allocation, and if
	 * so, free it.
	 */
	WT_ROW_REF_FOREACH(page, rref, i) {
		if ((ikey = rref->key) != NULL)
			__wt_sb_free(session, ikey->sb, ikey);
		if (rref->ref.addr != NULL &&
		    __wt_off_page(page, rref->ref.addr)) {
			__wt_free(session, ((WT_ADDR *)rref->ref.addr)->addr);
			__wt_free(session, rref->ref.addr);
		}
	}

	/* Free the subtree-reference array. */
	__wt_free(session, page->u.row_int.t);
}

/*
 * __free_page_row_leaf --
 *	Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__free_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_ROW *rip;
	uint32_t i;

	/*
	 * Free the in-memory index array.
	 *
	 * For each entry, see if the key was an allocation (that is, if it
	 * points somewhere other than the original page), and if so, free
	 * the memory.
	 */
	WT_ROW_FOREACH(page, rip, i)
		if ((ikey = rip->key) != NULL && __wt_off_page(page, ikey))
			__wt_sb_free(session, ikey->sb, ikey);
	__wt_free(session, page->u.row_leaf.d);

	/*
	 * Free the insert array.
	 *
	 * Row-store tables have one additional slot in the insert array (the
	 * insert array has an extra slot to hold keys that sort before keys
	 * found on the original page).
	 */
	if (page->u.row_leaf.ins != NULL)
		__free_insert(
		    session, page->u.row_leaf.ins, page->entries + 1);

	/* Free the update array. */
	if (page->u.row_leaf.upd != NULL)
		__free_update(
		    session, page->u.row_leaf.upd, page->entries);
}

/*
 * __free_insert --
 *	Discard the insert array.
 */
static void
__free_insert(
    WT_SESSION_IMPL *session, WT_INSERT_HEAD **insert_head, uint32_t entries)
{
	WT_INSERT_HEAD **insheadp;

	/*
	 * For each non-NULL slot in the page's array of inserts, free the
	 * linked list anchored in that slot.
	 */
	for (insheadp = insert_head; entries > 0; --entries, ++insheadp)
		if (*insheadp != NULL) {
			__free_insert_list(session, WT_SKIP_FIRST(*insheadp));
			__wt_free(session, *insheadp);
		}

	/* Free the page's array of inserts. */
	__wt_free(session, insert_head);
}

/*
 * __free_insert_list --
 *	Walk a WT_INSERT forward-linked list and free the per-thread combination
 * of a WT_INSERT structure and its associated chain of WT_UPDATE structures.
 */
static void
__free_insert_list(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	WT_INSERT *next;

	do {
		__free_update_list(session, ins->upd);

		next = WT_SKIP_NEXT(ins);
		__wt_sb_free(session, ins->sb, ins);
	} while ((ins = next) != NULL);
}

/*
 * __free_update --
 *	Discard the update array.
 */
static void
__free_update(
    WT_SESSION_IMPL *session, WT_UPDATE **update_head, uint32_t entries)
{
	WT_UPDATE **updp;

	/*
	 * For each non-NULL slot in the page's array of updates, free the
	 * linked list anchored in that slot.
	 */
	for (updp = update_head; entries > 0; --entries, ++updp)
		if (*updp != NULL)
			__free_update_list(session, *updp);

	/* Free the page's array of updates. */
	__wt_free(session, update_head);
}

/*
 * __free_update_list --
 *	Walk a WT_UPDATE forward-linked list and free the per-thread combination
 *	of a WT_UPDATE structure and its associated data.
 */
static void
__free_update_list(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	do {
		next = upd->next;
		__wt_sb_free(session, upd->sb, upd);
	} while ((upd = next) != NULL);
}
