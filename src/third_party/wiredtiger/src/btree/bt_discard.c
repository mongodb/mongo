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
 * __wt_ref_out --
 *	Discard an in-memory page, freeing all memory associated with it.
 */
void
__wt_ref_out(WT_SESSION_IMPL *session, WT_REF *ref)
{
	/*
	 * A version of the page-out function that allows us to make additional
	 * diagnostic checks.
	 */
	WT_ASSERT(session, S2BT(session)->evict_ref != ref);

	__wt_page_out(session, &ref->page);
}

/*
 * __wt_page_out --
 *	Discard an in-memory page, freeing all memory associated with it.
 */
void
__wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
	WT_PAGE *page;
	WT_PAGE_HEADER *dsk;
	WT_PAGE_MODIFY *mod;

	/*
	 * Kill our caller's reference, do our best to catch races.
	 */
	page = *pagep;
	*pagep = NULL;

	/*
	 * We should never discard a dirty page, the file's current eviction
	 * point or a page queued for LRU eviction.
	 */
	WT_ASSERT(session, !__wt_page_is_modified(page));
	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU));
	WT_ASSERT(session, !F_ISSET_ATOMIC(page, WT_PAGE_SPLITTING));

#ifdef HAVE_DIAGNOSTIC
	{
	WT_HAZARD *hp;
	int i;
	/*
	 * Make sure no other thread has a hazard pointer on the page we are
	 * about to discard.  This is complicated by the fact that readers
	 * publish their hazard pointer before re-checking the page state, so
	 * our check can race with readers without indicating a real problem.
	 * Wait for up to a second for hazard pointers to be cleared.
	 */
	for (hp = NULL, i = 0; i < 100; i++) {
		if ((hp = __wt_page_hazard_check(session, page)) == NULL)
			break;
		__wt_sleep(0, 10000);
	}
	if (hp != NULL)
		__wt_errx(session,
		    "discarded page has hazard pointer: (%p: %s, line %d)",
		    hp->page, hp->file, hp->line);
	WT_ASSERT(session, hp == NULL);
	}
#endif

	/*
	 * If a root page split, there may be one or more pages linked from the
	 * page; walk the list, discarding pages.
	 */
	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		mod = page->modify;
		if (mod != NULL && mod->mod_root_split != NULL)
			__wt_page_out(session, &mod->mod_root_split);
		break;
	}

	/* Update the cache's information. */
	__wt_cache_page_evict(session, page);

	/*
	 * If discarding the page as part of process exit, the application may
	 * configure to leak the memory rather than do the work.
	 */
	if (F_ISSET(S2C(session), WT_CONN_LEAK_MEMORY))
		return;

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
	dsk = (WT_PAGE_HEADER *)page->dsk;
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_ALLOC))
		__wt_overwrite_and_free_len(session, dsk, dsk->mem_size);
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_MAPPED))
		(void)__wt_mmap_discard(session, dsk, dsk->mem_size);

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
	WT_MULTI *multi;
	WT_PAGE_MODIFY *mod;
	uint32_t i;

	mod = page->modify;

	switch (F_ISSET(mod, WT_PM_REC_MASK)) {
	case WT_PM_REC_MULTIBLOCK:
		/* Free list of replacement blocks. */
		for (multi = mod->mod_multi,
		    i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
			switch (page->type) {
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				__wt_free(session, multi->key.ikey);
				break;
			}
			__wt_free(session, multi->skip);
			__wt_free(session, multi->skip_dsk);
			__wt_free(session, multi->addr.addr);
		}
		__wt_free(session, mod->mod_multi);
		break;
	case WT_PM_REC_REPLACE:
		/*
		 * Discard any replacement address: this memory is usually moved
		 * into the parent's WT_REF, but at the root that can't happen.
		 */
		__wt_free(session, mod->mod_replace.addr);
		break;
	}

	switch (page->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		/* Free the append array. */
		if ((append = WT_COL_APPEND(page)) != NULL) {
			__free_skip_list(session, WT_SKIP_FIRST(append));
			__wt_free(session, append);
			__wt_free(session, mod->mod_append);
		}

		/* Free the insert/update array. */
		if (mod->mod_update != NULL)
			__free_skip_array(session, mod->mod_update,
			    page->type ==
			    WT_PAGE_COL_FIX ? 1 : page->pg_var_entries);
		break;
	}

	/* Free the overflow on-page, reuse and transaction-cache skiplists. */
	__wt_ovfl_reuse_free(session, page);
	__wt_ovfl_txnc_free(session, page);
	__wt_ovfl_discard_free(session, page);

	__wt_free(session, page->modify->ovfl_track);

	__wt_free(session, page->modify);
}

/*
 * __free_page_int --
 *	Discard a WT_PAGE_COL_INT or WT_PAGE_ROW_INT page.
 */
static void
__free_page_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_free_ref_index(session, page, WT_INTL_INDEX_COPY(page), 0);
}

/*
 * __wt_free_ref --
 *	Discard the contents of a WT_REF structure (optionally including the
 * pages it references).
 */
void
__wt_free_ref(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref, int free_pages)
{
	WT_IKEY *ikey;

	if (ref == NULL)
		return;

	/*
	 * Optionally free the referenced pages.  (The path to free referenced
	 * page is used for error cleanup, no instantiated and then discarded
	 * page should have WT_REF entries with real pages.  The page may have
	 * been marked dirty as well; page discard checks for that, so we mark
	 * it clean explicitly.)
	 */
	if (free_pages && ref->page != NULL) {
		if (ref->page->modify != NULL) {
			ref->page->modify->write_gen = 0;
			__wt_cache_dirty_decr(session, ref->page);
		}
		__wt_page_out(session, &ref->page);
	}

	/* Free any key allocation. */
	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if ((ikey = __wt_ref_key_instantiated(ref)) != NULL)
			__wt_free(session, ikey);
		break;
	}

	/* Free any address allocation. */
	if (ref->addr != NULL && __wt_off_page(page, ref->addr)) {
		__wt_free(session, ((WT_ADDR *)ref->addr)->addr);
		__wt_free(session, ref->addr);
	}

	/* Free any page-deleted information. */
	if (ref->page_del != NULL) {
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	__wt_overwrite_and_free(session, ref);
}

/*
 * __wt_free_ref_index --
 *	Discard a page index and it's references.
 */
void
__wt_free_ref_index(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_PAGE_INDEX *pindex, int free_pages)
{
	uint32_t i;

	if (pindex == NULL)
		return;

	for (i = 0; i < pindex->entries; ++i)
		__wt_free_ref(session, page, pindex->index[i], free_pages);
	__wt_free(session, pindex);
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
	void *copy;

	/*
	 * Free the in-memory index array.
	 *
	 * For each entry, see if the key was an allocation (that is, if it
	 * points somewhere other than the original page), and if so, free
	 * the memory.
	 */
	WT_ROW_FOREACH(page, rip, i) {
		copy = WT_ROW_KEY_COPY(rip);
		(void)__wt_row_leaf_key_info(
		    page, copy, &ikey, NULL, NULL, NULL);
		if (ikey != NULL)
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
		    F_ISSET(session, WT_SESSION_DISCARD_FORCE) ||
		    upd->txnid == WT_TXN_ABORTED ||
		    __wt_txn_visible_all(session, upd->txnid));

		next = upd->next;
		__wt_free(session, upd);
	}
}
