/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __cursor_config_debug(WT_CURSOR *cursor, const char *cfg[]);

/*
 * __wti_cursor_noop --
 *     Cursor noop.
 */
int
__wti_cursor_noop(WT_CURSOR *cursor)
{
    WT_UNUSED(cursor);

    return (0);
}

/*
 * __wt_cursor_cached --
 *     No actions on a closed and cached cursor are allowed.
 */
int
__wt_cursor_cached(WT_CURSOR *cursor)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);
    WT_RET_MSG(session, ENOTSUP, "Cursor has been closed");
}

/*
 * __wt_cursor_notsup --
 *     Unsupported cursor actions.
 */
int
__wt_cursor_notsup(WT_CURSOR *cursor)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);
    WT_RET_MSG(session, ENOTSUP, "Unsupported cursor operation");
}

/*
 * __wti_cursor_get_value_notsup --
 *     WT_CURSOR.get_value not-supported.
 */
int
__wti_cursor_get_value_notsup(WT_CURSOR *cursor, ...)
{
    return (__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_get_raw_key_value_notsup --
 *     WT_CURSOR.get_raw_key_value not-supported.
 */
int
__wti_cursor_get_raw_key_value_notsup(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value)
{
    WT_UNUSED(key);
    WT_UNUSED(value);
    return (__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_set_key_notsup --
 *     WT_CURSOR.set_key not-supported.
 */
void
__wti_cursor_set_key_notsup(WT_CURSOR *cursor, ...)
{
    WT_IGNORE_RET(__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_set_value_notsup --
 *     WT_CURSOR.set_value not-supported.
 */
void
__wti_cursor_set_value_notsup(WT_CURSOR *cursor, ...)
{
    WT_IGNORE_RET(__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_compare_notsup --
 *     Unsupported cursor comparison.
 */
int
__wti_cursor_compare_notsup(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_UNUSED(b);
    WT_UNUSED(cmpp);

    return (__wt_cursor_notsup(a));
}

/*
 * __wti_cursor_equals_notsup --
 *     Unsupported cursor equality.
 */
int
__wti_cursor_equals_notsup(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
{
    WT_UNUSED(other);
    WT_UNUSED(equalp);

    return (__wt_cursor_notsup(cursor));
}

/*
 * Cursor modify operation is supported only in limited cursor types and also the modify
 * operation is supported only with 'S' and 'u' value formats of the cursors. Because of
 * the conditional support of cursor modify operation, To provide a better error description
 * to the application whenever the cursor modify is used based on the cursor types, two
 * default not supported functions are used.
 *
 * __wt_cursor_modify_notsup - Default function for cursor types where the modify operation
 * is not supported.
 *
 * __wti_cursor_modify_value_format_notsup - Default function for cursor types where the modify
 * operation is supported with specific value formats of the cursor.
 */

/*
 * __wt_cursor_modify_notsup --
 *     Unsupported cursor modify.
 */
int
__wt_cursor_modify_notsup(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_UNUSED(entries);
    WT_UNUSED(nentries);

    return (__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_modify_value_format_notsup --
 *     Unsupported value format for cursor modify.
 */
int
__wti_cursor_modify_value_format_notsup(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_SESSION_IMPL *session;

    WT_UNUSED(entries);
    WT_UNUSED(nentries);

    if (cursor->value_format != NULL && strlen(cursor->value_format) != 0) {
        session = CUR2S(cursor);
        WT_RET_MSG(
          session, ENOTSUP, "WT_CURSOR.modify only supported for 'S' and 'u' value formats");
    }
    return (__wt_cursor_notsup(cursor));
}

/*
 * __wt_cursor_search_near_notsup --
 *     Unsupported cursor search-near.
 */
int
__wt_cursor_search_near_notsup(WT_CURSOR *cursor, int *exact)
{
    WT_UNUSED(exact);

    return (__wt_cursor_notsup(cursor));
}

/*
 * __wt_cursor_config_notsup --
 *     Unsupported cursor API call which takes config.
 */
int
__wt_cursor_config_notsup(WT_CURSOR *cursor, const char *config)
{
    WT_UNUSED(config);

    return (__wt_cursor_notsup(cursor));
}

/*
 * __wt_cursor_reopen_notsup --
 *     Unsupported cursor reopen.
 */
int
__wt_cursor_reopen_notsup(WT_CURSOR *cursor, bool check_only)
{
    WT_UNUSED(check_only);

    return (__wt_cursor_notsup(cursor));
}

/*
 * __wti_cursor_set_notsup --
 *     Reset the cursor methods to not-supported.
 */
void
__wti_cursor_set_notsup(WT_CURSOR *cursor)
{
    /*
     * Set cursor methods other than close, reconfigure and reset, to fail. Close is unchanged so
     * the cursor can be discarded; reset is set to a no-op because session transactional operations
     * reset all of the cursors in a session. Reconfigure is left open in case it's possible in the
     * future to change these configurations.
     */
    cursor->bound = __wt_cursor_config_notsup;
    cursor->compare = __wti_cursor_compare_notsup;
    cursor->insert = __wt_cursor_notsup;
    cursor->modify = __wt_cursor_modify_notsup;
    cursor->next = __wt_cursor_notsup;
    cursor->prev = __wt_cursor_notsup;
    cursor->remove = __wt_cursor_notsup;
    cursor->reserve = __wt_cursor_notsup;
    cursor->reset = __wti_cursor_noop;
    cursor->search = __wt_cursor_notsup;
    cursor->search_near = __wt_cursor_search_near_notsup;
    cursor->update = __wt_cursor_notsup;
}

/*
 * __wt_cursor_kv_not_set --
 *     Standard error message for key/values not set.
 */
int
__wt_cursor_kv_not_set(WT_CURSOR *cursor, bool key) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);

    WT_RET_MSG(session, cursor->saved_err == 0 ? EINVAL : cursor->saved_err, "requires %s be set",
      key ? "key" : "value");
}

/*
 * __wt_cursor_copy_release_item --
 *     Release memory used by the key or value item in cursor copy debug mode.
 */
int
__wt_cursor_copy_release_item(WT_CURSOR *cursor, WT_ITEM *item) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);

    /* Bail out if the item has been cleared. */
    if (item->data == NULL)
        return (0);

    /*
     * Whether or not we own the memory for the item, make a copy of the data and use that instead.
     * That allows us to overwrite and free memory owned by the item, potentially uncovering
     * programming errors related to retaining pointers to key/value memory beyond API boundaries.
     */
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_buf_set(session, tmp, item->data, item->size));
    __wt_explicit_overwrite(item->mem, item->memsize);
    __wt_buf_free(session, item);
    WT_ERR(__wt_buf_set(session, item, tmp->data, tmp->size));
err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_cursor_get_key --
 *     WT_CURSOR->get_key default implementation.
 */
int
__wt_cursor_get_key(WT_CURSOR *cursor, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, cursor);
    ret = __wti_cursor_get_keyv(cursor, cursor->flags, ap);
    va_end(ap);
    return (ret);
}

/*
 * __wt_cursor_set_key --
 *     WT_CURSOR->set_key default implementation.
 */
void
__wt_cursor_set_key(WT_CURSOR *cursor, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, cursor);
    if ((ret = __wti_cursor_set_keyv(cursor, cursor->flags, ap)) != 0)
        WT_IGNORE_RET(__wt_panic(CUR2S(cursor), ret, "failed to set key"));
    va_end(ap);
}

/*
 * __wt_cursor_get_raw_key --
 *     Temporarily force raw mode in a cursor to get a canonical copy of the key.
 */
int
__wt_cursor_get_raw_key(WT_CURSOR *cursor, WT_ITEM *key)
{
    WT_DECL_RET;
    bool raw_set;

    raw_set = F_ISSET(cursor, WT_CURSTD_RAW);
    if (!raw_set)
        F_SET(cursor, WT_CURSTD_RAW);
    ret = cursor->get_key(cursor, key);
    if (!raw_set)
        F_CLR(cursor, WT_CURSTD_RAW);
    return (ret);
}

/*
 * __wt_cursor_set_raw_key --
 *     Temporarily force raw mode in a cursor to set a canonical copy of the key.
 */
void
__wt_cursor_set_raw_key(WT_CURSOR *cursor, WT_ITEM *key)
{
    bool raw_set;

    raw_set = F_ISSET(cursor, WT_CURSTD_RAW);
    if (!raw_set)
        F_SET(cursor, WT_CURSTD_RAW);
    cursor->set_key(cursor, key);
    if (!raw_set)
        F_CLR(cursor, WT_CURSTD_RAW);
}

/*
 * __wt_cursor_get_raw_value --
 *     Temporarily force raw mode in a cursor to get a canonical copy of the value.
 */
int
__wt_cursor_get_raw_value(WT_CURSOR *cursor, WT_ITEM *value)
{
    WT_DECL_RET;
    bool raw_set;

    raw_set = F_ISSET(cursor, WT_CURSTD_RAW);
    if (!raw_set)
        F_SET(cursor, WT_CURSTD_RAW);
    ret = cursor->get_value(cursor, value);
    if (!raw_set)
        F_CLR(cursor, WT_CURSTD_RAW);
    return (ret);
}

/*
 * __wt_cursor_set_raw_value --
 *     Temporarily force raw mode in a cursor to set a canonical copy of the value.
 */
void
__wt_cursor_set_raw_value(WT_CURSOR *cursor, WT_ITEM *value)
{
    bool raw_set;

    raw_set = F_ISSET(cursor, WT_CURSTD_RAW);
    if (!raw_set)
        F_SET(cursor, WT_CURSTD_RAW);
    cursor->set_value(cursor, value);
    if (!raw_set)
        F_CLR(cursor, WT_CURSTD_RAW);
}

/*
 * __wti_cursor_get_keyv --
 *     WT_CURSOR->get_key worker function.
 */
int
__wti_cursor_get_keyv(WT_CURSOR *cursor, uint64_t flags, va_list ap)
{
    WT_DECL_RET;
    WT_ITEM *key;
    WT_SESSION_IMPL *session;
    size_t size;
    const char *fmt;

    CURSOR_API_CALL(cursor, session, ret, get_key, NULL);
    if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))
        WT_ERR(__wt_cursor_kv_not_set(cursor, true));

    /* Force an allocated copy when using cursor copy debug. */
    if (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
        WT_ERR(__wt_buf_grow(session, &cursor->key, cursor->key.size));
        F_SET(cursor, WT_CURSTD_DEBUG_COPY_KEY);
    }

    if (WT_CURSOR_RECNO(cursor)) {
        if (LF_ISSET(WT_CURSTD_RAW)) {
            key = va_arg(ap, WT_ITEM *);
            key->data = cursor->raw_recno_buf;
            WT_ERR(__wt_struct_size(session, &size, "q", cursor->recno));
            key->size = size;
            ret = __wt_struct_pack(
              session, cursor->raw_recno_buf, sizeof(cursor->raw_recno_buf), "q", cursor->recno);
        } else
            *va_arg(ap, uint64_t *) = cursor->recno;
    } else {
        /*
         * Fast path some common cases. The case we care most about being fast is "u", check that
         * first.
         */
        fmt = cursor->key_format;
        if (WT_STREQ(fmt, "u") || LF_ISSET(WT_CURSOR_RAW_OK)) {
            key = va_arg(ap, WT_ITEM *);
            key->data = cursor->key.data;
            key->size = cursor->key.size;
        } else if (WT_STREQ(fmt, "S"))
            *va_arg(ap, const char **) = cursor->key.data;
        else
            ret = __wt_struct_unpackv(session, cursor->key.data, cursor->key.size, fmt, ap);
    }

err:
    API_END_RET_STAT(session, ret, cursor_get_key);
}

/*
 * __wti_cursor_set_keyv --
 *     WT_CURSOR->set_key default implementation.
 */
int
__wti_cursor_set_keyv(WT_CURSOR *cursor, uint64_t flags, va_list ap)
{
    WT_DECL_RET;
    WT_ITEM *buf, *item, tmp;
    WT_SESSION_IMPL *session;
    size_t sz;
    const char *fmt, *str;
    va_list ap_copy;

    buf = &cursor->key;
    tmp.mem = NULL;

    CURSOR_API_CALL(cursor, session, ret, set_key, NULL);
    WT_ERR(__cursor_copy_release(cursor));
    if (F_ISSET(cursor, WT_CURSTD_KEY_SET) && WT_DATA_IN_ITEM(buf)) {
        tmp = *buf;
        buf->mem = NULL;
        buf->memsize = 0;
    }

    F_CLR(cursor, WT_CURSTD_KEY_SET);

    if (WT_CURSOR_RECNO(cursor)) {
        if (LF_ISSET(WT_CURSTD_RAW)) {
            item = va_arg(ap, WT_ITEM *);
            WT_ERR(__wt_struct_unpack(session, item->data, item->size, "q", &cursor->recno));
        } else
            cursor->recno = va_arg(ap, uint64_t);
        if (cursor->recno == WT_RECNO_OOB)
            WT_ERR_MSG(session, EINVAL, "%d is an invalid record number", WT_RECNO_OOB);
        buf->data = &cursor->recno;
        sz = sizeof(cursor->recno);
    } else {
        /*
         * Fast path some common cases and special case WT_ITEMs. The case we care most about being
         * fast is "u", check that first.
         */
        fmt = cursor->key_format;
        if (WT_STREQ(fmt, "u") || LF_ISSET(WT_CURSOR_RAW_OK | WT_CURSTD_DUMP_JSON)) {
            item = va_arg(ap, WT_ITEM *);
            sz = item->size;
            buf->data = item->data;
        } else if (WT_STREQ(fmt, "S")) {
            str = va_arg(ap, const char *);
            sz = strlen(str) + 1;
            buf->data = (void *)str;
        } else {
            va_copy(ap_copy, ap);
            ret = __wt_struct_sizev(session, &sz, cursor->key_format, ap_copy);
            va_end(ap_copy);
            WT_ERR(ret);

            WT_ERR(__wt_buf_initsize(session, buf, sz));
            WT_ERR(__wt_struct_packv(session, buf->mem, sz, cursor->key_format, ap));
        }
    }
    if (sz == 0)
        WT_ERR_MSG(session, EINVAL, "Empty keys not permitted");
    else if ((uint32_t)sz != sz)
        WT_ERR_MSG(session, EINVAL, "Key size (%" PRIu64 ") out of range", (uint64_t)sz);
    cursor->saved_err = 0;
    buf->size = sz;
    F_SET(cursor, WT_CURSTD_KEY_EXT);
    if (0) {
err:
        cursor->saved_err = ret;
    }

    /*
     * If we copied the key, either put the memory back into the cursor, or if we allocated some
     * memory in the meantime, free it.
     */
    if (tmp.mem != NULL) {
        if (buf->mem == NULL && !FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
            buf->mem = tmp.mem;
            buf->memsize = tmp.memsize;
            F_SET(cursor, WT_CURSTD_DEBUG_COPY_KEY);
        } else
            __wt_free(session, tmp.mem);
    }
    API_END_RET(session, ret);
}

/*
 * __wt_cursor_get_value --
 *     WT_CURSOR->get_value default implementation.
 */
int
__wt_cursor_get_value(WT_CURSOR *cursor, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, cursor);
    ret = __wti_cursor_get_valuev(cursor, ap);
    va_end(ap);
    return (ret);
}

/*
 * __wti_cursor_get_valuev --
 *     WT_CURSOR->get_value worker implementation.
 */
int
__wti_cursor_get_valuev(WT_CURSOR *cursor, va_list ap)
{
    WT_DECL_RET;
    WT_ITEM *value;
    WT_SESSION_IMPL *session;
    const char *fmt;

    CURSOR_API_CALL(cursor, session, ret, get_value, NULL);

    if (!F_ISSET(cursor, WT_CURSTD_VALUE_EXT | WT_CURSTD_VALUE_INT))
        WT_ERR(__wt_cursor_kv_not_set(cursor, false));

    /* Force an allocated copy when using cursor copy debug. */
    if (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
        WT_ERR(__wt_buf_grow(session, &cursor->value, cursor->value.size));
        F_SET(cursor, WT_CURSTD_DEBUG_COPY_VALUE);
    }

    /*
     * Fast path some common cases. The case we care most about being fast is "u", check that first.
     */
    fmt = cursor->value_format;
    if (WT_STREQ(fmt, "u") || F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
        value = va_arg(ap, WT_ITEM *);
        value->data = cursor->value.data;
        value->size = cursor->value.size;
    } else if (WT_STREQ(fmt, "S"))
        *va_arg(ap, const char **) = cursor->value.data;
    else if (WT_STREQ(fmt, "t") || (__wt_isdigit((u_char)fmt[0]) && WT_STREQ(fmt + 1, "t")))
        *va_arg(ap, uint8_t *) = *(uint8_t *)cursor->value.data;
    else
        ret = __wt_struct_unpackv(session, cursor->value.data, cursor->value.size, fmt, ap);

err:
    API_END_RET_STAT(session, ret, cursor_get_value);
}

/*
 * __wt_cursor_get_raw_key_value --
 *     WT_CURSOR->get_raw_key_value default implementation
 */
int
__wt_cursor_get_raw_key_value(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL(cursor, session, ret, get_raw_key_value, NULL);

    if ((key != NULL) && !F_ISSET(cursor, WT_CURSTD_KEY_SET))
        WT_ERR(__wt_cursor_kv_not_set(cursor, true));

    if ((value != NULL) && !F_ISSET(cursor, WT_CURSTD_VALUE_SET))
        WT_ERR(__wt_cursor_kv_not_set(cursor, false));

    /* Force an allocated copy when using cursor copy debug. */
    if (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
        WT_ERR(__wt_buf_grow(session, &cursor->key, cursor->key.size));
        F_SET(cursor, WT_CURSTD_DEBUG_COPY_KEY);
        WT_ERR(__wt_buf_grow(session, &cursor->value, cursor->value.size));
        F_SET(cursor, WT_CURSTD_DEBUG_COPY_VALUE);
    }

    if (key != NULL) {
        key->data = cursor->key.data;
        key->size = cursor->key.size;
    }

    if (value != NULL) {
        value->data = cursor->value.data;
        value->size = cursor->value.size;
    }

err:
    API_END_RET(session, ret);
}

/*
 * __wt_cursor_set_value --
 *     WT_CURSOR->set_value default implementation.
 */
void
__wt_cursor_set_value(WT_CURSOR *cursor, ...)
{
    va_list ap;

    va_start(ap, cursor);
    WT_IGNORE_RET(__wti_cursor_set_valuev(cursor, cursor->value_format, ap));
    va_end(ap);
}

/*
 * __wti_cursor_set_valuev --
 *     WT_CURSOR->set_value worker implementation.
 */
int
__wti_cursor_set_valuev(WT_CURSOR *cursor, const char *fmt, va_list ap)
{
    WT_DECL_RET;
    WT_ITEM *buf, *item, tmp;
    WT_SESSION_IMPL *session;
    size_t sz;
    const char *str;
    va_list ap_copy;

    buf = &cursor->value;
    tmp.mem = NULL;

    CURSOR_API_CALL(cursor, session, ret, set_value, NULL);
    WT_ERR(__cursor_copy_release(cursor));
    if (F_ISSET(cursor, WT_CURSTD_VALUE_SET) && WT_DATA_IN_ITEM(buf)) {
        tmp = *buf;
        buf->mem = NULL;
        buf->memsize = 0;
    }

    F_CLR(cursor, WT_CURSTD_VALUE_SET);

    /*
     * Fast path some common cases. The case we care most about being fast is "u", check that first.
     */
    if (WT_STREQ(fmt, "u") || F_ISSET(cursor, WT_CURSOR_RAW_OK | WT_CURSTD_DUMP_JSON)) {
        item = va_arg(ap, WT_ITEM *);
        sz = item->size;
        buf->data = item->data;
    } else if (WT_STREQ(fmt, "S")) {
        str = va_arg(ap, const char *);
        sz = strlen(str) + 1;
        buf->data = str;
    } else if (WT_STREQ(fmt, "t") || (__wt_isdigit((u_char)fmt[0]) && WT_STREQ(fmt + 1, "t"))) {
        sz = 1;
        WT_ERR(__wt_buf_initsize(session, buf, sz));
        *(uint8_t *)buf->mem = (uint8_t)va_arg(ap, int);
    } else {
        va_copy(ap_copy, ap);
        ret = __wt_struct_sizev(session, &sz, fmt, ap_copy);
        va_end(ap_copy);
        WT_ERR(ret);
        WT_ERR(__wt_buf_initsize(session, buf, sz));
        WT_ERR(__wt_struct_packv(session, buf->mem, sz, fmt, ap));
    }
    F_SET(cursor, WT_CURSTD_VALUE_EXT);
    buf->size = sz;

    if (0) {
err:
        cursor->saved_err = ret;
    }

    /*
     * If we copied the value, either put the memory back into the cursor, or if we allocated some
     * memory in the meantime, free it.
     */
    if (tmp.mem != NULL) {
        if (buf->mem == NULL && !FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
            buf->mem = tmp.mem;
            buf->memsize = tmp.memsize;
            F_SET(cursor, WT_CURSTD_DEBUG_COPY_VALUE);
        } else
            __wt_free(session, tmp.mem);
    }

    API_END_RET(session, ret);
}

/*
 * __wti_cursor_cache --
 *     Add this cursor to the cache.
 */
int
__wti_cursor_cache(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t bucket;

    session = CUR2S(cursor);
    WT_ASSERT(session, !F_ISSET(cursor, WT_CURSTD_CACHED) && dhandle != NULL);
    /* Since open_cursor_count decremented below */
    WT_ASSERT(session, F_ISSET(cursor, WT_CURSTD_OPEN));

    WT_TRET(cursor->reset(cursor));

    /*
     * Cursor reset clears bounds on cursors when called externally, we need to clear the bounds
     * manually when we cache a cursor.
     */
    __wt_cursor_bound_reset(cursor);

    /*
     * Check to see if the top level key/value buffers in the cursor are allocated. If so, we want
     * to keep them around, they may be useful for the next use of this cursor. They will be freed
     * by the session cursor sweep. If they are not allocated, free all the buffers in the cursor.
     */
    if (__wt_cursor_has_cached_memory(cursor)) {
        F_SET(cursor, WT_CURSTD_CACHED_WITH_MEM);
        cursor->key.data = NULL;
        cursor->key.size = 0;
        cursor->value.data = NULL;
        cursor->value.size = 0;
    } else
        __wt_cursor_free_cached_memory(cursor);

    /*
     * Acquire a reference while decrementing the in-use counter. After this point, the dhandle may
     * be marked dead, but the actual handle won't be removed.
     */
    session->dhandle = dhandle;
    WT_DHANDLE_ACQUIRE(dhandle);
    __wt_cursor_dhandle_decr_use(session);

    /* Move the cursor from the open list to the caching hash table. */
    if (cursor->uri_hash == 0)
        cursor->uri_hash = __wt_hash_city64(cursor->uri, strlen(cursor->uri));
    bucket = cursor->uri_hash & (S2C(session)->hash_size - 1);
    TAILQ_REMOVE(&session->cursors, cursor, q);
    TAILQ_INSERT_HEAD(&session->cursor_cache[bucket], cursor, q);

    (void)__wt_atomic_sub32(&S2C(session)->open_cursor_count, 1);
    WT_STAT_CONN_INCR_ATOMIC(session, cursor_cached_count);
    WT_STAT_DSRC_DECR(session, cursor_open_count);
    F_SET(cursor, WT_CURSTD_CACHED);

    /* Document the flags cleared, and set by this function */
    WT_ASSERT(session, !WT_CURSOR_BOUNDS_SET(cursor)); /* __wt_cursor_bound_reset() */
    WT_ASSERT(session, F_AREALLSET(cursor, WT_CURSTD_CACHEABLE | WT_CURSTD_CACHED));

    return (ret);
}

/*
 * __wti_cursor_reopen --
 *     Reopen this cursor from the cached state.
 */
void
__wti_cursor_reopen(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle)
{
    WT_SESSION_IMPL *session;
    uint64_t bucket;

    session = CUR2S(cursor);
    WT_ASSERT(session, F_ISSET(cursor, WT_CURSTD_CACHED));
    WT_ASSERT(session, dhandle != NULL);

    session->dhandle = dhandle;
    __wt_cursor_dhandle_incr_use(session);
    WT_DHANDLE_RELEASE(dhandle);

    (void)__wt_atomic_add32(&S2C(session)->open_cursor_count, 1);
    WT_STAT_CONN_DECR_ATOMIC(session, cursor_cached_count);
    WT_STAT_DSRC_INCR(session, cursor_open_count);

    bucket = cursor->uri_hash & (S2C(session)->hash_size - 1);
    TAILQ_REMOVE(&session->cursor_cache[bucket], cursor, q);
    TAILQ_INSERT_HEAD(&session->cursors, cursor, q);
    F_CLR(cursor, WT_CURSTD_CACHED);
}

/*
 * __wti_cursor_cache_release --
 *     Put the cursor into a cached state, called during cursor close operations.
 */
int
__wti_cursor_cache_release(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool *released)
{
    WT_DECL_RET;

    *released = false;
    if (!F_ISSET(cursor, WT_CURSTD_CACHEABLE) || !F_ISSET(session, WT_SESSION_CACHE_CURSORS))
        return (0);

    WT_ASSERT(session, !F_ISSET(cursor, WT_CURSTD_BULK | WT_CURSTD_CACHED));

    /*
     * Do any sweeping first, if there are errors, it will be easier to clean up if the cursor is
     * not already cached. When sweeping, don't use the "big" option. We want only a modest sweep as
     * we're in a performance path.
     */
    if (--session->cursor_sweep_countdown == 0) {
        session->cursor_sweep_countdown = WT_SESSION_CURSOR_SWEEP_COUNTDOWN;
        WT_RET(__wt_session_cursor_cache_sweep(session, false));
        __wt_session_dhandle_sweep(session);
    }

    /*
     * Caching the cursor releases its data handle. So we have to update statistics first. If
     * caching fails, we'll decrement the statistics after reopening the cursor (and getting the
     * data handle back).
     */
    WT_STAT_CONN_DSRC_INCR(session, cursor_cache);
    WT_ERR(cursor->cache(cursor));
    WT_ASSERT(session, F_ISSET(cursor, WT_CURSTD_CACHED));
    *released = true;

    if (0) {
        /*
         * If caching fails, we must restore the state of the cursor back to open so that the close
         * works from a known state. The reopen may also fail, but that doesn't matter at this
         * point.
         */
err:
        WT_TRET(cursor->reopen(cursor, false));
        WT_ASSERT(session, !F_ISSET(cursor, WT_CURSTD_CACHED));
        WT_STAT_CONN_DSRC_DECR(session, cursor_cache);
    }

    return (ret);
}

/*
 * __wt_cursor_get_hash --
 *     Get hash value from the given uri.
 */
void
__wt_cursor_get_hash(
  WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *to_dup, uint64_t *hash_value)
{
    if (to_dup != NULL) {
        WT_ASSERT(session, uri == NULL);
        *hash_value = to_dup->uri_hash;
    } else {
        WT_ASSERT(session, uri != NULL);
        *hash_value = __wt_hash_city64(uri, strlen(uri));
    }
}

/*
 * __cursors_must_be_readonly --
 *     Determine whether connection and/or cfg[] requires that a cursor be read-only.
 */
static int
__cursors_must_be_readonly(WT_SESSION_IMPL *session, const char *cfg[], bool *readonlyp)
{
    WT_CONFIG_ITEM cval;
    bool readonly;

    /*
     * checkpoint, readonly Checkpoint cursors are permanently read-only, avoid the extra work of
     * two configuration string checks.
     */
    readonly = F_ISSET(S2C(session), WT_CONN_READONLY);
    if (!readonly && cfg != NULL) {
        WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
        readonly = cval.len != 0;

        if (!readonly) {
            WT_RET(__wt_config_gets_def(session, cfg, "readonly", 0, &cval));
            readonly = cval.val != 0;
        }
    }
    *readonlyp = readonly;
    return (0);
}

/*
 * __cursors_can_be_cached --
 *     Determine whether cfg[] allows that a cursor be cached.
 */
static int
__cursors_can_be_cached(WT_SESSION_IMPL *session, const char *cfg[], bool *cacheablep)
{
    WT_CONFIG_ITEM cval;

    /*
     * Any cursors that have special configuration cannot be cached. There are some exceptions for
     * configurations that only differ by a cursor flag, which we can patch up if we find a matching
     * cursor.
     */
    WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
    if (cval.val)
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "debug", 0, &cval));
    if (cval.len != 0)
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "dump", 0, &cval));
    if (cval.len != 0)
        goto return_false;

    WT_RET(__wt_config_gets(session, cfg, "force", &cval));
    if (WT_CONFIG_LIT_MATCH("true", cval))
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0)
        goto return_false;

    WT_RET(__wt_config_gets_def(session, cfg, "readonly", 0, &cval));
    if (cval.val)
        goto return_false;

    /* Checkpoints are readonly, we won't cache them. */
    WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
    if (cval.val) {
return_false:
        *cacheablep = false;
    } else {
        *cacheablep = true;
    }

    return (0);
}

