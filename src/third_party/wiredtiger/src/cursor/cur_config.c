/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curconfig_close --
 *     WT_CURSOR->close method for the config cursor type.
 */
static int
__curconfig_close(WT_CURSOR *cursor)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    __wt_cursor_close(cursor);

    API_END_RET(session, ret);
}

/*
 * __wt_curconfig_open --
 *     WT_SESSION->open_cursor method for config cursors.
 */
int
__wt_curconfig_open(
  WT_SESSION_IMPL *session, const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __wt_cursor_compare_notsup,                     /* compare */
      __wt_cursor_equals_notsup,                      /* equals */
      __wt_cursor_notsup,                             /* next */
      __wt_cursor_notsup,                             /* prev */
      __wt_cursor_noop,                               /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curconfig_close);
    WT_CURSOR_CONFIG *cconfig;
    WT_CURSOR *cursor;
    WT_DECL_RET;

    WT_STATIC_ASSERT(offsetof(WT_CURSOR_CONFIG, iface) == 0);

    WT_RET(__wt_calloc_one(session, &cconfig));
    cursor = (WT_CURSOR *)cconfig;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = cursor->value_format = "S";

    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
err:
        WT_TRET(__curconfig_close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
