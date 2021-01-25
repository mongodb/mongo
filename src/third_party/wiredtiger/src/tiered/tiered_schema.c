/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_tiered_create --
 *     Create a tiered tree structure for the given name.
 */
int
__wt_tiered_create(WT_SESSION_IMPL *session, const char *uri, bool exclusive, const char *config)
{
    WT_DECL_RET;
    char *meta_value;
    const char *cfg[] = {WT_CONFIG_BASE(session, tiered_meta), config, NULL};
    const char *metadata;

    metadata = NULL;

    /* If it can be opened, it already exists. */
    if ((ret = __wt_metadata_search(session, uri, &meta_value)) != WT_NOTFOUND) {
        if (exclusive)
            WT_TRET(EEXIST);
        goto err;
    }
    WT_RET_NOTFOUND_OK(ret);

    if (!F_ISSET(S2C(session), WT_CONN_READONLY)) {
        WT_ERR(__wt_config_merge(session, cfg, NULL, &metadata));
        WT_ERR(__wt_metadata_insert(session, uri, metadata));
    }

err:
    __wt_free(session, metadata);
    return (ret);
}

/*
 * __wt_tiered_drop --
 *     Drop a tiered store.
 */
int
__wt_tiered_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DATA_HANDLE *tier;
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;

    /* Get the tiered data handle. */
    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
    tiered = (WT_TIERED *)session->dhandle;

    /* Drop the tiers. */
    for (i = 0; i < tiered->ntiers; i++) {
        tier = tiered->tiers[i];
        WT_ERR(__wt_schema_drop(session, tier->name, cfg));
    }

    ret = __wt_metadata_remove(session, uri);

err:
    F_SET(session->dhandle, WT_DHANDLE_DISCARD);
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_tiered_rename --
 *     Rename a tiered data source.
 */
int
__wt_tiered_rename(
  WT_SESSION_IMPL *session, const char *olduri, const char *newuri, const char *cfg[])
{
    WT_DECL_RET;
    WT_TIERED *tiered;

    /* Get the tiered data handle. */
    WT_RET(__wt_session_get_dhandle(session, olduri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
    tiered = (WT_TIERED *)session->dhandle;

    /* TODO */
    WT_UNUSED(olduri);
    WT_UNUSED(newuri);
    WT_UNUSED(cfg);
    WT_UNUSED(tiered);

    F_SET(session->dhandle, WT_DHANDLE_DISCARD);
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_tiered_truncate --
 *     Truncate for a tiered data source.
 */
int
__wt_tiered_truncate(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, WT_DHANDLE_EXCLUSIVE));
    tiered = (WT_TIERED *)session->dhandle;

    WT_STAT_DATA_INCR(session, cursor_truncate);

    /* Truncate the column groups. */
    for (i = 0; i < tiered->ntiers; i++)
        WT_ERR(__wt_schema_truncate(session, tiered->tiers[i]->name, cfg));

err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_tiered_worker --
 *     Run a schema worker operation on each tier of a tiered data source.
 */
int
__wt_tiered_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;

    /*
     * If this was an alter operation, we need to alter the configuration for the overall tree and
     * then reread it so it isn't out of date. TODO not yet supported.
     */
    if (FLD_ISSET(open_flags, WT_BTREE_ALTER))
        WT_RET(ENOTSUP);

    WT_RET(__wt_session_get_dhandle(session, uri, NULL, NULL, open_flags));
    tiered = (WT_TIERED *)session->dhandle;

    for (i = 0; i < tiered->ntiers; i++) {
        dhandle = tiered->tiers[i];
        WT_SAVE_DHANDLE(session,
          ret = __wt_schema_worker(session, dhandle->name, file_func, name_func, cfg, open_flags));
        WT_ERR(ret);
    }

err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __tiered_open --
 *     Open a tiered data handle (internal version).
 */
static int
__tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval, tierconf;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_TIERED *tiered;
    u_int i;
    const char **tiered_cfg;

    dhandle = session->dhandle;
    tiered = (WT_TIERED *)dhandle;
    tiered_cfg = dhandle->cfg;

    WT_UNUSED(cfg);

    WT_RET(__wt_config_gets(session, tiered_cfg, "key_format", &cval));
    WT_RET(__wt_strndup(session, cval.str, cval.len, &tiered->key_format));
    WT_RET(__wt_config_gets(session, tiered_cfg, "value_format", &cval));
    WT_RET(__wt_strndup(session, cval.str, cval.len, &tiered->value_format));

    /* Point to some items in the copy to save re-parsing. */
    WT_RET(__wt_config_gets(session, tiered_cfg, "tiered.tiers", &tierconf));

    /*
     * Count the number of tiers.
     */
    __wt_config_subinit(session, &cparser, &tierconf);
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
        ++tiered->ntiers;
    WT_RET_NOTFOUND_OK(ret);

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_calloc_def(session, tiered->ntiers, &tiered->tiers));

    __wt_config_subinit(session, &cparser, &tierconf);
    for (i = 0; i < tiered->ntiers; i++) {
        WT_ERR(__wt_config_next(&cparser, &ckey, &cval));
        WT_ERR(__wt_buf_fmt(session, buf, "%.*s", (int)ckey.len, ckey.str));
        WT_ERR(__wt_session_get_dhandle(session, (const char *)buf->data, NULL, cfg, 0));
        __wt_atomic_addi32(&session->dhandle->session_inuse, 1);
        /* Load in reverse order (based on LSM logic). */
        tiered->tiers[(tiered->ntiers - 1) - i] = session->dhandle;
        WT_ERR(__wt_session_release_dhandle(session));
    }

    if (0) {
err:
        __wt_free(session, tiered->tiers);
    }
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_tiered_open --
 *     Open a tiered data handle.
 */
int
__wt_tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = __tiered_open(session, cfg));

    return (ret);
}

/*
 * __wt_tiered_close --
 *     Close a tiered data handle.
 */
int
__wt_tiered_close(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_DECL_RET;
    u_int i;

    ret = 0;
    __wt_free(session, tiered->key_format);
    __wt_free(session, tiered->value_format);
    if (tiered->tiers != NULL) {
        for (i = 0; i < tiered->ntiers; i++)
            __wt_atomic_subi32(&tiered->tiers[i]->session_inuse, 1);
        __wt_free(session, tiered->tiers);
    }

    return (ret);
}
