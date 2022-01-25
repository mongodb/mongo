/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __curtable_open_indices(WT_CURSOR_TABLE *ctable);
static int __curtable_update(WT_CURSOR *cursor);

#define APPLY_CG(ctable, f)                                                             \
    do {                                                                                \
        WT_CURSOR **__cp;                                                               \
        u_int __i;                                                                      \
        for (__i = 0, __cp = (ctable)->cg_cursors; __i < WT_COLGROUPS((ctable)->table); \
             __i++, __cp++) {                                                           \
            WT_TRET((*__cp)->f(*__cp));                                                 \
            WT_ERR_NOTFOUND_OK(ret, true);                                              \
        }                                                                               \
    } while (0)

/* Cursor type for custom extractor callback. */
typedef struct {
    WT_CURSOR iface;
    WT_CURSOR_TABLE *ctable;
    WT_CURSOR *idxc;
    int (*f)(WT_CURSOR *);
} WT_CURSOR_EXTRACTOR;

/*
 * __curextract_insert --
 *     Handle a key produced by a custom extractor.
 */
static int
__curextract_insert(WT_CURSOR *cursor)
{
    WT_CURSOR_EXTRACTOR *cextract;
    WT_DECL_RET;
    WT_ITEM *key, ikey, pkey;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL(cursor, session, insert, NULL);

    cextract = (WT_CURSOR_EXTRACTOR *)cursor;

    WT_ITEM_SET(ikey, cursor->key);
    /*
     * We appended a padding byte to the key to avoid rewriting the last column. Strip that away
     * here.
     */
    WT_ASSERT(session, ikey.size > 0);
    --ikey.size;
    WT_ERR(__wt_cursor_get_raw_key(cextract->ctable->cg_cursors[0], &pkey));

    /*
     * We have the index key in the format we need, and all of the primary key columns are required:
     * just append them.
     */
    key = &cextract->idxc->key;
    WT_ERR(__wt_buf_grow(session, key, ikey.size + pkey.size));
    memcpy((uint8_t *)key->mem, ikey.data, ikey.size);
    memcpy((uint8_t *)key->mem + ikey.size, pkey.data, pkey.size);
    key->size = ikey.size + pkey.size;

    /*
     * The index key is now set and the value is empty (it starts clear and is never set).
     */
    F_SET(cextract->idxc, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);

    /* Call the underlying cursor function to update the index. */
    ret = cextract->f(cextract->idxc);

err:
    API_END_RET(session, ret);
}

/*
 * __wt_apply_single_idx --
 *     Apply an operation to a single index of a table.
 */
int
__wt_apply_single_idx(WT_SESSION_IMPL *session, WT_INDEX *idx, WT_CURSOR *cur,
  WT_CURSOR_TABLE *ctable, int (*f)(WT_CURSOR *))
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __wt_cursor_compare_notsup,                     /* compare */
      __wt_cursor_equals_notsup,                      /* equals */
      __wt_cursor_notsup,                             /* next */
      __wt_cursor_notsup,                             /* prev */
      __wt_cursor_notsup,                             /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __curextract_insert,                            /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_reconfigure_notsup,                 /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_notsup);                            /* close */
    WT_CURSOR_EXTRACTOR extract_cursor;
    WT_DECL_RET;
    WT_ITEM key, value;

    if (idx->extractor) {
        extract_cursor.iface = iface;
        extract_cursor.iface.session = &session->iface;
        extract_cursor.iface.key_format = idx->exkey_format;
        extract_cursor.ctable = ctable;
        extract_cursor.idxc = cur;
        extract_cursor.f = f;

        WT_RET(__wt_cursor_get_raw_key(&ctable->iface, &key));
        WT_RET(__wt_cursor_get_raw_value(&ctable->iface, &value));
        ret = idx->extractor->extract(
          idx->extractor, &session->iface, &key, &value, &extract_cursor.iface);

        __wt_buf_free(session, &extract_cursor.iface.key);
        WT_RET(ret);
    } else {
        WT_RET(__wt_schema_project_merge(
          session, ctable->cg_cursors, idx->key_plan, idx->key_format, &cur->key));
        /*
         * The index key is now set and the value is empty (it starts clear and is never set).
         */
        F_SET(cur, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
        WT_RET(f(cur));
    }
    return (0);
}

