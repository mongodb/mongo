/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_project_in --
 *     Given list of cursors and a projection, read columns from the application into the dependent
 *     cursors.
 */
int
__wt_schema_project_in(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg, va_list ap)
{
    WT_CURSOR *c;
    WT_DECL_ITEM(buf);
    WT_DECL_PACK(pack);
    WT_DECL_PACK_VALUE(pv);
    WT_PACK_VALUE old_pv;
    size_t len, offset, old_len;
    u_long arg;
    uint8_t *p, *end;
    const uint8_t *next;
    char *proj;

    p = end = NULL; /* -Wuninitialized */

    /* Reset any of the buffers we will be setting. */
    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);
        if (*proj == WT_PROJ_KEY) {
            c = cp[arg];
            WT_RET(__wt_buf_init(session, &c->key, 0));
        } else if (*proj == WT_PROJ_VALUE) {
            c = cp[arg];
            WT_RET(__wt_buf_init(session, &c->value, 0));
        }
    }

    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);

        switch (*proj) {
        case WT_PROJ_KEY:
            c = cp[arg];
            if (WT_CURSOR_RECNO(c)) {
                c->key.data = &c->recno;
                c->key.size = sizeof(c->recno);
                WT_RET(__pack_init(session, &pack, "R"));
            } else
                WT_RET(__pack_init(session, &pack, c->key_format));
            buf = &c->key;
            end = p = (uint8_t *)buf->data;
            if (end != NULL)
                end += buf->size;
            continue;

        case WT_PROJ_VALUE:
            c = cp[arg];
            WT_RET(__pack_init(session, &pack, c->value_format));
            buf = &c->value;
            end = p = (uint8_t *)buf->data;
            if (end != NULL)
                end += buf->size;
            continue;
        }

        /* We have to get a key or value before any operations. */
        WT_ASSERT(session, buf != NULL);

        /*
         * Otherwise, the argument is a count, where a missing count means a count of 1.
         */
        for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
            switch (*proj) {
            case WT_PROJ_SKIP:
                WT_RET(__pack_next(&pack, &pv));
                /*
                 * A nasty case: if we are inserting out-of-order, we may reach the end of the data.
                 * That's okay: we want to append in that case, and we're positioned to do that.
                 */
                if (p == end) {
                    /* Set up an empty value. */
                    WT_CLEAR(pv.u);
                    if (pv.type == 'S' || pv.type == 's')
                        pv.u.s = "";

                    WT_RET(__pack_size(session, &pv, &len));
                    WT_RET(__wt_buf_grow(session, buf, buf->size + len));
                    p = (uint8_t *)buf->mem + buf->size;
                    WT_RET(__pack_write(session, &pv, &p, len));
                    buf->size += len;
                    end = (uint8_t *)buf->mem + buf->size;
                } else if (*proj == WT_PROJ_SKIP)
                    WT_RET(__unpack_read(session, &pv, (const uint8_t **)&p, (size_t)(end - p)));
                break;

            case WT_PROJ_NEXT:
                WT_RET(__pack_next(&pack, &pv));
                WT_PACK_GET(session, pv, ap);
                /* FALLTHROUGH */

            case WT_PROJ_REUSE:
                /* Read the item we're about to overwrite. */
                next = p;
                if (p < end) {
                    old_pv = pv;
                    WT_RET(__unpack_read(session, &old_pv, &next, (size_t)(end - p)));
                }
                old_len = (size_t)(next - p);

                WT_RET(__pack_size(session, &pv, &len));
                offset = WT_PTRDIFF(p, buf->mem);
                WT_RET(__wt_buf_grow(session, buf, (buf->size + len) - old_len));
                p = (uint8_t *)buf->mem + offset;
                end = (uint8_t *)buf->mem + (buf->size + len) - old_len;
                /* Make room if we're inserting out-of-order. */
                if (offset + old_len < buf->size)
                    memmove(p + len, p + old_len, buf->size - (offset + old_len));
                WT_RET(__pack_write(session, &pv, &p, len));
                buf->size += len - old_len;
                break;

            default:
                WT_RET_MSG(session, EINVAL, "unexpected projection plan: %c", (int)*proj);
            }
        }
    }

    return (0);
}

/*
 * __wt_schema_project_out --
 *     Given list of cursors and a projection, read columns from the dependent cursors and return
 *     them to the application.
 */
