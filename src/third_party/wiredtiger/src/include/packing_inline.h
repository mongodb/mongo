/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Throughout this code we have to be aware of default argument conversion.
 *
 * Refer to Chapter 8 of "Expert C Programming" by Peter van der Linden for the gory details. The
 * short version is that we have less cases to deal with because the compiler promotes shorter types
 * to int or unsigned int.
 */
typedef struct {
    union {
        int64_t i;
        uint64_t u;
        const char *s;
        WT_ITEM item;
    } u;
    uint32_t size;
    int8_t havesize;
    char type;
} WT_PACK_VALUE;

/* Default to size = 1 if there is no size prefix. */
#define WT_PACK_VALUE_INIT \
    {                      \
        {0}, 1, 0, 0       \
    }
#define WT_DECL_PACK_VALUE(pv) WT_PACK_VALUE pv = WT_PACK_VALUE_INIT

typedef struct {
    WT_SESSION_IMPL *session;
    const char *cur, *end, *orig;
    unsigned long repeats;
    WT_PACK_VALUE lastv;
} WT_PACK;

#define WT_PACK_INIT                                  \
    {                                                 \
        NULL, NULL, NULL, NULL, 0, WT_PACK_VALUE_INIT \
    }
#define WT_DECL_PACK(pack) WT_PACK pack = WT_PACK_INIT

typedef struct {
    WT_CONFIG config;
    char buf[20];
    int count;
    bool iskey;
    int genname;
} WT_PACK_NAME;

/*
 * __pack_initn --
 *     Initialize a pack iterator with the specified string and length.
 */
static WT_INLINE int
__pack_initn(WT_SESSION_IMPL *session, WT_PACK *pack, const char *fmt, size_t len)
{
    if (*fmt == '@' || *fmt == '<' || *fmt == '>')
        return (EINVAL);
    if (*fmt == '.') {
        ++fmt;
        if (len > 0)
            --len;
    }

    pack->session = session;
    pack->cur = pack->orig = fmt;
    pack->end = fmt + len;
    pack->repeats = 0;
    return (0);
}

/*
 * __pack_init --
 *     Initialize a pack iterator with the specified string.
 */
static WT_INLINE int
__pack_init(WT_SESSION_IMPL *session, WT_PACK *pack, const char *fmt)
{
    return (__pack_initn(session, pack, fmt, strlen(fmt)));
}

/*
 * __pack_name_init --
 *     Initialize the name of a pack iterator.
 */
static WT_INLINE void
__pack_name_init(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *names, bool iskey, WT_PACK_NAME *pn)
{
    WT_CLEAR(*pn);
    pn->iskey = iskey;

    if (names->str != NULL)
        __wt_config_subinit(session, &pn->config, names);
    else
        pn->genname = 1;
}

/*
 * __pack_name_next --
 *     Get the next field type from a pack iterator.
 */
static WT_INLINE int
__pack_name_next(WT_PACK_NAME *pn, WT_CONFIG_ITEM *name)
{
    WT_CONFIG_ITEM ignore;

    if (pn->genname) {
        WT_RET(
          __wt_snprintf(pn->buf, sizeof(pn->buf), (pn->iskey ? "key%d" : "value%d"), pn->count));
        WT_CLEAR(*name);
        name->str = pn->buf;
        name->len = strlen(pn->buf);
        /*
         * C++ treats nested structure definitions differently to C, as such we need to use scope
         * resolution to fully define the type.
         */
#ifdef __cplusplus
        name->type = WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING;
#else
        name->type = WT_CONFIG_ITEM_STRING;
#endif
        pn->count++;
    } else
        WT_RET(__wt_config_next(&pn->config, name, &ignore));

    return (0);
}

/*
 * __pack_next --
 *     Next pack iterator.
 */