/*
 * __cursor_reuse_or_init --
 *     Initialization shared between reuse of a cached cursor and initialization of a new cursor.
 */
static int
__cursor_reuse_or_init(WT_SESSION_IMPL *session, WT_CURSOR *cursor, const char *cfg[],
  bool *readonlyp, WT_CURSOR **ownerp, WT_CURSOR **cdumpp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cdump, *owner;

    cdump = NULL;
    owner = ownerp ? *ownerp : NULL;

    if (cfg != NULL) {
        /*
         * WT_CURSTD_APPEND is only relevant to column stores.
         */
        if (WT_CURSOR_RECNO(cursor)) {
            WT_RET(__wt_config_gets_def(session, cfg, "append", 0, &cval));
            if (cval.val != 0)
                F_SET(cursor, WT_CURSTD_APPEND);
            else
                F_CLR(cursor, WT_CURSTD_APPEND);
        }

        /* WT_CURSTD_RAW */
        WT_RET(__wt_config_gets_def(session, cfg, "raw", 0, &cval));
        if (cval.val != 0)
            F_SET(cursor, WT_CURSTD_RAW);

        /* WT_CURSTD_DEBUG_RESET_EVICT */
        WT_RET(__cursor_config_debug(cursor, cfg));

        /*
         * If an index cursor is opened with the dump configuration option, then this function is
         * called on the index files, with the dump config string, and with the index cursor as an
         * owner. We don't want to create a dump cursor in that case, because we'll create the dump
         * cursor on the index cursor itself.
         */
        WT_RET(__wt_config_gets_def(session, cfg, "dump", 0, &cval));
        if (cval.len != 0 && owner == NULL) {
            uint64_t dump_flag;
            if (WT_CONFIG_LIT_MATCH("json", cval))
                dump_flag = WT_CURSTD_DUMP_JSON;
            else if (WT_CONFIG_LIT_MATCH("print", cval))
                dump_flag = WT_CURSTD_DUMP_PRINT;
            else if (WT_CONFIG_LIT_MATCH("pretty", cval))
                dump_flag = WT_CURSTD_DUMP_PRETTY;
            else if (WT_CONFIG_LIT_MATCH("pretty_hex", cval))
                dump_flag = WT_CURSTD_DUMP_HEX | WT_CURSTD_DUMP_PRETTY;
            else
                dump_flag = WT_CURSTD_DUMP_HEX;
            F_SET(cursor, dump_flag);

            /*
             * Dump cursors should not have owners: only the top-level cursor should be wrapped in a
             * dump cursor.
             */
            WT_RET(__wti_curdump_create(cursor, owner, &cdump));
            *ownerp = cdump;
            F_CLR(cursor, WT_CURSTD_CACHEABLE);
        }
    }
    *cdumpp = cdump;

    /* Readonly? */
    WT_RET(__cursors_must_be_readonly(session, cfg, readonlyp));

    /* WT_CURSTD_CACHEABLE */
    if (*readonlyp) /* We do not cache read-only cursors. */
        F_CLR(cursor, WT_CURSTD_CACHEABLE);

    return (0);
}