/*
 * __apply_idx --
 *     Apply an operation to all indices of a table.
 */
static int
__apply_idx(WT_CURSOR_TABLE *ctable, size_t func_off, bool skip_immutable)
{
    WT_CURSOR **cp;
    WT_INDEX *idx;
    WT_SESSION_IMPL *session;
    u_int i;
    int (*f)(WT_CURSOR *);

    cp = ctable->idx_cursors;
    session = CUR2S(ctable);

    for (i = 0; i < ctable->table->nindices; i++, cp++) {
        idx = ctable->table->indices[i];
        if (skip_immutable && F_ISSET(idx, WT_INDEX_IMMUTABLE))
            continue;

        f = *(int (**)(WT_CURSOR *))((uint8_t *)*cp + func_off);
        WT_RET(__wt_apply_single_idx(session, idx, *cp, ctable, f));
        WT_RET((*cp)->reset(*cp));
    }

    return (0);
}

/*
 * __wt_curtable_get_key --
 *     WT_CURSOR->get_key implementation for tables.
 */
int
__wt_curtable_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *primary;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    va_list ap;

    ctable = (WT_CURSOR_TABLE *)cursor;
    primary = *ctable->cg_cursors;

    va_start(ap, cursor);
    ret = __wt_cursor_get_keyv(primary, cursor->flags, ap);
    va_end(ap);

    return (ret);
}

/*
 * __wt_curtable_get_value --
 *     WT_CURSOR->get_value implementation for tables.
 */
int
__wt_curtable_get_value(WT_CURSOR *cursor, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    JOINABLE_CURSOR_API_CALL(cursor, session, get_value, NULL);

    va_start(ap, cursor);
    ret = __wt_curtable_get_valuev(cursor, ap);
    va_end(ap);

err:
    API_END_RET(session, ret);
}

/*
 * __wt_curtable_set_key --
 *     WT_CURSOR->set_key implementation for tables.
 */
void
__wt_curtable_set_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR **cp, *primary;
    WT_CURSOR_TABLE *ctable;
    u_int i;
    va_list ap;

    ctable = (WT_CURSOR_TABLE *)cursor;
    cp = ctable->cg_cursors;
    primary = *cp++;

    va_start(ap, cursor);
    WT_IGNORE_RET(__wt_cursor_set_keyv(primary, cursor->flags, ap));
    va_end(ap);

    if (!F_ISSET(primary, WT_CURSTD_KEY_SET))
        return;

    /* Copy the primary key to the other cursors. */
    for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
        (*cp)->recno = primary->recno;
        (*cp)->key.data = primary->key.data;
        (*cp)->key.size = primary->key.size;
        F_SET(*cp, WT_CURSTD_KEY_EXT);
    }
}

/*
 * __curtable_set_valuev --
 *     WT_CURSOR->set_value implementation for tables.
 */
static int
__curtable_set_valuev(WT_CURSOR *cursor, va_list ap)
{
    WT_CURSOR **cp;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_ITEM *item, *tmp;
    WT_SESSION_IMPL *session;
    u_int i;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, set_value, NULL);

    if (F_ISSET(cursor, WT_CURSOR_RAW_OK | WT_CURSTD_DUMP_JSON)) {
        item = va_arg(ap, WT_ITEM *);
        cursor->value.data = item->data;
        cursor->value.size = item->size;
        ret = __wt_schema_project_slice(
          session, ctable->cg_cursors, ctable->plan, 0, cursor->value_format, &cursor->value);
    } else {
        /*
         * The user may be passing us pointers returned by get_value that point into the buffers we
         * are about to update. Move them aside first.
         */
        for (i = 0, cp = ctable->cg_cursors; i < WT_COLGROUPS(ctable->table); i++, cp++) {
            item = &(*cp)->value;
            if (F_ISSET(*cp, WT_CURSTD_VALUE_SET) && WT_DATA_IN_ITEM(item)) {
                ctable->cg_valcopy[i] = *item;
                item->mem = NULL;
                item->memsize = 0;
            }
        }

        ret = __wt_schema_project_in(session, ctable->cg_cursors, ctable->plan, ap);

        for (i = 0, cp = ctable->cg_cursors; i < WT_COLGROUPS(ctable->table); i++, cp++) {
            tmp = &ctable->cg_valcopy[i];
            if (tmp->mem != NULL) {
                item = &(*cp)->value;
                if (item->mem == NULL) {
                    item->mem = tmp->mem;
                    item->memsize = tmp->memsize;
                } else
                    __wt_free(session, tmp->mem);
            }
        }
    }

    for (i = 0, cp = ctable->cg_cursors; i < WT_COLGROUPS(ctable->table); i++, cp++)
        if (ret == 0)
            F_SET(*cp, WT_CURSTD_VALUE_EXT);
        else {
            (*cp)->saved_err = ret;
            F_CLR(*cp, WT_CURSTD_VALUE_SET);
        }

