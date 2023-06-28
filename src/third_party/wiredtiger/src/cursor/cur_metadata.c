/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Custom NEED macros for metadata cursors - that copy the values into the backing metadata table
 * cursor.
 */
#define WT_MD_CURSOR_NEEDKEY(cursor)                                                      \
    do {                                                                                  \
        WT_ERR(__cursor_needkey(cursor));                                                 \
        WT_ERR(__wt_buf_set(session, &((WT_CURSOR_METADATA *)(cursor))->file_cursor->key, \
          (cursor)->key.data, (cursor)->key.size));                                       \
        F_SET(((WT_CURSOR_METADATA *)(cursor))->file_cursor, WT_CURSTD_KEY_EXT);          \
    } while (0)

#define WT_MD_CURSOR_NEEDVALUE(cursor)                                                      \
    do {                                                                                    \
        WT_ERR(__cursor_needvalue(cursor));                                                 \
        WT_ERR(__wt_buf_set(session, &((WT_CURSOR_METADATA *)(cursor))->file_cursor->value, \
          (cursor)->value.data, (cursor)->value.size));                                     \
        F_SET(((WT_CURSOR_METADATA *)(cursor))->file_cursor, WT_CURSTD_VALUE_EXT);          \
    } while (0)

/*
 * __schema_source_config --
 *     Extract the "source" configuration key, lookup its metadata.
 */
static int
__schema_source_config(
  WT_SESSION_IMPL *session, WT_CURSOR *srch, const char *config, const char **result)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    char *v;

    WT_ERR(__wt_config_getones(session, config, "source", &cval));
    WT_ERR(__wt_scr_alloc(session, cval.len + 10, &buf));
    WT_ERR(__wt_buf_fmt(session, buf, "%.*s", (int)cval.len, cval.str));
    srch->set_key(srch, buf->data);
    if ((ret = srch->search(srch)) != 0)
        WT_ERR_MSG(session, ret, "metadata information for source configuration \"%s\" not found",
          (const char *)buf->data);
    WT_ERR(srch->get_value(srch, &v));
    WT_ERR(__wt_strdup(session, v, result));

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __schema_create_collapse --
 *     Discard any configuration information from a schema entry that is not applicable to an
 *     session.create call. For a table URI that contains no named column groups, fold in the
 *     configuration from the implicit column group and its source. For a named column group or
 *     index URI, fold in its source. For a table URI that contains named column groups, we return
 *     only the table portion.
 */
static int
__schema_create_collapse(WT_SESSION_IMPL *session, WT_CURSOR_METADATA *mdc, const char *key,
  const char *value, char **value_ret)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM cgconf, ckey, cval;
    WT_CURSOR *c;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    int i, ncolgroups;
    const char *_cfg[7] = {NULL, NULL, NULL, NULL, NULL, value, NULL};
    const char **cfg, **firstcfg, **lastcfg, *v;
    bool tiered_shared;

    lastcfg = cfg = &_cfg[5]; /* position on value */
    tiered_shared = false;
    c = NULL;
    if (key != NULL && WT_PREFIX_SKIP(key, "table:")) {
        /*
         * Check if the table has declared column groups. If it does, return just the table info.
         * One can get the creation metadata for an index or column group table itself or for simple
         * tables.
         */
        WT_RET(__wt_config_getones(session, value, "colgroups", &cgconf));

        __wt_config_subinit(session, &cparser, &cgconf);
        if ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0) {
            firstcfg = cfg;
            goto skip;
        }
        WT_RET_NOTFOUND_OK(ret);

        if (((ret = __wt_config_getones(session, value, "shared", &cval)) == 0) && cval.val)
            tiered_shared = true;
        WT_RET_NOTFOUND_OK(ret);

        /*
         * A simple table have default one column group except the tiered storage shared table that
         * will have default 2 column groups.
         */
        ncolgroups = tiered_shared ? 2 : 1;
        c = mdc->create_cursor;
        WT_ERR(__wt_scr_alloc(session, 0, &buf));

        for (i = 0; i < ncolgroups; i++) {
            if (tiered_shared)
                /* When a tiered storage shared table is created, we create two column groups. */
                WT_ERR(__wt_schema_tiered_shared_colgroup_name(
                  session, key, i == 0 ? true : false, buf));
            else
                /* When a table is created without column groups, we create one without a name. */
                WT_ERR(__wt_buf_fmt(session, buf, "colgroup:%s", key));

            c->set_key(c, buf->data);
            if ((ret = c->search(c)) != 0)
                WT_ERR_MSG(session, ret,
                  "metadata information for source configuration \"%s\" not found",
                  (const char *)buf->data);
            WT_ERR(c->get_value(c, &v));
            WT_ERR(__wt_strdup(session, v, --cfg));
            WT_ERR(__schema_source_config(session, c, v, --cfg));
        }
    } else if (key != NULL && (WT_PREFIX_SKIP(key, "colgroup:") || WT_PREFIX_SKIP(key, "index:"))) {
        if (strchr(key, ':') != NULL) {
            c = mdc->create_cursor;
            WT_ERR(__wt_strdup(session, value, --cfg));
            WT_ERR(__schema_source_config(session, c, value, --cfg));
        }
    }

    firstcfg = cfg;
    *--firstcfg = WT_CONFIG_BASE(session, WT_SESSION_create);