int
__wt_schema_project_out(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg, va_list ap)
{
    WT_CURSOR *c;
    WT_DECL_PACK(pack);
    WT_DECL_PACK_VALUE(pv);
    u_long arg;
    uint8_t *p, *end;
    char *proj;

    p = end = NULL; /* -Wuninitialized */

    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);

        switch (*proj) {
        case WT_PROJ_KEY:
            c = cp[arg];
            if (WT_CURSOR_RECNO(c)) {
                c->key.data = &c->recno;
                c->key.size = sizeof(c->recno);
                WT_RET(__pack_init(session, &pack, "R"));
            } else
                WT_RET(__pack_init(session, &pack, c->key_format));
            p = (uint8_t *)c->key.data;
            end = p + c->key.size;
            continue;

        case WT_PROJ_VALUE:
            c = cp[arg];
            WT_RET(__pack_init(session, &pack, c->value_format));
            p = (uint8_t *)c->value.data;
            end = p + c->value.size;
            continue;
        }

        /*
         * Otherwise, the argument is a count, where a missing count means a count of 1.
         */
        for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
            switch (*proj) {
            case WT_PROJ_NEXT:
            case WT_PROJ_SKIP:
            case WT_PROJ_REUSE:
                WT_RET(__pack_next(&pack, &pv));
                WT_RET(__unpack_read(session, &pv, (const uint8_t **)&p, (size_t)(end - p)));
                /* Only copy the value out once. */
                if (*proj != WT_PROJ_NEXT)
                    break;
                WT_UNPACK_PUT(session, pv, ap);
                break;
            }
        }
    }

    return (0);
}

/*
 * __wt_schema_project_slice --
 *     Given list of cursors and a projection, read columns from a raw buffer.
 */
int
__wt_schema_project_slice(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  bool key_only, const char *vformat, WT_ITEM *value)
{
    WT_CURSOR *c;
    WT_DECL_ITEM(buf);
    WT_DECL_PACK(pack);
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_PACK_VALUE(vpv);
    WT_PACK vpack;
    size_t len, offset, old_len;
    u_long arg;
    uint8_t *end, *p;
    const uint8_t *next, *vp, *vend;
    char *proj;
    bool skip;

    p = end = NULL; /* -Wuninitialized */

    WT_RET(__pack_init(session, &vpack, vformat));
    vp = value->data;
    vend = vp + value->size;

    /* Reset any of the buffers we will be setting. */
    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);
        if (*proj == WT_PROJ_KEY) {
            c = cp[arg];
            WT_RET(__wt_buf_init(session, &c->key, 0));
        } else if (*proj == WT_PROJ_VALUE && !key_only) {
            c = cp[arg];
            WT_RET(__wt_buf_init(session, &c->value, 0));
        }
    }

    skip = key_only;
    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);

        switch (*proj) {
        case WT_PROJ_KEY:
            skip = false;
            c = cp[arg];
            if (WT_CURSOR_RECNO(c)) {
                c->key.data = &c->recno;
                c->key.size = sizeof(c->recno);
                WT_RET(__pack_init(session, &pack, "R"));
            } else
                WT_RET(__pack_init(session, &pack, c->key_format));
            buf = &c->key;
            p = (uint8_t *)buf->data;
            end = p + buf->size;
            continue;

        case WT_PROJ_VALUE:
            skip = key_only;
            if (skip)
                continue;
            c = cp[arg];
            WT_RET(__pack_init(session, &pack, c->value_format));
            buf = &c->value;
            p = (uint8_t *)buf->data;
            end = p + buf->size;
            continue;
        }

        /* We have to get a key or value before any operations. */
        WT_ASSERT(session, skip || buf != NULL);

        /*
         * Otherwise, the argument is a count, where a missing count means a count of 1.
         */
        for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
            switch (*proj) {
            case WT_PROJ_SKIP:
                if (skip)
                    break;
                WT_RET(__pack_next(&pack, &pv));

                /*
                 * A nasty case: if we are inserting out-of-order, append a zero value to keep the
                 * buffer in the correct format.
                 */
                if (p == end) {
                    /* Set up an empty value. */
                    WT_CLEAR(pv.u);
                    if (pv.type == 'S' || pv.type == 's')
                        pv.u.s = "";

                    WT_RET(__pack_size(session, &pv, &len));
                    WT_RET(__wt_buf_grow(session, buf, buf->size + len));
                    p = (uint8_t *)buf->data + buf->size;
                    WT_RET(__pack_write(session, &pv, &p, len));
                    end = p;
                    buf->size += len;
                } else
                    WT_RET(__unpack_read(session, &pv, (const uint8_t **)&p, (size_t)(end - p)));
                break;

            case WT_PROJ_NEXT:
                WT_RET(__pack_next(&vpack, &vpv));
                WT_RET(__unpack_read(session, &vpv, &vp, (size_t)(vend - vp)));
                /* FALLTHROUGH */

            case WT_PROJ_REUSE:
                if (skip)
                    break;

                /*
                 * Read the item we're about to overwrite.
                 *
                 * There is subtlety here: the value format may not exactly match the cursor's
                 * format. In particular, we need lengths with raw columns in the middle of a packed
                 * struct, but not if they are at the end of a struct.
                 */
                WT_RET(__pack_next(&pack, &pv));

                next = p;
                if (p < end)
                    WT_RET(__unpack_read(session, &pv, &next, (size_t)(end - p)));
                old_len = (size_t)(next - p);

                /* Make sure the types are compatible. */
                WT_ASSERT(session, __wt_tolower((u_char)pv.type) == __wt_tolower((u_char)vpv.type));
                pv.u = vpv.u;

                WT_RET(__pack_size(session, &pv, &len));
                offset = WT_PTRDIFF(p, buf->data);
                /*
                 * Avoid growing the buffer if the value fits. This is not just a performance issue:
                 * it covers the case of record number keys, which have to be written to
                 * cursor->recno.
                 */
                if (len > old_len)
                    WT_RET(__wt_buf_grow(session, buf, buf->size + len - old_len));
                p = (uint8_t *)buf->data + offset;
                /* Make room if we're inserting out-of-order. */
                if (offset + old_len < buf->size)
                    memmove(p + len, p + old_len, buf->size - (offset + old_len));
                WT_RET(__pack_write(session, &pv, &p, len));
                buf->size += len - old_len;
                end = (uint8_t *)buf->data + buf->size;
                break;
            default:
                WT_RET_MSG(session, EINVAL, "unexpected projection plan: %c", (int)*proj);
            }
        }
    }

    return (0);
}

