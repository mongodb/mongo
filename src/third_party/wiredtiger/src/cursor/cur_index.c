/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curindex_get_value --
 *     WT_CURSOR->get_value implementation for index cursors.
 */
static int
__curindex_get_value(WT_CURSOR *cursor, ...)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    JOINABLE_CURSOR_API_CALL(cursor, session, get_value, NULL);

    va_start(ap, cursor);
    ret = __wt_curindex_get_valuev(cursor, ap);
    va_end(ap);

err:
    API_END_RET(session, ret);
}

/*
 * __curindex_set_valuev --
 *     WT_CURSOR->set_value implementation for index cursors.
 */
static int
__curindex_set_valuev(WT_CURSOR *cursor, va_list ap)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_UNUSED(ap);

    JOINABLE_CURSOR_API_CALL(cursor, session, set_value, NULL);
    WT_ERR_MSG(session, ENOTSUP, "WT_CURSOR.set_value not supported for index cursors");

err:
    cursor->saved_err = ret;
    F_CLR(cursor, WT_CURSTD_VALUE_SET);
    API_END_RET(session, ret);
}

/*
 * __curindex_set_value --
 *     WT_CURSOR->set_value implementation for index cursors.
 */
static void
__curindex_set_value(WT_CURSOR *cursor, ...)
{
    va_list ap;

    va_start(ap, cursor);
    WT_IGNORE_RET(__curindex_set_valuev(cursor, ap));
    va_end(ap);
}

/*
 * __curindex_compare --
 *     WT_CURSOR->compare method for the index cursor type.
 */
static int
__curindex_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cindex = (WT_CURSOR_INDEX *)a;
    JOINABLE_CURSOR_API_CALL(a, session, compare, NULL);

    /* Check both cursors are "index:" type. */
    if (!WT_PREFIX_MATCH(a->uri, "index:") || strcmp(a->uri, b->uri) != 0)
        WT_ERR_MSG(session, EINVAL, "Cursors must reference the same object");

    WT_ERR(__cursor_checkkey(a));
    WT_ERR(__cursor_checkkey(b));

    ret = __wt_compare(session, cindex->index->collator, &a->key, &b->key, cmpp);

err:
    API_END_RET(session, ret);
}

/*
 * __curindex_move --
 *     When an index cursor changes position, set the primary key in the associated column groups
 *     and update their positions to match.
 */