/*
 * __wt_cursor_cache_get --
 *     Open a matching cursor from the cache.
 */
int
__wt_cursor_cache_get(WT_SESSION_IMPL *session, const char *uri, uint64_t hash_value,
  WT_CURSOR *to_dup, const char **cfg, WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cdump, *cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    uint64_t bucket, overwrite_flag;
    bool cacheable, have_config, readonly;

    /* cacheable */
    if (!F_ISSET(session, WT_SESSION_CACHE_CURSORS))
        return (WT_NOTFOUND);

    /* If original config string is NULL or "", don't check it. */
    have_config =
      (cfg != NULL && cfg[0] != NULL && cfg[1] != NULL && (cfg[2] != NULL || cfg[1][0] != '\0'));

    /* Use a null configuration array when applicable for fastest configuration lookups. */
    if (!have_config)
        cfg = NULL;

    /* WT_CURSTD_OVERWRITE: Fast path overwrite configuration */
    if (have_config) {
        if (cfg[2] == NULL && strcmp(cfg[1], "overwrite=false") == 0) {
            have_config = false;
            overwrite_flag = 0;
            cfg = NULL;
        } else {
            WT_RET(__wt_config_gets_def(session, cfg, "overwrite", 1, &cval));
            overwrite_flag = (cval.val != 0) ? WT_CURSTD_OVERWRITE : 0;
        }
    } else
        overwrite_flag = WT_CURSTD_OVERWRITE;

    if (have_config) {
        WT_RET(__cursors_can_be_cached(session, cfg, &cacheable));
        if (!cacheable)
            return (WT_NOTFOUND);
    }

    if (to_dup != NULL)
        uri = to_dup->uri;
    /*
     * Walk through all cursors, if there is a cached cursor that matches uri and configuration, use
     * it.
     */
    bucket = hash_value & (S2C(session)->hash_size - 1);
    TAILQ_FOREACH (cursor, &session->cursor_cache[bucket], q) {
        /* Document some flags always set in the cache */
        WT_ASSERT(session, F_AREALLSET(cursor, WT_CURSTD_CACHEABLE | WT_CURSTD_CACHED));

        if (cursor->uri_hash == hash_value && strcmp(cursor->uri, uri) == 0) {
            if ((ret = cursor->reopen(cursor, false)) != 0) {
                F_CLR(cursor, WT_CURSTD_CACHEABLE);
                session->dhandle = NULL;
                (void)cursor->close(cursor);
                return (ret);
            }

            /*
             * For these configuration values, there is no difference in the resulting cursor other
             * than flag values, so fix them up according to the given configuration.
             */
            F_CLR(cursor, WT_CURSTD_APPEND | WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);
            F_SET(cursor, overwrite_flag);
            /*
             * If this is a btree cursor, clear its read_once flag.
             */
            if (WT_BTREE_PREFIX(cursor->internal_uri)) {
                cbt = (WT_CURSOR_BTREE *)cursor;
                F_CLR(cbt, WT_CBT_READ_ONCE);
            } else {
                cbt = NULL;
            }

            cdump = NULL;
            WT_RET(__cursor_reuse_or_init(session, cursor, cfg, &readonly, NULL, &cdump));
            /* Since owner == NULL */
            WT_ASSERT(session, cdump == NULL);

            if (cbt) {
                if (have_config) {
                    WT_RET(__wt_config_gets_def(session, cfg, "read_once", 0, &cval));
                    if (cval.val != 0)
                        F_SET(cbt, WT_CBT_READ_ONCE);
                }

                /*
                 * A side effect of a cursor open is to leave the session's data handle set. Honor
                 * that for a "reopen".
                 */
                session->dhandle = cbt->dhandle;
            }

            *cursorp = cursor;
            return (0);
        }
    }
    return (WT_NOTFOUND);
}

