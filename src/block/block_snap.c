/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __snapshot_process(WT_SESSION_IMPL *, WT_BLOCK *, WT_SNAPSHOT *);
static int __snapshot_string(
	WT_SESSION_IMPL *, WT_BLOCK *, const uint8_t *, WT_ITEM *);
static int __snapshot_update(WT_SESSION_IMPL *,
	WT_BLOCK *, WT_SNAPSHOT *, WT_BLOCK_SNAPSHOT *, uint64_t, int);

/*
 * __wt_block_snap_init --
 *	Initialize a snapshot structure.
 */
int
__wt_block_snap_init(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si, int is_live)
{
	WT_DECL_RET;

	/*
	 * If we're loading a new live snapshot, there shouldn't be one already
	 * loaded.  The btree engine should prevent this from ever happening,
	 * but paranoia is a healthy thing.
	 */
	if (is_live) {
		__wt_spin_lock(session, &block->live_lock);
		if (block->live_load)
			ret = EINVAL;
		else
			block->live_load = 1;
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

	si->file_size = WT_BLOCK_DESC_SECTOR;

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
	WT_BLOCK_SNAPSHOT *si;
	WT_DECL_RET;
	WT_ITEM *tmp;

	WT_UNUSED(addr_size);

	tmp = NULL;

	/*
	 * Sometimes we don't find a root page (we weren't given a snapshot,
	 * or the referenced snapshot was empty).  In that case we return a
	 * root page size of 0.  Set that up now.
	 */
	dsk->size = 0;

	si = &block->live;
	WT_RET(__wt_block_snap_init(session, block, si, 1));

	if (WT_VERBOSE_ISSET(session, snapshot)) {
		if (addr != NULL) {
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__snapshot_string(session, block, addr, tmp));
		}
		WT_VERBOSE_ERR(session, snapshot,
		    "%s: load-snapshot: %s", block->name,
		    addr == NULL ? "[Empty]" : (char *)tmp->data);
	}

	/* If not loading a snapshot from disk, we're done. */
	if (addr == NULL || addr_size == 0)
		return (0);

	/* Crack the snapshot cookie. */
	if (addr != NULL)
		WT_ERR(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/* Verify sets up next. */
	if (block->verify)
		WT_ERR(__wt_verify_snap_load(session, block, si));

	/* Read, and optionally verify, any root page. */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET) {
		WT_ERR(__wt_block_read_off(session, block,
		    dsk, si->root_offset, si->root_size, si->root_cksum));
		if (block->verify) {
			if (tmp == NULL) {
				WT_ERR(__wt_scr_alloc(session, 0, &tmp));
				WT_ERR(__snapshot_string(
				    session, block, addr, tmp));
			}
			WT_ERR(
			    __wt_verify_dsk(session, (char *)tmp->data, dsk));
		}
	}

	/*
	 * Rolling a snapshot forward requires the avail list, the blocks from
	 * which we can allocate.
	 */
	if (!readonly)
		WT_ERR(__wt_block_extlist_read(session, block, &si->avail));

	/*
	 * If the snapshot can be written, that means anything written after
	 * the snapshot is no longer interesting.  Truncate the file.
	 */
	if (!readonly) {
		WT_VERBOSE_ERR(session, snapshot,
		    "truncate file to %" PRIuMAX, (uintmax_t)si->file_size);
		WT_ERR(__wt_ftruncate(session, block->fh, si->file_size));
	}

	if (0) {
err:		(void)__wt_block_snapshot_unload(session, block);
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
	WT_DECL_RET;

	WT_VERBOSE_RETVAL(
	    session, snapshot, ret, "%s: unload snapshot", block->name);

	si = &block->live;

	/* Verify cleanup. */
	if (block->verify)
		WT_TRET(__wt_verify_snap_unload(session, block, si));

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

	/* Process the list of snapshots, deleting and updating as required. */
	WT_RET(__snapshot_process(session, block, snapbase));

	/*
	 * Snapshots have to hit disk (it would be reasonable to configure for
	 * lazy snapshots, but we don't support them yet).  Regardless, we're
	 * not holding any locks, other writers can proceed while we wait.
	 */
	if (!F_ISSET(S2C(session), WT_CONN_NOSYNC))
		WT_RET(__wt_fsync(session, block->fh));

	return (0);
}

/*
 * __snapshot_extlist_fblocks --
 *	If an extent list was read from disk, free its space to the live avail
 * list.
 */
static inline int
__snapshot_extlist_fblocks(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
	if (el->offset == WT_BLOCK_INVALID_OFFSET)
		return (0);
	return (__wt_block_insert_ext(
	    session, &block->live.avail, el->offset, el->size));
}

/*
 * __snapshot_process --
 *	Process the list of snapshots.
 */
static int
__snapshot_process(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_SNAPSHOT *snapbase)
{
	WT_BLOCK_SNAPSHOT *a, *b, *si;
	WT_DECL_RET;
	WT_ITEM *tmp;
	WT_SNAPSHOT *snap;
	uint64_t snapshot_size;
	int deleting, locked;

	tmp = NULL;
	locked = 0;

	/*
	 * We've allocated our last page, update the snapshot size.  We need to
	 * calculate the live system's snapshot size before reading and merging
	 * snapshot allocation and discard information from the snapshots we're
	 * deleting, those operations will change the underlying byte counts.
	 */
	si = &block->live;
	snapshot_size = si->snapshot_size;
	snapshot_size += si->alloc.bytes;
	snapshot_size -= si->discard.bytes;

	/*
	 * To delete a snapshot, we'll need snapshot information for it, and we
	 * have to read that from the disk.
	 */
	deleting = 0;
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		/*
		 * To delete a snapshot, we'll need snapshot information for it
		 * and the subsequent snapshot.  The test is tricky, we have to
		 * load the current snapshot's information if it's marked for
		 * deletion, or if it follows a snapshot marked for deletion,
		 * where the boundary cases are the first snapshot in the list
		 * and the last snapshot in the list: if we're deleting the last
		 * snapshot in the list, there's no next snapshot, the snapshot
		 * will be merged into the live tree.
		 */
		if (!F_ISSET(snap, WT_SNAP_DELETE) &&
		    (snap == snapbase ||
		    F_ISSET(snap, WT_SNAP_ADD) ||
		    !F_ISSET(snap - 1, WT_SNAP_DELETE)))
			continue;
		deleting = 1;

		/*
		 * Allocate a snapshot structure, crack the cookie and read the
		 * snapshot's extent lists.
		 *
		 * Ignore the avail list: snapshot avail lists are only useful
		 * if we are rolling forward from the particular snapshot and
		 * they represent our best understanding of what blocks can be
		 * allocated.  If we are not operating on the live snapshot,
		 * subsequent snapshots might have allocated those blocks, and
		 * the avail list is useless.  We don't discard it, because it
		 * is useful as part of verification, but we don't re-write it
		 * either.
		 */
		WT_ERR(__wt_calloc(
		    session, 1, sizeof(WT_BLOCK_SNAPSHOT), &snap->bpriv));
		si = snap->bpriv;
		WT_ERR(__wt_block_snap_init(session, block, si, 0));
		WT_ERR(__wt_block_buffer_to_snapshot(
		    session, block, snap->raw.data, si));
		WT_ERR(__wt_block_extlist_read(session, block, &si->alloc));
		WT_ERR(__wt_block_extlist_read(session, block, &si->discard));
	}

	/*
	 * Hold a lock so the live extent lists and the file size can't change
	 * underneath us.  I suspect we'll tighten this if snapshots take too
	 * much time away from real work: we read historic snapshot information
	 * without a lock, but we could also merge and re-write the delete
	 * snapshot information without a lock, except for ranges merged into
	 * the live tree.
	 */
	__wt_spin_lock(session, &block->live_lock);
	locked = 1;

	/* Skip the additional processing if we aren't deleting snapshots. */
	if (!deleting)
		goto live_update;

	/*
	 * Delete any no-longer-needed snapshots: we do this first as it frees
	 * blocks to the live lists, and the freed blocks will then be included
	 * when writing the live extent lists.
	 */
	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		if (!F_ISSET(snap, WT_SNAP_DELETE))
			continue;

		if (WT_VERBOSE_ISSET(session, snapshot)) {
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
			WT_ERR(__snapshot_string(
			    session, block, snap->raw.data, tmp));
			WT_VERBOSE_ERR(session, snapshot,
			    "%s: delete-snapshot: %s: %s",
			    block->name, snap->name, (char *)tmp->data);
		}

		/*
		 * Set the from/to snapshot structures, where the "to" value
		 * may be the live tree.
		 */
		a = snap->bpriv;
		if (F_ISSET(snap + 1, WT_SNAP_ADD))
			b = &block->live;
		else
			b = (snap + 1)->bpriv;

		/*
		 * Free the root page: there's nothing special about this free,
		 * the root page is allocated using normal rules, that is, it
		 * may have been taken from the avail list, and was entered on
		 * the live system's alloc list at that time.  We free it into
		 * the snapshot's discard list, however, not the live system's
		 * list because it appears on the snapshot's alloc list and so
		 * must be paired in the snapshot.
		 */
		if (a->root_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_block_insert_ext(session,
			    &a->discard, a->root_offset, a->root_size));

		/*
		 * Free the blocks used to hold the "from" snapshot's extent
		 * lists directly to the live system's avail list, they were
		 * never on any alloc list.   Include the "from" snapshot's
		 * avail list, it's going away.
		 */
		WT_RET(__snapshot_extlist_fblocks(session, block, &a->alloc));
		WT_RET(__snapshot_extlist_fblocks(session, block, &a->avail));
		WT_RET(__snapshot_extlist_fblocks(session, block, &a->discard));

		/*
		 * Roll the "from" alloc and discard extent lists into the "to"
		 * snapshot's lists.
		 */
		if (a->alloc.entries != 0)
			WT_ERR(__wt_block_extlist_merge(
			    session, &a->alloc, &b->alloc));
		if (a->discard.entries != 0)
			WT_ERR(__wt_block_extlist_merge(
			    session, &a->discard, &b->discard));

		/*
		 * If the "to" snapshot is also being deleted, we're done with
		 * it, it's merged into some other snapshot in the next loop.
		 * This means the extent lists may aggregate over a number of
		 * snapshots, but that's OK, they're disjoint sets of ranges.
		 */
		if (F_ISSET(snap + 1, WT_SNAP_DELETE))
			continue;

		/*
		 * Check for blocks we can re-use: any place the "to" snapshot's
		 * allocate and discard lists overlap is fair game: if a range
		 * appears on both lists, move it to the live system's available
		 * list, it can be re-used.
		 */
		WT_ERR(__wt_block_extlist_overlap(session, block, b));

		/*
		 * If we're updating the live system's information, we're done.
		 */
		if (F_ISSET(snap + 1, WT_SNAP_ADD))
			continue;

		/*
		 * We have to write the "to" snapshot's extent lists out in new
		 * blocks, and update its cookie.
		 *
		 * Free the blocks used to hold the "to" snapshot's extent lists
		 * directly to the live system's avail list, they were never on
		 * any alloc list.  Do not include the "to" snapshot's avail
		 * list, it's not changing.
		 */
		WT_RET(__snapshot_extlist_fblocks(session, block, &b->alloc));
		WT_RET(__snapshot_extlist_fblocks(session, block, &b->discard));

		F_SET(snap + 1, WT_SNAP_UPDATE);
	}

	/* Update snapshots marked for update. */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (F_ISSET(snap, WT_SNAP_UPDATE)) {
			WT_ASSERT(session,
			    !F_ISSET(snap, WT_SNAP_ADD));
			WT_ERR(__snapshot_update(
			    session, block, snap, snap->bpriv, 0, 0));
		}

