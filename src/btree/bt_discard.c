/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __free_page_modify(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_int(WT_SESSION_IMPL *, WT_PAGE *);
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
__wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
	WT_PAGE *page;

	/*
	 * When a page is discarded, it's been disconnected from its parent and
	 * its parent's WT_REF structure may now point to a different page.
	 * Make sure we don't accidentally use the page itself or any other
	 * information.
	 */
	page = *pagep;
	*pagep = NULL;
	page->parent = NULL;

	/*
	 * We should never discard the file's current eviction point or a page
	 * queued for LRU eviction.
	 */
	WT_ASSERT(session, S2BT(session)->evict_page != page);
	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU));

#ifdef HAVE_DIAGNOSTIC
	{
	WT_HAZARD *hp;
	if ((hp = __wt_page_hazard_check(session, page)) != NULL)
		__wt_errx(session,
		    "discarded page has hazard pointer: (%p: %s, line %d)",
		    hp->page, hp->file, hp->line);
	WT_ASSERT(session, hp == NULL);
	}
#endif
	/* Update the cache's information. */
	__wt_cache_page_evict(session, page);

	/* Free the page modification information. */
	if (page->modify != NULL)
		__free_page_modify(session, page);

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		__free_page_int(session, page);
		break;
	case WT_PAGE_COL_VAR:
		__free_page_col_var(session, page);
		break;
	case WT_PAGE_ROW_LEAF:
		__free_page_row_leaf(session, page);
		break;
	}

	/* Discard any disk image. */
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_ALLOC))
		__wt_overwrite_and_free_len(
		    session, page->dsk, page->dsk->mem_size);
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_MAPPED))
		(void)__wt_mmap_discard(
		    session, page->dsk, page->dsk->mem_size);

	__wt_overwrite_and_free(session, page);
}

/*
 * __free_page_modify --
 *	Discard the page's associated modification structures.
 */
static void
__free_page_modify(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT_HEAD *append;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	mod = page->modify;

	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_SPLIT:
		/*
		 * If a root page split, there may be one or more pages linked
		 * from the page; walk the list, discarding pages.
		 */
		if (mod->root_split != NULL)
			__wt_page_out(session, &mod->root_split);
		break;
	case WT_PM_REC_REPLACE:
		/*
		 * Discard any replacement address: this memory is usually moved
		 * into the parent's WT_REF, but at the root that can't happen.
		 */
		__wt_free(session, mod->u.replace.addr);
		break;
	default:
		break;
	}

	/* Free any reconciliation-created list of replacement blocks. */
	if (mod->u.m.multi != NULL) {
		for (i = 0; i < mod->u.m.multi_entries; ++i) {
			switch (page->type) {
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				__wt_free(session, mod->u.m.multi[i].key.ikey);
				break;
			}

			__wt_free(session, mod->u.m.multi[i].skip);
			__wt_free(session, mod->u.m.multi[i].skip_dsk);

			__wt_free(session, mod->u.m.multi[i].addr.addr);
		}
		__wt_free(session, mod->u.m.multi);
	}

	/*
	 * Free any split chunks created when underlying pages split into this
	 * page.
	 */
	for (i = 0; i < mod->splits_entries; ++i) {
		__wt_free_ref_array(
		    session, page, mod->splits[i].refs, mod->splits[i].entries);
		__wt_free(session, mod->splits[i].refs);
	}
	__wt_free(session, mod->splits);

	/* Free the append array. */
	if ((append = WT_COL_APPEND(page)) != NULL) {
		__free_skip_list(session, WT_SKIP_FIRST(append));
		__wt_free(session, append);
		__wt_free(session, mod->append);
	}

	/* Free the insert/update array. */
	if (mod->update != NULL)
		__free_skip_array(session, mod->update,
		    page->type == WT_PAGE_COL_FIX ? 1 : page->pg_var_entries);

	/* Free the overflow on-page, reuse and transaction-cache skiplists. */
	__wt_ovfl_onpage_discard(session, page);
	__wt_ovfl_reuse_discard(session, page);
	__wt_ovfl_txnc_discard(session, page);

	__wt_free(session, page->modify->ovfl_track);

	__wt_free(session, page->modify);
}

/*
 * __free_page_int --
 *	Discard a WT_PAGE_ROW_INT page.
 */
static void
__free_page_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Free any memory allocated for the original set of WT_REFs. */
	__wt_free_ref_array(session,
	    page, page->pg_intl_orig_index, page->pg_intl_orig_entries);

	/* Free any memory allocated for the child index array. */
	__wt_free(session, page->pg_intl_index);
}

/*
 * __free_ref --
 *	Discard the contents of a WT_REF structure.
 */
static void
__free_ref(WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref)
{
	WT_IKEY *ikey;

	/* Free any key allocation. */
	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if ((ikey = __wt_ref_key_instantiated(ref)) != NULL)
			__wt_free(session, ikey);
		break;
	}

	/* Free any address allocation. */
	if (ref->addr != NULL &&
	    (page == NULL || __wt_off_page(page, ref->addr))) {
		__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
		__wt_free(session, ref->addr);
	}

#ifdef HAVE_DIAGNOSTIC
	/*
	 * Races sometimes appear as accessing a WT_REF structure that has
	 * been discarded; make sure nobody uses this information again.
	 */
	memset(ref, WT_DEBUG_BYTE, sizeof(*ref));
#endif
}

/*
 * __wt_free_ref_array --
 *	Discard an array of WT_REFs.
 */
void
__wt_free_ref_array(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, uint32_t entries)
{
	uint32_t i;

	if (ref == NULL)
		return;
	for (i = 0; i < entries; ++i, ++ref)
		__free_ref(session, page, ref);
}

/*
 * __free_page_col_var --
 *	Discard a WT_PAGE_COL_VAR page.
 */
static void
__free_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Free the RLE lookup array. */
	__wt_free(session, page->pg_var_repeats);
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
	WT_ROW_FOREACH(page, rip, i) {
		ikey = WT_ROW_KEY_COPY(rip);
		if (ikey != NULL && __wt_off_page(page, ikey))
			__wt_free(session, ikey);
	}

	/*
	 * Free the insert array.
	 *
	 * Row-store tables have one additional slot in the insert array (the
	 * insert array has an extra slot to hold keys that sort before keys
	 * found on the original page).
	 */
	if (page->pg_row_ins != NULL)
		__free_skip_array(
		    session, page->pg_row_ins, page->pg_row_entries + 1);

	/* Free the update array. */
	if (page->pg_row_upd != NULL)
		__free_update(session, page->pg_row_upd, page->pg_row_entries);
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

	/* Free the header array. */
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

	for (; ins != NULL; ins = next) {
		__free_update_list(session, ins->upd);
		next = WT_SKIP_NEXT(ins);
		__wt_free(session, ins);
	}
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

	/* Free the update array. */
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

	for (; upd != NULL; upd = next) {
		/* Everything we free should be visible to everyone. */
		WT_ASSERT(session,
		    upd->txnid == WT_TXN_ABORTED ||
		    __wt_txn_visible_all(session, upd->txnid));

		next = upd->next;
		__wt_free(session, upd);
	}
}