err:
    API_END_RET(session, ret);
}

/*
 * __wt_curtable_set_value --
 *     WT_CURSOR->set_value implementation for tables.
 */
void
__wt_curtable_set_value(WT_CURSOR *cursor, ...)
{
    va_list ap;

    va_start(ap, cursor);
    WT_IGNORE_RET(__curtable_set_valuev(cursor, ap));
    va_end(ap);
}

/*
 * __curtable_compare --
 *     WT_CURSOR->compare implementation for tables.
 */
static int
__curtable_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    JOINABLE_CURSOR_API_CALL(a, session, compare, NULL);

    /*
     * Confirm both cursors refer to the same source and have keys, then call the underlying
     * object's comparison routine.
     */
    if (strcmp(a->internal_uri, b->internal_uri) != 0)
        WT_ERR_MSG(session, EINVAL, "comparison method cursors must reference the same object");
    WT_ERR(__cursor_checkkey(WT_CURSOR_PRIMARY(a)));
    WT_ERR(__cursor_checkkey(WT_CURSOR_PRIMARY(b)));

    ret = WT_CURSOR_PRIMARY(a)->compare(WT_CURSOR_PRIMARY(a), WT_CURSOR_PRIMARY(b), cmpp);

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_next --
 *     WT_CURSOR->next method for the table cursor type.
 */
static int
__curtable_next(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, next, NULL);
    APPLY_CG(ctable, next);

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_next_random --
 *     WT_CURSOR->next method for the table cursor type when configured with next_random.
 */
static int
__curtable_next_random(WT_CURSOR *cursor)
{
    WT_CURSOR *primary, **cp;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, next, NULL);
    cp = ctable->cg_cursors;

    /* Split out the first next, it retrieves the random record. */
    primary = *cp++;
    WT_ERR(primary->next(primary));

    /* Fill in the rest of the columns. */
    for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
        (*cp)->key.data = primary->key.data;
        (*cp)->key.size = primary->key.size;
        (*cp)->recno = primary->recno;
        F_SET(*cp, WT_CURSTD_KEY_EXT);
        WT_ERR((*cp)->search(*cp));
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_prev --
 *     WT_CURSOR->prev method for the table cursor type.
 */
static int
__curtable_prev(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, prev, NULL);
    APPLY_CG(ctable, prev);

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_reset --
 *     WT_CURSOR->reset method for the table cursor type.
 */
static int
__curtable_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);
    APPLY_CG(ctable, reset);

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_search --
 *     WT_CURSOR->search method for the table cursor type.
 */
static int
__curtable_search(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, search, NULL);
    APPLY_CG(ctable, search);

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_search_near --
 *     WT_CURSOR->search_near method for the table cursor type.
 */
static int
__curtable_search_near(WT_CURSOR *cursor, int *exact)
{
    WT_CURSOR *primary, **cp;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, search_near, NULL);
    cp = ctable->cg_cursors;
    primary = *cp;
    WT_ERR(primary->search_near(primary, exact));

    for (i = 1, ++cp; i < WT_COLGROUPS(ctable->table); i++) {
        (*cp)->key.data = primary->key.data;
        (*cp)->key.size = primary->key.size;
        (*cp)->recno = primary->recno;
        F_SET(*cp, WT_CURSTD_KEY_EXT);
        WT_ERR((*cp)->search(*cp));
    }

