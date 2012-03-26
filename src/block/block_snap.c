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
 * __wt_block_snap_init --
 *	Initialize a snapshot structure.
 */
int
__wt_block_snap_init(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_BLOCK_SNAPSHOT *si, int is_live)
{
	/*
	 * If we're loading a new live snapshot, there shouldn't be one
	 * already loaded.
	 */
	if (is_live && block->live_load)
		WT_RET_MSG(session, EINVAL, "snapshot already loaded");

	memset(si, 0, sizeof(*si));

	si->alloc.name = "alloc";
	si->alloc.offset = WT_BLOCK_INVALID_OFFSET;

	si->avail.name = "avail";
	si->avail.offset = WT_BLOCK_INVALID_OFFSET;

	si->discard.name = "discard";
	si->discard.offset = WT_BLOCK_INVALID_OFFSET;

	return (0);
}

/*
 * __wt_block_snap_load --
 *	Load a snapshot.
 */
int
__wt_block_snap_load(WT_SESSION_IMPL *session,
    WT_BLOCK *block, WT_ITEM *dsk, const uint8_t *addr, uint32_t addr_size,
    int readonly)
{
	WT_ITEM *tmp;
	WT_BLOCK_SNAPSHOT *si;
	int ret;

	tmp = NULL;
	ret = 0;

	if (addr == NULL)
		WT_VERBOSE(session, block, "load-snapshot: [Empty]");
	else
		WT_VERBOSE_CALL_RET(session, block,
		    __wt_block_snapshot_string(
		    session, block, addr, "load-snapshot", NULL));

	si = &block->live;
	WT_RET(__wt_block_snap_init(session, block, si, readonly ? 0 : 1));

	/* If not loading a snapshot from disk, we're done. */
	if (addr == NULL || addr_size == 0)
		goto done;

	/* Crack the snapshot cookie. */
	WT_ERR(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/*
	 * Verify has a fair amount of work to do when we load the snapshot,
	 * get it done.
	 */
	if (block->verify)
		WT_ERR(__wt_verify_snap_load(session, block, si));

	/* Read, and optionally verify, any root page. */
	if (si->root_offset == WT_BLOCK_INVALID_OFFSET)
		dsk->size = 0;
	else {
		WT_ERR(__wt_block_read(session, block,
		    dsk, si->root_offset, si->root_size, si->root_cksum));
		if (block->verify) {
			WT_ERR(__wt_block_snapshot_string(
			    session, block, addr, NULL, &tmp));
			WT_ERR(__wt_verify_addfrag(session,
			    block, si->root_offset, (off_t)si->root_size));
			WT_ERR(
			    __wt_verify_dsk(session, (char *)tmp->data, dsk));
		}
	}

	/*
	 * If the snapshot can be written, read the avail list (the list of
	 * blocks from which we can allocate on write).
	 */
	if (!readonly && si->avail.offset != WT_BLOCK_INVALID_OFFSET)
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

done:	if (!readonly)
		block->live_load = 1;

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

	/* Discard the extent lists. */
	__wt_block_extlist_free(session, &si->alloc);
	__wt_block_extlist_free(session, &si->avail);
	__wt_block_extlist_free(session, &si->discard);

	WT_RET(__wt_block_snap_init(session, block, si, 0));

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
		    &si->root_offset, &si->root_size, &si->root_cksum, 0));

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
	WT_RET(__wt_block_extlist_write(session, block, &si->alloc));
	__wt_block_extlist_free(session, &si->alloc);

	WT_RET(__wt_block_extlist_write(session, block, &si->discard));
	__wt_block_extlist_free(session, &si->discard);

	/* Write the available list, but don't discard it. */
	WT_RET(__wt_block_extlist_write(session, block, &si->avail));

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
	WT_BLOCK_SNAPSHOT *live, *si, __si;

	WT_VERBOSE_CALL_RET(session, block,
	    __wt_block_snapshot_string(
	    session, block, addr, "delete-snapshot", NULL));

	live = &block->live;
	si = &__si;
	WT_RET(__wt_block_snap_init(session, block, si, 0));

	/* If there's a snapshot, crack the cookie. */
	WT_RET(__wt_block_buffer_to_snapshot(session, block, addr, si));

	/*
	 * Free the root page: there's nothing special about this free, the root
	 * page is allocated using normal rules, that is, it may have been taken
	 * from the avail list, and was entered on the alloc list at that time.
	 */
	if (si->root_offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->root_offset, si->root_size, 0));

	/*
	 * Discard the avail list: snapshot avail lists are only useful if we
	 * are rolling forward from the particular snapshot and they represent
	 * our best understanding of what blocks can be allocated.  If we're
	 * not operating on the live snapshot, subsequent snapshots may have
	 * allocated those blocks, and the avail list is useless.
	 *
	 * The avail list is an extent: extent blocks must be freed directly to
	 * the live system's avail list, they were never on any alloc list.
	 */
	if (si->avail.offset != WT_BLOCK_INVALID_OFFSET)
		WT_RET(__wt_block_free(
		    session, block, si->avail.offset, si->avail.size, 1));

	/*
	 * Migrate the allocation and deletion lists forward, in this case,
	 * into the live system.  This is done by (1) first reading the
	 * extent list, (2) merging into the corresponding live system's
	 * extent list, (3) deleting the free list and (4) discarding the
	 * blocks that made up the list.
	 *
	 * The alloc and discard lists are extents: extent blocks must be freed
	 * directly to the live system's avail list, they were never on any
	 * alloc list.
	 */
	if (si->alloc.offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, &si->alloc));
		WT_RET(__wt_block_extlist_merge(
		    session, &si->alloc, &live->alloc));
		__wt_block_extlist_free(session, &si->alloc);
		WT_RET(__wt_block_free(
		    session, block, si->alloc.offset, si->alloc.size, 1));
	}
	if (si->discard.offset != WT_BLOCK_INVALID_OFFSET) {
		WT_RET(__wt_block_extlist_read(session, block, &si->discard));
		WT_RET(__wt_block_extlist_merge(
		    session, &si->discard, &live->discard));
		__wt_block_extlist_free(session, &si->discard);
		WT_RET(__wt_block_free(
		    session, block, si->discard.offset, si->discard.size, 1));
	}

	/*
	 * Figure out which blocks we can re-use.   This is done by checking
	 * the live system's allocate and discard lists for overlaps: if an
	 * extent appears on both lists, move it to the avail list, it can be
	 * re-used immediately.
	 */
	WT_RET(__wt_block_extlist_match(session, block, live));

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
