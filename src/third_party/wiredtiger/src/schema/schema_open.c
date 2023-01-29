/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_colgroup_name --
 *     Get the URI for a column group. This is used for metadata lookups. The only complexity here
 *     is that simple tables (with a single column group) use a simpler naming scheme.
 */
int
__wt_schema_colgroup_name(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *cgname, size_t len, WT_ITEM *buf)
{
    const char *tablename;

    tablename = table->iface.name;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "table:");

    return ((table->ncolgroups == 0) ?
        __wt_buf_fmt(session, buf, "colgroup:%s", tablename) :
        __wt_buf_fmt(session, buf, "colgroup:%s:%.*s", tablename, (int)len, cgname));
}

/*
 * __wt_schema_tiered_shared_colgroup_name --
 *     Get the URI for a tiered storage shared column group. This is used for metadata lookups.
 */
int
__wt_schema_tiered_shared_colgroup_name(
  WT_SESSION_IMPL *session, const char *tablename, bool active, WT_ITEM *buf)
{
    WT_PREFIX_SKIP(tablename, "table:");
    return (__wt_buf_fmt(session, buf, "colgroup:%s.%s", tablename, active ? "active" : "shared"));
}

/*
 * __wt_schema_open_colgroups --
 *     Open the column groups for a table.
 */
int
__wt_schema_open_colgroups(WT_SESSION_IMPL *session, WT_TABLE *table)
{
    WT_COLGROUP *colgroup;
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    u_int i;
    char *cgconfig;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE));

    if (table->cg_complete)
        return (0);

    colgroup = NULL;
    cgconfig = NULL;

    WT_RET(__wt_scr_alloc(session, 0, &buf));

    __wt_config_subinit(session, &cparser, &table->cgconf);

    /* Open each column group. */
    for (i = 0; i < WT_COLGROUPS(table); i++) {
        if (table->ncolgroups > 0)
            WT_ERR(__wt_config_next(&cparser, &ckey, &cval));
        else
            WT_CLEAR(ckey);

        /*
         * Always open from scratch: we may have failed part of the way through opening a table, or
         * column groups may have changed.
         */
        __wt_schema_destroy_colgroup(session, &table->cgroups[i]);

        WT_ERR(__wt_buf_init(session, buf, 0));
        if (table->is_tiered_shared)
            WT_ERR(__wt_schema_tiered_shared_colgroup_name(
              session, table->iface.name, i == 0 ? true : false, buf));
        else
            WT_ERR(__wt_schema_colgroup_name(session, table, ckey.str, ckey.len, buf));
        if ((ret = __wt_metadata_search(session, buf->data, &cgconfig)) != 0) {
            /* It is okay if the table is incomplete. */
            if (ret == WT_NOTFOUND)
                ret = 0;
            goto err;
        }

        WT_ERR(__wt_calloc_one(session, &colgroup));
        WT_ERR(__wt_strndup(session, buf->data, buf->size, &colgroup->name));
        colgroup->config = cgconfig;
        cgconfig = NULL;
        WT_ERR(__wt_config_getones(session, colgroup->config, "columns", &colgroup->colconf));
        WT_ERR(__wt_config_getones(session, colgroup->config, "source", &cval));
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &colgroup->source));
        table->cgroups[i] = colgroup;
        colgroup = NULL;
    }

    if (!table->is_simple) {
        WT_ERR(__wt_table_check(session, table));

        WT_ERR(__wt_buf_init(session, buf, 0));
        WT_ERR(__wt_struct_plan(session, table, table->colconf.str, table->colconf.len, true, buf));
        WT_ERR(__wt_strndup(session, buf->data, buf->size, &table->plan));
    }

    table->cg_complete = true;

err:
    __wt_scr_free(session, &buf);
    __wt_schema_destroy_colgroup(session, &colgroup);
    __wt_free(session, cgconfig);
    return (ret);
}

/*
 * __open_index --
 *     Open an index.
 */