/*
 * __wt_cursor_close --
 *     WT_CURSOR->close default implementation.
 */
void
__wt_cursor_close(WT_CURSOR *cursor)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);

    if (F_ISSET(cursor, WT_CURSTD_OPEN)) {
        TAILQ_REMOVE(&session->cursors, cursor, q);

        (void)__wt_atomic_sub32(&S2C(session)->open_cursor_count, 1);
        WT_STAT_DSRC_DECR(session, cursor_open_count);
    }
    __wt_buf_free(session, &cursor->key);
    __wt_buf_free(session, &cursor->value);

    __wt_buf_free(session, &cursor->lower_bound);
    __wt_buf_free(session, &cursor->upper_bound);

    __wt_free(session, cursor->internal_uri);
    __wt_free(session, cursor->uri);
    __wt_overwrite_and_free(session, cursor);
}

/*
 * __wt_cursor_equals --
 *     WT_CURSOR->equals default implementation.
 */
int
__wt_cursor_equals(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int cmp;

    CURSOR_API_CALL(cursor, session, ret, equals, NULL);

    WT_ERR(cursor->compare(cursor, other, &cmp));
    *equalp = (cmp == 0) ? 1 : 0;

err:
    API_END_RET_STAT(session, ret, cursor_equals);
}

/*
 * __cursor_modify --
 *     WT_CURSOR->modify default implementation.
 */
