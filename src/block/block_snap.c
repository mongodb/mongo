/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __block_snap_delete(WT_SESSION_IMPL *, WT_BLOCK *, const uint8_t *);
static int __block_snap_extlists_write(
    WT_SESSION_IMPL *, WT_BLOCK *, WT_BLOCK_SNAPSHOT *);

/*
 * __wt_block_snap_load --
 *	Load a snapshot.
 */
int
__wt_block_snap_load(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *dsk, const uint8_t *addr, uint32_t addr_size)
{
	WT_ITEM *tmp;
	WT_BLOCK_SNAPSHOT *si;
	int ret;

	tmp = NULL;
	ret = 0;

	WT_VERBOSE(session, block, "%s: load snapshot", block->name);

	/* Work on the "live" snapshot. */
	if (block->live_load)
		WT_RET_MSG(session, EINVAL, "snapshot already loaded");

	si = &block->live;
	memset(si, 0, sizeof(*si));
	block->live.alloc.name = "live: alloc";
	block->live.alloc_offset = WT_BLOCK_INVALID_OFFSET;
	block->live.avail.name = "live: avail";
	block->live.avail_offset = WT_BLOCK_INVALID_OFFSET;
	block->live.discard.name = "live: discard";
	block->live.discard_offset = WT_BLOCK_INVALID_OFFSET;

	/* If not loading a snapshot from disk, we're done. */
	if (addr == NULL || addr_size == 0)
		goto done;

	WT_VERBOSE_CALL_ERR(session, block,
	    __wt_block_snapshot_string(
	    session, block, addr, "load-snapshot", NULL));

	/* Crack the snapshot cookie. */
	WT_ERR(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/* Read, and optionally verify, any root page. */
	if (si->root_offset == WT_BLOCK_INVALID_OFFSET)
		dsk->size = 0;
	else {
		WT_ERR(__wt_block_read(session, block,
		    dsk, si->root_offset, si->root_size, si->root_cksum));
		if (block->fragbits != NULL) {
			WT_ERR(__wt_block_snapshot_string(
			    session, block, addr, NULL, &tmp));
			WT_ERR(__wt_verify_addfrag(session,
			    block, si->root_offset, (off_t)si->root_size));
			WT_ERR(
			    __wt_verify_dsk(session, (char *)tmp->data, dsk));
		}
	}

	/*
	 * Read the snapshot's extent lists.
	 *
	 * This snapshot can potentially be written (we rely on upper-levels of
	 * the btree engine to not allow writing, currently), and we only need
	 * the avail list (that is, we need to have a list of blocks from which
	 * we can allocate on write).
	 *
	 * XXX
	 * If we're opening anything other than the last snapshot, then we don't
	 * even need the avail list, but in the current code we don't know if we
	 * are opening a snapshot for writing or not.
	 */
	if (si->avail_offset != WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_block_extlist_read(session, block, &si->avail,
		    si->avail_offset, si->avail_size, si->avail_cksum));

	/*
	 * If we're verifying, then add the disk blocks used to store the extent
	 * lists to the list of blocks we've "seen".
	 */
	if (block->fragbits != NULL) {
		if (si->avail_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_verify_addfrag(session, block,
			    si->avail_offset, (off_t)si->avail_size));
		if (si->alloc_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_verify_addfrag(session, block,
			    si->alloc_offset, (off_t)si->alloc_size));
		if (si->discard_offset != WT_BLOCK_INVALID_OFFSET)
			WT_ERR(__wt_verify_addfrag(session, block,
			    si->discard_offset, (off_t)si->discard_size));
	}

	/* Ignore the file size for now. */
	/* XXX */

done:	block->live_load = 1;

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __wt_block_snap_unload --
 *	Unload a snapshot.
 */
int
__wt_block_snap_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_SNAPSHOT *si;

	WT_VERBOSE(session, block, "%s: unload snapshot", block->name);

	/* Work on the "live" snapshot. */
	if (!block->live_load)
		WT_RET_MSG(session, EINVAL, "no snapshot to unload");
	block->live_load = 0;
	si = &block->live;

	/* If we're verifying the file, verify the extent lists. */
	if (block->fragbits != NULL) {
		WT_RET(__wt_verify_extlist(session, block, &si->alloc));
		WT_RET(__wt_verify_extlist(session, block, &si->avail));
		WT_RET(__wt_verify_extlist( session, block, &si->discard));
	}

	/* Discard the extent lists. */
	__wt_block_extlist_free(session, &si->alloc);
	__wt_block_extlist_free(session, &si->avail);
	__wt_block_extlist_free(session, &si->discard);

	memset(si, 0, sizeof(*si));

	return (0);
}

/*
 * __wt_block_write_snapshot --
 *	Write a buffer into a block and create a new snapshot.
 */
int
__wt_block_write_buf_snapshot(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, WT_ITEM *snap)
{
	WT_BLOCK_SNAPSHOT *si;

	si = &block->live;
	si->version = WT_BM_SNAPSHOT_VERSION;

	/* Delete any no-longer-needed snapshots. */
	if (snap->data != NULL && snap->size != 0) {
		WT_RET(__block_snap_delete(session, block, snap->data));

		/* UPDATE FLAG !? XXX */
		snap->data = NULL;
	}

	/*
	 * Write the root page: it's possible for there to be a snapshot of
	 * an empty tree, in which case, we store an illegal root offset.
	 */
	if (buf == NULL) {
		si->root_offset = WT_BLOCK_INVALID_OFFSET;
		si->root_size = si->root_cksum = 0;
	} else
		WT_RET(__wt_block_write(session, block, buf,
		    &si->root_offset, &si->root_size, &si->root_cksum));

#if 0
	/*
	 * We hold the lock so the live extent lists and the file size doesn't
	 * change underneath us.  This will probably eventually need fixing: we
	 * could read all the snapshot information without a lock, and we could
	 * merge and re-write all of the deleted snapshot information except for
	 * ranges merged into the live tree, without a lock.
	 */
	__wt_spin_lock(session, &block->live_lock);
#endif

	/* Write the live extent lists. */
	WT_RET(__block_snap_extlists_write(session, block, si));

	/* Set the file size. */
	WT_RET(__wt_filesize(session, block->fh, &si->file_size));

#if 0
	__wt_spin_unlock(session, &block->live_lock);
#endif

	/*
	 * Snapshots have to hit disk (it would be reasonable to configure for
	 * lazy snapshots, but we don't support them yet).
	 */
	WT_RET(__wt_fsync(session, block->fh));

#if 0
	uint8_t *endp;
	u_int next;
	/*
	 * Copy the snapshot information into the snapshot array's address
	 * cookie.
	 */
	WT_SNAPSHOT_FOREACH(snap, next)
		if (FLD_ISSET(snap[next].flags, WT_SNAP_ADD))
			break;
	if (snap[next].name == NULL)
		WT_RET_MSG(session, EINVAL, "snapshot list has no added slot");
	WT_RET(__wt_calloc_def(
	    session, WT_BTREE_MAX_ADDR_COOKIE, &snap[next].addr.addr));
	endp = snap[next].addr.addr;
	WT_RET(__wt_block_snapshot_to_buffer(session, block, &endp, si));
	snap[next].addr.size = WT_PTRDIFF32(endp, snap[next].addr.addr);
	WT_VERBOSE_CALL_RET(session, block,
	    __wt_block_snapshot_string(session, block,
	    snap[next].addr.addr, "create-snapshot", NULL));
#else
	{
	uint8_t *endp;

	endp = snap->mem;
	WT_RET(__wt_block_snapshot_to_buffer(session, block, &endp, si));
	snap->data = snap->mem;
	snap->size = WT_PTRDIFF32(endp, snap->mem);
	WT_VERBOSE_CALL_RET(session, block,
	    __wt_block_snapshot_string(session, block,
	    snap->data, "create-snapshot", NULL));
	}
#endif

	return (0);
}

/*
 * __block_snap_extlists_write --
 *	Write the extent lists.
 */
static int
__block_snap_extlists_write(
    WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si)
{
	/* Truncate the file if possible. */
	WT_RET(__wt_block_extlist_truncate(session, block, &si->avail));

	/* Write and discard the allocation and discard extent lists. */
	WT_RET(__wt_block_extlist_write(
	    session, block, &si->alloc,
	    &si->alloc_offset, &si->alloc_size, &si->alloc_cksum));
	__wt_block_extlist_free(session, &si->alloc);

	WT_RET(__wt_block_extlist_write(
	    session, block, &si->discard,
	    &si->discard_offset, &si->discard_size, &si->discard_cksum));
	__wt_block_extlist_free(session, &si->discard);

	/* Write the available list, but don't discard it. */
	WT_RET(__wt_block_extlist_write(
	    session, block, &si->avail,
	    &si->avail_offset, &si->avail_size, &si->avail_cksum));

	return (0);
}

/*
 * __block_snap_delete --
 *	Delete snapshots.
 */
static int
__block_snap_delete(
    WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr)
{
	WT_BLOCK_SNAPSHOT *si, __si;

	si = &__si;

	WT_VERBOSE_CALL_RET(session, block,
	    __wt_block_snapshot_string(
	    session, block, addr, "delete-snapshot", NULL));

	/*
	 * If there's a snapshot, crack the cookie and free the snapshot's
	 * extent lists and the snapshot's root page.
	 */
	WT_RET(__wt_block_buffer_to_snapshot(session, block, addr, si));

	if (si->alloc_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->alloc_offset, si->alloc_size));
	if (si->avail_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->avail_offset, si->avail_size));
	if (si->discard_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->discard_offset, si->discard_size));
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->root_offset, si->root_size));

	return (0);
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

	si = &_si;
	tmp = NULL;
	ret = 0;

	/* Crack the cookie. */
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
	if (si->alloc_offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", alloc=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", alloc=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->alloc_offset,
		    (uintmax_t)(si->alloc_offset + si->alloc_size),
		    si->alloc_size, si->alloc_cksum));
	if (si->avail_offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", avail=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", avail=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->avail_offset,
		    (uintmax_t)(si->avail_offset + si->avail_size),
		    si->avail_size, si->avail_cksum));
	if (si->discard_offset == WT_BLOCK_INVALID_OFFSET)
		WT_ERR(__wt_buf_catfmt(session, tmp, ", discard=[Empty]"));
	else
		WT_ERR(__wt_buf_catfmt(session, tmp,
		    ", discard=[%"
		    PRIuMAX "-%" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
		    (uintmax_t)si->discard_offset,
		    (uintmax_t)(si->discard_offset + si->discard_size),
		    si->discard_size, si->discard_cksum));
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
