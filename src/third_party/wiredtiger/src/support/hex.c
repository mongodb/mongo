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
static WT_INLINE void
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
 * __wt_log_data_dump --
 *     Log a preamble message followed by a hex dump of a data buffer in 1KB chunks.
 */
void
__wt_log_data_dump(WT_SESSION_IMPL *session, const void *data, size_t size, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((cold)) WT_GCC_FUNC_ATTRIBUTE((format(printf, 4, 5)))
{
    WT_DECL_ITEM(preamble);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    size_t chunk, i, nchunks;

    WT_ERR_MSG_CHK(session, __wt_scr_alloc(session, 0, &preamble), "preamble buffer allocation");
    WT_VA_ARGS_BUF_FORMAT(session, preamble, fmt, false);

    if (size == 0) {
        __wt_errx(session, "%.*s: empty buffer, no dump available", (int)preamble->size,
          (char *)preamble->data);
        goto err;
    }

    WT_ERR_MSG_CHK(session, __wt_scr_alloc(session, 4 * 1024, &tmp), "hex buffer allocation");

    nchunks = size / 1024 + (size % 1024 == 0 ? 0 : 1);
#define DATA_DUMP_MAX_SIZE (64 * 1024) /* Avoid run-away dumps. */
    for (chunk = i = 0;;) {
        WT_ERR_MSG_CHK(session, __wt_buf_catfmt(session, tmp, "%02x ", ((const uint8_t *)data)[i]),
          "hex format");
        if (++i >= size || i % 1024 == 0) {
            __wt_errx(session, "%.*s: (chunk %" WT_SIZET_FMT " of %" WT_SIZET_FMT "): %.*s",
              (int)preamble->size, (char *)preamble->data, ++chunk, nchunks, (int)tmp->size,
              (char *)tmp->data);
            if (i >= size)
                break;
            if (i >= DATA_DUMP_MAX_SIZE) {
                __wt_errx(session, "%.*s: data dump truncated after %" WT_SIZET_FMT " bytes",
                  (int)preamble->size, (char *)preamble->data, i);
                break;
            }
            WT_ERR_MSG_CHK(session, __wt_buf_set(session, tmp, "", 0), "hex buffer reset");
        }
    }

err:
    __wt_scr_free(session, &preamble);
    __wt_scr_free(session, &tmp);
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
 * __wti_hex2byte --
 *     Convert a pair of hex characters into a byte.
 */
int
__wti_hex2byte(const u_char *from, u_char *to)
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
        if (__wti_hex2byte(p, t))
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
            if (p[0] == '\0' || p[1] == '\0' || __wti_hex2byte(p, t))
                return (__hex_fmterr(session));
            ++p;
        }
    }
    to->size = WT_PTRDIFF(t, to->mem);
    return (0);
}