static int
__cursor_modify(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL(cursor, session, ret, modify, NULL);

    /* Check for a rational modify vector count. */
    if (nentries <= 0)
        WT_ERR_MSG(session, EINVAL, "Illegal modify vector with %d entries", nentries);

    /*
     * The underlying btree code cannot support WT_CURSOR.modify within a read-committed or
     * read-uncommitted transaction, or outside of an explicit transaction. Disallow here as well,
     * for consistency.
     */
    if (session->txn->isolation != WT_ISO_SNAPSHOT)
        WT_ERR_MSG(
          session, ENOTSUP, "not supported in read-committed or read-uncommitted transactions");
    if (F_ISSET(session->txn, WT_TXN_AUTOCOMMIT))
        WT_ERR_MSG(session, ENOTSUP, "not supported in implicit transactions");

    WT_ERR(__cursor_checkkey(cursor));

    /* Get the current value, apply the modifications. */
    WT_ERR(cursor->search(cursor));
    WT_ERR(__wt_modify_apply_api(cursor, entries, nentries));

    /* We know both key and value are set, "overwrite" doesn't matter. */
    ret = cursor->update(cursor);

err:
    API_END_RET_STAT(session, ret, cursor_modify);
}

/*
 * __cursor_config_debug --
 *     Set configuration options for debug category.
 */
