/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * raw_to_dump --
 *	We have a buffer where the data item contains a raw value, convert it
 * to a printable string.
 */
static int
raw_to_dump(WT_CURSOR *cursor, WT_ITEM *from, WT_BUF *to)
{
	static const u_char hex[] = "0123456789abcdef";
	WT_SESSION_IMPL *session;
	WT_BUF *tmp;
	uint32_t i, size;
	const uint8_t *p;
	uint8_t *t;

	session = (WT_SESSION_IMPL *)cursor->session;

	/*
	 * In the worst case, every character takes up 3 spaces, plus a
	 * trailing nul byte.
	 */
	WT_RET(__wt_scr_alloc(session, from->size * 3 + 10, &tmp));

	p = from->data;
	t = tmp->mem;
	size = 0;
	if (F_ISSET(cursor, WT_CURSTD_DUMP_HEX))
		for (i = from->size; i > 0; --i, ++p) {
			*t++ = hex[(*p & 0xf0) >> 4];
			*t++ = hex[*p & 0x0f];
			size += 2;
		}
	else
		for (i = from->size; i > 0; --i, ++p)
			if (isprint((int)*p)) {
				if (*p == '\\') {
					*t++ = '\\';
					++size;
				}
				*t++ = *p;
				++size;
			} else {
				*t++ = '\\';
				*t++ = hex[(*p & 0xf0) >> 4];
				*t++ = hex[*p & 0x0f];
				size += 3;
			}
	*t++ = '\0';
	++size;
	tmp->size = size;

	__wt_buf_swap(to, tmp);
	__wt_scr_free(&tmp);

	return (0);
}

/*
 * hex2byte --
 *	Convert a pair of hex characters into a byte.
 */
static inline int
hex2byte(uint8_t *from, uint8_t *to)
{
	uint8_t byte;

	switch (from[0]) {
	case '0': byte = 0; break;
	case '1': byte = 1 << 4; break;
	case '2': byte = 2 << 4; break;
	case '3': byte = 3 << 4; break;
	case '4': byte = 4 << 4; break;
	case '5': byte = 5 << 4; break;
	case '6': byte = 6 << 4; break;
	case '7': byte = 7 << 4; break;
	case '8': byte = 8 << 4; break;
	case '9': byte = 9 << 4; break;
	case 'a': byte = 10 << 4; break;
	case 'b': byte = 11 << 4; break;
	case 'c': byte = 12 << 4; break;
	case 'd': byte = 13 << 4; break;
	case 'e': byte = 14 << 4; break;
	case 'f': byte = 15 << 4; break;
	default:
		return (1);
	}

	switch (from[1]) {
	case '0': break;
	case '1': byte |= 1; break;
	case '2': byte |= 2; break;
	case '3': byte |= 3; break;
	case '4': byte |= 4; break;
	case '5': byte |= 5; break;
	case '6': byte |= 6; break;
	case '7': byte |= 7; break;
	case '8': byte |= 8; break;
	case '9': byte |= 9; break;
	case 'a': byte |= 10; break;
	case 'b': byte |= 11; break;
	case 'c': byte |= 12; break;
	case 'd': byte |= 13; break;
	case 'e': byte |= 14; break;
	case 'f': byte |= 15; break;
	default:
		return (1);
	}
	*to = byte;
	return (0);
}

/*
 * dump_to_raw --
 *	We have a scratch buffer where the data item contains a dump string,
 *	convert it to a raw value.
 */
