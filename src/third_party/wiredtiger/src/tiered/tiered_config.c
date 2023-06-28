/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __tiered_confchk --
 *     Check for a valid tiered storage source.
 */
static int
__tiered_confchk(
  WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name, WT_NAMED_STORAGE_SOURCE **nstoragep)
{
    WT_CONNECTION_IMPL *conn;
    WT_NAMED_STORAGE_SOURCE *nstorage;

    *nstoragep = NULL;

    if (name->len == 0 || WT_STRING_MATCH("none", name->str, name->len))
        return (0);

    conn = S2C(session);
    TAILQ_FOREACH (nstorage, &conn->storagesrcqh, q)
        if (WT_STRING_MATCH(nstorage->name, name->str, name->len)) {
            *nstoragep = nstorage;
            return (0);
        }
    WT_RET_MSG(session, EINVAL, "unknown storage source '%.*s'", (int)name->len, name->str);
}

/*
 * __tiered_common_config --
 *     Parse configuration options common to connection and btrees.
 */
static int
__tiered_common_config(WT_SESSION_IMPL *session, const char **cfg, WT_BUCKET_STORAGE *bstorage)
{
    WT_CONFIG_ITEM cval;

    if (bstorage == NULL)
        return (0);

    WT_RET(__wt_config_gets(session, cfg, "tiered_storage.local_retention", &cval));
    bstorage->retain_secs = (uint64_t)cval.val;

    return (0);
}

/*
 * __wt_tiered_bucket_config --
 *     Given a configuration, (re)configure the bucket storage and return that structure.
 */
int
__wt_tiered_bucket_config(
  WT_SESSION_IMPL *session, const char *cfg[], WT_BUCKET_STORAGE **bstoragep)
{
    WT_BUCKET_STORAGE *bstorage, *new;
    WT_CONFIG_ITEM auth, bucket, cachedir, name, prefix, shared;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_NAMED_STORAGE_SOURCE *nstorage;
    WT_STORAGE_SOURCE *storage;
    uint64_t hash_bucket, hash;

    *bstoragep = NULL;

    WT_RET(__wt_config_gets(session, cfg, "tiered_storage.name", &name));
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    bstorage = new = NULL;
    conn = S2C(session);

    __wt_spin_lock(session, &conn->storage_lock);

    WT_ERR(__tiered_confchk(session, &name, &nstorage));
    if (nstorage == NULL) {
        WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.bucket", &bucket));
        if (bucket.len != 0)
            WT_ERR_MSG(
              session, EINVAL, "tiered_storage.bucket requires tiered_storage.name to be set");
        goto done;
    }
    /*
     * Check if tiered storage is set on the connection. If someone wants tiered storage on a table,
     * it needs to be configured on the database as well.
     */
    if (conn->bstorage == NULL && bstoragep != &conn->bstorage)
        WT_ERR_MSG(
          session, EINVAL, "table tiered storage requires connection tiered storage to be set");
    /* A bucket and bucket_prefix are required, cache_directory and auth_token are not. */
    WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.auth_token", &auth));
    WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.bucket", &bucket));
    if (bucket.len == 0)
        WT_ERR_MSG(session, EINVAL, "table tiered storage requires bucket to be set");
    WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.bucket_prefix", &prefix));
    if (prefix.len == 0)
        WT_ERR_MSG(session, EINVAL, "table tiered storage requires bucket_prefix to be set");
    WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.cache_directory", &cachedir));
    WT_ERR_NOTFOUND_OK(__wt_config_gets(session, cfg, "tiered_storage.shared", &shared), false);

    /*
     * Check if tiered storage shared is set on the connection. If someone wants tiered storage on a
     * table, it needs to be configured on the database as well.
     */
    if (conn->bstorage != NULL && conn->bstorage->tiered_shared == false && shared.val)
        WT_ERR_MSG(session, EINVAL,
          "table tiered storage shared requires connection tiered storage shared to be set");

    hash = __wt_hash_city64(bucket.str, bucket.len);
    hash_bucket = hash & (conn->hash_size - 1);
    TAILQ_FOREACH (bstorage, &nstorage->buckethashqh[hash_bucket], q) {
        if (WT_STRING_MATCH(bstorage->bucket, bucket.str, bucket.len) &&
          (WT_STRING_MATCH(bstorage->bucket_prefix, prefix.str, prefix.len))) {
            *bstoragep = bstorage;
            goto done;
        }
    }

    WT_ERR(__wt_calloc_one(session, &new));
    WT_ERR(__wt_strndup(session, auth.str, auth.len, &new->auth_token));
    WT_ERR(__wt_strndup(session, bucket.str, bucket.len, &new->bucket));
    WT_ERR(__wt_strndup(session, prefix.str, prefix.len, &new->bucket_prefix));
    WT_ERR(__wt_strndup(session, cachedir.str, cachedir.len, &new->cache_directory));

    storage = nstorage->storage_source;
    if (cachedir.len != 0)
        WT_ERR(__wt_buf_fmt(session, buf, "cache_directory=%s", new->cache_directory));
    WT_ERR(storage->ss_customize_file_system(
      storage, &session->iface, new->bucket, new->auth_token, buf->data, &new->file_system));
    new->storage_source = storage;
    if (shared.val)
        new->tiered_shared = true;

    /* If we're creating a new bucket storage, parse the other settings into it. */
    TAILQ_INSERT_HEAD(&nstorage->bucketqh, new, q);
    TAILQ_INSERT_HEAD(&nstorage->buckethashqh[hash_bucket], new, hashq);
    F_SET(new, WT_BUCKET_FREE);
    WT_ERR(__tiered_common_config(session, cfg, new));
    *bstoragep = new;

done:
    if (0) {
err:
        if (new != NULL) {
            __wt_free(session, new->bucket);
            __wt_free(session, new->bucket_prefix);
        }
        __wt_free(session, new);
    }
    __wt_spin_unlock(session, &conn->storage_lock);
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_tiered_conn_config --
 *     Parse and setup the storage server options for the connection.
 */
int
__wt_tiered_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (!reconfig)
        WT_RET(__wt_tiered_bucket_config(session, cfg, &conn->bstorage));
    else
        WT_ERR(__tiered_common_config(session, cfg, conn->bstorage));

    /* If the connection is not set up for tiered storage there is nothing more to do. */
    if (conn->bstorage == NULL)
        return (0);
    __wt_verbose(session, WT_VERB_TIERED, "TIERED_CONFIG: bucket %s", conn->bstorage->bucket);
    __wt_verbose(
      session, WT_VERB_TIERED, "TIERED_CONFIG: prefix %s", conn->bstorage->bucket_prefix);

    WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.interval", &cval));
    conn->tiered_interval = (uint64_t)cval.val;

    WT_ASSERT(session, conn->bstorage != NULL);
    WT_STAT_CONN_SET(session, tiered_retention, conn->bstorage->retain_secs);

    /*
     * Set up the designated file system for the "none" bucket.
     */
    WT_ASSERT(session, conn->file_system != NULL);
    conn->bstorage_none.file_system = conn->file_system;

    return (0);

err:
    __wt_free(session, conn->bstorage->auth_token);
    __wt_free(session, conn->bstorage->bucket);
    __wt_free(session, conn->bstorage->bucket_prefix);
    __wt_free(session, conn->bstorage->cache_directory);
    __wt_free(session, conn->bstorage);
    return (ret);
}