static int
__cursor_config_debug(WT_CURSOR *cursor, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)cursor->session;

    /*
     * Debug options. Special handling for options that aren't found - since reconfigure passes in
     * just the single configuration string, not the stack.
     */
    if ((ret = __wt_config_gets_def(session, cfg, "debug.release_evict", 0, &cval)) == 0) {
        if (cval.val)
            F_SET(cursor, WT_CURSTD_DEBUG_RESET_EVICT);
        else
            F_CLR(cursor, WT_CURSTD_DEBUG_RESET_EVICT);
    } else
        WT_RET_NOTFOUND_OK(ret);
    return (0);
}

/*
 * __wti_cursor_reconfigure --
 *     Set runtime-configurable settings.
 */
int
__wti_cursor_reconfigure(WT_CURSOR *cursor, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL_CONF(cursor, session, ret, reconfigure, config, cfg, NULL);

    /* Reconfiguration resets the cursor. */
    WT_ERR(cursor->reset(cursor));

    /*
     * append Only relevant to column stores.
     */
    if (WT_CURSOR_RECNO(cursor)) {
        if ((ret = __wt_config_getones(session, config, "append", &cval)) == 0) {
            if (cval.val)
                F_SET(cursor, WT_CURSTD_APPEND);
            else
                F_CLR(cursor, WT_CURSTD_APPEND);
        } else
            WT_ERR_NOTFOUND_OK(ret, false);
    }

    /*
     * overwrite
     */
    if ((ret = __wt_config_getones(session, config, "overwrite", &cval)) == 0) {
        if (cval.val)
            F_SET(cursor, WT_CURSTD_OVERWRITE);
        else
            F_CLR(cursor, WT_CURSTD_OVERWRITE);
    } else
        WT_ERR_NOTFOUND_OK(ret, false);

    /* WT_CURSTD_DEBUG_RESET_EVICT */
    WT_ERR(__cursor_config_debug(cursor, cfg));

err:
    API_END_RET_STAT(session, ret, cursor_reconfigure);
}

