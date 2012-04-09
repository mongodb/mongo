/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __block_snap_delete(WT_SESSION_IMPL *, WT_BLOCK *, WT_SNAPSHOT *);
static int __block_snap_extlists(
	WT_SESSION_IMPL *, WT_BLOCK *, WT_BLOCK_SNAPSHOT *);

/*
 * __wt_block_snap_init --
 *	Initialize a snapshot structure.
 */
int
__wt_block_snap_init(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si, int is_live)
{
	int ret;

	/*
	 * If we're loading a new live snapshot, there shouldn't be one already
	 * loaded.  The btree engine should prevent this from ever happening,
	 * but paranoia is a healthy thing.
	 */
	if (is_live) {
		__wt_spin_lock(session, &block->live_lock);
		if (block->live_load)
			ret = EINVAL;
		else {
			block->live_load = 1;
			ret = 0;
		}
		__wt_spin_unlock(session, &block->live_lock);
		if (ret)
			WT_RET_MSG(session, EINVAL, "snapshot already loaded");
	}

	memset(si, 0, sizeof(*si));

	si->root_offset = WT_BLOCK_INVALID_OFFSET;

	si->alloc.name = "alloc";
	si->alloc.offset = WT_BLOCK_INVALID_OFFSET;

	si->avail.name = "avail";
	si->avail.offset = WT_BLOCK_INVALID_OFFSET;

	si->discard.name = "discard";
	si->discard.offset = WT_BLOCK_INVALID_OFFSET;

	return (0);
}

/*
 * __wt_block_snapshot_load --
 *	Load a snapshot.
 */
int
__wt_block_snapshot_load(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *dsk, const uint8_t *addr, uint32_t addr_size,
    int readonly)
{
	WT_ITEM *tmp;
	WT_BLOCK_SNAPSHOT *si;
	int ret;

	tmp = NULL;
	ret = 0;

	/*
	 * Sometimes we don't find a root page (we weren't given a snapshot,
	 * or the referenced snapshot was empty).  In that case we return a
	 * size of 0.  Set that up now.
	 */
	dsk->size = 0;

	if (addr == NULL)
		WT_VERBOSE(session, block, "load-snapshot: [Empty]");
	else
		WT_VERBOSE_CALL_RET(session, block,
		    __wt_block_snapshot_string(
		    session, block, addr, "load-snapshot", NULL));

	si = &block->live;
	WT_RET(__wt_block_snap_init(session, block, si, 1));

	/* If not loading a snapshot from disk, we're done. */
	if (addr == NULL || addr_size == 0)
		return (0);

	/* Crack the snapshot cookie. */
	WT_ERR(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/*
	 * Verify has a fair amount of work to do when we load the snapshot,
	 * get it done.
	 */
	if (block->verify)
		WT_ERR(__wt_verify_snap_load(session, block, si));

	/* Read, and optionally verify, any root page. */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET) {
		WT_ERR(__wt_block_read_off(session, block,
		    dsk, si->root_offset, si->root_size, si->root_cksum));
		if (block->verify) {
			WT_ERR(__wt_block_snapshot_string(
			    session, block, addr, NULL, &tmp));
			WT_ERR(
			    __wt_verify_dsk(session, (char *)tmp->data, dsk));
		}
	}

	/*
	 * If the snapshot can be written, read the avail list (the list of
	 * blocks from which we can allocate on write).
	 */
	if (!readonly)
		WT_ERR(__wt_block_extlist_read(session, block, &si->avail));

	/*
	 * If the snapshot can be written, that means anything written after
	 * the snapshot is no longer interesting.  Truncate the file.
	 */
	if (!readonly) {
		WT_VERBOSE(session, block,
		    "snapshot truncates file to %" PRIuMAX,
		    (uintmax_t)si->file_size);
		WT_ERR(__wt_ftruncate(session, block->fh, si->file_size));
	}

	if (0) {
err:		block->live_load = 0;
	}

	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_snapshot_unload --
 *	Unload a snapshot.
 */
int
__wt_block_snapshot_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_SNAPSHOT *si;
	int ret;

	ret = 0;

	WT_VERBOSE(session, block, "%s: unload snapshot", block->name);

	/* Work on the "live" snapshot. */
	if (!block->live_load)
		WT_RET_MSG(session, EINVAL, "no snapshot to unload");

	si = &block->live;

	/* Verify cleanup. */
	if (block->verify)
		ret = __wt_verify_snap_unload(session, block, si);

	/* Discard the extent lists. */
	__wt_block_extlist_free(session, &si->alloc);
	__wt_block_extlist_free(session, &si->avail);
	__wt_block_extlist_free(session, &si->discard);

	block->live_load = 0;

	return (ret);
}