static int
dump_to_raw(WT_CURSOR *cursor, const char *src_arg, WT_ITEM *item)
{
	WT_BUF *tmp;
	WT_SESSION_IMPL *session;
	uint8_t *p, *t;

	session = (WT_SESSION_IMPL *)cursor->session;

	/*
	 * XXX
	 * Overwrite the string in place: the underlying cursor set_key and
	 * set_value functions are going to use the cursor's key and value
	 * buffers, which means we can't.  This should probably be fixed by
	 * layering the dump cursor on top of other cursors and then we can
	 * use the dump cursor's key/value buffers.
	 */
	p = t = (uint8_t *)src_arg;
	if (F_ISSET(cursor, WT_CURSTD_DUMP_HEX)) {
		if (strlen(src_arg) % 2 != 0)
			goto format;
		for (; *p != '\0'; p += 2, ++t)
			if (hex2byte(p, t))
				goto hexerr;
	} else
		for (; *p != '\0'; ++p, ++t) {
			if ((*t = *p) != '\\')
				continue;
			++p;
			if (p[0] != '\\') {
				if (p[0] == '\0' || p[1] == '\0')
					goto format;
				if (hex2byte(p, t))
					goto hexerr;
				++p;
			}
		}

	memset(item, 0, sizeof(WT_ITEM));
	item->data = src_arg;
	item->size = WT_PTRDIFF32(t, src_arg);
	return (0);

hexerr:	__wt_errx(session,
	    "Invalid escaped value: expecting a hexadecimal value");

	if (0) {
format:		__wt_errx(
		    session, "Unexpected end of input in an escaped value");
	}

	__wt_scr_free(&tmp);
	return (EINVAL);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	int ret;
	uint64_t recno;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (WT_CURSOR_RECNO(cursor) && !F_ISSET(cursor, WT_CURSTD_RAW)) {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &recno));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &recno));

		WT_ERR(__wt_buf_fmt(session, &cursor->key, "%" PRIu64, recno));
	} else {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &item));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &item));

		WT_ERR(raw_to_dump(cursor, &item, &cursor->key));
	}

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		itemp = va_arg(ap, WT_ITEM *);
		itemp->data = cursor->key.data;
		itemp->size = cursor->key.size;
	} else
		*va_arg(ap, const char **) = cursor->key.data;
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * str2recno --
 *	Convert a string to a record number.
 */
static int
str2recno(WT_SESSION_IMPL *session, const char *p, uint64_t *recnop)
{
	uint64_t recno;
	char *endptr;

	/*
	 * strtouq takes lots of things like hex values, signs and so on and so
	 * forth -- none of them are OK with us.  Check the string starts with
	 * digit, that turns off the special processing.
	 */
	if (!isdigit(p[0]))
		goto format;

	errno = 0;
	recno = strtouq(p, &endptr, 0);
	if (recno == ULLONG_MAX && errno == ERANGE) {
		__wt_err(session, ERANGE, "%s: invalid record number", p);
		return (ERANGE);
	}
	if (endptr[0] != '\0') {
format:		__wt_errx(session, "%s: invalid record number", p);
		return (EINVAL);
	}
	*recnop = recno;
	return (0);
}

/*
 * __curdump_set_key --
 *	WT_CURSOR->set_key for dump cursors.
 */
static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	va_list ap;
	const char *p;
	int ret;

	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	if (WT_CURSOR_RECNO(cursor) && !F_ISSET(cursor, WT_CURSTD_RAW)) {
		WT_ERR(str2recno(session, p, &recno));

		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			__wt_curtable_set_key(cursor, recno);
		else
			__wt_cursor_set_key(cursor, recno);
	} else {
		WT_ERR(dump_to_raw(cursor, p, &item));

		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			__wt_curtable_set_key(cursor, &item);
		else
			__wt_cursor_set_key(cursor, &item);
	}

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}

	API_END(session);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM item, *itemp;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (F_ISSET(cursor, WT_CURSTD_TABLE))
		WT_ERR(__wt_curtable_get_value(cursor, &item));
	else
		WT_ERR(__wt_cursor_get_value(cursor, &item));

	WT_ERR(raw_to_dump(cursor, &item, &cursor->value));

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW)) {
		itemp = va_arg(ap, WT_ITEM *);
		itemp->data = cursor->value.data;
		itemp->size = cursor->value.size;
	} else
		*va_arg(ap, const char **) = cursor->value.data;
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * __curdump_set_value --
 *	WT_CURSOR->set_value for dump cursors.
 */
static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;
	const char *p;

	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	if (F_ISSET(cursor, WT_CURSTD_RAW))
		p = va_arg(ap, WT_ITEM *)->data;
	else
		p = va_arg(ap, const char *);
	va_end(ap);

	WT_ERR(dump_to_raw(cursor, p, &item));

	if (F_ISSET(cursor, WT_CURSTD_TABLE))
		__wt_curtable_set_value(cursor, &item);
	else
		__wt_cursor_set_value(cursor, &item);

	if (0) {
err:		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
	}

	API_END(session);
}

/*
 * __wt_curdump_init --
 *	initialize a dump cursor.
 */
void
__wt_curdump_init(WT_CURSOR *cursor)
{
	cursor->get_key = __curdump_get_key;
	cursor->get_value = __curdump_get_value;
	cursor->set_key = __curdump_set_key;
	cursor->set_value = __curdump_set_value;
}
