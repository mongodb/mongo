/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __convert_to_dump --
 *	We have a scratch buffer where the data item contains a raw value,
 *	convert it to a dumpable string.
 */
static int
__convert_to_dump(WT_SESSION_IMPL *session, WT_BUF *buf)
{
	static const char hex[] = "0123456789abcdef";
	WT_BUF *tmp;
	uint32_t i, size;
	const uint8_t *p;
	uint8_t *t;

	if (buf->size == 0)
		return (0);

	WT_RET(__wt_scr_alloc(session, buf->size * 3, &tmp));
	for (p = buf->data,
	    i = buf->size, t = tmp->mem, size = 0; i > 0; --i, ++p)
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
	tmp->size = size;

	__wt_buf_swap(buf, tmp);
	__wt_scr_release(&tmp);

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
	dest = src = (uint8_t *)buf->data;
	end = src + buf->size;
	while (src < end) {
		if (*src == '\\') {
			if (src + 1 < end && *++src == '\\') {
				*dest++ = '\\';
				continue;
			}

			if (src + 2 > end) {
				__wt_errx(session, "Unexpected end of input "
				    "in an escaped value");
				return (EINVAL);
			}

			*dest = 0;
			for (i = 0; i < 2; i++) {
				*dest <<= 4;
				if ('0' <= *src && *src <= '9')
					*dest = (*src++ - '0');
				else if ('a' <= *src && *src <= 'f')
					*dest = (*src++ - 'a');
				else {
					__wt_errx(session,
					    "Invalid escaped value: "
					    "expecting a hexadecimal value");
					return (EINVAL);
				}
			}
		} else
			*dest++ = *src++;
	}

	buf->size = (uint32_t)(dest - (uint8_t *)buf->data);
	return (0);
}

/*
 * __curdump_get_key --
 *	WT_CURSOR->get_key for dump cursors.
 */
static int
__curdump_get_key(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_ITEM *key;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_key, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	if (F_ISSET(cursor, WT_CURSTD_PRINT))
		WT_RET(__convert_to_dump(session, &cursor->key));

	va_start(ap, cursor);
	key = va_arg(ap, WT_ITEM *);
	key->data = cursor->key.data;
	key->size = cursor->key.size;
	va_end(ap);

	API_END(session);
	return (0);
}

/*
 * __curdump_get_value --
 *	WT_CURSOR->get_value for dump cursors.
 */
static int
__curdump_get_value(WT_CURSOR *cursor, ...)
{
	WT_SESSION_IMPL *session;
	WT_ITEM *value;
	va_list ap;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))
		return ((cursor->saved_err != 0) ? cursor->saved_err : EINVAL);

	if (F_ISSET(cursor, WT_CURSTD_PRINT))
		WT_RET(__convert_to_dump(session, &cursor->value));
	va_start(ap, cursor);
	value = va_arg(ap, WT_ITEM *);
	value->data = cursor->value.data;
	value->size = cursor->value.size;
	va_end(ap);

	API_END(session);
	return (0);
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
__wt_curdump_init(WT_CURSOR *cursor, int printable)
{
	cursor->get_key = __curdump_get_key;
	cursor->get_value = __curdump_get_value;
	cursor->set_key = __curdump_set_key;
	cursor->set_value = __curdump_set_value;

	if (printable)
		F_SET(cursor, WT_CURSTD_PRINT);
}