/*
 * __wt_block_snapshot --
 *	Create a new snapshot.
 */
int
__wt_block_snapshot(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *buf, WT_SNAPSHOT *snapbase)
{
	WT_BLOCK_SNAPSHOT *si;
	WT_SNAPSHOT *snap;
	uint8_t *endp;
	int ret;

	ret = 0;

	si = &block->live;
	si->version = WT_BM_SNAPSHOT_VERSION;

	/*
	 * Write the root page: it's possible for there to be a snapshot of
	 * an empty tree, in which case, we store an illegal root offset.
	 *
	 * XXX
	 * We happen to know that snapshots are single-threaded above us in
	 * the btree engine.  That's probably something we want to guarantee
	 * for any WiredTiger block manager.
	 */
	if (buf == NULL) {
		si->root_offset = WT_BLOCK_INVALID_OFFSET;
		si->root_size = si->root_cksum = 0;
	} else
		WT_RET(__wt_block_write_off(session, block, buf,
		    &si->root_offset, &si->root_size, &si->root_cksum, 0));

	/*
	 * Hold a lock so the live extent lists and the file size can't change
	 * underneath us.  I suspect we'll tighten this if snapshots take too
	 * much time away from real work: we could read the historic snapshot
	 * information without a lock, and we could merge and re-write all of
	 * the deleted snapshot information except for ranges merged into the
	 * live tree, without a lock.
	 */
	__wt_spin_lock(session, &block->live_lock);

	/*
	 * Delete any no-longer-needed snapshots: we do this first as it frees
	 * blocks to the live lists, and the freed blocks will then be included
	 * when writing the live extent lists.
	 */
	WT_ERR(__block_snap_delete(session, block, snapbase));

	/* Resolve the live extent lists. */
	WT_ERR(__block_snap_extlists(session, block, si));

	/* Set the file size. */
	WT_ERR(__wt_filesize(session, block->fh, &si->file_size));

err:	__wt_spin_unlock(session, &block->live_lock);
	WT_RET(ret);

	/*
	 * Copy the snapshot information into the snapshot array's address
	 * cookie.
	 */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (FLD_ISSET(snap->flags, WT_SNAP_ADD))
			break;
	if (snap->name == NULL)
		WT_RET_MSG(session, EINVAL, "snapshot list has no added slot");
	WT_RET(__wt_buf_init(session, &snap->raw, WT_BTREE_MAX_ADDR_COOKIE));
	endp = snap->raw.mem;
	WT_RET(__wt_block_snapshot_to_buffer(session, block, &endp, si));
	snap->raw.size = WT_PTRDIFF32(endp, snap->raw.mem);
	WT_VERBOSE_CALL_RET(session, block,
	    __wt_block_snapshot_string(
	    session, block, snap->raw.data, "create-snapshot", NULL));

	/*
	 * Snapshots have to hit disk (it would be reasonable to configure for
	 * lazy snapshots, but we don't support them yet).  Regardless, we're
	 * not holding any locks, other writers can proceed while we wait.
	 */
	WT_RET(__wt_fsync(session, block->fh));

	return (0);
}

/*
 * __block_snap_extlists --
 *	Extent list handling for the snapshot.
 */
