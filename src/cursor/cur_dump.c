/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __raw_to_dump --
 *	We have a buffer where the data item contains a raw value, convert it
 * to a printable string.
 */
static int
__raw_to_dump(WT_CURSOR *cursor, WT_ITEM *from, WT_BUF *to)
{
	static const u_char hex[] = "0123456789abcdef";
	WT_SESSION_IMPL *session;
	WT_BUF *tmp;
	uint32_t i, size;
	const uint8_t *p;
	uint8_t *t;

	session = (WT_SESSION_IMPL *)cursor->session;

	if (from->size == 0) {
		to->size = 0;
		return (0);
	}

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
			*t++ = '\\';
			*t++ = hex[(*p & 0xf0) >> 4];
			*t++ = hex[*p & 0x0f];
			size += 3;
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
 * convert_from_dump --
 *	We have a scratch buffer where the data item contains a dump string,
 *	convert it to a raw value.
 */
static int
__convert_from_dump(WT_SESSION_IMPL *session, WT_BUF *buf)
{
	uint8_t *dest, *end, *src;
	int i;

	WT_UNUSED(session);

	if (buf->size == 0)
		return (0);

	/*
	 * Overwrite in place: the converted string will always be smaller
	 * than the printable representation, so dest <= src in this loop.
	 */
	src = dest = (uint8_t *)buf->data;
	end = src + buf->size;
	for (; src < end; ++src, ++dest) {
		if (*src == '\\') {
			if (src < end && src[1] == '\\') {
				++src;
				*dest = '\\';
				continue;
			}

			if (src + 2 >= end) {
				__wt_errx(session,
				    "Unexpected end of input in an escaped "
				    "value");
				return (EINVAL);
			}

			*dest = 0;
			for (i = 0; i < 2; i++) {
				*dest <<= 4;
				switch (*++src) {
				case '0': *dest |= 0; break;
				case '1': *dest |= 1; break;
				case '2': *dest |= 2; break;
				case '3': *dest |= 3; break;
				case '4': *dest |= 4; break;
				case '5': *dest |= 5; break;
				case '6': *dest |= 6; break;
				case '7': *dest |= 7; break;
				case '8': *dest |= 8; break;
				case '9': *dest |= 9; break;
				case 'a': *dest |= 10; break;
				case 'b': *dest |= 11; break;
				case 'c': *dest |= 12; break;
				case 'd': *dest |= 13; break;
				case 'e': *dest |= 14; break;
				case 'f': *dest |= 15; break;
				default:
					__wt_errx(session,
					    "Invalid escaped value: expecting "
					    "a hexadecimal value");
					return (EINVAL);
				}
			}
		} else
			*dest = *src;
	}

	buf->size = WT_PTRDIFF32(dest, buf->data);
	return (0);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	int key_recno, ret;
	uint64_t recno;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_key, NULL);

	key_recno =
	    cursor->key_format[0] == 'r' &&
	    cursor->key_format[1] == '\0' ? 1 : 0;

	if (!key_recno || F_ISSET(cursor, WT_CURSTD_RAW)) {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &item));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &item));

		WT_ERR(__raw_to_dump(cursor, &item, &cursor->key));

		va_start(ap, cursor);
		*va_arg(ap, const char **) = cursor->key.data;
		va_end(ap);
	} else {
		if (F_ISSET(cursor, WT_CURSTD_TABLE))
			WT_ERR(__wt_curtable_get_key(cursor, &recno));
		else
			WT_ERR(__wt_cursor_get_key(cursor, &recno));

		va_start(ap, cursor);
		*va_arg(ap, uint64_t *) = recno;
		va_end(ap);
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	WT_ITEM item;
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (F_ISSET(cursor, WT_CURSTD_TABLE))
		WT_ERR(__wt_curtable_get_value(cursor, &item));
	else
		WT_ERR(__wt_cursor_get_value(cursor, &item));

	WT_ERR(__raw_to_dump(cursor, &item, &cursor->value));

	va_start(ap, cursor);
	*va_arg(ap, const char **) = cursor->value.data;
	va_end(ap);

err:	API_END(session);
	return (ret);
}

/*
 * __curdump_set_key --
 *	WT_CURSOR->set_key for dump cursors.
 */
static void
__curdump_set_key(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, set_key, NULL);

	va_start(ap, cursor);
	*(WT_ITEM *)&cursor->key = *va_arg(ap, WT_ITEM *);

	if ((ret = __convert_from_dump(session, &cursor->key)) == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_KEY_RAW);
	else {
		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_KEY_SET);
	}
	va_end(ap);

	API_END(session);
}

/*
 * __curdump_set_value --
 *	WT_CURSOR->set_value for dump cursors.
 */
static void
__curdump_set_value(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	va_list ap;
	int ret;

	CURSOR_API_CALL(cursor, session, set_value, NULL);

	va_start(ap, cursor);
	*(WT_ITEM *)&cursor->value = *va_arg(ap, WT_ITEM *);

	if ((ret = __convert_from_dump(session, &cursor->value)) == 0)
		F_SET(cursor, WT_CURSTD_VALUE_SET | WT_CURSTD_VALUE_RAW);
	else {
		cursor->saved_err = ret;
		F_CLR(cursor, WT_CURSTD_VALUE_SET);
	}
	va_end(ap);

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