/*
 * __wti_cursor_bound --
 *     WT_CURSOR->bound implementation shared between table cursors and layered cursors.
 */
int
__wti_cursor_bound(WT_CURSOR *cursor, WT_CONF *conf, WT_COLLATOR *collator)
{
    WT_CONFIG_ITEM cval;
    WT_ITEM key;
    WT_SESSION_IMPL *session;
    int exact;
    bool inclusive;

    session = CUR2S(cursor);
    exact = 0;
    inclusive = true;

    /* Action is default to "set". */
    WT_RET(__wt_conf_gets(session, conf, action, &cval));
    if (WT_CONF_STRING_MATCH(set, cval)) {

        /* The cursor must have a key set to place the lower or upper bound. */
        WT_RET(__cursor_checkkey(cursor));

        WT_RET(__wt_conf_gets_def(session, conf, inclusive, true, &cval));
        if (cval.val == 0)
            inclusive = false;

        WT_RET(__wt_conf_gets(session, conf, bound, &cval));
        if (WT_CONF_STRING_MATCH(upper, cval)) {
            /*
             * If the lower bounds are set, make sure that the upper bound is greater than the lower
             * bound.
             */
            WT_RET(__wt_cursor_get_raw_key(cursor, &key));
            if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER)) {
                WT_RET(__wt_compare(session, collator, &key, &cursor->lower_bound, &exact));
                if (exact < 0)
                    WT_RET_MSG(session, EINVAL, "The provided cursor bounds are overlapping");
                /*
                 * If the lower bound and upper bound are equal, both inclusive flags must be
                 * specified.
                 */
                if (exact == 0 && (!F_ISSET(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE) || !inclusive))
                    WT_RET_MSG(
                      session, EINVAL, "The provided cursor bounds are equal but not inclusive");
            }
            /* Copy the key over to the upper bound item and set upper bound and inclusive flags. */
            F_SET(cursor, WT_CURSTD_BOUND_UPPER);
            if (inclusive)
                F_SET(cursor, WT_CURSTD_BOUND_UPPER_INCLUSIVE);
            else
                F_CLR(cursor, WT_CURSTD_BOUND_UPPER_INCLUSIVE);
            WT_RET(__wt_buf_set(session, &cursor->upper_bound, key.data, key.size));
        } else if (WT_CONF_STRING_MATCH(lower, cval)) {
            /*
             * If the upper bounds are set, make sure that the lower bound is less than the upper
             * bound.
             */
            WT_RET(__wt_cursor_get_raw_key(cursor, &key));
            if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER)) {
                WT_RET(__wt_compare(session, collator, &key, &cursor->upper_bound, &exact));
                if (exact > 0)
                    WT_RET_MSG(session, EINVAL, "The provided cursor bounds are overlapping");
                /*
                 * If the lower bound and upper bound are equal, both inclusive flags must be
                 * specified.
                 */
                if (exact == 0 && (!F_ISSET(cursor, WT_CURSTD_BOUND_UPPER_INCLUSIVE) || !inclusive))
                    WT_RET_MSG(
                      session, EINVAL, "The provided cursor bounds are equal but not inclusive");
            }
            /* Copy the key over to the lower bound item and set upper bound and inclusive flags. */
            F_SET(cursor, WT_CURSTD_BOUND_LOWER);
            if (inclusive)
                F_SET(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
            else
                F_CLR(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE);
            WT_RET(__wt_buf_set(session, &cursor->lower_bound, key.data, key.size));
        } else
            WT_RET_MSG(session, EINVAL,
              "a bound must be specified when setting bounds, either \"lower\" or \"upper\"");
    } else {
        /*
         * The config API errors out earlier if an invalid option is provided. We must be in the
         * clear case if we're here.
         */
        WT_ASSERT(session, (WT_CONF_STRING_MATCH(clear, cval)));
        F_CLR(cursor, WT_CURSTD_BOUND_ALL);
        __wt_buf_free(session, &cursor->upper_bound);
        __wt_buf_free(session, &cursor->lower_bound);
        WT_CLEAR(cursor->upper_bound);
        WT_CLEAR(cursor->lower_bound);
    }
    return (0);
}

/*
 * __wt_cursor_bounds_save --
 *     Save cursor bounds to restore the state.
 */