skip:
    WT_ERR(__wt_config_collapse(session, firstcfg, value_ret));

err:
    for (; cfg < lastcfg; cfg++)
        __wt_free(session, *cfg);
    if (c != NULL)
        WT_TRET(c->reset(c));
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __curmetadata_setkv --
 *     Copy key/value into the public cursor, stripping internal metadata for "create-only" cursors.
 */
static int
__curmetadata_setkv(WT_CURSOR_METADATA *mdc, WT_CURSOR *fc)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    char *value;

    value = NULL;
    c = &mdc->iface;
    session = (WT_SESSION_IMPL *)c->session;

    c->key.data = fc->key.data;
    c->key.size = fc->key.size;
    if (F_ISSET(mdc, WT_MDC_CREATEONLY)) {
        WT_ERR(__schema_create_collapse(session, mdc, fc->key.data, fc->value.data, &value));
        WT_ERR(__wt_buf_set(session, &c->value, value, strlen(value) + 1));
    } else {
        c->value.data = fc->value.data;
        c->value.size = fc->value.size;
    }

    F_SET(c, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    F_CLR(mdc, WT_MDC_ONMETADATA);
    F_SET(mdc, WT_MDC_POSITIONED);

err:
    __wt_free(session, value);
    return (ret);
}

/*
 * Check if a key matches the metadata. The public value is "metadata:", but also check for the
 * internal version of the URI.
 */
#define WT_KEY_IS_METADATA(key)                                          \
    ((key)->size > 0 &&                                                  \
      (WT_STRING_MATCH(WT_METADATA_URI, (key)->data, (key)->size - 1) || \
        WT_STRING_MATCH(WT_METAFILE_URI, (key)->data, (key)->size - 1)))

/*
 * __curmetadata_metadata_search --
 *     Retrieve the metadata for the metadata table
 */
static int
__curmetadata_metadata_search(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    char *value, *stripped;

    mdc = (WT_CURSOR_METADATA *)cursor;

    /* The metadata search interface allocates a new string in value. */
    WT_RET(__wt_metadata_search(session, WT_METAFILE_URI, &value));

    if (F_ISSET(mdc, WT_MDC_CREATEONLY)) {
        ret = __schema_create_collapse(session, mdc, NULL, value, &stripped);
        __wt_free(session, value);
        WT_RET(ret);
        value = stripped;
    }

    ret = __wt_buf_setstr(session, &cursor->value, value);
    __wt_free(session, value);
    WT_RET(ret);

    WT_RET(__wt_buf_setstr(session, &cursor->key, WT_METADATA_URI));

    F_SET(mdc, WT_MDC_ONMETADATA | WT_MDC_POSITIONED);
    F_SET(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    return (0);
}

/*
 * __curmetadata_compare --
 *     WT_CURSOR->compare method for the metadata cursor type.
 */
static int
__curmetadata_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_CURSOR *a_file_cursor, *b_file_cursor;
    WT_CURSOR_METADATA *a_mdc, *b_mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    a_mdc = ((WT_CURSOR_METADATA *)a);
    b_mdc = ((WT_CURSOR_METADATA *)b);
    a_file_cursor = a_mdc->file_cursor;
    b_file_cursor = b_mdc->file_cursor;

    CURSOR_API_CALL(a, session, compare, CUR2BT(a_file_cursor));

    if (b->compare != __curmetadata_compare)
        WT_ERR_MSG(session, EINVAL, "Can only compare cursors of the same type");

    WT_MD_CURSOR_NEEDKEY(a);
    WT_MD_CURSOR_NEEDKEY(b);

    if (F_ISSET(a_mdc, WT_MDC_ONMETADATA)) {
        if (F_ISSET(b_mdc, WT_MDC_ONMETADATA))
            *cmpp = 0;
        else
            *cmpp = 1;
    } else if (F_ISSET(b_mdc, WT_MDC_ONMETADATA))
        *cmpp = -1;
    else
        ret = a_file_cursor->compare(a_file_cursor, b_file_cursor, cmpp);

err:
    API_END_RET(session, ret);
}

