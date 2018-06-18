/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Fast-delete support.
 *
 * This file contains most of the code that allows WiredTiger to delete pages
 * of data without reading them into the cache.  (This feature is currently
 * only available for row-store objects.)
 *
 * The way cursor truncate works in a row-store object is it explicitly reads
 * the first and last pages of the truncate range, then walks the tree with a
 * flag so the tree walk code skips reading eligible pages within the range
 * and instead just marks them as deleted, by changing their WT_REF state to
 * WT_REF_DELETED. Pages ineligible for this fast path include pages already
 * in the cache, having overflow items, or requiring lookaside records.
 * Ineligible pages are read and have their rows updated/deleted individually.
 * The transaction for the delete operation is stored in memory referenced by
 * the WT_REF.page_del field.
 *
 * Future cursor walks of the tree will skip the deleted page based on the
 * transaction stored for the delete, but it gets more complicated if a read is
 * done using a random key, or a cursor walk is done with a transaction where
 * the delete is not visible.  In those cases, we read the original contents of
 * the page.  The page-read code notices a deleted page is being read, and as
 * part of the read instantiates the contents of the page, creating a WT_UPDATE
 * with a deleted operation, in the same transaction as deleted the page.  In
 * other words, the read process makes it appear as if the page was read and
 * each individual row deleted, exactly as would have happened if the page had
 * been in the cache all along.
 *
 * There's an additional complication to support rollback of the page delete.
 * When the page was marked deleted, a pointer to the WT_REF was saved in the
 * deleting session's transaction list and the delete is unrolled by resetting
 * the WT_REF_DELETED state back to WT_REF_DISK.  However, if the page has been
 * instantiated by some reading thread, that's not enough, each individual row
 * on the page must have the delete operation reset.  If the page split, the
 * WT_UPDATE lists might have been saved/restored during reconciliation and
 * appear on multiple pages, and the WT_REF stored in the deleting session's
 * transaction list is no longer useful.  For this reason, when the page is
 * instantiated by a read, a list of the WT_UPDATE structures on the page is
 * stored in the WT_REF.page_del field, with the transaction ID, that way the
 * session committing/unrolling the delete can find all WT_UPDATE structures
 * that require update.
 *
 * One final note: pages can also be marked deleted if emptied and evicted.  In
 * that case, the WT_REF state will be set to WT_REF_DELETED but there will not
 * be any associated WT_REF.page_del field.  These pages are always skipped
 * during cursor traversal (the page could not have been evicted if there were
 * updates that weren't globally visible), and if read is forced to instantiate
 * such a page, it simply creates an empty page from scratch.
 */

/*
 * __wt_delete_page --
 *	If deleting a range, try to delete the page without instantiating it.
 */
int
__wt_delete_page(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
	WT_ADDR *ref_addr;
	WT_DECL_RET;
	uint32_t previous_state;

	*skipp = false;

	/* If we have a clean page in memory, attempt to evict it. */
	previous_state = ref->state;
	if ((previous_state == WT_REF_MEM || previous_state == WT_REF_LIMBO) &&
	    __wt_atomic_casv32(&ref->state, previous_state, WT_REF_LOCKED)) {
		if (__wt_page_is_modified(ref->page)) {
			ref->state = previous_state;
			return (0);
		}

		(void)__wt_atomic_addv32(&S2BT(session)->evict_busy, 1);
		ret = __wt_evict(session, ref, false);
		(void)__wt_atomic_subv32(&S2BT(session)->evict_busy, 1);
		WT_RET_BUSY_OK(ret);
		ret = 0;
	}

	/*
	 * Fast check to see if it's worth locking, then atomically switch the
	 * page's state to lock it.
	 */
	previous_state = ref->state;
	switch (previous_state) {
	case WT_REF_DISK:
	case WT_REF_LOOKASIDE:
		break;
	default:
		return (0);
	}
	if (!__wt_atomic_casv32(&ref->state, previous_state, WT_REF_LOCKED))
		return (0);

	/*
	 * If this WT_REF was previously part of a truncate operation, there
	 * may be existing page-delete information. The structure is only read
	 * while the state is locked, free the previous version.
	 *
	 * Note: changes have been made, we must publish any state change from
	 * this point on.
	 */
	if (ref->page_del != NULL) {
		WT_ASSERT(session, ref->page_del->txnid == WT_TXN_ABORTED);
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	/*
	 * We cannot truncate pages that have overflow key/value items as the
	 * overflow blocks have to be discarded.  The way we figure that out is
	 * to check the page's cell type, cells for leaf pages without overflow
	 * items are special.
	 *
	 * To look at an on-page cell, we need to look at the parent page, and
	 * that's dangerous, our parent page could change without warning if
	 * the parent page were to split, deepening the tree. We can look at
	 * the parent page itself because the page can't change underneath us.
	 * However, if the parent page splits, our reference address can change;
	 * we don't care what version of it we read, as long as we don't read
	 * it twice.
	 */
	WT_ORDERED_READ(ref_addr, ref->addr);
	if (ref_addr != NULL &&
	    (__wt_off_page(ref->home, ref_addr) ?
	    ref_addr->type != WT_ADDR_LEAF_NO :
	    __wt_cell_type_raw((WT_CELL *)ref_addr) != WT_CELL_ADDR_LEAF_NO))
		goto err;

	/*
	 * This action dirties the parent page: mark it dirty now, there's no
	 * future reconciliation of the child leaf page that will dirty it as
	 * we write the tree.
	 */
	WT_ERR(__wt_page_parent_modify_set(session, ref, false));

	/* Allocate and initialize the page-deleted structure. */
	WT_ERR(__wt_calloc_one(session, &ref->page_del));
	ref->page_del->previous_state = previous_state;

	WT_ERR(__wt_txn_modify_page_delete(session, ref));

	*skipp = true;
	WT_STAT_CONN_INCR(session, rec_page_delete_fast);
	WT_STAT_DATA_INCR(session, rec_page_delete_fast);

	/* Publish the page to its new state, ensuring visibility. */
	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (0);

err:	__wt_free(session, ref->page_del);

	/* Publish the page to its previous state, ensuring visibility. */
	WT_PUBLISH(ref->state, previous_state);
	return (ret);
}

/*
 * __wt_delete_page_rollback --
 *	Abort pages that were deleted without being instantiated.
 */
int
__wt_delete_page_rollback(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_UPDATE **updp;
	uint64_t sleep_usecs, yield_count;
	uint32_t current_state;
	bool locked;

	/*
	 * If the page is still "deleted", it's as we left it, reset the state
	 * to on-disk and we're done.  Otherwise, we expect the page is either
	 * instantiated or being instantiated.  Loop because it's possible for
	 * the page to return to the deleted state if instantiation fails.
	 */
	for (locked = false, sleep_usecs = yield_count = 0;;) {
		switch (current_state = ref->state) {
		case WT_REF_DELETED:
			/*
			 * If the page is still "deleted", it's as we left it,
			 * reset the state.
			 */
			if (__wt_atomic_casv32(&ref->state,
			    WT_REF_DELETED, ref->page_del->previous_state))
				goto done;
			break;
		case WT_REF_LOCKED:
			/*
			 * A possible state, the page is being instantiated.
			 */
			break;
		case WT_REF_MEM:
		case WT_REF_SPLIT:
			if (__wt_atomic_casv32(
			    &ref->state, current_state, WT_REF_LOCKED))
				locked = true;
			break;
		case WT_REF_DISK:
		case WT_REF_LIMBO:
		case WT_REF_LOOKASIDE:
		case WT_REF_READING:
		default:
			return (__wt_illegal_value(session,
			    "illegal WT_REF.state rolling back deleted page"));
		}

		if (locked)
			break;

		/*
		 * We wait for the change in page state, yield before retrying,
		 * and if we've yielded enough times, start sleeping so we
		 * don't burn CPU to no purpose.
		 */
		__wt_spin_backoff(&yield_count, &sleep_usecs);
		WT_STAT_CONN_INCRV(session,
		    page_del_rollback_blocked, sleep_usecs);
	}

	/*
	 * We can't use the normal read path to get a copy of the page
	 * because the session may have closed the cursor, we no longer
	 * have the reference to the tree required for a hazard
	 * pointer.  We're safe because with unresolved transactions,
	 * the page isn't going anywhere.
	 *
	 * The page is in an in-memory state, which means it
	 * was instantiated at some point. Walk any list of
	 * update structures and abort them.
	 */
	WT_ASSERT(session, locked);
	if ((updp = ref->page_del->update_list) != NULL)
		for (; *updp != NULL; ++updp)
			(*updp)->txnid = WT_TXN_ABORTED;

	ref->state = current_state;

done:	/*
	 * Now mark the truncate aborted: this must come last because after
	 * this point there is nothing preventing the page from being evicted.
	 */
	WT_PUBLISH(ref->page_del->txnid, WT_TXN_ABORTED);
	return (0);
}

/*
 * __wt_delete_page_skip --
 *	If iterating a cursor, skip deleted pages that are either visible to
 * us or globally visible.
 */
bool
__wt_delete_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, bool visible_all)
{
	bool skip;

	/*
	 * Deleted pages come from two sources: either it's a truncate as
	 * described above, or the page has been emptied by other operations
	 * and eviction deleted it.
	 *
	 * In both cases, the WT_REF state will be WT_REF_DELETED.  In the case
	 * of a truncated page, there will be a WT_PAGE_DELETED structure with
	 * the transaction ID of the transaction that deleted the page, and the
	 * page is visible if that transaction ID is visible.  In the case of an
	 * empty page, there will be no WT_PAGE_DELETED structure and the delete
	 * is by definition visible, eviction could not have deleted the page if
	 * there were changes on it that were not globally visible.
	 *
	 * We're here because we found a WT_REF state set to WT_REF_DELETED.  It
	 * is possible the page is being read into memory right now, though, and
	 * the page could switch to an in-memory state at any time.  Lock down
	 * the structure, just to be safe.
	 */
	if (ref->page_del == NULL)
		return (true);

	if (!__wt_atomic_casv32(&ref->state, WT_REF_DELETED, WT_REF_LOCKED))
		return (false);

	skip = !__wt_page_del_active(session, ref, visible_all);

	/*
	 * The page_del structure can be freed as soon as the delete is stable:
	 * it is only read when the ref state is locked. It is worth checking
	 * every time we come through because once this is freed, we no longer
	 * need synchronization to check the ref.
	 */
	if (skip && ref->page_del != NULL && (visible_all ||
	    __wt_txn_visible_all(session, ref->page_del->txnid,
	    WT_TIMESTAMP_NULL(&ref->page_del->timestamp)))) {
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (skip);
}

/*
 * __tombstone_update_alloc --
 *	Allocate and initialize a page-deleted tombstone update structure.
 */
static int
__tombstone_update_alloc(WT_SESSION_IMPL *session,
    WT_PAGE_DELETED *page_del, WT_UPDATE **updp, size_t *sizep)
{
	WT_UPDATE *upd;

	WT_RET(
	    __wt_update_alloc(session, NULL, &upd, sizep, WT_UPDATE_TOMBSTONE));

	/*
	 * Cleared memory matches the lowest possible transaction ID and
	 * timestamp, do nothing.
	 */
	if (page_del != NULL) {
		upd->txnid = page_del->txnid;
		__wt_timestamp_set(&upd->timestamp, &page_del->timestamp);
		upd->prepare_state = page_del->prepare_state;
	}
	*updp = upd;
	return (0);
}

/*
 * __wt_delete_page_instantiate --
 *	Instantiate an entirely deleted row-store leaf page.
 */
int
__wt_delete_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *insert;
	WT_PAGE *page;
	WT_PAGE_DELETED *page_del;
	WT_ROW *rip;
	WT_UPDATE **upd_array, *upd;
	size_t size;
	uint32_t count, i;

	btree = S2BT(session);
	page = ref->page;

	WT_STAT_CONN_INCR(session, cache_read_deleted);
	WT_STAT_DATA_INCR(session, cache_read_deleted);

	/*
	 * Give the page a modify structure.
	 *
	 * If the tree is already dirty and so will be written, mark the page
	 * dirty.  (We'd like to free the deleted pages, but if the handle is
	 * read-only or if the application never modifies the tree, we're not
	 * able to do so.)
	 */
	WT_RET(__wt_page_modify_init(session, page));
	if (btree->modified)
		__wt_page_modify_set(session, page);

	if (ref->page_del != NULL &&
	    ref->page_del->prepare_state != WT_PREPARE_INIT) {
		WT_STAT_CONN_INCR(session, cache_read_deleted_prepared);
		WT_STAT_DATA_INCR(session, cache_read_deleted_prepared);
	}

	/*
	 * An operation is accessing a "deleted" page, and we're building an
	 * in-memory version of the page (making it look like all entries in
	 * the page were individually updated by a remove operation).  There
	 * are two cases where we end up here:
	 *
	 * First, a running transaction used a truncate call to delete the page
	 * without reading it, in which case the page reference includes a
	 * structure with a transaction ID; the page we're building might split
	 * in the future, so we update that structure to include references to
	 * all of the update structures we create, so the transaction can abort.
	 *
	 * Second, a truncate call deleted a page and the truncate committed,
	 * but an older transaction in the system forced us to keep the old
	 * version of the page around, then we crashed and recovered or we're
	 * running inside a checkpoint, and now we're being forced to read that
	 * page.
	 *
	 * Expect a page-deleted structure if there's a running transaction that
	 * needs to be resolved, otherwise, there may not be one (and, if the
	 * transaction has resolved, we can ignore the page-deleted structure).
	 */
	page_del = __wt_page_del_active(session, ref, true) ?
	    ref->page_del : NULL;

	/*
	 * Allocate the per-page update array if one doesn't already exist. (It
	 * might already exist because deletes are instantiated after lookaside
	 * table updates.)
	 */
	if (page->entries != 0 && page->modify->mod_row_update == NULL)
		WT_RET(__wt_calloc_def(
		    session, page->entries, &page->modify->mod_row_update));

	/*
	 * Allocate the per-reference update array; in the case of instantiating
	 * a page deleted in a running transaction, we need a list of the update
	 * structures for the eventual commit or abort.
	 */
	if (page_del != NULL) {
		count = 0;
		if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
			WT_SKIP_FOREACH(ins, insert)
				++count;
		WT_ROW_FOREACH(page, rip, i) {
			++count;
			if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
				WT_SKIP_FOREACH(ins, insert)
					++count;
		}
		WT_RET(__wt_calloc_def(
		    session, count + 1, &page_del->update_list));
	}

	/* Walk the page entries, giving each one a tombstone. */
	size = 0;
	count = 0;
	upd_array = page->modify->mod_row_update;
	if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		WT_SKIP_FOREACH(ins, insert) {
			WT_ERR(__tombstone_update_alloc(
			    session, page_del, &upd, &size));
			upd->next = ins->upd;
			ins->upd = upd;

			if (page_del != NULL)
				page_del->update_list[count++] = upd;
		}
	WT_ROW_FOREACH(page, rip, i) {
		WT_ERR(__tombstone_update_alloc(
		    session, page_del, &upd, &size));
		upd->next = upd_array[WT_ROW_SLOT(page, rip)];
		upd_array[WT_ROW_SLOT(page, rip)] = upd;

		if (page_del != NULL)
			page_del->update_list[count++] = upd;

		if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
			WT_SKIP_FOREACH(ins, insert) {
				WT_ERR(__tombstone_update_alloc(
				    session, page_del, &upd, &size));
				upd->next = ins->upd;
				ins->upd = upd;

				if (page_del != NULL)
					page_del->update_list[count++] = upd;
			}
	}

	__wt_cache_page_inmem_incr(session, page, size);

	return (0);

err:	/*
	 * The page-delete update structure may have existed before we were
	 * called, and presumably might be in use by a running transaction.
	 * The list of update structures cannot have been created before we
	 * were called, and should not exist if we exit with an error.
	 */
	if (page_del != NULL)
		__wt_free(session, page_del->update_list);
	return (ret);
}
