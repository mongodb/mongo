/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static const u_char hex[] = "0123456789abcdef";

/*
 * __wt_raw_to_hex --
 *	Convert a chunk of data to a printable hex string.
 */
void
__wt_raw_to_hex(const void *from, void *to, uint32_t *sizep)
{
	uint32_t i;
	const uint8_t *p;
	uint8_t *t, *t_start;

	for (p = from, t = t_start = to, i = *sizep; i > 0; --i, ++p) {
		*t++ = hex[(*p & 0xf0) >> 4];
		*t++ = hex[*p & 0x0f];
	}
	*t++ = '\0';
	*sizep = WT_PTRDIFF32(t, t_start);
}

/*
 * __wt_raw_to_esc_hex --
 *	Convert a chunk of data to an printable string using escaped hex as
 * necessary.
 */
void
__wt_raw_to_esc_hex(const void *from, void *to, uint32_t *sizep)
{
	uint32_t i;
	const uint8_t *p;
	uint8_t *t, *t_start;

	for (p = from, t = t_start = to, i = *sizep; i > 0; --i, ++p)
		if (isprint((int)*p)) {
			if (*p == '\\')
				*t++ = '\\';
			*t++ = *p;
		} else {
			*t++ = '\\';
			*t++ = hex[(*p & 0xf0) >> 4];
			*t++ = hex[*p & 0x0f];
		}
	*t++ = '\0';
	*sizep = WT_PTRDIFF32(t, t_start);
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

static int
__hex_fmterr(WT_SESSION_IMPL *session)
{
	WT_RET_MSG(session, EINVAL, "Invalid format in hexadecimal string");
}

/*
 * __wt_hex_to_raw --
 *	Convert a printable hex string to a chunk of data.
 */
int
__wt_hex_to_raw(
    WT_SESSION_IMPL *session, void *from, void *to, uint32_t *sizep)
{
	uint8_t *p, *t, *t_start;

	if (strlen(from) % 2 != 0)
		return (__hex_fmterr(session));

	for (p = from, t = t_start = to; *p != '\0'; p += 2, ++t)
		if (hex2byte(p, t))
			return (__hex_fmterr(session));

	*sizep = WT_PTRDIFF32(t, t_start);
	return (0);
}

/*
 * __wt_esc_hex_to_raw --
 *	Convert a printable string using escaped hex as necessary to a chunk
 * of data.
 */
int
__wt_esc_hex_to_raw(
    WT_SESSION_IMPL *session, void *from, void *to, uint32_t *sizep)
{
	uint8_t *p, *t, *t_start;

	for (p = from, t = t_start = to; *p != '\0'; ++p, ++t) {
		if ((*t = *p) != '\\')
			continue;
		++p;
		if (p[0] != '\\') {
			if (p[0] == '\0' || p[1] == '\0' || hex2byte(p, t))
				return (__hex_fmterr(session));
			++p;
		}
	}
	*sizep = WT_PTRDIFF32(t, t_start);
	return (0);
}