int
__wt_cursor_bounds_save(
  WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_CURSOR_BOUNDS_STATE *bounds_state)
{
    /* Save the bound flags to the state. */
    bounds_state->bound_flags = F_MASK(cursor, WT_CURSTD_BOUND_ALL);

    if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER)) {
        WT_RET(__wt_scr_alloc(session, cursor->lower_bound.size, &bounds_state->lower_bound));
        WT_RET(__wt_buf_set(
          session, bounds_state->lower_bound, cursor->lower_bound.data, cursor->lower_bound.size));
    }
    if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER)) {
        WT_RET(__wt_scr_alloc(session, cursor->upper_bound.size, &bounds_state->upper_bound));
        WT_RET(__wt_buf_set(
          session, bounds_state->upper_bound, cursor->upper_bound.data, cursor->upper_bound.size));
    }

    return (0);
}

/*
 * __wt_cursor_bounds_restore --
 *     Restore the cursor's bounds state. We want to change only related flags as we can't guarantee
 *     the initial flag state of the column group cursors are the same.
 */
int
__wt_cursor_bounds_restore(
  WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_CURSOR_BOUNDS_STATE *bounds_state)
{
    WT_DECL_RET;

    /* Clear all the bound flags. */
    F_CLR(cursor, WT_CURSTD_BOUND_ALL);
    /* Set the saved bound flags back to the cursor. */
    F_SET(cursor, bounds_state->bound_flags);

    if (bounds_state->lower_bound != NULL)
        if ((ret = __wt_buf_set(session, &cursor->lower_bound, bounds_state->lower_bound->data,
               bounds_state->lower_bound->size)) != 0)
            WT_RET_PANIC(session, ret, "Unrecoverable error encountered while restoring bounds");

    if (bounds_state->upper_bound != NULL)
        if ((ret = __wt_buf_set(session, &cursor->upper_bound, bounds_state->upper_bound->data,
               bounds_state->upper_bound->size)) != 0)
            WT_RET_PANIC(session, ret, "Unrecoverable error encountered while restoring bounds");

    return (0);
}

/*
 * __wt_cursor_dup_position --
 *     Set a cursor to another cursor's position.
 */
int
__wt_cursor_dup_position(WT_CURSOR *to_dup, WT_CURSOR *cursor)
{
    WT_DECL_RET;
    WT_ITEM key;

    /*
     * Get a copy of the cursor's raw key, and set it in the new cursor, then search for that key to
     * position the cursor.
     *
     * We don't clear the WT_ITEM structure: all that happens when getting and setting the key is
     * the data/size fields are reset to reference the original cursor's key.
     *
     * That said, we're playing games with the cursor flags: setting the key sets the key/value
     * application-set flags in the new cursor, which may or may not be correct, but there's nothing
     * simple that fixes it. We depend on the subsequent cursor search to clean things up, as search
     * is required to copy and/or reference private memory after success.
     */
    WT_RET(__wt_cursor_get_raw_key(to_dup, &key));
    __wt_cursor_set_raw_key(cursor, &key);

    /*
     * We now have a reference to the raw key, but we don't know anything about the memory in which
     * it's stored, it could be btree/file page memory in the cache, application memory or the
     * original cursor's key/value WT_ITEMs. Memory allocated in support of another cursor could be
     * discarded when that cursor is closed, so it's a problem. However, doing a search to position
     * the cursor will fix the problem: cursors cannot reference application memory after cursor
     * operations and that requirement will save the day.
     */
    F_SET(cursor, WT_CURSTD_RAW_SEARCH);
    ret = cursor->search(cursor);
    F_CLR(cursor, WT_CURSTD_RAW_SEARCH);

    return (ret);
}

/*
 * __wt_cursor_init --
 *     Default cursor initialization.
 */
int
__wt_cursor_init(
  WT_CURSOR *cursor, const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cdump;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool readonly;

    session = CUR2S(cursor);

    if (cursor->internal_uri == NULL) {
        /* Various cursor code assumes there is an internal URI, so there better be one to set. */
        WT_ASSERT(session, uri != NULL);
        WT_RET(__wt_strdup(session, uri, &cursor->internal_uri));
    }

    /* WT_CURSTD_OVERWRITE */
    WT_ERR(__wt_config_gets_def(session, cfg, "overwrite", 1, &cval));
    if (cval.val)
        F_SET(cursor, WT_CURSTD_OVERWRITE);
    else
        F_CLR(cursor, WT_CURSTD_OVERWRITE);

    WT_ERR(__cursor_reuse_or_init(session, cursor, cfg, &readonly, &owner, &cdump));

    if (readonly) {
        cursor->insert = __wt_cursor_notsup;
        cursor->modify = __wt_cursor_modify_notsup;
        cursor->remove = __wt_cursor_notsup;
        cursor->reserve = __wt_cursor_notsup;
        cursor->update = __wt_cursor_notsup;
    }

    /*
     * WT_CURSOR.modify supported on 'S' and 'u' value formats, but may have been already
     * initialized (file cursors have a faster implementation).
     */
    if ((WT_STREQ(cursor->value_format, "u") || WT_STREQ(cursor->value_format, "S")) &&
      cursor->modify == __wti_cursor_modify_value_format_notsup)
        cursor->modify = __cursor_modify;

    /* Tiered cursors are not yet candidates for caching. */
    if (uri != NULL && WT_PREFIX_MATCH(uri, "tiered:"))
        F_CLR(cursor, WT_CURSTD_CACHEABLE);

    /*
     * Cursors that are internal to some other cursor (such as file cursors inside a table cursor)
     * should be closed after the containing cursor. Arrange for that to happen by putting internal
     * cursors after their owners on the queue.
     */
    if (owner != NULL) {
        WT_ASSERT(session, F_ISSET(owner, WT_CURSTD_OPEN));
        TAILQ_INSERT_AFTER(&session->cursors, owner, cursor, q);
    } else
        TAILQ_INSERT_HEAD(&session->cursors, cursor, q);

    /* WT_CURSTD_OPEN */
    F_SET(cursor, WT_CURSTD_OPEN);
    (void)__wt_atomic_add32(&S2C(session)->open_cursor_count, 1);
    WT_STAT_DSRC_INCR(session, cursor_open_count);

    *cursorp = (cdump != NULL) ? cdump : cursor;
    return (0);

err:
    if (ret != 0) {
        __wt_free(session, cursor->internal_uri);
        cursor->internal_uri = NULL;
    }
    return (ret);
}
