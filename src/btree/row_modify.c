/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_row_modify --
 *	Row-store insert, update and delete.
 */
int
__wt_row_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *ins_head, **ins_headp;
	WT_ITEM *key, *value;
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd, **upd_entry, *upd_obsolete;
	size_t ins_size, upd_size;
	uint32_t ins_slot;
	u_int skipdepth;
	int logged;

	key = &cbt->iface.key;
	value = is_remove ? NULL : &cbt->iface.value;

	page = cbt->page;

	/* If we don't yet have a modify structure, we'll need one. */
	WT_RET(__wt_page_modify_init(session, page));

	ins = NULL;
	upd = NULL;
	logged = 0;

	/*
	 * Modify: allocate an update array as necessary, build a WT_UPDATE
	 * structure, and call a serialized function to insert the WT_UPDATE
	 * structure.
	 *
	 * Insert: allocate an insert array as necessary, build a WT_INSERT
	 * and WT_UPDATE structure pair, and call a serialized function to
	 * insert the WT_INSERT structure.
	 */
	if (cbt->compare == 0) {
		if (cbt->ins == NULL) {
			/* Allocate an update array as necessary. */
			WT_PAGE_ALLOC_AND_SWAP(session, page,
			    page->u.row.upd, upd_entry, page->entries);

			/* Set the WT_UPDATE array reference. */
			upd_entry = &page->u.row.upd[cbt->slot];
		} else
			upd_entry = &cbt->ins->upd;

		/* Make sure the update can proceed. */
		WT_ERR(__wt_txn_update_check(session, old_upd = *upd_entry));

		/* Allocate the WT_UPDATE structure and transaction ID. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		WT_ERR(__wt_txn_modify(session, cbt, upd));
		logged = 1;

		/* Serialize the update. */
		WT_ERR(__wt_update_serial(session, page,
		    upd_entry, old_upd, &upd, upd_size, &upd_obsolete));

		/* Discard any obsolete WT_UPDATE structures. */
		if (upd_obsolete != NULL)
			__wt_update_obsolete_free(session, page, upd_obsolete);
	} else {
		/*
		 * Allocate the insert array as necessary.
		 *
		 * We allocate an additional insert array slot for insert keys
		 * sorting less than any key on the page.  The test to select
		 * that slot is baroque: if the search returned the first page
		 * slot, we didn't end up processing an insert list, and the
		 * comparison value indicates the search key was smaller than
		 * the returned slot, then we're using the smallest-key insert
		 * slot.  That's hard, so we set a flag.
		 */
		WT_PAGE_ALLOC_AND_SWAP(session, page,
		    page->u.row.ins, ins_headp, page->entries + 1);

		ins_slot = F_ISSET(cbt, WT_CBT_SEARCH_SMALLEST) ?
		    page->entries : cbt->slot;
		ins_headp = &page->u.row.ins[ins_slot];

		/* Allocate the WT_INSERT_HEAD structure as necessary. */
		WT_PAGE_ALLOC_AND_SWAP(session, page, *ins_headp, ins_head, 1);
		ins_head = *ins_headp;

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and
		 * update the cursor to reference it.
		 */
		WT_ERR(__wt_row_insert_alloc(
		    session, key, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		ins->upd = upd;
		ins_size += upd_size;

		/*
		 * Update the cursor: the insert head may have been allocated,
		 * the ins field was allocated.
		 */
		cbt->ins_head = ins_head;
		cbt->ins = ins;
		WT_ERR(__wt_txn_modify(session, cbt, upd));
		logged = 1;

		/* Insert the WT_INSERT structure. */
		WT_ERR(__wt_insert_serial(session, page,
		    cbt->ins_head, cbt->ins_stack, cbt->next_stack,
		    &ins, ins_size, skipdepth));
	}

	if (0) {
err:		/*
		 * Remove the update from the current transaction, so we don't
		 * try to modify it on rollback.
		 */
		if (logged)
			__wt_txn_unmodify(session);
		__wt_free(session, ins);
		cbt->ins = NULL;
		__wt_free(session, upd);
	}

	return (ret);
}

/*
 * __wt_row_insert_alloc --
 *	Row-store insert: allocate a WT_INSERT structure and fill it in.
 */
int
__wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key, u_int skipdepth, WT_INSERT **insp, size_t *ins_sizep)
{
	WT_INSERT *ins;
	size_t ins_size;

	/*
	 * Allocate the WT_INSERT structure, next pointers for the skip list,
	 * and room for the key.  Then copy the key into place.
	 */
	ins_size = sizeof(WT_INSERT) +
	    skipdepth * sizeof(WT_INSERT *) + key->size;
	WT_RET(__wt_calloc(session, 1, ins_size, &ins));

	ins->u.key.offset = WT_STORE_SIZE(ins_size - key->size);
	WT_INSERT_KEY_SIZE(ins) = key->size;
	memcpy(WT_INSERT_KEY(ins), key->data, key->size);

	*insp = ins;
	if (ins_sizep != NULL)
		*ins_sizep = ins_size;
	return (0);
}

