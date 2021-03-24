/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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

    /* Count the number of tiers. */
    __wt_config_subinit(session, &cparser, &tierconf);
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
        ++tiered->ntiers;
    WT_RET_NOTFOUND_OK(ret);

    WT_ASSERT(session, tiered->ntiers > 0);

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_calloc_def(session, tiered->ntiers, &tiered->tiers));

    __wt_config_subinit(session, &cparser, &tierconf);
    for (i = 0; i < tiered->ntiers; i++) {
        WT_ERR(__wt_config_next(&cparser, &ckey, &cval));
        WT_ERR(__wt_buf_fmt(session, buf, "%.*s", (int)ckey.len, ckey.str));
        WT_ERR(__wt_session_get_dhandle(session, (const char *)buf->data, NULL, cfg, 0));
        (void)__wt_atomic_addi32(&session->dhandle->session_inuse, 1);
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
            (void)__wt_atomic_subi32(&tiered->tiers[i]->session_inuse, 1);
        __wt_free(session, tiered->tiers);
    }

    return (ret);
}
