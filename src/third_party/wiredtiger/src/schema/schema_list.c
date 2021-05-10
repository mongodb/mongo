/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __schema_get_tiered_uri --
 *     Get the tiered handle for the named table. This function overwrites the dhandle.
 */
static int
__schema_get_tiered_uri(
  WT_SESSION_IMPL *session, const char *uri, uint32_t flags, WT_TIERED **tieredp)
{
    WT_DECL_RET;
    WT_TIERED *tiered;

    *tieredp = NULL;

    WT_ERR(__wt_session_get_dhandle(session, uri, NULL, NULL, flags));
    tiered = (WT_TIERED *)session->dhandle;
    *tieredp = tiered;
err:
    return (ret);
}
/*
 * __wt_schema_get_tiered_uri --
 *     Get the tiered handle for the named table.
 */
int
__wt_schema_get_tiered_uri(
  WT_SESSION_IMPL *session, const char *uri, uint32_t flags, WT_TIERED **tieredp)
{
    WT_DECL_RET;

    WT_SAVE_DHANDLE(session, ret = __schema_get_tiered_uri(session, uri, flags, tieredp));
    return (ret);
}

/*
 * __wt_schema_release_tiered --
 *     Release a tiered handle.
 */
int
__wt_schema_release_tiered(WT_SESSION_IMPL *session, WT_TIERED **tieredp)
{
    WT_DECL_RET;
    WT_TIERED *tiered;

    if ((tiered = *tieredp) == NULL)
        return (0);
    *tieredp = NULL;

    WT_WITH_DHANDLE(session, &tiered->iface, ret = __wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_schema_get_table_uri --
 *     Get the table handle for the named table.
 */
int
__wt_schema_get_table_uri(
  WT_SESSION_IMPL *session, const char *uri, bool ok_incomplete, uint32_t flags, WT_TABLE **tablep)
{
    WT_DATA_HANDLE *saved_dhandle;
    WT_DECL_RET;
    WT_TABLE *table;

    *tablep = NULL;

    saved_dhandle = session->dhandle;

    WT_ERR(__wt_session_get_dhandle(session, uri, NULL, NULL, flags));
    table = (WT_TABLE *)session->dhandle;
    if (!ok_incomplete && !table->cg_complete) {
        WT_ERR(__wt_session_release_dhandle(session));
        ret = __wt_set_return(session, EINVAL);
        WT_ERR_MSG(session, ret, "'%s' cannot be used until all column groups are created",
          table->iface.name);
    }
    *tablep = table;

err:
    session->dhandle = saved_dhandle;
    return (ret);
}

/*
 * __wt_schema_get_table --
 *     Get the table handle for the named table.
 */
int
__wt_schema_get_table(WT_SESSION_IMPL *session, const char *name, size_t namelen,
  bool ok_incomplete, uint32_t flags, WT_TABLE **tablep)
{
    WT_DECL_ITEM(namebuf);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, namelen + 1, &namebuf));
    WT_ERR(__wt_buf_fmt(session, namebuf, "table:%.*s", (int)namelen, name));

    WT_ERR(__wt_schema_get_table_uri(session, namebuf->data, ok_incomplete, flags, tablep));

err:
    __wt_scr_free(session, &namebuf);
    return (ret);
}

/*
 * __wt_schema_release_table --
 *     Release a table handle.
 */
int
__wt_schema_release_table(WT_SESSION_IMPL *session, WT_TABLE **tablep)
{
    WT_DECL_RET;
    WT_TABLE *table;

    if ((table = *tablep) == NULL)
        return (0);
    *tablep = NULL;

    WT_WITH_DHANDLE(session, &table->iface, ret = __wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_schema_destroy_colgroup --
 *     Free a column group handle.
 */
void
__wt_schema_destroy_colgroup(WT_SESSION_IMPL *session, WT_COLGROUP **colgroupp)
{
    WT_COLGROUP *colgroup;

    if ((colgroup = *colgroupp) == NULL)
        return;
    *colgroupp = NULL;

    __wt_free(session, colgroup->name);
    __wt_free(session, colgroup->source);
    __wt_free(session, colgroup->config);
    __wt_free(session, colgroup);
}

/*
 * __wt_schema_destroy_index --
 *     Free an index handle.
 */
int
__wt_schema_destroy_index(WT_SESSION_IMPL *session, WT_INDEX **idxp)
{
    WT_DECL_RET;
    WT_INDEX *idx;

    if ((idx = *idxp) == NULL)
        return (0);
    *idxp = NULL;

    /* If there is a custom collator configured, terminate it. */
    if (idx->collator != NULL && idx->collator_owned && idx->collator->terminate != NULL) {
        WT_TRET(idx->collator->terminate(idx->collator, &session->iface));
        idx->collator = NULL;
        idx->collator_owned = 0;
    }

    /* If there is a custom extractor configured, terminate it. */
    if (idx->extractor != NULL && idx->extractor_owned && idx->extractor->terminate != NULL) {
        WT_TRET(idx->extractor->terminate(idx->extractor, &session->iface));
        idx->extractor = NULL;
        idx->extractor_owned = 0;
    }

    __wt_free(session, idx->name);
    __wt_free(session, idx->source);
    __wt_free(session, idx->config);
    __wt_free(session, idx->key_format);
    __wt_free(session, idx->key_plan);
    __wt_free(session, idx->value_plan);
    __wt_free(session, idx->idxkey_format);
    __wt_free(session, idx->exkey_format);
    __wt_free(session, idx);

    return (ret);
}

/*
 * __wt_schema_close_table --
 *     Close a table handle.
 */
int
__wt_schema_close_table(WT_SESSION_IMPL *session, WT_TABLE *table)
{
    WT_DECL_RET;
    u_int i;

    __wt_free(session, table->plan);
    __wt_free(session, table->key_format);
    __wt_free(session, table->value_format);
    if (table->cgroups != NULL) {
        for (i = 0; i < WT_COLGROUPS(table); i++)
            __wt_schema_destroy_colgroup(session, &table->cgroups[i]);
        __wt_free(session, table->cgroups);
    }
    if (table->indices != NULL) {
        for (i = 0; i < table->nindices; i++)
            WT_TRET(__wt_schema_destroy_index(session, &table->indices[i]));
        __wt_free(session, table->indices);
    }
    table->idx_alloc = 0;

    WT_ASSERT(session,
      FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE) ||
        F_ISSET(S2C(session), WT_CONN_CLOSING));
    table->cg_complete = table->idx_complete = false;

    return (ret);
}