err:
    API_END_RET(session, ret);
}

/*
 * __curtable_insert --
 *     WT_CURSOR->insert method for the table cursor type.
 */
static int
__curtable_insert(WT_CURSOR *cursor)
{
    WT_CURSOR *primary, **cp;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint32_t flag_orig;
    u_int i;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_UPDATE_API_CALL(cursor, session, insert);
    WT_ERR(__curtable_open_indices(ctable));

    cp = ctable->cg_cursors;
    primary = *cp++;

    /*
     * Split out the first insert, it may be allocating a recno.
     *
     * If the table has indices, we also need to know whether this record is replacing an existing
     * record so that the existing index entries can be removed. We discover if this is an overwrite
     * by configuring the primary cursor for no-overwrite, and checking if the insert detects a
     * duplicate key. By default, when insert finds a duplicate, it returns the value it found. We
     * don't want that value to overwrite our own, override that behavior.
     */
    flag_orig = F_MASK(primary, WT_CURSTD_OVERWRITE);
    if (ctable->table->nindices > 0) {
        F_CLR(primary, WT_CURSTD_OVERWRITE);
        F_SET(primary, WT_CURSTD_DUP_NO_VALUE);
    }
    ret = primary->insert(primary);

    /*
     * WT_CURSOR.insert clears the set internally/externally flags but doesn't touch the items. We
     * could make a copy each time for overwrite cursors, but for now we just reset the flags.
     */
    F_SET(primary, flag_orig | WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    F_CLR(primary, WT_CURSTD_DUP_NO_VALUE);

    if (ret == WT_DUPLICATE_KEY && F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
        WT_ERR(__curtable_update(cursor));

        /*
         * The cursor is no longer positioned. This isn't just cosmetic, without a reset, iteration
         * on this cursor won't start at the beginning/end of the table.
         */
        APPLY_CG(ctable, reset);
    } else {
        WT_ERR(ret);

        for (i = 1; i < WT_COLGROUPS(ctable->table); i++, cp++) {
            (*cp)->recno = primary->recno;
            WT_ERR((*cp)->insert(*cp));
        }

        WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, insert), false));
    }

    /*
     * Insert is the one cursor operation that doesn't end with the cursor pointing to an on-page
     * item (except for column-store appends, where we are returning a key). That is, the
     * application's cursor continues to reference the application's memory after a successful
     * cursor call, which isn't true anywhere else. We don't want to have to explain that scoping
     * corner case, so we reset the application's cursor so it can free the referenced memory and
     * continue on without risking subsequent core dumps.
     */
    F_CLR(primary, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (F_ISSET(primary, WT_CURSTD_APPEND))
        F_SET(primary, WT_CURSTD_KEY_EXT);

err:
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __curtable_update --
 *     WT_CURSOR->update method for the table cursor type.
 */
static int
__curtable_update(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_ITEM(value_copy);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_UPDATE_API_CALL(cursor, session, update);
    WT_ERR(__curtable_open_indices(ctable));

    /*
     * If the table has indices, first delete any old index keys, then update the primary, then
     * insert the new index keys. This is complicated by the fact that we need the old value to
     * generate the old index keys, so we make a temporary copy of the new value.
     */
    if (ctable->table->nindices > 0) {
        WT_ERR(__wt_scr_alloc(session, ctable->cg_cursors[0]->value.size, &value_copy));
        WT_ERR(__wt_schema_project_merge(
          session, ctable->cg_cursors, ctable->plan, cursor->value_format, value_copy));
        APPLY_CG(ctable, search);

        /* Remove only if the key exists. */
        if (ret == 0) {
            WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, remove), true));
            WT_ERR(__wt_schema_project_slice(
              session, ctable->cg_cursors, ctable->plan, 0, cursor->value_format, value_copy));
        } else
            WT_ERR_NOTFOUND_OK(ret, false);
    }

    APPLY_CG(ctable, update);
    WT_ERR(ret);

    if (ctable->table->nindices > 0)
        WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, insert), true));

err:
    CURSOR_UPDATE_API_END(session, ret);
    __wt_scr_free(session, &value_copy);
    return (ret);
}

