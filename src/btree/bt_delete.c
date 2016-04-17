/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
 * flag so the cursor walk code marks any page within the range, that hasn't
 * yet been read and which has no overflow items, as deleted, by changing the
 * WT_REF state to WT_REF_DELETED.  Pages already in the cache or with overflow
 * items, have their rows updated/deleted individually. The transaction for the
 * delete operation is stored in memory referenced by the WT_REF.page_del field.
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
 * session unrolling the delete can find all of the WT_UPDATE structures that
 * require update.
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
	WT_DECL_RET;
	WT_PAGE *parent;

	*skipp = false;

	/* If we have a clean page in memory, attempt to evict it. */
	if (ref->state == WT_REF_MEM &&
	    __wt_atomic_casv32(&ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
		if (__wt_page_is_modified(ref->page)) {
			WT_PUBLISH(ref->state, WT_REF_MEM);
			return (0);
		}

		(void)__wt_atomic_addv32(&S2BT(session)->evict_busy, 1);
		ret = __wt_evict(session, ref, false);
		(void)__wt_atomic_subv32(&S2BT(session)->evict_busy, 1);
		WT_RET_BUSY_OK(ret);
	}

	/*
	 * Atomically switch the page's state to lock it.  If the page is not
	 * on-disk, other threads may be using it, no fast delete.
	 *
	 * Possible optimization: if the page is already deleted and the delete
	 * is visible to us (the delete has been committed), we could skip the
	 * page instead of instantiating it and figuring out there are no rows
	 * in the page.  While that's a huge amount of work to no purpose, it's
	 * unclear optimizing for overlapping range deletes is worth the effort.
	 */
	if (ref->state != WT_REF_DISK ||
	    !__wt_atomic_casv32(&ref->state, WT_REF_DISK, WT_REF_LOCKED))
		return (0);

	/*
	 * We cannot fast-delete pages that have overflow key/value items as
	 * the overflow blocks have to be discarded.  The way we figure that
	 * out is to check the page's cell type, cells for leaf pages without
	 * overflow items are special.
	 *
	 * To look at an on-page cell, we need to look at the parent page, and
	 * that's dangerous, our parent page could change without warning if
	 * the parent page were to split, deepening the tree.  It's safe: the
	 * page's reference will always point to some valid page, and if we find
	 * any problems we simply fail the fast-delete optimization.
	 */
	parent = ref->home;
	if (__wt_off_page(parent, ref->addr) ?
	    ((WT_ADDR *)ref->addr)->type != WT_ADDR_LEAF_NO :
	    __wt_cell_type_raw(ref->addr) != WT_CELL_ADDR_LEAF_NO)
		goto err;

	/*
	 * This action dirties the parent page: mark it dirty now, there's no
	 * future reconciliation of the child leaf page that will dirty it as
	 * we write the tree.
	 */
	WT_ERR(__wt_page_parent_modify_set(session, ref, false));

	/*
	 * Record the change in the transaction structure and set the change's
	 * transaction ID.
	 */
	WT_ERR(__wt_calloc_one(session, &ref->page_del));
	ref->page_del->txnid = session->txn.id;

	WT_ERR(__wt_txn_modify_ref(session, ref));

	*skipp = true;
	WT_STAT_FAST_CONN_INCR(session, rec_page_delete_fast);
	WT_STAT_FAST_DATA_INCR(session, rec_page_delete_fast);
	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (0);

err:	__wt_free(session, ref->page_del);

	/*
	 * Restore the page to on-disk status, we'll have to instantiate it.
	 */
	WT_PUBLISH(ref->state, WT_REF_DISK);
	return (ret);
}

/*
 * __wt_delete_page_rollback --
 *	Abort pages that were deleted without being instantiated.
 */
