/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __fill_hex --
 *     In-memory conversion of raw bytes to a hexadecimal representation.
 */
static inline void
__fill_hex(const uint8_t *src, size_t src_max, uint8_t *dest, size_t dest_max, size_t *lenp)
{
    uint8_t *dest_orig;

    dest_orig = dest;
    if (dest_max > 0) /* save a byte for nul-termination */
        --dest_max;
    for (; src_max > 0 && dest_max > 1; src_max -= 1, dest_max -= 2, ++src) {
        *dest++ = __wt_hex((*src & 0xf0) >> 4);
        *dest++ = __wt_hex(*src & 0x0f);
    }
    *dest++ = '\0';
    if (lenp != NULL)
        *lenp = WT_PTRDIFF(dest, dest_orig);
}

/*
 * __wt_fill_hex --
 *     In-memory conversion of raw bytes to a hexadecimal representation.
 */
void
__wt_fill_hex(const uint8_t *src, size_t src_max, uint8_t *dest, size_t dest_max, size_t *lenp)
{
    __fill_hex(src, src_max, dest, dest_max, lenp);
}

/*
 * __wt_raw_to_hex --
 *     Convert a chunk of data to a nul-terminated printable hex string.
 */
int
__wt_raw_to_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
{
    size_t len;

    /*
     * Every byte takes up 2 spaces, plus a trailing nul byte.
     */
    len = size * 2 + 1;
    WT_RET(__wt_buf_init(session, to, len));

    __fill_hex(from, size, to->mem, len, &to->size);
    return (0);
}

/*
 * __wt_raw_to_esc_hex --
 *     Convert a chunk of data to a nul-terminated printable string using escaped hex, as necessary.
 */
int
__wt_raw_to_esc_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
{
    size_t i;
    const uint8_t *p;
    u_char *t;

    /*
     * In the worst case, every character takes up 3 spaces, plus a trailing nul byte.
     */
    WT_RET(__wt_buf_init(session, to, size * 3 + 1));

    for (p = from, t = to->mem, i = size; i > 0; --i, ++p)
        if (__wt_isprint((u_char)*p)) {
            if (*p == '\\')
                *t++ = '\\';
            *t++ = *p;
        } else {
            *t++ = '\\';
            *t++ = __wt_hex((*p & 0xf0) >> 4);
            *t++ = __wt_hex(*p & 0x0f);
        }
    *t++ = '\0';
    to->size = WT_PTRDIFF(t, to->mem);
    return (0);
}

/*
 * __wt_hex2byte --
 *     Convert a pair of hex characters into a byte.
 */
int
__wt_hex2byte(const u_char *from, u_char *to)
{
    uint8_t byte;

    switch (from[0]) {
    case '0':
        byte = 0;
        break;
    case '1':
        byte = 1 << 4;
        break;
    case '2':
        byte = 2 << 4;
        break;
    case '3':
        byte = 3 << 4;
        break;
    case '4':
        byte = 4 << 4;
        break;
    case '5':
        byte = 5 << 4;
        break;
    case '6':
        byte = 6 << 4;
        break;
    case '7':
        byte = 7 << 4;
        break;
    case '8':
        byte = 8 << 4;
        break;
    case '9':
        byte = 9 << 4;
        break;
    case 'A':
        byte = 10 << 4;
        break;
    case 'B':
        byte = 11 << 4;
        break;
    case 'C':
        byte = 12 << 4;
        break;
    case 'D':
        byte = 13 << 4;
        break;
    case 'E':
        byte = 14 << 4;
        break;
    case 'F':
        byte = 15 << 4;
        break;
    case 'a':
        byte = 10 << 4;
        break;
    case 'b':
        byte = 11 << 4;
        break;
    case 'c':
        byte = 12 << 4;
        break;
    case 'd':
        byte = 13 << 4;
        break;
    case 'e':
        byte = 14 << 4;
        break;
    case 'f':
        byte = 15 << 4;
        break;
    default:
        return (1);
    }

    switch (from[1]) {
    case '0':
        break;
    case '1':
        byte |= 1;
        break;
    case '2':
        byte |= 2;
        break;
    case '3':
        byte |= 3;
        break;
    case '4':
        byte |= 4;
        break;
    case '5':
        byte |= 5;
        break;
    case '6':
        byte |= 6;
        break;
    case '7':
        byte |= 7;
        break;
    case '8':
        byte |= 8;
        break;
    case '9':
        byte |= 9;
        break;
    case 'A':
        byte |= 10;
        break;
    case 'B':
        byte |= 11;
        break;
    case 'C':
        byte |= 12;
        break;
    case 'D':
        byte |= 13;
        break;
    case 'E':
        byte |= 14;
        break;
    case 'F':
        byte |= 15;
        break;
    case 'a':
        byte |= 10;
        break;
    case 'b':
        byte |= 11;
        break;
    case 'c':
        byte |= 12;
        break;
    case 'd':
        byte |= 13;
        break;
    case 'e':
        byte |= 14;
        break;
    case 'f':
        byte |= 15;
        break;
    default:
        return (1);
    }
    *to = byte;
    return (0);
}

/*
 * __hex_fmterr --
 *     Hex format error message.
 */
static int
__hex_fmterr(WT_SESSION_IMPL *session)
{
    WT_RET_MSG(session, EINVAL, "Invalid format in hexadecimal string");
}

/*
 * __wt_hex_to_raw --
 *     Convert a nul-terminated printable hex string to a chunk of data.
 */
int
__wt_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
{
    return (__wt_nhex_to_raw(session, from, strlen(from), to));
}

/*
 * __wt_nhex_to_raw --
 *     Convert a printable hex string to a chunk of data.
 */
int
__wt_nhex_to_raw(WT_SESSION_IMPL *session, const char *from, size_t size, WT_ITEM *to)
{
    u_char *t;
    const u_char *p;

    if (size % 2 != 0)
        return (__hex_fmterr(session));

    WT_RET(__wt_buf_init(session, to, size / 2));

    for (p = (u_char *)from, t = to->mem; size > 0; p += 2, size -= 2, ++t)
        if (__wt_hex2byte(p, t))
            return (__hex_fmterr(session));

    to->size = WT_PTRDIFF(t, to->mem);
    return (0);
}

/*
 * __wt_esc_hex_to_raw --
 *     Convert a printable string, encoded in escaped hex, to a chunk of data.
 */
int
__wt_esc_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
{
    u_char *t;
    const u_char *p;

    WT_RET(__wt_buf_init(session, to, strlen(from)));

    for (p = (u_char *)from, t = to->mem; *p != '\0'; ++p, ++t) {
        if ((*t = *p) != '\\')
            continue;
        ++p;
        if (p[0] != '\\') {
            if (p[0] == '\0' || p[1] == '\0' || __wt_hex2byte(p, t))
                return (__hex_fmterr(session));
            ++p;
        }
    }
    to->size = WT_PTRDIFF(t, to->mem);
    return (0);
}