live_update:
	si = &block->live;

	/* Truncate the file if that's possible. */
	WT_RET(__wt_block_extlist_truncate(session, block, &si->avail));

	/* Update the final, added snapshot based on the live system. */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (F_ISSET(snap, WT_SNAP_ADD)) {
			WT_ERR(__snapshot_update(
			    session, block, snap, si, snapshot_size, 1));

			/*
			 * XXX
			 * Our caller wants two pieces of information: the time
			 * the snapshot was taken and the final snapshot size.
			 * This violates layering but the alternative is a call
			 * for the btree layer to crack the snapshot cookie into
			 * its components, and that's a fair amount of work.
			 * (We could just read the system time in the session
			 * layer when updating the metadata file, but that won't
			 * work for the snapshot size, and so we do both here.
			 */
			snap->snapshot_size = si->snapshot_size;
			WT_ERR(__wt_epoch(session, &snap->sec, NULL));
		}

	/*
	 * Discard the live system's alloc and discard extent lists, leave the
	 * avail list alone.
	 */
	__wt_block_extlist_free(session, &si->alloc);
	__wt_block_extlist_free(session, &si->discard);

#ifdef HAVE_DIAGNOSTIC
	/*
	 * The first snapshot in the system should always have an empty discard
	 * list.  If we've read that snapshot and/or created it, check.
	 */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (!F_ISSET(snap, WT_SNAP_DELETE))
			break;
	if ((a = snap->bpriv) == NULL)
		a = &block->live;
	if (a->discard.entries != 0) {
		__wt_errx(session,
		    "snapshot incorrectly has blocks on the discard list");
		WT_ERR(WT_ERROR);
	}
