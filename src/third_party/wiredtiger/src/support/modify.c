/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_modify_pack --
 *	Pack a modify structure into a buffer.
 */
int
__wt_modify_pack(WT_SESSION_IMPL *session,
    WT_ITEM **modifyp, WT_MODIFY *entries, int nentries)
{
	WT_ITEM *modify;
	size_t len, *p;
	uint8_t *data;
	int i;

	/*
	 * Build the in-memory modify value. It's the entries count, followed
	 * by the modify structure offsets written in order, followed by the
	 * data (data at the end to minimize unaligned reads/writes).
	 */
	len = sizeof(size_t);                           /* nentries */
	for (i = 0; i < nentries; ++i) {
		len += 3 * sizeof(size_t);              /* WT_MODIFY fields */
		len += entries[i].data.size;            /* data */
	}

	WT_RET(__wt_scr_alloc(session, len, &modify));

	data = (uint8_t *)modify->mem +
	    sizeof(size_t) + ((size_t)nentries * 3 * sizeof(size_t));
	p = modify->mem;
	*p++ = (size_t)nentries;
	for (i = 0; i < nentries; ++i) {
		*p++ = entries[i].data.size;
		*p++ = entries[i].offset;
		*p++ = entries[i].size;

		memcpy(data, entries[i].data.data, entries[i].data.size);
		data += entries[i].data.size;
	}
	modify->size = WT_PTRDIFF(data, modify->data);
	*modifyp = modify;
	return (0);
}

/*
 * __modify_apply_one --
 *	Apply a single modify structure change to the buffer.
 */
static int
__modify_apply_one(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
    size_t data_size, size_t offset, size_t size, const uint8_t *data)
{
	WT_ITEM *value;
	size_t len;
	uint8_t *from, *to;
	bool sformat;

	value = &cursor->value;
	sformat = cursor->value_format[0] == 'S';

	/*
	 * Grow the buffer to the maximum size we'll need. This is pessimistic
	 * because it ignores replacement bytes, but it's a simpler calculation.
	 *
	 * Grow the buffer before we fast-path the expected case. This function
	 * is often called using a cursor buffer referencing on-page memory and
	 * it's easy to overwrite a page. A side-effect of growing the buffer is
	 * to ensure the buffer's value is in buffer-local memory.
	 *
	 * Because the buffer may reference an overflow item, the data may not
	 * start at the start of the buffer's memory and we have to correct for
	 * that.
	 */
	len = WT_DATA_IN_ITEM(value) ? WT_PTRDIFF(value->data, value->mem) : 0;
	WT_RET(__wt_buf_grow(session, value,
	    len + WT_MAX(value->size, offset) + data_size));

	/*
	 * Fast-path the expected case, where we're overwriting a set of bytes
	 * that already exist in the buffer.
	 */
	if (value->size > offset + data_size && data_size == size) {
		memmove((uint8_t *)value->data + offset, data, data_size);
		return (0);
	}

	/*
	 * Decrement the size to discard the trailing nul (done after growing
	 * the buffer to ensure it can be restored without further checking).
	 */
	if (sformat)
		--value->size;

	/*
	 * If appending bytes past the end of the value, initialize gap bytes
	 * and copy the new bytes into place.
	 */
	if (value->size <= offset) {
		if (value->size < offset)
			memset((uint8_t *)value->data + value->size,
			    sformat ? ' ' : 0, offset - value->size);
		memmove((uint8_t *)value->data + offset, data, data_size);
		value->size = offset + data_size;

		/* Restore the trailing nul. */
		if (sformat)
			((char *)value->data)[value->size++] = '\0';
		return (0);
	}

	/*
	 * Correct the replacement size if it's nonsense, we can't replace more
	 * bytes than remain in the value. (Nonsense sizes are permitted in the
	 * API because we don't want to handle the errors.)
	 */
	if (value->size < offset + size)
		size = value->size - offset;

	if (data_size == size) {			/* Overwrite */
		/* Copy in the new data. */
		memmove((uint8_t *)value->data + offset, data, data_size);

		/*
		 * The new data must overlap the buffer's end (else, we'd use
		 * the fast-path code above). Set the buffer size to include
		 * the new data.
		 */
		value->size = offset + data_size;
	} else {					/* Shrink or grow */
		/* Move trailing data forward/backward to its new location. */
		from = (uint8_t *)value->data + (offset + size);
		WT_ASSERT(session, WT_DATA_IN_ITEM(value) &&
		    from + (value->size - (offset + size)) <=
		    (uint8_t *)value->mem + value->memsize);
		to = (uint8_t *)value->data + (offset + data_size);
		WT_ASSERT(session, WT_DATA_IN_ITEM(value) &&
		    to + (value->size - (offset + size)) <=
		    (uint8_t *)value->mem + value->memsize);
		memmove(to, from, value->size - (offset + size));

		/* Copy in the new data. */
		memmove((uint8_t *)value->data + offset, data, data_size);

		/*
		 * Correct the size. This works because of how the C standard
		 * defines unsigned arithmetic, and gcc7 complains about more
		 * verbose forms:
		 *
		 *	if (data_size > size)
		 *		value->size += (data_size - size);
		 *	else
		 *		value->size -= (size - data_size);
		 *
		 * because the branches are identical.
		 */
		 value->size += (data_size - size);
	}

	/* Restore the trailing nul. */
	if (sformat)
		((char *)value->data)[value->size++] = '\0';

	return (0);
}

/*
 * __wt_modify_apply_api --
 *	Apply a single set of WT_MODIFY changes to a buffer, the cursor API
 * interface.
 */
int
__wt_modify_apply_api(WT_SESSION_IMPL *session,
    WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
    WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
	int i;

	for (i = 0; i < nentries; ++i)
		WT_RET(__modify_apply_one(session, cursor, entries[i].data.size,
		    entries[i].offset, entries[i].size, entries[i].data.data));

	return (0);
}

/*
 * __wt_modify_apply --
 *	Apply a single set of WT_MODIFY changes to a buffer.
 */
int
__wt_modify_apply(
    WT_SESSION_IMPL *session, WT_CURSOR *cursor, const void *modify)
{
	size_t data_size, nentries, offset, size;
	const size_t *p;
	const uint8_t *data;

	/*
	 * Get the number of entries, and set a second pointer to reference the
	 * change data. The modify string isn't necessarily aligned for size_t
	 * access, copy to be sure.
	 */
	p = modify;
	memcpy(&nentries, p++, sizeof(size_t));
	data = (uint8_t *)modify +
	    sizeof(size_t) + (nentries * 3 * sizeof(size_t));

	/* Step through the list of entries, applying them in order. */
	for (; nentries-- > 0; data += data_size) {
		memcpy(&data_size, p++, sizeof(size_t));
		memcpy(&offset, p++, sizeof(size_t));
		memcpy(&size, p++, sizeof(size_t));
		WT_RET(__modify_apply_one(
		    session, cursor, data_size, offset, size, data));
	}

	return (0);
}
