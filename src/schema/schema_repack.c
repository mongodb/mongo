/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "intpack.i"
#include "packing.i"

/*
 * __wt_struct_repack --
 *	Given a cursor containing a valid key/value pair, repack some of the
 *	columns into a buffer.  This is used to construct index keys and column
 *	group values from table keys.
 */
int
__wt_struct_repack(WT_CURSOR *cursor, int *mapping, WT_BUF *dest)
{
	WT_ITEM item;
	WT_PACK_VALUE pv;
	WT_SESSION_IMPL *session;
	struct {
		WT_PACK pack;
		const char *format;
		const uint8_t *p, *end;
		int col;
	} kstuff, vstuff, *psrc;
	const char *s;
	const uint8_t *last;
	uint8_t *destp;
	uint64_t uval;
	int64_t ival;
	size_t len, size;
	int col, key_columns, ret;

	session = (WT_SESSION_IMPL *)cursor->session;

	/* Count the number of key columns. */
	WT_RET(__pack_init(session, &kstuff.pack, cursor->key_format));
	for (key_columns = 0; (ret = __pack_next(&kstuff.pack, &pv)) == 0;)
		++key_columns;
	WT_ASSERT(session, ret == WT_NOTFOUND);

	kstuff.format = cursor->key_format;
	kstuff.p = (const uint8_t *)cursor->key.data;
	kstuff.end = kstuff.p + cursor->key.size;
	vstuff.format = cursor->value_format;
	vstuff.p = (const uint8_t *)cursor->value.data;
	vstuff.end = vstuff.p + cursor->value.size;
	kstuff.col = vstuff.col = -1;

	dest->data = dest->mem;

	for (; *mapping != -1; ++mapping) {
		/* Is it a key or a value column? */
		if (*mapping < key_columns) {
			col = *mapping;
			psrc = &kstuff;
		} else {
			col = *mapping - key_columns;
			psrc = &vstuff;
		}

		/* Do we have to rewind? */
		if (col > psrc->col) {
			WT_RET(__pack_init(session, &psrc->pack, psrc->format));
			psrc->col = 0;
		}

		/* Skip to the specified column and extract the value. */
		while (psrc->col < col &&
		    (ret = __pack_next(&psrc->pack, &pv)) == 0) {
			last = psrc->p;
			switch (pv.type) {
			case 'x':
				psrc->p += pv.size;
				continue;
			case 's':
			case 'S':
				if (pv.type == 's' || pv.havesize)
					len = pv.size;
				else
					len = strlen(
					    (const char *)psrc->p) + 1;
				s = (const char *)psrc->p;
				psrc->p += len;
				break;
			case 'U':
				WT_RET(__wt_vunpack_uint(session, &psrc->p,
				    (size_t)(psrc->end - psrc->p), &uval));
				len = (size_t)uval;
				/* FALLTHROUGH */
			case 'u':
				if (pv.havesize)
					len = pv.size;
				else if (pv.type != 'U')
					len = (size_t)(psrc->end - psrc->p);
				item.data = psrc->p;
				item.size = (uint32_t)len;
				psrc->p += len;
				break;
			case 'b':
			case 'h':
			case 'i':
			case 'l':
			case 'q':
				WT_RET(__wt_vunpack_int(session, &psrc->p,
				    (size_t)(psrc->end - psrc->p), &ival));
				break;
			case 'r':
				/*
				 * Special handling if this is a column store
				 * cursor: get the record number directly, it
				 * isn't packed into the key item.
				 */
				if (psrc == &kstuff &&
				    strcmp(psrc->format, "r") == 0) {
					uval = cursor->recno;
					break;
				}
				/* FALLTHROUGH */
			case 'B':
			case 'H':
			case 'I':
			case 'L':
			case 'Q':
				WT_RET(__wt_vunpack_uint(session, &psrc->p,
				    (size_t)(psrc->end - psrc->p), &uval));
				break;
			}
			++psrc->col;
		}

		WT_ASSERT(session, psrc->col == col);

		/* How big was the item in the source? */
		size = (size_t)(last - psrc->p);

		/*
		 * Check whether we're moving a WT_ITEM from the end to the
		 * middle, or vice-versa.  This determines whether the size
		 * needs to be prepended.  This is the only case where the
		 * destination size can be larger than the source size.
		 */
		if (pv.type == 'u' && !pv.havesize && mapping[1] != -1) {
			pv.type = 'U';
			size += __wt_vsize_uint(len);
		} else if (pv.type == 'U' && mapping[1] == -1)
			pv.type = 'u';

		WT_RET(__wt_buf_grow(session, dest,
		    WT_PTRDIFF32(dest->data, dest->mem) + size));
		destp = (uint8_t *)dest->data;

		/* Append the value. */
		switch (pv.type) {
		case 'x':
			WT_ASSERT(session, pv.type != pv.type);
			break;
		case 's':
		case 'S':
			if (len > 0)
				memcpy(destp, s, len);
			destp += len;
			break;
		case 'U':
			WT_RET(__wt_vpack_uint(session, &destp, size, len));
			/* FALLTHROUGH */
		case 'u':
			if (item.size > 0)
				memcpy(destp, item.data, len);
			destp += len;
			break;
		case 'b':
		case 'h':
		case 'i':
		case 'l':
		case 'q':
			WT_RET(__wt_vpack_int(session, &destp, size, ival));
			break;
		case 'B':
		case 'H':
		case 'I':
		case 'L':
		case 'Q':
		case 'r':
			WT_RET(__wt_vpack_uint(session, &destp, size, uval));
			break;
		}

		dest->data = destp;
	}

	return (0);
}

