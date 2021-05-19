/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_metadata_insert --
 *     Insert a row into the metadata (external API version).
 */
int
__wt_ext_metadata_insert(
  WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key, const char *value)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    return (__wt_metadata_insert(session, key, value));
}

/*
 * __wt_ext_metadata_remove --
 *     Remove a row from the metadata (external API version).
 */
int
__wt_ext_metadata_remove(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    return (__wt_metadata_remove(session, key));
}

/*
 * __wt_ext_metadata_search --
 *     Return a copied row from the metadata (external API version). The caller is responsible for
 *     freeing the allocated memory.
 */
int
__wt_ext_metadata_search(
  WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key, char **valuep)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    return (__wt_metadata_search(session, key, valuep));
}

/*
 * __wt_ext_metadata_update --
 *     Update a row in the metadata (external API version).
 */
int
__wt_ext_metadata_update(
  WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *key, const char *value)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    return (__wt_metadata_update(session, key, value));
}

/*
 * __wt_metadata_get_ckptlist --
 *     Public entry point to __wt_meta_ckptlist_get (for wt list).
 */
int
__wt_metadata_get_ckptlist(WT_SESSION *session, const char *name, WT_CKPT **ckptbasep)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    return (__wt_meta_ckptlist_get((WT_SESSION_IMPL *)session, name, false, ckptbasep, NULL));
}

/*
 * __wt_metadata_free_ckptlist --
 *     Public entry point to __wt_meta_ckptlist_free (for wt list).
 */
void
__wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    __wt_meta_ckptlist_free((WT_SESSION_IMPL *)session, &ckptbase);
}