void
__wt_delete_page_rollback(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_UPDATE **upd;

	/*
	 * If the page is still "deleted", it's as we left it, reset the state
	 * to on-disk and we're done.  Otherwise, we expect the page is either
	 * instantiated or being instantiated.  Loop because it's possible for
	 * the page to return to the deleted state if instantiation fails.
	 */
	for (;; __wt_yield())
		switch (ref->state) {
		case WT_REF_DISK:
		case WT_REF_READING:
			WT_ASSERT(session, 0);		/* Impossible, assert */
			break;
		case WT_REF_DELETED:
			/*
			 * If the page is still "deleted", it's as we left it,
			 * reset the state.
			 */
			if (__wt_atomic_casv32(
			    &ref->state, WT_REF_DELETED, WT_REF_DISK))
				return;
			break;
		case WT_REF_LOCKED:
			/*
			 * A possible state, the page is being instantiated.
			 */
			break;
		case WT_REF_MEM:
		case WT_REF_SPLIT:
			/*
			 * We can't use the normal read path to get a copy of
			 * the page because the session may have closed the
			 * cursor, we no longer have the reference to the tree
			 * required for a hazard pointer.  We're safe because
			 * with unresolved transactions, the page isn't going
			 * anywhere.
			 *
			 * The page is in an in-memory state, walk the list of
			 * update structures and abort them.
			 */
			for (upd =
			    ref->page_del->update_list; *upd != NULL; ++upd)
				(*upd)->txnid = WT_TXN_ABORTED;

			/*
			 * Discard the memory, the transaction can't abort
			 * twice.
			 */
			__wt_free(session, ref->page_del->update_list);
			__wt_free(session, ref->page_del);
			return;
		}
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
	 * Deleted pages come from two sources: either it's a fast-delete as
	 * described above, or the page has been emptied by other operations
	 * and eviction deleted it.
	 *
	 * In both cases, the WT_REF state will be WT_REF_DELETED.  In the case
	 * of a fast-delete page, there will be a WT_PAGE_DELETED structure with
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

	skip = ref->page_del == NULL || (visible_all ?
	    __wt_txn_visible_all(session, ref->page_del->txnid) :
	    __wt_txn_visible(session, ref->page_del->txnid));

	/*
	 * The page_del structure can be freed as soon as the delete is stable:
	 * it is only read when the ref state is WT_REF_DELETED.  It is worth
	 * checking every time we come through because once this is freed, we
	 * no longer need synchronization to check the ref.
	 */
	if (skip && ref->page_del != NULL && (visible_all ||
	    __wt_txn_visible_all(session, ref->page_del->txnid))) {
		__wt_free(session, ref->page_del->update_list);
		__wt_free(session, ref->page_del);
	}

	WT_PUBLISH(ref->state, WT_REF_DELETED);
	return (skip);
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
	WT_PAGE *page;
	WT_PAGE_DELETED *page_del;
	WT_UPDATE **upd_array, *upd;
	size_t size;
	uint32_t i;

	btree = S2BT(session);
	page = ref->page;
	page_del = ref->page_del;

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
	 * version of the page around, then we crashed and recovered, and now
	 * we're being forced to read that page.
	 *
	 * In the first case, we have a page reference structure, in the second
	 * second, we don't.
	 *
	 * Allocate the per-reference update array; in the case of instantiating
	 * a page, deleted by a running transaction that might eventually abort,
	 * we need a list of the update structures so we can do that abort.  The
	 * hard case is if a page splits: the update structures might be moved
	 * to different pages, and we still have to find them all for an abort.
	 */

	if (page_del != NULL)
		WT_RET(__wt_calloc_def(
		    session, page->pg_row_entries + 1, &page_del->update_list));

	/* Allocate the per-page update array. */
	WT_ERR(__wt_calloc_def(session, page->pg_row_entries, &upd_array));
	page->modify->mod_row_update = upd_array;

	/*
	 * Fill in the per-reference update array with references to update
	 * structures, fill in the per-page update array with references to
	 * deleted items.
	 */
	for (i = 0, size = 0; i < page->pg_row_entries; ++i) {
		WT_ERR(__wt_calloc_one(session, &upd));
		WT_UPDATE_DELETED_SET(upd);

		if (page_del == NULL)
			upd->txnid = WT_TXN_NONE;	/* Globally visible */
		else {
			upd->txnid = page_del->txnid;
			page_del->update_list[i] = upd;
		}

		upd->next = upd_array[i];
		upd_array[i] = upd;

		size += sizeof(WT_UPDATE *) + WT_UPDATE_MEMSIZE(upd);
	}

	__wt_cache_page_inmem_incr(session, page, size);

	return (0);

err:	/*
	 * There's no need to free the page update structures on error, our
	 * caller will discard the page and do that work for us.  We could
	 * similarly leave the per-reference update array alone because it
	 * won't ever be used by any page that's not in-memory, but cleaning
	 * it up makes sense, especially if we come back in to this function
	 * attempting to instantiate this page again.
	 */
	if (page_del != NULL)
		__wt_free(session, page_del->update_list);
	return (ret);
}