/*
 * __wt_insert_serial_func --
 *	Server function to add an WT_INSERT entry to the page.
 */
int
__wt_insert_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_INSERT *new_ins, ***ins_stack, **next_stack;
	WT_INSERT_HEAD *ins_head;
	WT_PAGE *page;
	u_int i, skipdepth;

	__wt_insert_unpack(args, &page,
	    &ins_head, &ins_stack, &next_stack, &new_ins, &skipdepth);

	/*
	 * Largely ignore the page's write-generation, just confirm it hasn't
	 * wrapped.
	 */
	WT_RET(__wt_page_write_gen_wrapped_check(page));

	/*
	 * If an empty WT_INSERT_HEAD, the cursor's information cannot be
	 * correct, search could not have initialized it.
	 */
	if (WT_SKIP_FIRST(ins_head) == NULL)
		for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
			ins_stack[i] = &ins_head->head[i];
			next_stack[i] = NULL;
		}
	else
		/*
		 * Confirm we are still in the expected position, and no item
		 * has been added where our insert belongs.  Take extra care
		 * at the beginning and end of the list (at each level): retry
		 * if we race there.
		 *
		 * !!!
		 * Note the test for ins_stack[0] == NULL: that's the test for
		 * an uninitialized cursor, ins_stack[0] is cleared as part of
		 * initializing a cursor for a search.
		 */
		for (i = 0; i < skipdepth; i++) {
			if (ins_stack[i] == NULL ||
			    *ins_stack[i] != next_stack[i])
				return (WT_RESTART);
			if (next_stack[i] == NULL &&
			    ins_head->tail[i] != NULL &&
			    ins_stack[i] != &ins_head->tail[i]->next[i])
				return (WT_RESTART);
		}

	/*
	 * Publish: First, point the new WT_INSERT item's skiplist references
	 * to the next elements in the insert list, then flush memory.  Second,
	 * update the skiplist elements that reference the new WT_INSERT item,
	 * this ensures the list is never inconsistent.
	 */
	for (i = 0; i < skipdepth; i++)
		new_ins->next[i] = *ins_stack[i];
	WT_WRITE_BARRIER();
	for (i = 0; i < skipdepth; i++) {
		if (ins_head->tail[i] == NULL ||
		    ins_stack[i] == &ins_head->tail[i]->next[i])
			ins_head->tail[i] = new_ins;
		*ins_stack[i] = new_ins;
	}

	__wt_insert_new_ins_taken(args);

	__wt_page_and_tree_modify_set(session, page);
	return (0);
}

/*
 * __wt_update_alloc --
 *	Allocate a WT_UPDATE structure and associated value and fill it in.
 */
int
__wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value, WT_UPDATE **updp, size_t *sizep)
{
	WT_UPDATE *upd;
	size_t size;

	/*
	 * Allocate the WT_UPDATE structure and room for the value, then copy
	 * the value into place.
	 */
	size = value == NULL ? 0 : value->size;
	WT_RET(__wt_calloc(session, 1, sizeof(WT_UPDATE) + size, &upd));
	if (value == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = WT_STORE_SIZE(size);
		memcpy(WT_UPDATE_DATA(upd), value->data, size);
	}

	*updp = upd;
	*sizep = sizeof(WT_UPDATE) + size;
	return (0);
}