/*
 * __curmetadata_next --
 *     WT_CURSOR->next method for the metadata cursor type.
 */
static int
__curmetadata_next(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, next, CUR2BT(file_cursor));

    if (!F_ISSET(mdc, WT_MDC_POSITIONED))
        WT_ERR(__curmetadata_metadata_search(session, cursor));
    else {
        /*
         * When applications open metadata cursors, they expect to see all schema-level operations
         * reflected in the results. Query at read-uncommitted to avoid confusion caused by the
         * current transaction state.
         *
         * Don't exit from the scan if we find an incomplete entry: just skip over it.
         */
        for (;;) {
            WT_WITH_TXN_ISOLATION(
              session, WT_ISO_READ_UNCOMMITTED, ret = file_cursor->next(mdc->file_cursor));
            WT_ERR(ret);
            WT_WITH_TXN_ISOLATION(
              session, WT_ISO_READ_UNCOMMITTED, ret = __curmetadata_setkv(mdc, file_cursor));
            if (ret == 0)
                break;
            WT_ERR_NOTFOUND_OK(ret, false);
        }
    }

err:
    if (ret != 0) {
        F_CLR(mdc, WT_MDC_POSITIONED | WT_MDC_ONMETADATA);
        F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    }
    API_END_RET(session, ret);
}

/*
 * __curmetadata_prev --
 *     WT_CURSOR->prev method for the metadata cursor type.
 */
static int
__curmetadata_prev(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, prev, CUR2BT(file_cursor));

    if (F_ISSET(mdc, WT_MDC_ONMETADATA)) {
        ret = WT_NOTFOUND;
        goto err;
    }

    /*
     * Don't exit from the scan if we find an incomplete entry: just skip over it.
     */
    for (;;) {
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = file_cursor->prev(file_cursor));
        if (ret == WT_NOTFOUND) {
            WT_ERR(__curmetadata_metadata_search(session, cursor));
            break;
        }
        WT_ERR(ret);
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = __curmetadata_setkv(mdc, file_cursor));
        if (ret == 0)
            break;
        WT_ERR_NOTFOUND_OK(ret, false);
    }

err:
    if (ret != 0) {
        F_CLR(mdc, WT_MDC_POSITIONED | WT_MDC_ONMETADATA);
        F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    }
    API_END_RET(session, ret);
}

/*
 * __curmetadata_reset --
 *     WT_CURSOR->reset method for the metadata cursor type.
 */
static int
__curmetadata_reset(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, CUR2BT(file_cursor));

    if (F_ISSET(mdc, WT_MDC_POSITIONED) && !F_ISSET(mdc, WT_MDC_ONMETADATA))
        ret = file_cursor->reset(file_cursor);
    F_CLR(mdc, WT_MDC_POSITIONED | WT_MDC_ONMETADATA);
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __curmetadata_search --
 *     WT_CURSOR->search method for the metadata cursor type.
 */
static int
__curmetadata_search(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, search, CUR2BT(file_cursor));

    WT_MD_CURSOR_NEEDKEY(cursor);

    if (WT_KEY_IS_METADATA(&cursor->key))
        WT_ERR(__curmetadata_metadata_search(session, cursor));
    else {
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = file_cursor->search(file_cursor));
        WT_ERR(ret);
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = __curmetadata_setkv(mdc, file_cursor));
        WT_ERR(ret);
    }

err:
    if (ret != 0) {
        F_CLR(mdc, WT_MDC_POSITIONED | WT_MDC_ONMETADATA);
        F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    }
    API_END_RET(session, ret);
}

/*
 * __curmetadata_search_near --
 *     WT_CURSOR->search_near method for the metadata cursor type.
 */
static int
__curmetadata_search_near(WT_CURSOR *cursor, int *exact)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, search_near, CUR2BT(file_cursor));

    WT_MD_CURSOR_NEEDKEY(cursor);

    if (WT_KEY_IS_METADATA(&cursor->key)) {
        WT_ERR(__curmetadata_metadata_search(session, cursor));
        *exact = 1;
    } else {
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = file_cursor->search_near(file_cursor, exact));
        WT_ERR(ret);
        WT_WITH_TXN_ISOLATION(
          session, WT_ISO_READ_UNCOMMITTED, ret = __curmetadata_setkv(mdc, file_cursor));
        WT_ERR(ret);
    }