static int
__open_index(WT_SESSION_IMPL *session, WT_TABLE *table, WT_INDEX *idx)
{
    WT_CONFIG colconf;
    WT_CONFIG_ITEM ckey, cval, metadata;
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(plan);
    WT_DECL_RET;
    u_int npublic_cols, i;

    WT_ERR(__wt_scr_alloc(session, 0, &buf));

    /* Get the data source from the index config. */
    WT_ERR(__wt_config_getones(session, idx->config, "source", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &idx->source));

    WT_ERR(__wt_config_getones(session, idx->config, "immutable", &cval));
    if (cval.val)
        F_SET(idx, WT_INDEX_IMMUTABLE);

    /*
     * Compatibility: we didn't always maintain collator information in index metadata, cope when it
     * isn't found.
     */
    WT_CLEAR(cval);
    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, idx->config, "collator", &cval), false);
    if (cval.len != 0) {
        WT_CLEAR(metadata);
        WT_ERR_NOTFOUND_OK(
          __wt_config_getones(session, idx->config, "app_metadata", &metadata), false);
        WT_ERR(__wt_collator_config(
          session, idx->name, &cval, &metadata, &idx->collator, &idx->collator_owned));
    }

    WT_ERR(__wt_extractor_config(
      session, idx->name, idx->config, &idx->extractor, &idx->extractor_owned));

    WT_ERR(__wt_config_getones(session, idx->config, "key_format", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &idx->key_format));

    /*
     * The key format for an index is somewhat subtle: the application specifies a set of columns
     * that it will use for the key, but the engine usually adds some hidden columns in order to
     * derive the primary key. These hidden columns are part of the file's key.
     *
     * The file's key_format is stored persistently, we need to calculate the index cursor key
     * format (which will usually omit some of those keys).
     */
    WT_ERR(__wt_buf_init(session, buf, 0));
    WT_ERR(__wt_config_getones(session, idx->config, "columns", &idx->colconf));

    /* Start with the declared index columns. */
    __wt_config_subinit(session, &colconf, &idx->colconf);
    for (npublic_cols = 0; (ret = __wt_config_next(&colconf, &ckey, &cval)) == 0; ++npublic_cols)
        WT_ERR(__wt_buf_catfmt(session, buf, "%.*s,", (int)ckey.len, ckey.str));
    if (ret != WT_NOTFOUND)
        goto err;

    /*
     * If we didn't find any columns, the index must have an extractor. We don't rely on this
     * unconditionally because it was only added to the metadata after version 2.3.1.
     */
    if (npublic_cols == 0) {
        WT_ERR(__wt_config_getones(session, idx->config, "index_key_columns", &cval));
        npublic_cols = (u_int)cval.val;
        WT_ASSERT(session, npublic_cols != 0);
        for (i = 0; i < npublic_cols; i++)
            WT_ERR(__wt_buf_catfmt(session, buf, "\"bad col\","));
    }

    /*
     * Now add any primary key columns from the table that are not already part of the index key.
     */
    __wt_config_subinit(session, &colconf, &table->colconf);
    for (i = 0; i < table->nkey_columns && (ret = __wt_config_next(&colconf, &ckey, &cval)) == 0;
         i++) {
        /*
         * If the primary key column is already in the secondary key, don't add it again.
         */
        if (__wt_config_subgetraw(session, &idx->colconf, &ckey, &cval) == 0)
            continue;
        WT_ERR(__wt_buf_catfmt(session, buf, "%.*s,", (int)ckey.len, ckey.str));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /*
     * If the table doesn't yet have its column groups, don't try to calculate a plan: we are just
     * checking that the index creation is sane.
     */
    if (!table->cg_complete)
        goto err;

    WT_ERR(__wt_scr_alloc(session, 0, &plan));
    WT_ERR(__wt_struct_plan(session, table, buf->data, buf->size, false, plan));
    WT_ERR(__wt_strndup(session, plan->data, plan->size, &idx->key_plan));

    /* Set up the cursor key format (the visible columns). */
    WT_ERR(__wt_buf_init(session, buf, 0));
    WT_ERR(__wt_struct_truncate(session, idx->key_format, npublic_cols, buf));
    WT_ERR(__wt_strndup(session, buf->data, buf->size, &idx->idxkey_format));

    /*
     * Add a trailing padding byte to the format. This ensures that there will be no special
     * optimization of the last column, so the primary key columns can be simply appended.
     */
    WT_ERR(__wt_buf_catfmt(session, buf, "x"));
    WT_ERR(__wt_strndup(session, buf->data, buf->size, &idx->exkey_format));

    /* By default, index cursor values are the table value columns. */
    /* TODO Optimize to use index columns in preference to table lookups. */
    WT_ERR(__wt_buf_init(session, plan, 0));
    WT_ERR(__wt_struct_plan(session, table, table->colconf.str, table->colconf.len, true, plan));
    WT_ERR(__wt_strndup(session, plan->data, plan->size, &idx->value_plan));

err:
    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &plan);
    return (ret);
}

/*
 * __schema_open_index --
 *     Open one or more indices for a table (internal version).
 */