static WT_INLINE int
__pack_next(WT_PACK *pack, WT_PACK_VALUE *pv)
{
    char *endsize;

    if (pack->repeats > 0) {
        *pv = pack->lastv;
        --pack->repeats;
        return (0);
    }

next:
    if (pack->cur == pack->end)
        return (WT_NOTFOUND);

    if (__wt_isdigit((u_char)*pack->cur)) {
        pv->havesize = 1;
        pv->size = WT_STORE_SIZE(strtoul(pack->cur, &endsize, 10));
        pack->cur = endsize;
    } else {
        pv->havesize = 0;
        pv->size = 1;
    }

    pv->type = *pack->cur++;
    pack->repeats = 0;

    switch (pv->type) {
    case 'S':
        return (0);
    case 's':
        if (pv->size < 1)
            WT_RET_MSG(pack->session, EINVAL,
              "Fixed length strings must be at least 1 byte in format '%.*s'",
              (int)(pack->end - pack->orig), pack->orig);
        return (0);
    case 'x':
        return (0);
    case 't':
        if (pv->size < 1 || pv->size > 8)
            WT_RET_MSG(pack->session, EINVAL,
              "Bitfield sizes must be between 1 and 8 bits in format '%.*s'",
              (int)(pack->end - pack->orig), pack->orig);
        return (0);
    case 'u':
        /* Special case for items with a size prefix. */
        pv->type = (!pv->havesize && *pack->cur != '\0') ? 'U' : 'u';
        return (0);
    case 'U':
        /*
         * Don't change the type. 'U' is used internally, so this type was already changed to
         * explicitly include the size.
         */
        return (0);
    case 'b':
    case 'h':
    case 'i':
    case 'B':
    case 'H':
    case 'I':
    case 'l':
    case 'L':
    case 'q':
    case 'Q':
    case 'r':
    case 'R':
        /* Integral types repeat <size> times. */
        if (pv->size == 0)
            goto next;
        pv->havesize = 0;
        pack->repeats = pv->size - 1;
        pack->lastv = *pv;
        return (0);
    default:
        WT_RET_MSG(pack->session, EINVAL, "Invalid type '%c' found in format '%.*s'", pv->type,
          (int)(pack->end - pack->orig), pack->orig);
    }
}

#define WT_PACK_GET(session, pv, ap)                                                   \
    do {                                                                               \
        WT_ITEM *__item;                                                               \
        switch ((pv).type) {                                                           \
        case 'x':                                                                      \
            break;                                                                     \
        case 's':                                                                      \
        case 'S':                                                                      \
            (pv).u.s = va_arg(ap, const char *);                                       \
            break;                                                                     \
        case 'U':                                                                      \
        case 'u':                                                                      \
            __item = va_arg(ap, WT_ITEM *);                                            \
            (pv).u.item.data = __item->data;                                           \
            (pv).u.item.size = __item->size;                                           \
            break;                                                                     \
        case 'b':                                                                      \
        case 'h':                                                                      \
        case 'i':                                                                      \
        case 'l':                                                                      \
            /* Use the int type as compilers promote smaller sizes to int for variadic \
             * arguments.                                                              \
             * Note: 'l' accommodates 4 bytes                                          \
             */                                                                        \
            (pv).u.i = va_arg(ap, int);                                                \
            break;                                                                     \
        case 'B':                                                                      \
        case 'H':                                                                      \
        case 'I':                                                                      \
        case 'L':                                                                      \
        case 't':                                                                      \
            /* Use the int type as compilers promote smaller sizes to int for variadic \
             * arguments.                                                              \
             * Note: 'L' accommodates 4 bytes                                          \
             */                                                                        \
            (pv).u.u = va_arg(ap, unsigned int);                                       \
            break;                                                                     \
        case 'q':                                                                      \
            (pv).u.i = va_arg(ap, int64_t);                                            \
            break;                                                                     \
        case 'Q':                                                                      \
        case 'r':                                                                      \
        case 'R':                                                                      \
            (pv).u.u = va_arg(ap, uint64_t);                                           \
            break;                                                                     \
        default:                                                                       \
            /* User format strings have already been validated. */                     \
            return (__wt_illegal_value(session, (pv).type));                           \
        }                                                                              \
    } while (0)

/*
 * __pack_size --
 *     Get the size of a packed value.
 */