static int
__curindex_move(WT_CURSOR_INDEX *cindex)
{
    WT_CURSOR **cp, *first;
    WT_SESSION_IMPL *session;
    u_int i;

    session = CUR2S(cindex);
    first = NULL;

    /* Point the public cursor to the key in the child. */
    __wt_cursor_set_raw_key(&cindex->iface, &cindex->child->key);
    F_CLR(&cindex->iface, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    for (i = 0, cp = cindex->cg_cursors; i < WT_COLGROUPS(cindex->table); i++, cp++) {
        if (*cp == NULL)
            continue;
        if (first == NULL) {
            /*
             * Set the primary key -- note that we need the primary key columns, so we have to use
             * the full key format, not just the public columns.
             */
            WT_RET(__wt_schema_project_slice(session, cp, cindex->index->key_plan, 1,
              cindex->index->key_format, &cindex->iface.key));
            first = *cp;
        } else {
            (*cp)->key.data = first->key.data;
            (*cp)->key.size = first->key.size;
            (*cp)->recno = first->recno;
        }
        F_SET(*cp, WT_CURSTD_KEY_EXT);
        if (cindex->cg_needvalue[i])
            WT_RET((*cp)->search(*cp));
    }

    F_SET(&cindex->iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    return (0);
}

/*
 * __curindex_next --
 *     WT_CURSOR->next method for index cursors.
 */
static int
__curindex_next(WT_CURSOR *cursor)
{
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cindex = (WT_CURSOR_INDEX *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, next, NULL);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    if ((ret = cindex->child->next(cindex->child)) == 0)
        ret = __curindex_move(cindex);

err:
    API_END_RET(session, ret);
}

/*
 * __curindex_prev --
 *     WT_CURSOR->prev method for index cursors.
 */
static int
__curindex_prev(WT_CURSOR *cursor)
{
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cindex = (WT_CURSOR_INDEX *)cursor;
    JOINABLE_CURSOR_API_CALL(cursor, session, prev, NULL);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    if ((ret = cindex->child->prev(cindex->child)) == 0)
        ret = __curindex_move(cindex);

err:
    API_END_RET(session, ret);
}

/*
 * __curindex_reset --
 *     WT_CURSOR->reset method for index cursors.
 */
static int
__curindex_reset(WT_CURSOR *cursor)
{
    WT_CURSOR **cp;
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    cindex = (WT_CURSOR_INDEX *)cursor;
    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_TRET(cindex->child->reset(cindex->child));
    for (i = 0, cp = cindex->cg_cursors; i < WT_COLGROUPS(cindex->table); i++, cp++) {
        if (*cp == NULL)
            continue;
        WT_TRET((*cp)->reset(*cp));
    }

    /*
     * The bounded cursor API clears bounds on external calls to cursor->reset. We determine this by
     * guarding the call to cursor bound reset with the API_USER_ENTRY macro. Doing so prevents
     * internal API calls from resetting cursor bounds unintentionally, e.g. cursor->remove.
     */
    if (API_USER_ENTRY(session))
        __wt_cursor_bound_reset(cindex->child);
err:
    API_END_RET(session, ret);
}

/*
 * __curindex_search --
 *     WT_CURSOR->search method for index cursors.
 */
static int
__curindex_search(WT_CURSOR *cursor)
{
    WT_CURSOR *child;
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_ITEM found_key;
    WT_SESSION_IMPL *session;
    int cmp;

    cindex = (WT_CURSOR_INDEX *)cursor;
    child = cindex->child;
    JOINABLE_CURSOR_API_CALL(cursor, session, search, NULL);

    /*
     * We are searching using the application-specified key, which (usually) doesn't contain the
     * primary key, so it is just a prefix of any matching index key. Do a search_near, step to the
     * next entry if we land on one that is too small, then check that the prefix matches.
     */
    __wt_cursor_set_raw_key(child, &cursor->key);
    WT_ERR(child->search_near(child, &cmp));

    if (cmp < 0)
        WT_ERR(child->next(child));

    /*
     * We expect partial matches, and want the smallest record with a key greater than or equal to
     * the search key.
     *
     * If the key we find is shorter than the search key, it can't possibly match.
     *
     * The only way for the key to be exactly equal is if there is an index on the primary key,
     * because otherwise the primary key columns will be appended to the index key, but we don't
     * disallow that (odd) case.
     */
    found_key = child->key;
    if (found_key.size < cursor->key.size)
        WT_ERR(WT_NOTFOUND);

    /*
     * Custom collators expect to see complete keys, pass an item containing all the visible fields
     * so it unpacks correctly.
     */
    if (cindex->index->collator != NULL && !F_ISSET(cursor, WT_CURSTD_RAW_SEARCH))
        WT_ERR(__wt_struct_repack(
          session, child->key_format, cindex->iface.key_format, &child->key, &found_key));
    else
        found_key.size = cursor->key.size;

    WT_ERR(__wt_compare(session, cindex->index->collator, &cursor->key, &found_key, &cmp));
    if (cmp != 0) {
        ret = WT_NOTFOUND;
        goto err;
    }

    WT_ERR(__curindex_move(cindex));

    if (0) {
err:
        F_CLR(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }

    API_END_RET(session, ret);
}

/*
 * __curindex_search_near --
 *     WT_CURSOR->search_near method for index cursors.
 */
static int
__curindex_search_near(WT_CURSOR *cursor, int *exact)
{
    WT_CURSOR *child;
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_ITEM found_key;
    WT_SESSION_IMPL *session;
    int cmp;

    cindex = (WT_CURSOR_INDEX *)cursor;
    child = cindex->child;
    JOINABLE_CURSOR_API_CALL(cursor, session, search, NULL);

    /*
     * We are searching using the application-specified key, which (usually) doesn't contain the
     * primary key, so it is just a prefix of any matching index key. That said, if there is an
     * exact match, we want to find the first matching index entry and set exact equal to zero.
     *
     * Do a search_near, and if we find an entry that is too small, step to the next one. In the
     * unlikely event of a search past the end of the tree, go back to the last key.
     */
    __wt_cursor_set_raw_key(child, &cursor->key);
    WT_ERR(child->search_near(child, &cmp));

    if (cmp < 0) {
        if ((ret = child->next(child)) == WT_NOTFOUND)
            ret = child->prev(child);
        WT_ERR(ret);
    }

    /*
     * We expect partial matches, and want the smallest record with a key greater than or equal to
     * the search key.
     *
     * If the found key starts with the search key, we indicate a match by setting exact equal to
     * zero.
     *
     * The compare function expects application-supplied keys to come first so we flip the sign of
     * the result to match what callers expect.
     */
    found_key = child->key;
    if (found_key.size > cursor->key.size) {
        /*
         * Custom collators expect to see complete keys, pass an item containing all the visible
         * fields so it unpacks correctly.
         */
        if (cindex->index->collator != NULL)
            WT_ERR(__wt_struct_repack(session, cindex->child->key_format, cindex->iface.key_format,
              &child->key, &found_key));
        else
            found_key.size = cursor->key.size;
    }

    WT_ERR(__wt_compare(session, cindex->index->collator, &cursor->key, &found_key, exact));
    *exact = -*exact;

    WT_ERR(__curindex_move(cindex));

    if (0) {
err:
        F_CLR(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }

    API_END_RET(session, ret);
}

/*
 * __increment_bound_array --
 *     Increment the given buffer by one bit, return true if we incremented the buffer or not. If
 *     all of the values inside the buffer are UINT8_MAX value we do not increment the buffer.
 */
static inline bool
__increment_bound_array(WT_ITEM *user_item)
{
    size_t usz, i;
    uint8_t *userp;

    usz = user_item->size;
    userp = (uint8_t *)user_item->data;
    /*
     * First loop through all max values on the buffer from the end. This is to find the appropriate
     * position to increment add one to the byte.
     */
    for (i = usz - 1; i > 0 && userp[i] == UINT8_MAX; --i)
        ;

    /*
     * If all of the buffer are max values, we don't need to do increment the buffer as the key
     * format is a fixed length format. Ideally we double check that the table format has a fixed
     * length string.
     */
    if (i == 0 && userp[i] == UINT8_MAX)
        return (false);

    userp[i++] += 1;
    for (; i < usz; ++i)
        userp[i] = 0;
    return (true);
}

/*
 * __curindex_bound --
 *     WT_CURSOR->bound method for the index cursor type.
 *
 */
static int
__curindex_bound(WT_CURSOR *cursor, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *child;
    WT_CURSOR_BOUNDS_STATE saved_bounds;
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool inclusive;

    cindex = (WT_CURSOR_INDEX *)cursor;
    child = cindex->child;
    WT_CLEAR(saved_bounds);
    inclusive = false;

    JOINABLE_CURSOR_API_CALL_CONF(cursor, session, bound, config, cfg, NULL);

    /* Save the current state of the bounds in case we fail to apply the new state. */
    WT_ERR(__wt_cursor_bounds_save(session, child, &saved_bounds));

    WT_ERR(__wt_config_gets(session, cfg, "action", &cval));

    /* When setting bounds, we need to check that the key is set. */
    if (WT_STRING_MATCH("set", cval.str, cval.len)) {
        WT_ERR(__cursor_checkkey(cursor));

        /* Point the public cursor to the key in the child. */
        __wt_cursor_set_raw_key(child, &cursor->key);

        WT_ERR(__wt_config_gets(session, cfg, "inclusive", &cval));
        inclusive = cval.val != 0;

        /* Check if we have set the lower bound or upper bound. */
        WT_ERR(__wt_config_gets(session, cfg, "bound", &cval));
    }

    WT_ERR(child->bound(child, config));

    /*
     * Index tables internally combines the user chosen columns with the key format of the table to
     * maintain uniqueness between each key. However user's are not aware of the combining the key
     * format and cannot set bounds based on the combined index format. Therefore WiredTiger needs
     * to internally fix this by incrementing one bit to the array in two cases:
     *  1. If the set bound is lower and it is not inclusive.
     *  2. If the set bound is upper and it is inclusive.
     */
    if (WT_STRING_MATCH("lower", cval.str, cval.len) && !inclusive) {
        /*
         * In the case that we can't increment the lower bound, it means we have reached the max
         * possible key for the lower bound. This is a very tricky case since there isn't a trivial
         * way to set the lower bound to a key exclusively not show the max possible key. This is
         * due to how index key formats are combined with the main table's key format. In this edge
         * case we expect no entries to be returned, thus we return it back to the user with an
         * error instead.
         */
        if (!__increment_bound_array(&child->lower_bound)) {
            WT_ERR(__wt_cursor_bounds_restore(session, child, &saved_bounds));
            WT_ERR_MSG(session, EINVAL,
              "Cannot set index cursors with the max possible key as the lower bound");
        }
    }

    if (WT_STRING_MATCH("upper", cval.str, cval.len) && inclusive) {
        /*
         * In the case that we can't increment the upper bound, it means we have reached the max
         * possible key for the upper bound. In that case we can just clear upper bound.
         */
        if (!__increment_bound_array(&child->upper_bound))
            WT_ERR(child->bound(child, "action=clear,bound=upper"));
    }
err:

    __wt_scr_free(session, &saved_bounds.lower_bound);
    __wt_scr_free(session, &saved_bounds.upper_bound);
    API_END_RET(session, ret);
}

/*
 * __curindex_close --
 *     WT_CURSOR->close method for index cursors.
 */
static int
__curindex_close(WT_CURSOR *cursor)
{
    WT_CURSOR **cp;
    WT_CURSOR_INDEX *cindex;
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_SESSION_IMPL *session;
    u_int i;

    cindex = (WT_CURSOR_INDEX *)cursor;
    idx = cindex->index;
    JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    if (cindex->cg_cursors != NULL)
        for (i = 0, cp = cindex->cg_cursors; i < WT_COLGROUPS(cindex->table); i++, cp++)
            if (*cp != NULL) {
                WT_TRET((*cp)->close(*cp));
                *cp = NULL;
            }

    __wt_free(session, cindex->cg_needvalue);
    __wt_free(session, cindex->cg_cursors);
    if (cindex->key_plan != idx->key_plan)
        __wt_free(session, cindex->key_plan);
    if (cursor->value_format != cindex->table->value_format)
        __wt_free(session, cursor->value_format);
    if (cindex->value_plan != idx->value_plan)
        __wt_free(session, cindex->value_plan);

    if (cindex->child != NULL)
        WT_TRET(cindex->child->close(cindex->child));

    WT_TRET(__wt_schema_release_table(session, &cindex->table));
    /* The URI is owned by the index. */
    cursor->internal_uri = NULL;
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __curindex_open_colgroups --
 *     Open cursors on the column groups required for an index cursor.
 */
static int
__curindex_open_colgroups(WT_SESSION_IMPL *session, WT_CURSOR_INDEX *cindex, const char *cfg_arg[])
{
    WT_CURSOR **cp;
    WT_TABLE *table;
    u_long arg;
    /* Child cursors are opened with dump disabled. */
    const char *cfg[] = {cfg_arg[0], cfg_arg[1], "dump=\"\"", NULL};
    char *proj;
    size_t cgcnt;

    table = cindex->table;
    cgcnt = WT_COLGROUPS(table);
    WT_RET(__wt_calloc_def(session, cgcnt, &cindex->cg_needvalue));
    WT_RET(__wt_calloc_def(session, cgcnt, &cp));
    cindex->cg_cursors = cp;

    /* Work out which column groups we need. */
    for (proj = (char *)cindex->value_plan; *proj != '\0'; proj++) {
        arg = strtoul(proj, &proj, 10);
        if (*proj == WT_PROJ_VALUE)
            cindex->cg_needvalue[arg] = 1;
        if ((*proj != WT_PROJ_KEY && *proj != WT_PROJ_VALUE) || cp[arg] != NULL)
            continue;
        WT_RET(
          __wt_open_cursor(session, table->cgroups[arg]->source, &cindex->iface, cfg, &cp[arg]));
    }

    return (0);
}

/*
 * __wt_curindex_open --
 *     WT_SESSION->open_cursor method for index cursors.
 */
int
__wt_curindex_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __curindex_get_value,                           /* get-value */
      __wt_cursor_get_raw_key_value_notsup,           /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __curindex_set_value,                           /* set-value */
      __curindex_compare,                             /* compare */
      __wt_cursor_equals,                             /* equals */
      __curindex_next,                                /* next */
      __curindex_prev,                                /* prev */
      __curindex_reset,                               /* reset */
      __curindex_search,                              /* search */
      __curindex_search_near,                         /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __curindex_bound,                               /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curindex_close);                              /* close */
    WT_CURSOR_INDEX *cindex;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_TABLE *table;
    const char *columns, *idxname, *tablename;
    size_t namesize;

    tablename = uri;
    if (!WT_PREFIX_SKIP(tablename, "index:") || (idxname = strchr(tablename, ':')) == NULL)
        WT_RET_MSG(session, EINVAL, "Invalid cursor URI: '%s'", uri);
    namesize = (size_t)(idxname - tablename);
    ++idxname;

    if ((ret = __wt_schema_get_table(session, tablename, namesize, false, 0, &table)) != 0) {
        if (ret == WT_NOTFOUND)
            WT_RET_MSG(session, EINVAL, "Cannot open cursor '%s' on unknown table", uri);
        return (ret);
    }

    columns = strchr(idxname, '(');
    if (columns == NULL)
        namesize = strlen(idxname);
    else
        namesize = (size_t)(columns - idxname);

    if ((ret = __wt_schema_open_index(session, table, idxname, namesize, &idx)) != 0) {
        WT_TRET(__wt_schema_release_table(session, &table));
        return (ret);
    }
    WT_RET(__wt_calloc_one(session, &cindex));

    cursor = (WT_CURSOR *)cindex;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;

    cindex->table = table;
    cindex->index = idx;
    cindex->key_plan = idx->key_plan;
    cindex->value_plan = idx->value_plan;

    cursor->internal_uri = idx->name;
    cursor->key_format = idx->idxkey_format;
    cursor->value_format = table->value_format;

    /*
     * XXX A very odd corner case is an index with a recno key. The only way to get here is by
     * creating an index on a column store using only the primary's recno as the index key. Disallow
     * that for now.
     */
    if (WT_CURSOR_RECNO(cursor))
        WT_ERR_MSG(session, WT_ERROR,
          "Column store indexes based on a record number primary key are not supported");

    /* Handle projections. */
    if (columns != NULL) {
        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_struct_reformat(session, table, columns, strlen(columns), NULL, false, tmp));
        WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &cursor->value_format));

        WT_ERR(__wt_buf_init(session, tmp, 0));
        WT_ERR(__wt_struct_plan(session, table, columns, strlen(columns), false, tmp));
        WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &cindex->value_plan));
    }

    WT_ERR(__wt_cursor_init(cursor, cursor->internal_uri, owner, cfg, cursorp));

    WT_ERR(__wt_open_cursor(session, idx->source, cursor, cfg, &cindex->child));

    /* Open the column groups needed for this index cursor. */
    WT_ERR(__curindex_open_colgroups(session, cindex, cfg));

    if (F_ISSET(cursor, WT_CURSTD_DUMP_JSON))
        WT_ERR(
          __wt_json_column_init(cursor, uri, table->key_format, &idx->colconf, &table->colconf));

    if (0) {
err:
        WT_TRET(__curindex_close(cursor));
        *cursorp = NULL;
    }

    __wt_scr_free(session, &tmp);
    return (ret);
}