/*
 * __wt_update_obsolete_check --
 *	Check for obsolete updates.
 */
WT_UPDATE *
__wt_update_obsolete_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	/*
	 * This function identifies obsolete updates, and truncates them from
	 * the rest of the chain; because this routine is called from inside
	 * a serialization function, the caller has responsibility for actually
	 * freeing the memory.
	 *
	 * Walk the list of updates, looking for obsolete updates.  If we find
	 * an update no session will ever move past, we can discard any updates
	 * that appear after it.
	 */
	for (; upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid)) {
			/*
			 * We cannot discard this WT_UPDATE structure, we can
			 * only discard WT_UPDATE structures subsequent to it,
			 * other threads of control will terminate their walk
			 * in this element.  Save a reference to the list we
			 * will discard, and terminate the list.
			 */
			if ((next = upd->next) == NULL)
				return (NULL);
			if (!WT_ATOMIC_CAS(upd->next, next, NULL))
				return (NULL);

			return (next);
		}
	return (NULL);
}

/*
 * __wt_update_obsolete_free --
 *	Free an obsolete update list.
 */
void
__wt_update_obsolete_free(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd)
{
	WT_UPDATE *next;
	size_t size;

	/* Free a WT_UPDATE list. */
	for (size = 0; upd != NULL; upd = next) {
		/* Deleted items have a dummy size: don't include that. */
		size += sizeof(WT_UPDATE) +
		    (WT_UPDATE_DELETED_ISSET(upd) ? 0 : upd->size);

		next = upd->next;
		__wt_free(session, upd);
	}
	if (size != 0)
		__wt_cache_page_inmem_decr(session, page, size);
}

/*
 * __wt_page_obsolete --
 *	Discard all obsolete updates on a row-store leaf page.
 */
void
__wt_row_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/* For entries before the first on-page record... */
	WT_SKIP_FOREACH(ins, WT_ROW_INSERT_SMALLEST(page))
		if ((upd =
		    __wt_update_obsolete_check(session, ins->upd)) != NULL)
			__wt_update_obsolete_free(session, page, upd);

	/* For each entry on the page... */
	WT_ROW_FOREACH(page, rip, i) {
		if ((upd = __wt_update_obsolete_check(
		    session, WT_ROW_UPDATE(page, rip))) != NULL)
			__wt_update_obsolete_free(session, page, upd);

		WT_SKIP_FOREACH(ins, WT_ROW_INSERT(page, rip))
			if ((upd = __wt_update_obsolete_check(
			    session, ins->upd)) != NULL)
				__wt_update_obsolete_free(session, page, upd);
	}
}

/*
 * __wt_update_serial_func --
 *	Server function to add an WT_UPDATE entry in the page array.
 */
int
__wt_update_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd, **upd_entry, **upd_obsolete;

	__wt_update_unpack(
	    args, &page, &upd_entry, &old_upd, &upd, &upd_obsolete);

	/*
	 * Ignore the page's write-generation (other than the special case of
	 * it wrapping).  If we're still in the expected position, we're good
	 * to go and no update has been added where ours belongs.  If a new
	 * update has been added, check if our update is still permitted.
	 */
	WT_RET(__wt_page_write_gen_wrapped_check(page));
	if (old_upd != *upd_entry)
		WT_RET(__wt_txn_update_check(session, *upd_entry));

	upd->next = *upd_entry;
	/*
	 * Publish: there must be a barrier to ensure the new entry's next
	 * pointer is set before we update the linked list.
	 */
	WT_PUBLISH(*upd_entry, upd);
	__wt_update_upd_taken(args);

	/* Discard obsolete WT_UPDATE structures. */
	*upd_obsolete = __wt_update_obsolete_check(session, upd->next);

	__wt_page_and_tree_modify_set(session, page);
	return (0);
}