static WT_INLINE int
__pack_size(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv, size_t *vp)
{
    size_t s, pad;

    switch (pv->type) {
    case 'x':
        *vp = pv->size;
        return (0);
    case 'j':
    case 'J':
    case 'K':
        /* These formats are only used internally. */
        if (pv->type == 'j' || pv->havesize)
            s = pv->size;
        else {
            ssize_t len;

            /* The string was previously validated. */
            len = __wt_json_strlen((const char *)pv->u.item.data, pv->u.item.size);
            if (len < 0)
                WT_RET_MSG(session, EINVAL, "invalid JSON string length in pack_size");
            s = (size_t)len + (pv->type == 'K' ? 0 : 1);
        }
        *vp = s;
        return (0);
    case 's':
    case 'S':
        if (pv->type == 's' || pv->havesize) {
            s = pv->size;
            if (s == 0)
                WT_RET_MSG(session, EINVAL, "zero-length string in pack_size");
        } else
            s = strlen(pv->u.s) + 1;
        *vp = s;
        return (0);
    case 'U':
    case 'u':
        s = pv->u.item.size;
        pad = 0;
        if (pv->havesize && pv->size < s)
            s = pv->size;
        else if (pv->havesize)
            pad = pv->size - s;
        if (pv->type == 'U')
            s += __wt_vsize_uint(s + pad);
        *vp = s + pad;
        return (0);
    case 'b':
    case 'B':
    case 't':
        *vp = 1;
        return (0);
    case 'h':
    case 'i':
    case 'l':
    case 'q':
        *vp = __wt_vsize_int(pv->u.i);
        return (0);
    case 'H':
    case 'I':
    case 'L':
    case 'Q':
    case 'r':
        *vp = __wt_vsize_uint(pv->u.u);
        return (0);
    case 'R':
        *vp = sizeof(uint64_t);
        return (0);
    }

    WT_RET_MSG(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
}

/*
 * __pack_write --
 *     Pack a value into a buffer.
 */
static WT_INLINE int
__pack_write(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv, uint8_t **pp, size_t maxlen)
{
    size_t s, pad;
    uint8_t *oldp;

    switch (pv->type) {
    case 'x':
        WT_SIZE_CHECK_PACK(pv->size, maxlen);
        memset(*pp, 0, pv->size);
        *pp += pv->size;
        break;
    case 's':
        WT_SIZE_CHECK_PACK(pv->size, maxlen);
        memcpy(*pp, pv->u.s, pv->size);
        *pp += pv->size;
        break;
    case 'S':
        /*
         * When preceded by a size, that indicates the maximum number of bytes the string can store,
         * this does not include the terminating NUL character. In a string with characters less
         * than the specified size, the remaining bytes are NULL padded.
         */
        if (pv->havesize) {
            s = __wt_strnlen(pv->u.s, pv->size);
            pad = (s < pv->size) ? pv->size - s : 0;
        } else {
            s = strlen(pv->u.s);
            pad = 1;
        }
        WT_SIZE_CHECK_PACK(s + pad, maxlen);
        if (s > 0)
            memcpy(*pp, pv->u.s, s);
        *pp += s;
        if (pad > 0) {
            memset(*pp, 0, pad);
            *pp += pad;
        }
        break;
    case 'j':
    case 'J':
    case 'K':
        /* These formats are only used internally. */
        s = pv->u.item.size;
        if ((pv->type == 'j' || pv->havesize) && pv->size < s) {
            s = pv->size;
            pad = 0;
        } else if (pv->havesize)
            pad = pv->size - s;
        else if (pv->type == 'K')
            pad = 0;
        else
            pad = 1;
        if (s > 0) {
            oldp = *pp;
            WT_RET(__wt_json_strncpy(
              (WT_SESSION *)session, (char **)pp, maxlen, (const char *)pv->u.item.data, s));
            maxlen -= (size_t)(*pp - oldp);
        }
        if (pad > 0) {
            WT_SIZE_CHECK_PACK(pad, maxlen);
            memset(*pp, 0, pad);
            *pp += pad;
        }
        break;
    case 'U':
    case 'u':
        s = pv->u.item.size;
        pad = 0;
        if (pv->havesize && pv->size < s)
            s = pv->size;
        else if (pv->havesize)
            pad = pv->size - s;
        if (pv->type == 'U') {
            oldp = *pp;
            /*
             * Check that there is at least one byte available: the low-level routines treat zero
             * length as unchecked.
             */
            WT_SIZE_CHECK_PACK(1, maxlen);
            WT_RET(__wt_vpack_uint(pp, maxlen, s + pad));
            maxlen -= (size_t)(*pp - oldp);
        }
        WT_SIZE_CHECK_PACK(s + pad, maxlen);
        if (s > 0)
            memcpy(*pp, pv->u.item.data, s);
        *pp += s;
        if (pad > 0) {
            memset(*pp, 0, pad);
            *pp += pad;
        }
        break;
    case 'b':
        /* Translate to maintain ordering with the sign bit. */
        WT_SIZE_CHECK_PACK(1, maxlen);
        **pp = (uint8_t)(pv->u.i + 0x80);
        *pp += 1;
        break;
    case 'B':
    case 't':
        WT_SIZE_CHECK_PACK(1, maxlen);
        **pp = (uint8_t)pv->u.u;
        *pp += 1;
        break;
    case 'h':
    case 'i':
    case 'l':
    case 'q':
        /*
         * Check that there is at least one byte available: the low-level routines treat zero length
         * as unchecked.
         */
        WT_SIZE_CHECK_PACK(1, maxlen);
        WT_RET(__wt_vpack_int(pp, maxlen, pv->u.i));
        break;
    case 'H':
    case 'I':
    case 'L':
    case 'Q':
    case 'r':
        /*
         * Check that there is at least one byte available: the low-level routines treat zero length
         * as unchecked.
         */
        WT_SIZE_CHECK_PACK(1, maxlen);
        WT_RET(__wt_vpack_uint(pp, maxlen, pv->u.u));
        break;
    case 'R':
        WT_SIZE_CHECK_PACK(sizeof(uint64_t), maxlen);
        *(uint64_t *)*pp = pv->u.u;
        *pp += sizeof(uint64_t);
        break;
    default:
        WT_RET_MSG(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
    }

    return (0);
}

/*
 * __unpack_read --
 *     Read a packed value from a buffer.
 */
static WT_INLINE int
__unpack_read(WT_SESSION_IMPL *session, WT_PACK_VALUE *pv, const uint8_t **pp, size_t maxlen)
{
    size_t s;

    switch (pv->type) {
    case 'x':
        WT_SIZE_CHECK_UNPACK(pv->size, maxlen);
        *pp += pv->size;
        break;
    case 's':
    case 'S':
        if (pv->type == 's' || pv->havesize) {
            s = pv->size;
            if (s == 0)
                WT_RET_MSG(session, EINVAL, "zero-length string in unpack_read");
        } else
            s = strlen((const char *)*pp) + 1;
        if (s > 0)
            pv->u.s = (const char *)*pp;
        WT_SIZE_CHECK_UNPACK(s, maxlen);
        *pp += s;
        break;
    case 'U':
        /*
         * Check that there is at least one byte available: the low-level routines treat zero length
         * as unchecked.
         */
        WT_SIZE_CHECK_UNPACK(1, maxlen);
        WT_RET(__wt_vunpack_uint(pp, maxlen, &pv->u.u));
    /* FALLTHROUGH */
    case 'u':
        if (pv->havesize)
            s = pv->size;
        else if (pv->type == 'U')
            s = (size_t)pv->u.u;
        else
            s = maxlen;
        WT_SIZE_CHECK_UNPACK(s, maxlen);
        pv->u.item.data = *pp;
        pv->u.item.size = s;
        *pp += s;
        break;
    case 'b':
        /* Translate to maintain ordering with the sign bit. */
        WT_SIZE_CHECK_UNPACK(1, maxlen);
        pv->u.i = (int8_t)(*(*pp)++ - 0x80);
        break;
    case 'B':
    case 't':
        WT_SIZE_CHECK_UNPACK(1, maxlen);
        pv->u.u = *(*pp)++;
        break;
    case 'h':
    case 'i':
    case 'l':
    case 'q':
        /*
         * Check that there is at least one byte available: the low-level routines treat zero length
         * as unchecked.
         */
        WT_SIZE_CHECK_UNPACK(1, maxlen);
        WT_RET(__wt_vunpack_int(pp, maxlen, &pv->u.i));
        break;
    case 'H':
    case 'I':
    case 'L':
    case 'Q':
    case 'r':
        /*
         * Check that there is at least one byte available: the low-level routines treat zero length
         * as unchecked.
         */
        WT_SIZE_CHECK_UNPACK(1, maxlen);
        WT_RET(__wt_vunpack_uint(pp, maxlen, &pv->u.u));
        break;
    case 'R':
        WT_SIZE_CHECK_UNPACK(sizeof(uint64_t), maxlen);
        pv->u.u = *(const uint64_t *)*pp;
        *pp += sizeof(uint64_t);
        break;
    default:
        WT_RET_MSG(session, EINVAL, "unknown pack-value type: %c", (int)pv->type);
    }

    return (0);
}

#define WT_UNPACK_PUT(session, pv, ap)                                              \
    do {                                                                            \
        WT_ITEM *__item;                                                            \
        switch ((pv).type) {                                                        \
        case 'x':                                                                   \
            break;                                                                  \
        case 's':                                                                   \
        case 'S':                                                                   \
            *va_arg(ap, const char **) = (pv).u.s;                                  \
            break;                                                                  \
        case 'U':                                                                   \
        case 'u':                                                                   \
            __item = va_arg(ap, WT_ITEM *);                                         \
            __item->data = (pv).u.item.data;                                        \
            __item->size = (pv).u.item.size;                                        \
            break;                                                                  \
        case 'b':                                                                   \
            *va_arg(ap, int8_t *) = (int8_t)(pv).u.i;                               \
            break;                                                                  \
        case 'h':                                                                   \
            *va_arg(ap, int16_t *) = (short)(pv).u.i;                               \
            break;                                                                  \
        case 'i':                                                                   \
        case 'l':                                                                   \
            *va_arg(ap, int32_t *) = (int32_t)(pv).u.i;                             \
            break;                                                                  \
        case 'q':                                                                   \
            *va_arg(ap, int64_t *) = (pv).u.i;                                      \
            break;                                                                  \
        case 'B':                                                                   \
        case 't':                                                                   \
            *va_arg(ap, uint8_t *) = (uint8_t)(pv).u.u;                             \
            break;                                                                  \
        case 'H':                                                                   \
            *va_arg(ap, uint16_t *) = (uint16_t)(pv).u.u;                           \
            break;                                                                  \
        case 'I':                                                                   \
        case 'L':                                                                   \
            *va_arg(ap, uint32_t *) = (uint32_t)(pv).u.u;                           \
            break;                                                                  \
        case 'Q':                                                                   \
        case 'r':                                                                   \
        case 'R':                                                                   \
            *va_arg(ap, uint64_t *) = (pv).u.u;                                     \
            break;                                                                  \
        default:                                                                    \
            __wt_err(session, EINVAL, "unknown unpack-put type: %c", (int)pv.type); \
            break;                                                                  \
        }                                                                           \
    } while (0)

/*
 * __wt_struct_packv --
 *     Pack a byte string (va_list version).
 */
static WT_INLINE int
__wt_struct_packv(WT_SESSION_IMPL *session, void *buffer, size_t size, const char *fmt, va_list ap)
{
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    uint8_t *p, *end;

    p = (uint8_t *)buffer;
    end = p + size;

    if (fmt[0] != '\0' && fmt[1] == '\0') {
        pv.type = fmt[0];
        WT_PACK_GET(session, pv, ap);
        return (__pack_write(session, &pv, &p, size));
    }

    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        WT_PACK_GET(session, pv, ap);
        WT_RET(__pack_write(session, &pv, &p, (size_t)(end - p)));
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Be paranoid - __pack_write should never overflow. */
    if (p > end)
        WT_RET_MSG(session, EINVAL, "buffer overflow in wt_struct_packv");

    return (0);
}

/*
 * __wt_struct_sizev --
 *     Calculate the size of a packed byte string (va_list version).
 */
static WT_INLINE int
__wt_struct_sizev(WT_SESSION_IMPL *session, size_t *sizep, const char *fmt, va_list ap)
{
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    size_t v;

    *sizep = 0;

    if (fmt[0] != '\0' && fmt[1] == '\0') {
        pv.type = fmt[0];
        WT_PACK_GET(session, pv, ap);
        return (__pack_size(session, &pv, sizep));
    }

    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        WT_PACK_GET(session, pv, ap);
        WT_RET(__pack_size(session, &pv, &v));
        *sizep += v;
    }
    WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __wt_struct_unpackv --
 *     Unpack a byte string (va_list version).
 */
static WT_INLINE int
__wt_struct_unpackv(
  WT_SESSION_IMPL *session, const void *buffer, size_t size, const char *fmt, va_list ap)
{
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    const uint8_t *p, *end;

    p = (uint8_t *)buffer;
    end = p + size;

    if (fmt[0] != '\0' && fmt[1] == '\0') {
        pv.type = fmt[0];
        WT_RET(__unpack_read(session, &pv, &p, size));
        WT_UNPACK_PUT(session, pv, ap);
        return (0);
    }

    WT_RET(__pack_init(session, &pack, fmt));
    while ((ret = __pack_next(&pack, &pv)) == 0) {
        WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
        WT_UNPACK_PUT(session, pv, ap);
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Be paranoid - __pack_write should never overflow. */
    if (p > end)
        WT_RET_MSG(session, EINVAL, "buffer overflow in wt_struct_unpackv");

    return (0);
}

/*
 * __wt_struct_size_adjust --
 *     Adjust the size field for a packed structure. Sometimes we want to include the size as a
 *     field in a packed structure. This is done by calling __wt_struct_size with the expected
 *     format and a size of zero. Then we want to pack the structure using the final size. This
 *     function adjusts the size appropriately (taking into account the size of the final size or
 *     the size field itself).
 */
static WT_INLINE void
__wt_struct_size_adjust(WT_SESSION_IMPL *session, size_t *sizep)
{
    size_t curr_size, field_size, prev_field_size;

    curr_size = *sizep;
    prev_field_size = 1;

    while ((field_size = __wt_vsize_uint(curr_size)) != prev_field_size) {
        curr_size += field_size - prev_field_size;
        prev_field_size = field_size;
    }

    /* Make sure the field size we calculated matches the adjusted size. */
    WT_ASSERT(session, field_size == __wt_vsize_uint(curr_size));

    *sizep = curr_size;
}