/*
 * __wt_struct_reformat --
 *	Given key/value formats, map the columns into a new format string.
 *	This is used to construct index and column group formats for tables.
 *	The caller is responsible for freeing the result.
 */
int
__wt_struct_reformat(WT_SESSION_IMPL *session,
    const char *key_format, const char *value_format, int *mapping,
    char **result)
{
	WT_PACK_VALUE pv;
	struct {
		WT_PACK pack;
		const char *format;
		int col;
	} kstuff, vstuff, *psrc;
	size_t buf_len;
	char *buf, *end, *p;
	int col, key_columns, ret;

	/* Count the number of key columns. */
	WT_RET(__pack_init(session, &kstuff.pack, key_format));
	for (key_columns = 0; (ret = __pack_next(&kstuff.pack, &pv)) == 0;)
		++key_columns;
	WT_ASSERT(session, ret == WT_NOTFOUND);

	/*
	 * XXX Because of the nature of format strings, this length calculation
	 * may not be sufficient.  There is no way to know until we do the work:
	 * we may have to reallocate the buffer as we go.
	 */
	buf_len = strlen(key_format) + strlen(value_format) + 10;
	WT_RET(__wt_calloc_def(session, buf_len, &buf));
	p = buf;
	end = buf + buf_len;

	kstuff.format = key_format;
	vstuff.format = value_format;
	kstuff.col = vstuff.col = -1;

	for (; *mapping != -1; ++mapping) {
		/* Is it a key or a value column? */
		if (*mapping < key_columns) {
			col = *mapping;
			psrc = &kstuff;
		} else {
			col = *mapping - key_columns;
			psrc = &vstuff;
		}

		/* Do we have to rewind? */
		if (col > psrc->col) {
			WT_RET(__pack_init(session, &psrc->pack, psrc->format));
			psrc->col = 0;
		}

		/* Skip to the specified column and extract the type. */
		while (psrc->col < col &&
		    (ret = __pack_next(&psrc->pack, &pv)) == 0) {
			if (pv.type == 'x')
				continue;
			++psrc->col;
		}

		WT_ASSERT(session, psrc->col == col);

		/*
		 * Check whether we're moving an unsized WT_ITEM from the end
		 * to the middle, or vice-versa.  This determines whether the
		 * size needs to be prepended.  This is the only case where the
		 * destination size can be larger than the source size.
		 */
		if (pv.type == 'u' && !pv.havesize && mapping[1] != -1)
			pv.type = 'U';
		else if (pv.type == 'U' && mapping[1] == -1)
			pv.type = 'u';

		/* XXX check there is enough space! */

		if (pv.havesize)
			p += snprintf(p, (size_t)(end - p), "%d%c",
			    (int)pv.size, pv.type);
		else
			*p++ = pv.type;
	}

	WT_ASSERT(session, p < end);
	*result = buf;
	return (0);
}
