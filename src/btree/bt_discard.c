/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __free_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_skip_array(WT_SESSION_IMPL *, WT_INSERT_HEAD **, uint32_t);
static void __free_skip_list(WT_SESSION_IMPL *, WT_INSERT *);
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
	 * parent's WT_REF structure may now point to a different page.   Make
	 * sure we don't use any of that information by accident.
	 */
	page->parent = NULL;
	page->ref = NULL;

	WT_ASSERT(session, !F_ISSET(page, WT_PAGE_EVICT_LRU));

	/* If not a split merged into its parent, the page must be clean. */
	WT_ASSERT(session,
	    !__wt_page_is_modified(page) ||
	    F_ISSET(page, WT_PAGE_REC_SPLIT_MERGE));

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

#ifdef HAVE_DIAGNOSTIC
	memset(page, WT_DEBUG_BYTE, sizeof(WT_PAGE));
#endif
	__wt_free(session, page);
}

/*
 * __free_page_col_fix --
 *	Discard a WT_PAGE_COL_FIX page.
 */
static void
__free_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT_HEAD *append;

	/* Free the append array. */
	if ((append = WT_COL_APPEND(page)) != NULL) {
		__free_skip_list(session, WT_SKIP_FIRST(append));
		__wt_free(session, append);
		__wt_free(session, page->modify->append);
	}

	/* Free the update array. */
	if (page->modify != NULL && page->modify->update != NULL)
		__free_skip_array(session, page->modify->update, 1);
}

/*
 * __free_page_col_int --
 *	Discard a WT_PAGE_COL_INT page.
 */
static void
__free_page_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_REF *ref;
	uint32_t i;

	/*
	 * For each referenced addr, see if the addr was an allocation, and if
	 * so, free it.
	 */
	WT_REF_FOREACH(page, ref, i)
		if (ref->addr != NULL &&
		    __wt_off_page(page, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}

	/* Free the subtree-reference array. */
	__wt_free(session, page->u.intl.t);
}

/*
 * __free_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__free_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT_HEAD *append;

	/* Free the in-memory index array. */
	__wt_free(session, page->u.col_var.d);

	/* Free the RLE lookup array. */
	__wt_free(session, page->u.col_var.repeats);

	/* Free the append array. */
	if ((append = WT_COL_APPEND(page)) != NULL) {
		__free_skip_list(session, WT_SKIP_FIRST(append));
		__wt_free(session, append);
		__wt_free(session, page->modify->append);
	}

	/* Free the insert array. */
	if (page->modify != NULL && page->modify->update != NULL)
		__free_skip_array(session, page->modify->update, page->entries);
}

/*
 * __free_page_row_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__free_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_IKEY *ikey;
	WT_REF *ref;
	uint32_t i;

	/*
	 * Free any allocated keys.
	 *
	 * For each referenced addr, see if the addr was an allocation, and if
	 * so, free it.
	 */
	WT_REF_FOREACH(page, ref, i) {
		if ((ikey = ref->u.key) != NULL)
			__wt_free(session, ikey);
		if (ref->addr != NULL &&
		    __wt_off_page(page, ref->addr)) {
			__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
			__wt_free(session, ref->addr);
		}
	}

	/* Free the subtree-reference array. */
	__wt_free(session, page->u.intl.t);
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
			__wt_free(session, ikey);
	__wt_free(session, page->u.row.d);

	/*
	 * Free the insert array.
	 *
	 * Row-store tables have one additional slot in the insert array (the
	 * insert array has an extra slot to hold keys that sort before keys
	 * found on the original page).
	 */
	if (page->u.row.ins != NULL)
		__free_skip_array(session, page->u.row.ins, page->entries + 1);

	/* Free the update array. */
	if (page->u.row.upd != NULL)
		__free_update(session, page->u.row.upd, page->entries);
}

/*
 * __free_skip_array --
 *	Discard an array of skip list headers.
 */
static void
__free_skip_array(
    WT_SESSION_IMPL *session, WT_INSERT_HEAD **head_arg, uint32_t entries)
{
	WT_INSERT_HEAD **head;

	/*
	 * For each non-NULL slot in the page's array of inserts, free the
	 * linked list anchored in that slot.
	 */
	for (head = head_arg; entries > 0; --entries, ++head)
		if (*head != NULL) {
			__free_skip_list(session, WT_SKIP_FIRST(*head));
			__wt_free(session, *head);
		}

	/* Free the page's array of inserts. */
	__wt_free(session, head_arg);
}

/*
 * __free_skip_list --
 *	Walk a WT_INSERT forward-linked list and free the per-thread combination
 * of a WT_INSERT structure and its associated chain of WT_UPDATE structures.
 */
static void
__free_skip_list(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	WT_INSERT *next;

	do {
		__free_update_list(session, ins->upd);

		next = WT_SKIP_NEXT(ins);
		__wt_free(session, ins);
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
		__wt_free(session, upd);
	} while ((upd = next) != NULL);
}