/*
 * __curtable_remove --
 *     WT_CURSOR->remove method for the table cursor type.
 */
static int
__curtable_remove(WT_CURSOR *cursor)
{
    WT_CURSOR *primary;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool positioned;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_REMOVE_API_CALL(cursor, session, NULL);
    WT_ERR(__curtable_open_indices(ctable));

    /* Check if the cursor was positioned. */
    primary = *ctable->cg_cursors;
    positioned = F_ISSET(primary, WT_CURSTD_KEY_INT);

    /* Find the old record so it can be removed from indices */
    if (ctable->table->nindices > 0) {
        APPLY_CG(ctable, search);
        if (ret == WT_NOTFOUND)
            goto notfound;
        WT_ERR(ret);
        WT_ERR(__apply_idx(ctable, offsetof(WT_CURSOR, remove), false));
    }

    APPLY_CG(ctable, remove);
    if (ret == WT_NOTFOUND)
        goto notfound;
    WT_ERR(ret);

notfound:
    /*
     * If the cursor is configured to overwrite and the record is not found, that is exactly what we
     * want.
     */
    if (ret == WT_NOTFOUND && F_ISSET(primary, WT_CURSTD_OVERWRITE))
        ret = 0;

    /*
     * If the cursor was positioned, it stays positioned with a key but no no value, otherwise,
     * there's no position, key or value. This isn't just cosmetic, without a reset, iteration on
     * this cursor won't start at the beginning/end of the table.
     */
    F_CLR(primary, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (positioned)
        F_SET(primary, WT_CURSTD_KEY_INT);
    else
        APPLY_CG(ctable, reset);

err:
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __curtable_reserve --
 *     WT_CURSOR->reserve method for the table cursor type.
 */
static int
__curtable_reserve(WT_CURSOR *cursor)
{
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_UPDATE_API_CALL(cursor, session, update);

    /*
     * We don't have to open the indices here, but it makes the code similar to other cursor
     * functions, and it's odd for a reserve call to succeed but the subsequent update fail opening
     * indices.
     *
     * Check for a transaction before index open, opening the indices will start a transaction if
     * one isn't running.
     */
    WT_ERR(__wt_txn_context_check(session, true));
    WT_ERR(__curtable_open_indices(ctable));

    /* Reserve in column groups, ignore indices. */
    APPLY_CG(ctable, reserve);

err:
    CURSOR_UPDATE_API_END(session, ret);

    /*
     * The application might do a WT_CURSOR.get_value call when we return, so we need a value and
     * the underlying functions didn't set one up. For various reasons, those functions may not have
     * done a search and any previous value in the cursor might race with WT_CURSOR.reserve (and in
     * cases like LSM, the reserve never encountered the original key). For simplicity, repeat the
     * search here.
     */
    return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __wt_table_range_truncate --
 *     Truncate of a cursor range, table implementation.
 */
int
__wt_table_range_truncate(WT_CURSOR_TABLE *start, WT_CURSOR_TABLE *stop)
{
    WT_CURSOR *wt_start, *wt_stop;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_ITEM raw;
    WT_SESSION_IMPL *session;
    u_int i;
    int cmp;

    ctable = (start != NULL) ? start : stop;
    session = CUR2S(ctable);
    wt_start = start == NULL ? NULL : &start->iface;
    wt_stop = stop == NULL ? NULL : &stop->iface;

    /* Open any indices. */
    WT_RET(__curtable_open_indices(ctable));
    WT_RET(__wt_scr_alloc(session, 128, &key));
    WT_STAT_DATA_INCR(session, cursor_truncate);

    /*
     * Step through the cursor range, removing the index entries.
     *
     * If there are indices, copy the key we're using to step through the cursor range (so we can
     * reset the cursor to its original position), then remove all of the index records in the
     * truncated range. Copy the raw key because the memory is only valid until the cursor moves.
     */
    if (ctable->table->nindices > 0) {
        if (start == NULL) {
            WT_ERR(__wt_cursor_get_raw_key(wt_stop, &raw));
            WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

            do {
                APPLY_CG(stop, search);
                WT_ERR(ret);
                WT_ERR(__apply_idx(stop, offsetof(WT_CURSOR, remove), false));
            } while ((ret = wt_stop->prev(wt_stop)) == 0);
            WT_ERR_NOTFOUND_OK(ret, false);

            __wt_cursor_set_raw_key(wt_stop, key);
            APPLY_CG(stop, search);
        } else {
            WT_ERR(__wt_cursor_get_raw_key(wt_start, &raw));
            WT_ERR(__wt_buf_set(session, key, raw.data, raw.size));

            cmp = -1;
            do {
                APPLY_CG(start, search);
                WT_ERR(ret);
                WT_ERR(__apply_idx(start, offsetof(WT_CURSOR, remove), false));
                if (stop != NULL)
                    WT_ERR(wt_start->compare(wt_start, wt_stop, &cmp));
            } while (cmp < 0 && (ret = wt_start->next(wt_start)) == 0);
            WT_ERR_NOTFOUND_OK(ret, false);

            __wt_cursor_set_raw_key(wt_start, key);
            APPLY_CG(start, search);
        }
    }

    /* Truncate the column groups. */
    for (i = 0; i < WT_COLGROUPS(ctable->table); i++)
        WT_ERR(__wt_range_truncate((start == NULL) ? NULL : start->cg_cursors[i],
          (stop == NULL) ? NULL : stop->cg_cursors[i]));

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __curtable_largest_key --
 *     WT_CURSOR->largest_key method for the table cursor type.
 */
static int
__curtable_largest_key(WT_CURSOR *cursor)
{
    WT_CURSOR *primary;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, largest_key, NULL);

    WT_ERR(cursor->reset(cursor));
    primary = *ctable->cg_cursors;
    WT_ERR(primary->largest_key(primary));

err:
    if (ret != 0)
        WT_TRET(cursor->reset(cursor));
    API_END_RET(session, ret);
}

/*
 * __curtable_close --
 *     WT_CURSOR->close method for the table cursor type.
 */
static int
__curtable_close(WT_CURSOR *cursor)
{
    WT_CURSOR **cp;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    ctable = (WT_CURSOR_TABLE *)cursor;
    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    if (ctable->cg_cursors != NULL)
        for (i = 0, cp = ctable->cg_cursors; i < WT_COLGROUPS(ctable->table); i++, cp++)
            if (*cp != NULL) {
                WT_TRET((*cp)->close(*cp));
                *cp = NULL;
            }

    if (ctable->idx_cursors != NULL)
        for (i = 0, cp = ctable->idx_cursors; i < ctable->table->nindices; i++, cp++)
            if (*cp != NULL) {
                WT_TRET((*cp)->close(*cp));
                *cp = NULL;
            }

    if (ctable->plan != ctable->table->plan)
        __wt_free(session, ctable->plan);
    if (ctable->cfg != NULL) {
        for (i = 0; ctable->cfg[i] != NULL; ++i)
            __wt_free(session, ctable->cfg[i]);
        __wt_free(session, ctable->cfg);
    }
    if (cursor->value_format != ctable->table->value_format)
        __wt_free(session, cursor->value_format);
    __wt_free(session, ctable->cg_cursors);
    __wt_free(session, ctable->cg_valcopy);
    __wt_free(session, ctable->idx_cursors);

    WT_TRET(__wt_schema_release_table(session, &ctable->table));
    /* The URI is owned by the table. */
    cursor->internal_uri = NULL;
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __curtable_complete --
 *     Return failure if the table is not yet fully created.
 */
static int
__curtable_complete(WT_SESSION_IMPL *session, WT_TABLE *table)
{
    bool complete;

    if (table->cg_complete)
        return (0);

    /* If the table is incomplete, wait on the table lock and recheck. */
    WT_WITH_TABLE_READ_LOCK(session, complete = table->cg_complete);
    if (!complete)
        WT_RET_MSG(session, EINVAL, "'%s' not available until all column groups are created",
          table->iface.name);
    return (0);
}

/*
 * __curtable_open_colgroups --
 *     Open cursors on column groups for a table cursor.
 */
static int
__curtable_open_colgroups(WT_CURSOR_TABLE *ctable, const char *cfg_arg[])
{
    WT_CURSOR **cp;
    WT_SESSION_IMPL *session;
    WT_TABLE *table;
    /*
     * Underlying column groups are always opened without dump or readonly, and only the primary is
     * opened with next_random.
     */
    const char *cfg[] = {cfg_arg[0], cfg_arg[1], "dump=\"\",readonly=0", NULL, NULL};
    u_int i;

    session = CUR2S(ctable);
    table = ctable->table;

    WT_RET(__curtable_complete(session, table)); /* completeness check */

    WT_RET(__wt_calloc_def(session, WT_COLGROUPS(table), &ctable->cg_cursors));
    WT_RET(__wt_calloc_def(session, WT_COLGROUPS(table), &ctable->cg_valcopy));

    for (i = 0, cp = ctable->cg_cursors; i < WT_COLGROUPS(table); i++, cp++) {
        WT_RET(__wt_open_cursor(session, table->cgroups[i]->source, &ctable->iface, cfg, cp));
        cfg[3] = "next_random=false";
    }
    return (0);
}

/*
 * __curtable_open_indices --
 *     Open cursors on indices for a table cursor.
 */
static int
__curtable_open_indices(WT_CURSOR_TABLE *ctable)
{
    WT_CURSOR **cp, *primary;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_TABLE *table;
    u_int i;

    session = CUR2S(ctable);
    table = ctable->table;

    WT_RET(__wt_schema_open_indices(session, table));
    if (table->nindices == 0 || ctable->idx_cursors != NULL)
        return (0);

    /* Check for bulk cursors. */
    primary = *ctable->cg_cursors;
    if (F_ISSET(primary, WT_CURSTD_BULK))
        WT_RET_MSG(session, ENOTSUP, "Bulk load is not supported for tables with indices");

    WT_RET(__wt_calloc_def(session, table->nindices, &ctable->idx_cursors));
    for (i = 0, cp = ctable->idx_cursors; i < table->nindices; i++, cp++)
        WT_ERR(
          __wt_open_cursor(session, table->indices[i]->source, &ctable->iface, ctable->cfg, cp));

    if (0) {
err:
        /*
         * On failure, we can't leave a subset of the indices open, since the table cursor is
         * already open and will remain open after this call. It's all or nothing, so we need to
         * close them all, and leave things as they were before the first cursor operation.
         *
         * The column group open code does not need to do this. Unlike indices, column groups are
         * opened when the table cursor is opened, and a failure there cannot result in an open
         * table cursor.
         */
        for (i = 0, cp = ctable->idx_cursors; i < table->nindices; i++, cp++)
            if (*cp != NULL) {
                WT_TRET((*cp)->close(*cp));
                *cp = NULL;
            }
        __wt_free(session, ctable->idx_cursors);
    }
    return (ret);
}

/*
 * __wt_curtable_open --
 *     WT_SESSION->open_cursor method for table cursors.
 */
int
__wt_curtable_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_curtable_get_key, /* get-key */
      __wt_curtable_get_value,                          /* get-value */
      __wt_curtable_set_key,                            /* set-key */
      __wt_curtable_set_value,                          /* set-value */
      __curtable_compare,                               /* compare */
      __wt_cursor_equals,                               /* equals */
      __curtable_next,                                  /* next */
      __curtable_prev,                                  /* prev */
      __curtable_reset,                                 /* reset */
      __curtable_search,                                /* search */
      __curtable_search_near,                           /* search-near */
      __curtable_insert,                                /* insert */
      __wt_cursor_modify_notsup,                        /* modify */
      __curtable_update,                                /* update */
      __curtable_remove,                                /* remove */
      __curtable_reserve,                               /* reserve */
      __wt_cursor_reconfigure,                          /* reconfigure */
      __curtable_largest_key,                           /* largest_key */
      __wt_cursor_notsup,                               /* cache */
      __wt_cursor_reopen_notsup,                        /* reopen */
      __curtable_close);                                /* close */
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor;
    WT_CURSOR_TABLE *ctable;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TABLE *table;
    size_t size;
    int cfg_cnt;
    const char *tablename, *columns;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_TABLE, iface) == 0);

    tablename = uri;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "table:");
    columns = strchr(tablename, '(');
    if (columns == NULL)
        WT_RET(__wt_schema_get_table_uri(session, uri, false, 0, &table));
    else {
        size = WT_PTRDIFF(columns, tablename);
        WT_RET(__wt_schema_get_table(session, tablename, size, false, 0, &table));
    }

    WT_RET(__curtable_complete(session, table)); /* completeness check */

    if (table->is_simple) {
        /* Just return a cursor on the underlying data source. */
        ret = __wt_open_cursor(session, table->cgroups[0]->source, NULL, cfg, cursorp);

        WT_TRET(__wt_schema_release_table(session, &table));
        if (ret == 0) {
            /* Fix up the public URI to match what was passed in. */
            cursor = *cursorp;
            __wt_free(session, cursor->uri);
            WT_TRET(__wt_strdup(session, uri, &cursor->uri));
        }
        return (ret);
    }

    WT_RET(__wt_calloc_one(session, &ctable));
    cursor = (WT_CURSOR *)ctable;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->internal_uri = table->iface.name;
    cursor->key_format = table->key_format;
    cursor->value_format = table->value_format;

    ctable->table = table;
    ctable->plan = table->plan;

    /* Handle projections. */
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    if (columns != NULL) {
        WT_ERR(__wt_struct_reformat(session, table, columns, strlen(columns), NULL, false, tmp));
        WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &cursor->value_format));

        WT_ERR(__wt_buf_init(session, tmp, 0));
        WT_ERR(__wt_struct_plan(session, table, columns, strlen(columns), false, tmp));
        WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &ctable->plan));
    }

    /*
     * random_retrieval Random retrieval cursors only support next, reset and close.
     */
    WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0) {
        __wt_cursor_set_notsup(cursor);
        cursor->next = __curtable_next_random;
        cursor->reset = __curtable_reset;
    }

    WT_ERR(__wt_cursor_init(cursor, cursor->internal_uri, owner, cfg, cursorp));

    if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON))
        WT_ERR(__wt_json_column_init(cursor, uri, table->key_format, NULL, &table->colconf));

    /*
     * Open the colgroup cursors immediately: we're going to need them for
     * any operation.  We defer opening index cursors until we need them
     * for an update.  Note that this must come after the call to
     * __wt_cursor_init: the table cursor must already be on the list of
     * session cursors or we can't work out where to put the colgroup
     * cursor(s).
     */
    WT_ERR(__curtable_open_colgroups(ctable, cfg));

    /*
     * We'll need to squirrel away a copy of the cursor configuration for if/when we open indices.
     *
     * cfg[0] is the baseline configuration for the cursor open and we can acquire another copy from
     * the configuration structures, so it would be reasonable not to copy it here: but I'd rather
     * be safe than sorry.
     *
     * cfg[1] is the application configuration.
     *
     * Underlying indices are always opened without dump or readonly; that information is appended
     * to cfg[1] so later "fast" configuration calls (checking only cfg[0] and cfg[1]) work. I don't
     * expect to see more than two configuration strings here, but it's written to compact into two
     * configuration strings, a copy of cfg[0] and the rest in cfg[1].
     */
    WT_ERR(__wt_calloc_def(session, 3, &ctable->cfg));
    WT_ERR(__wt_strdup(session, cfg[0], &ctable->cfg[0]));
    WT_ERR(__wt_buf_set(session, tmp, "", 0));
    for (cfg_cnt = 1; cfg[cfg_cnt] != NULL; ++cfg_cnt)
        WT_ERR(__wt_buf_catfmt(session, tmp, "%s,", cfg[cfg_cnt]));
    WT_ERR(__wt_buf_catfmt(session, tmp, "dump=\"\",readonly=0"));
    WT_ERR(__wt_strdup(session, tmp->data, &ctable->cfg[1]));

    if (0) {
err:
        if (*cursorp != NULL) {
            /*
             * When a dump cursor is opened, then *cursorp, not cursor, is the dump cursor. Close
             * the dump cursor, and the table cursor will be closed as its child.
             */
            cursor = *cursorp;
            *cursorp = NULL;
        }
        WT_TRET(cursor->close(cursor));
    }

    __wt_scr_free(session, &tmp);
    return (ret);
}