static int
__block_snap_extlists(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si)
{
	/* Truncate the file if possible. */
	WT_RET(__wt_block_extlist_truncate(session, block, &si->avail));

#ifdef HAVE_DIAGNOSTIC
	/*
	 * Currently, we do not check if a freed block can be immediately put
	 * on the avail list (that is, if it was allocated during the current
	 * snapshot -- once that change is made, we should check for overlaps
	 * between the alloc and discard lists.
	 */
	WT_RET(__wt_block_extlist_check(session, si, "snapshot", 0));
#endif

	/* Write the extent lists. */
	WT_RET(__wt_block_extlist_write(session, block, &si->alloc));
	WT_RET(__wt_block_extlist_write(session, block, &si->avail));
	WT_RET(__wt_block_extlist_write(session, block, &si->discard));

	/* Discard the alloc and discard extent lists. */
	__wt_block_extlist_free(session, &si->alloc);
	__wt_block_extlist_free(session, &si->discard);

	return (0);
}

/*
 * __block_snap_delete --
 *	Delete snapshots.
 */
static int
__block_snap_delete(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_SNAPSHOT *snapbase)
{
	WT_BLOCK_SNAPSHOT *a, *b, *si;
	WT_SNAPSHOT *snap;
	int found, ret;

	ret = 0;

	/* Check for snapshots scheduled for deletion. */
	found = 0;
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		if (!FLD_ISSET(snap->flags, WT_SNAP_DELETE))
			continue;

		found = 1;
		WT_VERBOSE_CALL_ERR(session, block,
		    __wt_block_snapshot_string(
		    session, block, snap->raw.data, "delete", NULL));

		/*
		 * To delete a snapshot, we'll need snapshot information for it
		 * and the subsequent snapshot.  We're loading in pairs, so if
		 * multiple snapshots are being deleted, we might have already
		 * loaded snapshot information for the slot.
		 */
		if (snap->bpriv != NULL)
			continue;

		/*
		 * Allocate a snapshot structure, crack the cookie and read the
		 * snapshot's extent lists.
		 */
		WT_ERR(__wt_calloc(
		    session, 1, sizeof(WT_BLOCK_SNAPSHOT), &snap->bpriv));
		si = snap->bpriv;
		WT_ERR(__wt_block_snap_init(session, block, si, 0));
		WT_ERR(__wt_block_buffer_to_snapshot(
		    session, block, snap->raw.data, si));
		WT_ERR(__wt_block_extlist_read(session, block, &si->alloc));
		WT_ERR(__wt_block_extlist_read(session, block, &si->avail));
		WT_ERR(__wt_block_extlist_read(session, block, &si->discard));

		/*
		 * If deleting the last already existing snapshot in the list,
		 * there's no "next" snapshot, the snapshot will be merged into
		 * the live tree.
		 */
		++snap;
		if (FLD_ISSET(snap->flags, WT_SNAP_ADD))
			break;

		/*
		 * Repeat for the next slot:
		 * Allocate a snapshot structure, crack the cookie and read the
		 * snapshot's extent lists.
		 */
		WT_ERR(__wt_calloc(
		    session, 1, sizeof(WT_BLOCK_SNAPSHOT), &snap->bpriv));
		si = snap->bpriv;
		WT_ERR(__wt_block_snap_init(session, block, si, 0));
		WT_ERR(__wt_block_buffer_to_snapshot(
		    session, block, snap->raw.data, si));
		WT_ERR(__wt_block_extlist_read(session, block, &si->alloc));
		WT_ERR(__wt_block_extlist_read(session, block, &si->avail));
		WT_ERR(__wt_block_extlist_read(session, block, &si->discard));
	}

	if (!found)
		return (0);

	/* Delete the snapshots. */
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		if (!FLD_ISSET(snap->flags, WT_SNAP_DELETE))
			continue;

		/*
		 * Set the from/to snapshot structures, where to "to" value
		 * may be the live tree.
		 */
		a = snap->bpriv;
		++snap;
		b = FLD_ISSET(
		    snap->flags, WT_SNAP_ADD) ? &block->live : snap->bpriv;

		/*
		 * Free the root page: there's nothing special about this free,
		 * the root page is allocated using normal rules, that is, it
		 * may have been taken from the avail list, and was entered on
		 * the alloc list at that time.
		 */
		if (a->root_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_free_ext(
			    session, block, a->root_offset, a->root_size, 0));

		/*
		 * Free the blocks used to hold the extent lists directly to the
		 * live system's avail list, they were never on any alloc list.
		 */
		if (a->alloc.offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_free_ext(session,
			    block, a->alloc.offset, a->alloc.size, 1));
		if (a->avail.offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_free_ext(session,
			    block, a->avail.offset, a->avail.size, 1));
		if (a->discard.offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_free_ext(session,
			    block, a->discard.offset, a->discard.size, 1));

		/*
		 * Roll the "from" alloc and discard extent lists into the "to"
		 * snapshot's lists.
		 *
		 * Ignore the avail list: snapshot avail lists are only useful
		 * if we are rolling forward from the particular snapshot and
		 * they represent our best understanding of what blocks can be
		 * allocated.  If we are not operating on the live snapshot,
		 * subsequent snapshots might have allocated those blocks, and
		 * the avail list is useless.
		 */
		if (a->alloc.offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_extlist_merge(
			    session, &a->alloc, &b->alloc));
		if (a->discard.offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_extlist_merge(
			    session, &a->discard, &b->discard));

		/*
		 * Check for blocks we can re-use: any place the "to" snapshot's
		 * allocate and discard lists overlap is fair game: if a range
		 * appears on both lists, move it to the avail list, it can be
		 * re-used.
		 */
		WT_ERR(__wt_block_extlist_match(session, block, b));

		/*
		 * XXX
		 * If the next entry isn't also deleted, we have to write this
		 * entry out in new blocks, and update the cookie, this code
		 * only works if we end up at the live tree.
		 */
	}