static int
__schema_open_index(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname, size_t len, WT_INDEX **indexp)
{
    WT_CURSOR *cursor;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_INDEX *idx;
    u_int i;
    int cmp;
    const char *idxconf, *name, *tablename, *uri;
    bool match;

    /* Check if we've already done the work. */
    if (idxname == NULL && table->idx_complete)
        return (0);

    cursor = NULL;
    idx = NULL;
    match = false;

    /* Build a search key. */
    tablename = table->iface.name;
    WT_PREFIX_SKIP_REQUIRED(session, tablename, "table:");
    WT_ERR(__wt_scr_alloc(session, 512, &tmp));
    WT_ERR(__wt_buf_fmt(session, tmp, "index:%s:", tablename));

    /* Find matching indices. */
    WT_ERR(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, tmp->data);
    if ((ret = cursor->search_near(cursor, &cmp)) == 0 && cmp < 0)
        ret = cursor->next(cursor);
    for (i = 0; ret == 0; i++, ret = cursor->next(cursor)) {
        WT_ERR(cursor->get_key(cursor, &uri));
        name = uri;

        if (!WT_PREFIX_SKIP(name, tmp->data)) {
            /*
             * We reached the end of index list, remove the rest of in memory indices, they no
             * longer exist.
             */
            while (i < table->nindices) {
                WT_TRET(__wt_schema_destroy_index(session, &table->indices[table->nindices - 1]));
                table->indices[--table->nindices] = NULL;
            }
            break;
        }

        /* Is this the index we are looking for? */
        match = idxname == NULL || WT_STRING_MATCH(name, idxname, len);

        /*
         * Ensure there is space, including if we have to make room for a new entry in the middle of
         * the list.
         */
        WT_ERR(__wt_realloc_def(
          session, &table->idx_alloc, WT_MAX(i, table->nindices) + 1, &table->indices));

        /* Keep the in-memory list in sync with the metadata. */
        cmp = 0;
        while (table->indices[i] != NULL && (cmp = strcmp(uri, table->indices[i]->name)) > 0) {
            /* Index no longer exists, remove it. */
            WT_ERR(__wt_schema_destroy_index(session, &table->indices[i]));
            memmove(&table->indices[i], &table->indices[i + 1],
              (table->nindices - i) * sizeof(WT_INDEX *));
            table->indices[--table->nindices] = NULL;
        }
        if (cmp < 0) {
            /* Make room for a new index. */
            memmove(&table->indices[i + 1], &table->indices[i],
              (table->nindices - i) * sizeof(WT_INDEX *));
            table->indices[i] = NULL;
            ++table->nindices;
        }

        if (!match)
            continue;

        if (table->indices[i] == NULL) {
            WT_ERR(cursor->get_value(cursor, &idxconf));
            WT_ERR(__wt_calloc_one(session, &idx));
            WT_ERR(__wt_strdup(session, uri, &idx->name));
            WT_ERR(__wt_strdup(session, idxconf, &idx->config));
            WT_ERR(__open_index(session, table, idx));

            /*
             * If we're checking the creation of an index before a table is fully created, don't
             * save the index: it will need to be reopened once the table is complete.
             */
            if (!table->cg_complete) {
                WT_ERR(__wt_schema_destroy_index(session, &idx));
                if (idxname != NULL)
                    break;
                continue;
            }

            table->indices[i] = idx;
            idx = NULL;

            /*
             * If the slot is bigger than anything else we've seen, bump the number of indices.
             */
            if (i >= table->nindices)
                table->nindices = i + 1;
        }

        /* If we were looking for a single index, we're done. */
        if (indexp != NULL)
            *indexp = table->indices[i];
        if (idxname != NULL)
            break;
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    if (idxname != NULL && !match)
        ret = WT_NOTFOUND;

    /* If we did a full pass, we won't need to do it again. */
    if (idxname == NULL) {
        table->nindices = i;
        table->idx_complete = true;
    }

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    WT_TRET(__wt_schema_destroy_index(session, &idx));

    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_schema_open_index --
 *     Open one or more indices for a table.
 */
int
__wt_schema_open_index(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname, size_t len, WT_INDEX **indexp)
{
    WT_DECL_RET;

    WT_WITH_TABLE_WRITE_LOCK(session,
      WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED,
        ret = __schema_open_index(session, table, idxname, len, indexp)));
    return (ret);
}

/*
 * __wt_schema_open_indices --
 *     Open the indices for a table.
 */
int
__wt_schema_open_indices(WT_SESSION_IMPL *session, WT_TABLE *table)
{
    return (__wt_schema_open_index(session, table, NULL, 0, NULL));
}

/*
 * __schema_open_table --
 *     Open the data handle for a table (internal version).
 */
static int
__schema_open_table(WT_SESSION_IMPL *session)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_DECL_RET;
    WT_TABLE *table;
    const char **table_cfg;
    const char *tablename;

    table = (WT_TABLE *)session->dhandle;
    table_cfg = table->iface.cfg;
    tablename = table->iface.name;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE));

    WT_RET(__wt_config_gets(session, table_cfg, "columns", &cval));
    WT_RET(__wt_config_gets(session, table_cfg, "key_format", &cval));
    WT_RET(__wt_strndup(session, cval.str, cval.len, &table->key_format));
    WT_RET(__wt_config_gets(session, table_cfg, "value_format", &cval));
    WT_RET(__wt_strndup(session, cval.str, cval.len, &table->value_format));

    /* Point to some items in the copy to save re-parsing. */
    WT_RET(__wt_config_gets(session, table_cfg, "columns", &table->colconf));

    /*
     * Count the number of columns: tables are "simple" if the columns are not named.
     */
    __wt_config_subinit(session, &cparser, &table->colconf);
    table->is_simple = true;
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
        table->is_simple = false;
    WT_RET_NOTFOUND_OK(ret);

    /* Check that the columns match the key and value formats. */
    if (!table->is_simple)
        WT_RET(__wt_schema_colcheck(session, table->key_format, table->value_format,
          &table->colconf, &table->nkey_columns, NULL));

    WT_RET(__wt_config_gets(session, table_cfg, "colgroups", &table->cgconf));

    /* Count the number of column groups. */
    __wt_config_subinit(session, &cparser, &table->cgconf);
    table->ncolgroups = 0;
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
        ++table->ncolgroups;
    WT_RET_NOTFOUND_OK(ret);

    if (table->ncolgroups > 0 && table->is_simple)
        WT_RET_MSG(session, EINVAL, "%s requires a table with named columns", tablename);

    if ((ret = __wt_config_gets(session, table_cfg, "shared", &cval)) == 0)
        table->is_tiered_shared = true;
    WT_RET_NOTFOUND_OK(ret);

    WT_RET(__wt_calloc_def(session, WT_COLGROUPS(table), &table->cgroups));
    WT_RET(__wt_schema_open_colgroups(session, table));

    return (0);
}