/*
 * __wt_schema_project_merge --
 *     Given list of cursors and a projection, build a buffer containing the column values read from
 *     the cursors.
 */
int
__wt_schema_project_merge(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  const char *vformat, WT_ITEM *value)
{
    WT_CURSOR *c;
    WT_DECL_PACK(pack);
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_PACK_VALUE(vpv);
    WT_ITEM *buf;
    WT_PACK vpack;
    size_t len;
    u_long arg;
    uint8_t *vp;
    const uint8_t *p, *end;
    char *proj;

    p = end = NULL; /* -Wuninitialized */

    WT_RET(__wt_buf_init(session, value, 0));
    WT_RET(__pack_init(session, &vpack, vformat));

    for (proj = (char *)proj_arg; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);

        switch (*proj) {
        case WT_PROJ_KEY:
            c = cp[arg];
            if (WT_CURSOR_RECNO(c)) {
                c->key.data = &c->recno;
                c->key.size = sizeof(c->recno);
                WT_RET(__pack_init(session, &pack, "R"));
            } else
                WT_RET(__pack_init(session, &pack, c->key_format));
            buf = &c->key;
            p = buf->data;
            end = p + buf->size;
            continue;

        case WT_PROJ_VALUE:
            c = cp[arg];
            WT_RET(__pack_init(session, &pack, c->value_format));
            buf = &c->value;
            p = buf->data;
            end = p + buf->size;
            continue;
        }

        /*
         * Otherwise, the argument is a count, where a missing count means a count of 1.
         */
        for (arg = (arg == 0) ? 1 : arg; arg > 0; arg--) {
            switch (*proj) {
            case WT_PROJ_NEXT:
            case WT_PROJ_SKIP:
            case WT_PROJ_REUSE:
                WT_RET(__pack_next(&pack, &pv));
                WT_RET(__unpack_read(session, &pv, &p, (size_t)(end - p)));
                /* Only copy the value out once. */
                if (*proj != WT_PROJ_NEXT)
                    break;

                WT_RET(__pack_next(&vpack, &vpv));
                /* Make sure the types are compatible. */
                WT_ASSERT(session, __wt_tolower((u_char)pv.type) == __wt_tolower((u_char)vpv.type));
                vpv.u = pv.u;
                WT_RET(__pack_size(session, &vpv, &len));
                WT_RET(__wt_buf_grow(session, value, value->size + len));
                vp = (uint8_t *)value->mem + value->size;
                WT_RET(__pack_write(session, &vpv, &vp, len));
                value->size += len;
                break;
            }
        }
    }

    return (0);
}