#ifdef HAVE_DIAGNOSTIC
	WT_RET(__wt_block_extlist_check(
	    session, &block->live, "live after merge", 1));
#endif

	/* Discard the snapshot information, we no longer need it. */
err:	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if ((a = snap->bpriv) != NULL) {
			__wt_block_extlist_free(session, &a->alloc);
			__wt_block_extlist_free(session, &a->discard);
			__wt_block_extlist_free(session, &a->avail);
		}

	return (ret);
}

/*
 * __wt_block_snapshot_string --
 *	Return a printable string representation of a snapshot address cookie.
 */
int
__wt_block_snapshot_string(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, const char *tag, WT_ITEM **retp)
{
	WT_BLOCK_SNAPSHOT *si, _si;
	WT_ITEM *tmp;
	int ret;

	tmp = NULL;
	ret = 0;

	/* Initialize the snapshot, crack the cookie. */
	si = &_si;
	WT_RET(__wt_block_snap_init(session, block, si, 0));
	WT_RET(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/* Allocate a buffer. */
	WT_ERR(__wt_scr_alloc(session, 0, &tmp));

	WT_ERR(__wt_buf_fmt(session, tmp,
	    "version=%d",
	    si->version));
	if (si->root_offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", root=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", root=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->root_offset,
		    (uintmax_t)(si->root_offset + si->root_size),
		    si->root_size, si->root_cksum));
	if (si->alloc.offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", alloc=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", alloc=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->alloc.offset,
		    (uintmax_t)(si->alloc.offset + si->alloc.size),
		    si->alloc.size, si->alloc.cksum));
	if (si->avail.offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", avail=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", avail=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->avail.offset,
		    (uintmax_t)(si->avail.offset + si->avail.size),
		    si->avail.size, si->avail.cksum));
	if (si->discard.offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", discard=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", discard=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->discard.offset,
		    (uintmax_t)(si->discard.offset + si->discard.size),
		    si->discard.size, si->discard.cksum));
	WT_ERR(__wt_buf_catfmt(session, tmp,
	    ", file size=%" PRIuMAX
	    ", write generation=%" PRIu64,
	    (uintmax_t)si->file_size,
	    si->write_gen));

	/* Optionally output that a snapshot is being handled. */
	if (tag != NULL)
		WT_VERBOSE(session, block,
		    "%s: %s %s", block->name, tag, (char *)tmp->data);

	if (retp != NULL)
		*retp = tmp;

err:	if (retp == NULL || ret != 0)
		__wt_scr_free(&tmp);

	return (ret);
}