/*
 * __wt_schema_get_colgroup --
 *     Find a column group by URI.
 */
int
__wt_schema_get_colgroup(
  WT_SESSION_IMPL *session, const char *uri, bool quiet, WT_TABLE **tablep, WT_COLGROUP **colgroupp)
{
    WT_COLGROUP *colgroup;
    WT_TABLE *table;
    u_int i;
    const char *tablename, *tend;

    if (tablep != NULL)
        *tablep = NULL;
    *colgroupp = NULL;

    tablename = uri;
    if (!WT_PREFIX_SKIP(tablename, "colgroup:"))
        return (__wt_bad_object_type(session, uri));

    if ((tend = strchr(tablename, ':')) == NULL)
        tend = tablename + strlen(tablename);

    WT_RET(
      __wt_schema_get_table(session, tablename, WT_PTRDIFF(tend, tablename), false, 0, &table));

    for (i = 0; i < WT_COLGROUPS(table); i++) {
        colgroup = table->cgroups[i];
        if (strcmp(colgroup->name, uri) == 0) {
            *colgroupp = colgroup;
            if (tablep != NULL)
                *tablep = table;
            else
                WT_RET(__wt_schema_release_table(session, &table));
            return (0);
        }
    }

    WT_RET(__wt_schema_release_table(session, &table));
    if (quiet)
        WT_RET(ENOENT);
    WT_RET_MSG(session, ENOENT, "%s not found in table", uri);
}

/*
 * __wt_schema_get_index --
 *     Find an index by URI.
 */
int
__wt_schema_get_index(
  WT_SESSION_IMPL *session, const char *uri, bool invalidate, bool quiet, WT_INDEX **indexp)
{
    WT_DECL_RET;
    WT_INDEX *idx;
    WT_TABLE *table;
    u_int i;
    const char *tablename, *tend;

    *indexp = NULL;

    tablename = uri;
    if (!WT_PREFIX_SKIP(tablename, "index:") || (tend = strchr(tablename, ':')) == NULL)
        return (__wt_bad_object_type(session, uri));

    WT_RET(
      __wt_schema_get_table(session, tablename, WT_PTRDIFF(tend, tablename), false, 0, &table));

    /* Try to find the index in the table. */
    for (i = 0; i < table->nindices; i++) {
        idx = table->indices[i];
        if (idx != NULL && strcmp(idx->name, uri) == 0) {
            *indexp = idx;
            goto done;
        }
    }

    /* Otherwise, open it. */
    WT_ERR(__wt_schema_open_index(session, table, tend + 1, strlen(tend + 1), indexp));

done:
    if (invalidate)
        table->idx_complete = false;

err:
    WT_TRET(__wt_schema_release_table(session, &table));
    WT_RET(ret);

    if (*indexp != NULL)
        return (0);

    if (quiet)
        WT_RET(ENOENT);
    WT_RET_MSG(session, ENOENT, "%s not found in table", uri);
}

/*
 * __wt_schema_open_table --
 *     Open a named table.
 */
int
__wt_schema_open_table(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_WITH_TABLE_WRITE_LOCK(session,
      WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = __schema_open_table(session)));

    return (ret);
}