#endif

err:	if (locked)
		__wt_spin_unlock(session, &block->live_lock);

	/* Discard any snapshot information we loaded, we no longer need it. */
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if ((si = snap->bpriv) != NULL) {
			__wt_block_extlist_free(session, &si->alloc);
			__wt_block_extlist_free(session, &si->avail);
			__wt_block_extlist_free(session, &si->discard);
		}

	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __snapshot_update --
 *	Update a snapshot.
 */
static int
__snapshot_update(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_SNAPSHOT *snap,
    WT_BLOCK_SNAPSHOT *si, uint64_t snapshot_size, int is_live)
{
	WT_DECL_RET;
	WT_ITEM *tmp;
	uint8_t *endp;

	tmp = NULL;

#ifdef HAVE_DIAGNOSTIC
	/* Check the extent list combinations for overlaps. */
	WT_RET(__wt_block_extlist_check(session, &si->alloc, &si->avail));
	WT_RET(__wt_block_extlist_check(session, &si->discard, &si->avail));
	WT_RET(__wt_block_extlist_check(session, &si->alloc, &si->discard));
#endif
	/*
	 * Write the snapshot's extent lists; we only write an avail list for
	 * the live system, other snapshot's avail lists are static and never
	 * change.
	 */
	WT_RET(__wt_block_extlist_write(session, block, &si->alloc));
	if (is_live)
		WT_RET(__wt_block_extlist_write(session, block, &si->avail));
	WT_RET(__wt_block_extlist_write(session, block, &si->discard));

	/*
	 * Set the file size for the live system.
	 *
	 * XXX
	 * We do NOT set the file size when re-writing snapshots because we want
	 * to test the snapshot's blocks against a reasonable maximum file size
	 * during verification.  This is not good: imagine a snapshot appearing
	 * early in the file, re-written, and then the snapshot requires blocks
	 * at the end of the file, blocks after the listed file size.  If the
	 * application opens that snapshot for writing (discarding subsequent
	 * snapshots), we would truncate the file to the early chunk, discarding
	 * the re-written snapshot information.  The alternative, updating the
	 * file size has it's own problems, in that case we'd work correctly,
	 * but we'd lose all of the blocks between the original snapshot and
	 * the re-written snapshot.  Currently, there's no API to roll-forward
	 * intermediate snapshots, if there ever is, this will need to be fixed.
	 */
	if (is_live)
		WT_RET(__wt_filesize(session, block->fh, &si->file_size));

	/* Set the snapshot size for the live system. */
	if (is_live)
		si->snapshot_size = snapshot_size;

	/*
	 * Copy the snapshot information into the snapshot array's address
	 * cookie.
	 */
	WT_RET(__wt_buf_init(session, &snap->raw, WT_BTREE_MAX_ADDR_COOKIE));
	endp = snap->raw.mem;
	WT_RET(__wt_block_snapshot_to_buffer(session, block, &endp, si));
	snap->raw.size = WT_PTRDIFF32(endp, snap->raw.mem);

	if (WT_VERBOSE_ISSET(session, snapshot)) {
		WT_RET(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__snapshot_string(session, block, snap->raw.data, tmp));
		WT_VERBOSE_ERR(session, snapshot,
		    "%s: create-snapshot: %s: %s",
		    block->name, snap->name, (char *)tmp->data);
	}

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __snapshot_string --
 *	Return a printable string representation of a snapshot address cookie.
 */
static int
__snapshot_string(WT_SESSION_IMPL *session,
    WT_BLOCK *block, const uint8_t *addr, WT_ITEM *buf)
{
	WT_BLOCK_SNAPSHOT *si, _si;

	/* Initialize the snapshot, crack the cookie. */
	si = &_si;
	WT_RET(__wt_block_snap_init(session, block, si, 0));
	WT_RET(__wt_block_buffer_to_snapshot(session, block, addr, si));

	WT_RET(__wt_buf_fmt(session, buf,
	    "version=%d",
	    si->version));
	if (si->root_offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", root=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		    ", root=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->root_offset,
		    (uintmax_t)(si->root_offset + si->root_size),
		    si->root_size, si->root_cksum));
	if (si->alloc.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", alloc=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		    ", alloc=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->alloc.offset,
		    (uintmax_t)(si->alloc.offset + si->alloc.size),
		    si->alloc.size, si->alloc.cksum));
	if (si->avail.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", avail=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		    ", avail=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->avail.offset,
		    (uintmax_t)(si->avail.offset + si->avail.size),
		    si->avail.size, si->avail.cksum));
	if (si->discard.offset == WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_buf_catfmt(session, buf, ", discard=[Empty]"));
	else
		WT_RET(__wt_buf_catfmt(session, buf,
		    ", discard=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->discard.offset,
		    (uintmax_t)(si->discard.offset + si->discard.size),
		    si->discard.size, si->discard.cksum));
	WT_RET(__wt_buf_catfmt(session, buf,
	    ", file size=%" PRIuMAX
	    ", write generation=%" PRIu64,
	    (uintmax_t)si->file_size,
	    si->write_gen));

	return (0);
}
