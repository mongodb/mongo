/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_sb_alloc --
 *	Allocate memory from the SESSION's buffer and fill it in.
 */
int
__wt_sb_alloc(SESSION *session, size_t size, void *retp, SESSION_BUFFER **sbp)
{
	SESSION_BUFFER *sb;
	uint32_t align_size, alloc_size;
	int single_use;

	WT_ASSERT(session, size < UINT32_MAX);

	/*
	 * Allocate memory for an insert or change; there's a buffer in the
	 * SESSION structure for allocation of chunks of memory to hold changed
	 * or inserted values.
	 *
	 * We align allocations because we directly access WT_UPDATE structure
	 * fields in the memory (the x86 handles unaligned accesses, but I don't
	 * want to have to find and fix this code for a port to a system that
	 * doesn't handle unaligned accesses).  It wastes space, but this memory
	 * is never written to disk and there are fewer concerns about memory
	 * than with on-disk structures.  Any other code allocating memory from
	 * this buffer needs to align its allocations as well.
	 *
	 * The first thing in each chunk of memory is a SESSION_BUFFER structure
	 * (which we check is a multiple of 4B during initialization); then one
	 * or more WT_UPDATE structure plus value chunk pairs.
	 *
	 * XXX
	 * Figure out how much space we need: this code limits the maximum size
	 * of a data item stored in the file.  In summary, for a big item we
	 * have to store a SESSION_BUFFER structure, the WT_UPDATE structure and
	 * the data, all in an allocated buffer.   We only pass a 32-bit value
	 * to our allocation routine, so we can't store an item bigger than the
	 * maximum 32-bit value minus the sizes of those two structures, where
	 * the WT_UPDATE structure and data item are aligned to a 32-bit
	 * boundary.  We could fix this, but it's unclear it's worth the effort
	 * -- document you can store a (4GB - 20B) item max, and you're done,
	 * because it's insane to store a 4GB item in the file anyway.
	 *
	 * Check first we won't overflow when calculating an aligned size, then
	 * check the total required space for this item.
	 */
	if (size > UINT32_MAX - (sizeof(WT_UPDATE) + sizeof(uint32_t)))
		return (__wt_file_item_too_big(session));
	align_size = WT_ALIGN(size + sizeof(WT_UPDATE), sizeof(uint32_t));
	if (align_size > UINT32_MAX - sizeof(SESSION_BUFFER))
		return (__wt_file_item_too_big(session));

	/* If we already have a buffer and the data fits, we're done. */
	sb = session->sb;
	if (sb != NULL && align_size <= sb->space_avail)
		goto no_allocation;

	/*
	 * We start by allocating 4KB for the thread, then every time we have
	 * to re-allocate the buffer, we double the allocation size, up to a
	 * total of 256KB, that way any thread that is doing a lot of updates
	 * doesn't keep churning through memory.
	 */
	if (session->update_alloc_size == 0)
		session->update_alloc_size = 4 * 1024;

	/*
	 * Decide how much memory to allocate: if it's a one-off (that is, the
	 * value is bigger than anything we'll aggregate into these buffers,
	 * it's a one-off.  Otherwise, allocate the next power-of-two larger
	 * than 4 times the requested size and at least the default buffer size.
	 */
	if (align_size > session->update_alloc_size) {
		alloc_size = WT_SIZEOF32(SESSION_BUFFER) + align_size;
		single_use = 1;
	} else {
		if (session->update_alloc_size < 256 * 1024)
			session->update_alloc_size =
			    __wt_nlpo2(session->update_alloc_size);
		alloc_size = session->update_alloc_size;
		single_use = 0;
	}

	WT_RET(__wt_calloc(session, 1, alloc_size, &sb));
	sb->len = alloc_size;
	sb->space_avail = alloc_size - WT_SIZEOF32(SESSION_BUFFER);
	sb->first_free = (uint8_t *)sb + sizeof(SESSION_BUFFER);

	/*
	 * If it's a single use allocation, ignore any current SESSION buffer.
	 * Else, release the old SESSION buffer and replace it with the new one.
	 */
	if (!single_use) {
		/*
		 * The "in" reference count is artificially incremented by 1 as
		 * long as a SESSION buffer is referenced by the SESSION thread;
		 * we don't want them freed because a page was evicted and the
		 * count went to 0.  Decrement the reference count on the buffer
		 * as part of releasing it.  There's a similar reference count
		 * decrement when the SESSION structure is discarded.
		 *
		 * XXX
		 * There's a race here: if this code, or the SESSION structure
		 * close code, and the page discard code race, it's possible
		 * neither will realize the buffer is no longer needed and free
		 * it.  The fix is to involve the eviction or workQ threads:
		 * they may need a linked list of buffers they review to ensure
		 * it never happens.  I'm living with this now: it's unlikely
		 * and it's a memory leak if it ever happens.
		 */
		if (session->sb != NULL)
			--session->sb->in;
		session->sb = sb;

		sb->in = 1;
	}

no_allocation:
	*(void **)retp = sb->first_free;
	*sbp = sb;

	sb->first_free += align_size;
	sb->space_avail -= align_size;
	++sb->in;

	return (0);
}

/*
 * __wt_sb_free_error --
 *	Free a chunk of data from the SESSION_BUFFER, in an error path.
 */
void
__wt_sb_free_error(SESSION *session, SESSION_BUFFER *sb)
{
	/*
	 * It's possible we allocated a WT_UPDATE structure and associated item
	 * memory from the SESSION buffer, but then an error occurred.  Don't
	 * try and clean up the SESSION buffer, it's simpler to decrement the
	 * use count and let the page discard code deal with it during the
	 * page reconciliation process.  (Note we're still in the allocation
	 * path, so we decrement the "in" field, instead of incrementing the
	 * "out" field -- if the eviction thread updates that field, we could
	 * race.
	 */
	--sb->in;

	/*
	 * One other thing: if the SESSION buffer was a one-off, we have to free
	 * it here, it's not linked to any WT_PAGE in the system.
	 */
	if (sb->in == 0)
		__wt_free(session, sb);
}