err:
    if (ret != 0) {
        F_CLR(mdc, WT_MDC_POSITIONED | WT_MDC_ONMETADATA);
        F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
    }
    API_END_RET(session, ret);
}

/*
 * __curmetadata_insert --
 *     WT_CURSOR->insert method for the metadata cursor type.
 */
static int
__curmetadata_insert(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, insert, CUR2BT(file_cursor));

    WT_MD_CURSOR_NEEDKEY(cursor);
    WT_MD_CURSOR_NEEDVALUE(cursor);

    /*
     * Since the key/value formats are 's' the WT_ITEMs must contain a NULL terminated string.
     */
    ret = __wt_metadata_insert(session, cursor->key.data, cursor->value.data);

err:
    API_END_RET(session, ret);
}

/*
 * __curmetadata_update --
 *     WT_CURSOR->update method for the metadata cursor type.
 */
static int
__curmetadata_update(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, update, CUR2BT(file_cursor));

    WT_MD_CURSOR_NEEDKEY(cursor);
    WT_MD_CURSOR_NEEDVALUE(cursor);

    /*
     * Since the key/value formats are 's' the WT_ITEMs must contain a NULL terminated string.
     */
    ret = __wt_metadata_update(session, cursor->key.data, cursor->value.data);

err:
    API_END_RET(session, ret);
}

/*
 * __curmetadata_remove --
 *     WT_CURSOR->remove method for the metadata cursor type.
 */
static int
__curmetadata_remove(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    file_cursor = mdc->file_cursor;
    CURSOR_API_CALL(cursor, session, remove, CUR2BT(file_cursor));

    WT_MD_CURSOR_NEEDKEY(cursor);

    /*
     * Since the key format is 's' the WT_ITEM must contain a NULL terminated string.
     */
    ret = __wt_metadata_remove(session, cursor->key.data);

err:
    API_END_RET(session, ret);
}

/*
 * __curmetadata_close --
 *     WT_CURSOR->close method for the metadata cursor type.
 */
static int
__curmetadata_close(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    mdc = (WT_CURSOR_METADATA *)cursor;
    c = mdc->file_cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, c == NULL ? NULL : CUR2BT(c));
err:

    if (c != NULL)
        WT_TRET(c->close(c));
    if ((c = mdc->create_cursor) != NULL)
        WT_TRET(c->close(c));
    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_curmetadata_open --
 *     WT_SESSION->open_cursor method for metadata cursors. Metadata cursors are a similar to a file
 *     cursor on the special metadata table, except that the metadata for the metadata table (which
 *     is stored in the turtle file) can also be queried. Metadata cursors are read-only by default.
 */
int
__wt_curmetadata_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __curmetadata_compare,                          /* compare */
      __wt_cursor_equals,                             /* equals */
      __curmetadata_next,                             /* next */
      __curmetadata_prev,                             /* prev */
      __curmetadata_reset,                            /* reset */
      __curmetadata_search,                           /* search */
      __curmetadata_search_near,                      /* search-near */
      __curmetadata_insert,                           /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __curmetadata_update,                           /* update */
      __curmetadata_remove,                           /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curmetadata_close);                           /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_METADATA *mdc;
    WT_DECL_RET;
    WT_CONFIG_ITEM cval;

    WT_RET(__wt_calloc_one(session, &mdc));
    cursor = (WT_CURSOR *)mdc;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = "S";
    cursor->value_format = "S";

    /*
     * Open the file cursor for operations on the regular metadata; don't use the existing, cached
     * session metadata cursor, the configuration may not be the same.
     */
    WT_ERR(__wt_metadata_cursor_open(session, cfg[1], &mdc->file_cursor));

    /*
     * If we are only returning create config, strip internal metadata. We'll need some extra
     * cursors to pull out column group information and chase "source" entries.
     */
    if (strcmp(uri, "metadata:create") == 0) {
        F_SET(mdc, WT_MDC_CREATEONLY);
        WT_ERR(__wt_metadata_cursor_open(session, cfg[1], &mdc->create_cursor));
    }

    WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

    /*
     * Metadata cursors default to readonly; if not set to not-readonly, they are permanently
     * readonly and cannot be reconfigured.
     */
    WT_ERR(__wt_config_gets_def(session, cfg, "readonly", 1, &cval));
    if (cval.val != 0) {
        cursor->insert = __wt_cursor_notsup;
        cursor->update = __wt_cursor_notsup;
        cursor->remove = __wt_cursor_notsup;
    }

    if (0) {
err:
        WT_TRET(__curmetadata_close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
