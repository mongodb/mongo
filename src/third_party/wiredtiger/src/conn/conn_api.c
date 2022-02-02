/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * ext_collate --
 *     Call the collation function (external API version).
 */
static int
ext_collate(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, WT_COLLATOR *collator, WT_ITEM *first,
  WT_ITEM *second, int *cmpp)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    WT_RET(__wt_compare(session, collator, first, second, cmpp));

    return (0);
}

/*
 * ext_collator_config --
 *     Given a configuration, configure the collator (external API version).
 */
static int
ext_collator_config(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *uri,
  WT_CONFIG_ARG *cfg_arg, WT_COLLATOR **collatorp, int *ownp)
{
    WT_CONFIG_ITEM cval, metadata;
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    const char **cfg;

    conn = (WT_CONNECTION_IMPL *)wt_api->conn;
    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = conn->default_session;

    /* The default is a standard lexicographic comparison. */
    if ((cfg = (const char **)cfg_arg) == NULL)
        return (0);

    WT_CLEAR(cval);
    WT_RET_NOTFOUND_OK(__wt_config_gets_none(session, cfg, "collator", &cval));
    if (cval.len == 0)
        return (0);

    WT_CLEAR(metadata);
    WT_RET_NOTFOUND_OK(__wt_config_gets(session, cfg, "app_metadata", &metadata));
    return (__wt_collator_config(session, uri, &cval, &metadata, collatorp, ownp));
}

/*
 * __collator_confchk --
 *     Check for a valid custom collator.
 */
static int
__collator_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cname, WT_COLLATOR **collatorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_NAMED_COLLATOR *ncoll;

    *collatorp = NULL;

    if (cname->len == 0 || WT_STRING_MATCH("none", cname->str, cname->len))
        return (0);

    conn = S2C(session);
    TAILQ_FOREACH (ncoll, &conn->collqh, q)
        if (WT_STRING_MATCH(ncoll->name, cname->str, cname->len)) {
            *collatorp = ncoll->collator;
            return (0);
        }
    WT_RET_MSG(session, EINVAL, "unknown collator '%.*s'", (int)cname->len, cname->str);
}

/*
 * __wt_collator_config --
 *     Configure a custom collator.
 */
int
__wt_collator_config(WT_SESSION_IMPL *session, const char *uri, WT_CONFIG_ITEM *cname,
  WT_CONFIG_ITEM *metadata, WT_COLLATOR **collatorp, int *ownp)
{
    WT_COLLATOR *collator;

    *collatorp = NULL;
    *ownp = 0;

    WT_RET(__collator_confchk(session, cname, &collator));
    if (collator == NULL)
        return (0);

    if (collator->customize != NULL)
        WT_RET(collator->customize(collator, &session->iface, uri, metadata, collatorp));

    if (*collatorp == NULL)
        *collatorp = collator;
    else
        *ownp = 1;

    return (0);
}

/*
 * __conn_add_collator --
 *     WT_CONNECTION->add_collator method.
 */
static int
__conn_add_collator(
  WT_CONNECTION *wt_conn, const char *name, WT_COLLATOR *collator, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_COLLATOR *ncoll;
    WT_SESSION_IMPL *session;

    ncoll = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
    WT_UNUSED(cfg);

    if (strcmp(name, "none") == 0)
        WT_ERR_MSG(session, EINVAL, "invalid name for a collator: %s", name);

    WT_ERR(__wt_calloc_one(session, &ncoll));
    WT_ERR(__wt_strdup(session, name, &ncoll->name));
    ncoll->collator = collator;

    __wt_spin_lock(session, &conn->api_lock);
    TAILQ_INSERT_TAIL(&conn->collqh, ncoll, q);
    ncoll = NULL;
    __wt_spin_unlock(session, &conn->api_lock);

err:
    if (ncoll != NULL) {
        __wt_free(session, ncoll->name);
        __wt_free(session, ncoll);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_collator --
 *     Remove collator added by WT_CONNECTION->add_collator, only used internally.
 */
int
__wt_conn_remove_collator(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_COLLATOR *ncoll;

    conn = S2C(session);

    while ((ncoll = TAILQ_FIRST(&conn->collqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->collqh, ncoll, q);
        /* Call any termination method. */
        if (ncoll->collator->terminate != NULL)
            WT_TRET(ncoll->collator->terminate(ncoll->collator, (WT_SESSION *)session));

        __wt_free(session, ncoll->name);
        __wt_free(session, ncoll);
    }

    return (ret);
}

/*
 * __compressor_confchk --
 *     Validate the compressor.
 */
static int
__compressor_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_COMPRESSOR **compressorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_NAMED_COMPRESSOR *ncomp;

    *compressorp = NULL;

    if (cval->len == 0 || WT_STRING_MATCH("none", cval->str, cval->len))
        return (0);

    conn = S2C(session);
    TAILQ_FOREACH (ncomp, &conn->compqh, q)
        if (WT_STRING_MATCH(ncomp->name, cval->str, cval->len)) {
            *compressorp = ncomp->compressor;
            return (0);
        }
    WT_RET_MSG(session, EINVAL, "unknown compressor '%.*s'", (int)cval->len, cval->str);
}

/*
 * __wt_compressor_config --
 *     Given a configuration, configure the compressor.
 */
int
__wt_compressor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_COMPRESSOR **compressorp)
{
    return (__compressor_confchk(session, cval, compressorp));
}

/*
 * __conn_add_compressor --
 *     WT_CONNECTION->add_compressor method.
 */
static int
__conn_add_compressor(
  WT_CONNECTION *wt_conn, const char *name, WT_COMPRESSOR *compressor, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_COMPRESSOR *ncomp;
    WT_SESSION_IMPL *session;

    ncomp = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_compressor, config, cfg);
    WT_UNUSED(cfg);

    if (strcmp(name, "none") == 0)
        WT_ERR_MSG(session, EINVAL, "invalid name for a compressor: %s", name);

    WT_ERR(__wt_calloc_one(session, &ncomp));
    WT_ERR(__wt_strdup(session, name, &ncomp->name));
    ncomp->compressor = compressor;

    __wt_spin_lock(session, &conn->api_lock);
    TAILQ_INSERT_TAIL(&conn->compqh, ncomp, q);
    ncomp = NULL;
    __wt_spin_unlock(session, &conn->api_lock);

err:
    if (ncomp != NULL) {
        __wt_free(session, ncomp->name);
        __wt_free(session, ncomp);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_compressor --
 *     remove compressor added by WT_CONNECTION->add_compressor, only used internally.
 */
int
__wt_conn_remove_compressor(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_COMPRESSOR *ncomp;

    conn = S2C(session);

    while ((ncomp = TAILQ_FIRST(&conn->compqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->compqh, ncomp, q);
        /* Call any termination method. */
        if (ncomp->compressor->terminate != NULL)
            WT_TRET(ncomp->compressor->terminate(ncomp->compressor, (WT_SESSION *)session));

        __wt_free(session, ncomp->name);
        __wt_free(session, ncomp);
    }

    return (ret);
}

/*
 * __conn_add_data_source --
 *     WT_CONNECTION->add_data_source method.
 */
static int
__conn_add_data_source(
  WT_CONNECTION *wt_conn, const char *prefix, WT_DATA_SOURCE *dsrc, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_DATA_SOURCE *ndsrc;
    WT_SESSION_IMPL *session;

    ndsrc = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_data_source, config, cfg);
    WT_UNUSED(cfg);

    WT_ERR(__wt_calloc_one(session, &ndsrc));
    WT_ERR(__wt_strdup(session, prefix, &ndsrc->prefix));
    ndsrc->dsrc = dsrc;

    /* Link onto the environment's list of data sources. */
    __wt_spin_lock(session, &conn->api_lock);
    TAILQ_INSERT_TAIL(&conn->dsrcqh, ndsrc, q);
    ndsrc = NULL;
    __wt_spin_unlock(session, &conn->api_lock);

err:
    if (ndsrc != NULL) {
        __wt_free(session, ndsrc->prefix);
        __wt_free(session, ndsrc);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_data_source --
 *     Remove data source added by WT_CONNECTION->add_data_source.
 */
int
__wt_conn_remove_data_source(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_DATA_SOURCE *ndsrc;

    conn = S2C(session);

    while ((ndsrc = TAILQ_FIRST(&conn->dsrcqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->dsrcqh, ndsrc, q);
        /* Call any termination method. */
        if (ndsrc->dsrc->terminate != NULL)
            WT_TRET(ndsrc->dsrc->terminate(ndsrc->dsrc, (WT_SESSION *)session));

        __wt_free(session, ndsrc->prefix);
        __wt_free(session, ndsrc);
    }

    return (ret);
}

/*
 * __encryptor_confchk --
 *     Validate the encryptor.
 */
static int
__encryptor_confchk(
  WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_NAMED_ENCRYPTOR **nencryptorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_NAMED_ENCRYPTOR *nenc;

    if (nencryptorp != NULL)
        *nencryptorp = NULL;

    if (cval->len == 0 || WT_STRING_MATCH("none", cval->str, cval->len))
        return (0);

    conn = S2C(session);
    TAILQ_FOREACH (nenc, &conn->encryptqh, q)
        if (WT_STRING_MATCH(nenc->name, cval->str, cval->len)) {
            if (nencryptorp != NULL)
                *nencryptorp = nenc;
            return (0);
        }

    WT_RET_MSG(session, EINVAL, "unknown encryptor '%.*s'", (int)cval->len, cval->str);
}

/*
 * __wt_encryptor_config --
 *     Given a configuration, configure the encryptor.
 */
int
__wt_encryptor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_CONFIG_ITEM *keyid,
  WT_CONFIG_ARG *cfg_arg, WT_KEYED_ENCRYPTOR **kencryptorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ENCRYPTOR *custom, *encryptor;
    WT_KEYED_ENCRYPTOR *kenc;
    WT_NAMED_ENCRYPTOR *nenc;
    uint64_t bucket, hash;

    *kencryptorp = NULL;

    kenc = NULL;
    conn = S2C(session);

    __wt_spin_lock(session, &conn->encryptor_lock);

    WT_ERR(__encryptor_confchk(session, cval, &nenc));
    if (nenc == NULL) {
        if (keyid->len != 0)
            WT_ERR_MSG(session, EINVAL, "encryption.keyid requires encryption.name to be set");
        goto out;
    }

    /*
     * Check if encryption is set on the connection. If someone wants encryption on a table, it
     * needs to be configured on the database as well.
     */
    if (conn->kencryptor == NULL && kencryptorp != &conn->kencryptor)
        WT_ERR_MSG(session, EINVAL, "table encryption requires connection encryption to be set");
    hash = __wt_hash_city64(keyid->str, keyid->len);
    bucket = hash & (conn->hash_size - 1);
    TAILQ_FOREACH (kenc, &nenc->keyedhashqh[bucket], q)
        if (WT_STRING_MATCH(kenc->keyid, keyid->str, keyid->len))
            goto out;

    WT_ERR(__wt_calloc_one(session, &kenc));
    WT_ERR(__wt_strndup(session, keyid->str, keyid->len, &kenc->keyid));
    encryptor = nenc->encryptor;
    if (encryptor->customize != NULL) {
        custom = NULL;
        WT_ERR(encryptor->customize(encryptor, &session->iface, cfg_arg, &custom));
        if (custom != NULL) {
            kenc->owned = 1;
            encryptor = custom;
        }
    }
    WT_ERR(encryptor->sizing(encryptor, &session->iface, &kenc->size_const));
    kenc->encryptor = encryptor;
    TAILQ_INSERT_HEAD(&nenc->keyedqh, kenc, q);
    TAILQ_INSERT_HEAD(&nenc->keyedhashqh[bucket], kenc, hashq);

out:
    __wt_spin_unlock(session, &conn->encryptor_lock);
    *kencryptorp = kenc;
    return (0);

err:
    if (kenc != NULL) {
        __wt_free(session, kenc->keyid);
        __wt_free(session, kenc);
    }
    __wt_spin_unlock(session, &conn->encryptor_lock);
    return (ret);
}

/*
 * __conn_add_encryptor --
 *     WT_CONNECTION->add_encryptor method.
 */
static int
__conn_add_encryptor(
  WT_CONNECTION *wt_conn, const char *name, WT_ENCRYPTOR *encryptor, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_ENCRYPTOR *nenc;
    WT_SESSION_IMPL *session;
    uint64_t i;

    nenc = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_encryptor, config, cfg);
    WT_UNUSED(cfg);

    if (strcmp(name, "none") == 0)
        WT_ERR_MSG(session, EINVAL, "invalid name for an encryptor: %s", name);

    if (encryptor->encrypt == NULL || encryptor->decrypt == NULL || encryptor->sizing == NULL)
        WT_ERR_MSG(session, EINVAL, "encryptor: %s: required callbacks not set", name);

    /*
     * Verify that terminate is set if customize is set. We could relax this restriction and give an
     * error if customize returns an encryptor and terminate is not set. That seems more prone to
     * mistakes.
     */
    if (encryptor->customize != NULL && encryptor->terminate == NULL)
        WT_ERR_MSG(session, EINVAL, "encryptor: %s: has customize but no terminate", name);

    WT_ERR(__wt_calloc_one(session, &nenc));
    WT_ERR(__wt_strdup(session, name, &nenc->name));
    nenc->encryptor = encryptor;
    TAILQ_INIT(&nenc->keyedqh);
    WT_ERR(__wt_calloc_def(session, conn->hash_size, &nenc->keyedhashqh));
    for (i = 0; i < conn->hash_size; i++)
        TAILQ_INIT(&nenc->keyedhashqh[i]);

    TAILQ_INSERT_TAIL(&conn->encryptqh, nenc, q);
    nenc = NULL;

err:
    if (nenc != NULL) {
        __wt_free(session, nenc->keyedhashqh);
        __wt_free(session, nenc->name);
        __wt_free(session, nenc);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_encryptor --
 *     remove encryptors added by WT_CONNECTION->add_encryptor, only used internally.
 */
int
__wt_conn_remove_encryptor(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_KEYED_ENCRYPTOR *kenc;
    WT_NAMED_ENCRYPTOR *nenc;

    conn = S2C(session);

    while ((nenc = TAILQ_FIRST(&conn->encryptqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->encryptqh, nenc, q);
        while ((kenc = TAILQ_FIRST(&nenc->keyedqh)) != NULL) {
            /* Remove from the connection's list, free memory. */
            TAILQ_REMOVE(&nenc->keyedqh, kenc, q);
            /* Call any termination method. */
            if (kenc->owned && kenc->encryptor->terminate != NULL)
                WT_TRET(kenc->encryptor->terminate(kenc->encryptor, (WT_SESSION *)session));

            __wt_free(session, kenc->keyid);
            __wt_free(session, kenc);
        }

        /* Call any termination method. */
        if (nenc->encryptor->terminate != NULL)
            WT_TRET(nenc->encryptor->terminate(nenc->encryptor, (WT_SESSION *)session));

        __wt_free(session, nenc->keyedhashqh);
        __wt_free(session, nenc->name);
        __wt_free(session, nenc);
    }
    return (ret);
}

/*
 * __conn_add_extractor --
 *     WT_CONNECTION->add_extractor method.
 */
static int
__conn_add_extractor(
  WT_CONNECTION *wt_conn, const char *name, WT_EXTRACTOR *extractor, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_EXTRACTOR *nextractor;
    WT_SESSION_IMPL *session;

    nextractor = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_extractor, config, cfg);
    WT_UNUSED(cfg);

    if (strcmp(name, "none") == 0)
        WT_ERR_MSG(session, EINVAL, "invalid name for an extractor: %s", name);

    WT_ERR(__wt_calloc_one(session, &nextractor));
    WT_ERR(__wt_strdup(session, name, &nextractor->name));
    nextractor->extractor = extractor;

    __wt_spin_lock(session, &conn->api_lock);
    TAILQ_INSERT_TAIL(&conn->extractorqh, nextractor, q);
    nextractor = NULL;
    __wt_spin_unlock(session, &conn->api_lock);

err:
    if (nextractor != NULL) {
        __wt_free(session, nextractor->name);
        __wt_free(session, nextractor);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __extractor_confchk --
 *     Check for a valid custom extractor.
 */
static int
__extractor_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cname, WT_EXTRACTOR **extractorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_NAMED_EXTRACTOR *nextractor;

    *extractorp = NULL;

    if (cname->len == 0 || WT_STRING_MATCH("none", cname->str, cname->len))
        return (0);

    conn = S2C(session);
    TAILQ_FOREACH (nextractor, &conn->extractorqh, q)
        if (WT_STRING_MATCH(nextractor->name, cname->str, cname->len)) {
            *extractorp = nextractor->extractor;
            return (0);
        }
    WT_RET_MSG(session, EINVAL, "unknown extractor '%.*s'", (int)cname->len, cname->str);
}

/*
 * __wt_extractor_config --
 *     Given a configuration, configure the extractor.
 */
int
__wt_extractor_config(WT_SESSION_IMPL *session, const char *uri, const char *config,
  WT_EXTRACTOR **extractorp, int *ownp)
{
    WT_CONFIG_ITEM cname;
    WT_EXTRACTOR *extractor;

    *extractorp = NULL;
    *ownp = 0;

    WT_RET_NOTFOUND_OK(__wt_config_getones_none(session, config, "extractor", &cname));
    if (cname.len == 0)
        return (0);

    WT_RET(__extractor_confchk(session, &cname, &extractor));
    if (extractor == NULL)
        return (0);

    if (extractor->customize != NULL) {
        WT_RET(__wt_config_getones(session, config, "app_metadata", &cname));
        WT_RET(extractor->customize(extractor, &session->iface, uri, &cname, extractorp));
    }

    if (*extractorp == NULL)
        *extractorp = extractor;
    else
        *ownp = 1;

    return (0);
}

/*
 * __wt_conn_remove_extractor --
 *     Remove extractor added by WT_CONNECTION->add_extractor, only used internally.
 */
int
__wt_conn_remove_extractor(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_EXTRACTOR *nextractor;

    conn = S2C(session);

    while ((nextractor = TAILQ_FIRST(&conn->extractorqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->extractorqh, nextractor, q);
        /* Call any termination method. */
        if (nextractor->extractor->terminate != NULL)
            WT_TRET(nextractor->extractor->terminate(nextractor->extractor, (WT_SESSION *)session));

        __wt_free(session, nextractor->name);
        __wt_free(session, nextractor);
    }

    return (ret);
}

/*
 * __conn_add_storage_source --
 *     WT_CONNECTION->add_storage_source method.
 */
static int
__conn_add_storage_source(
  WT_CONNECTION *wt_conn, const char *name, WT_STORAGE_SOURCE *storage_source, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_STORAGE_SOURCE *nstorage;
    WT_SESSION_IMPL *session;
    uint64_t i;

    nstorage = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, add_storage_source, config, cfg);
    WT_UNUSED(cfg);

    WT_ERR(__wt_calloc_one(session, &nstorage));
    WT_ERR(__wt_strdup(session, name, &nstorage->name));
    nstorage->storage_source = storage_source;
    TAILQ_INIT(&nstorage->bucketqh);
    WT_ERR(__wt_calloc_def(session, conn->hash_size, &nstorage->buckethashqh));
    for (i = 0; i < conn->hash_size; i++)
        TAILQ_INIT(&nstorage->buckethashqh[i]);

    __wt_spin_lock(session, &conn->api_lock);
    TAILQ_INSERT_TAIL(&conn->storagesrcqh, nstorage, q);
    nstorage = NULL;
    __wt_spin_unlock(session, &conn->api_lock);

err:
    if (nstorage != NULL) {
        __wt_free(session, nstorage->name);
        __wt_free(session, nstorage);
    }

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_get_storage_source --
 *     WT_CONNECTION->get_storage_source method.
 */
static int
__conn_get_storage_source(
  WT_CONNECTION *wt_conn, const char *name, WT_STORAGE_SOURCE **storage_sourcep)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_STORAGE_SOURCE *nstorage_source;
    WT_STORAGE_SOURCE *storage_source;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    *storage_sourcep = NULL;

    ret = EINVAL;
    TAILQ_FOREACH (nstorage_source, &conn->storagesrcqh, q)
        if (WT_STREQ(nstorage_source->name, name)) {
            storage_source = nstorage_source->storage_source;
            WT_RET(storage_source->ss_add_reference(storage_source));
            *storage_sourcep = storage_source;
            ret = 0;
            break;
        }
    if (ret != 0)
        WT_RET_MSG(conn->default_session, ret, "unknown storage_source '%s'", name);

    return (ret);
}

/*
 * __wt_conn_remove_storage_source --
 *     Remove storage_source added by WT_CONNECTION->add_storage_source, only used internally.
 */
int
__wt_conn_remove_storage_source(WT_SESSION_IMPL *session)
{
    WT_BUCKET_STORAGE *bstorage;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_NAMED_STORAGE_SOURCE *nstorage;
    WT_STORAGE_SOURCE *storage;

    conn = S2C(session);

    while ((nstorage = TAILQ_FIRST(&conn->storagesrcqh)) != NULL) {
        /* Remove from the connection's list, free memory. */
        TAILQ_REMOVE(&conn->storagesrcqh, nstorage, q);
        while ((bstorage = TAILQ_FIRST(&nstorage->bucketqh)) != NULL) {
            /* Remove from the connection's list, free memory. */
            TAILQ_REMOVE(&nstorage->bucketqh, bstorage, q);
            __wt_free(session, bstorage->auth_token);
            __wt_free(session, bstorage->bucket);
            __wt_free(session, bstorage->bucket_prefix);
            __wt_free(session, bstorage->cache_directory);
            if (bstorage->file_system != NULL && bstorage->file_system->terminate != NULL)
                WT_TRET(
                  bstorage->file_system->terminate(bstorage->file_system, (WT_SESSION *)session));
            __wt_free(session, bstorage);
        }

        /* Call any termination method. */
        storage = nstorage->storage_source;
        WT_ASSERT(session, storage != NULL);
        if (storage->terminate != NULL)
            WT_TRET(storage->terminate(storage, (WT_SESSION *)session));

        __wt_free(session, nstorage->buckethashqh);
        __wt_free(session, nstorage->name);
        __wt_free(session, nstorage);
    }

    return (ret);
}

/*
 * __conn_ext_file_system_get --
 *     WT_EXTENSION.file_system_get method. Get file system in use.
 */
static int
__conn_ext_file_system_get(
  WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_FILE_SYSTEM **file_system)
{
    WT_FILE_SYSTEM *fs;

    WT_UNUSED(session);

    fs = ((WT_CONNECTION_IMPL *)wt_api->conn)->file_system;
    if (fs == NULL)
        return (WT_NOTFOUND);
    *file_system = fs;
    return (0);
}

/*
 * __conn_get_extension_api --
 *     WT_CONNECTION.get_extension_api method.
 */
static WT_EXTENSION_API *
__conn_get_extension_api(WT_CONNECTION *wt_conn)
{
    WT_CONNECTION_IMPL *conn;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    conn->extension_api.conn = wt_conn;
    conn->extension_api.err_printf = __wt_ext_err_printf;
    conn->extension_api.msg_printf = __wt_ext_msg_printf;
    conn->extension_api.strerror = __wt_ext_strerror;
    conn->extension_api.map_windows_error = __wt_ext_map_windows_error;
    conn->extension_api.scr_alloc = __wt_ext_scr_alloc;
    conn->extension_api.scr_free = __wt_ext_scr_free;
    conn->extension_api.collator_config = ext_collator_config;
    conn->extension_api.collate = ext_collate;
    conn->extension_api.config_get = __wt_ext_config_get;
    conn->extension_api.config_get_string = __wt_ext_config_get_string;
    conn->extension_api.config_parser_open = __wt_ext_config_parser_open;
    conn->extension_api.config_parser_open_arg = __wt_ext_config_parser_open_arg;
    conn->extension_api.file_system_get = __conn_ext_file_system_get;
    conn->extension_api.metadata_insert = __wt_ext_metadata_insert;
    conn->extension_api.metadata_remove = __wt_ext_metadata_remove;
    conn->extension_api.metadata_search = __wt_ext_metadata_search;
    conn->extension_api.metadata_update = __wt_ext_metadata_update;
    conn->extension_api.struct_pack = __wt_ext_struct_pack;
    conn->extension_api.struct_size = __wt_ext_struct_size;
    conn->extension_api.struct_unpack = __wt_ext_struct_unpack;
    conn->extension_api.spin_init = __wt_ext_spin_init;
    conn->extension_api.spin_lock = __wt_ext_spin_lock;
    conn->extension_api.spin_unlock = __wt_ext_spin_unlock;
    conn->extension_api.spin_destroy = __wt_ext_spin_destroy;
    conn->extension_api.transaction_id = __wt_ext_transaction_id;
    conn->extension_api.transaction_isolation_level = __wt_ext_transaction_isolation_level;
    conn->extension_api.transaction_notify = __wt_ext_transaction_notify;
    conn->extension_api.transaction_oldest = __wt_ext_transaction_oldest;
    conn->extension_api.transaction_visible = __wt_ext_transaction_visible;
    conn->extension_api.version = wiredtiger_version;

    /* Streaming pack/unpack API */
    conn->extension_api.pack_start = __wt_ext_pack_start;
    conn->extension_api.unpack_start = __wt_ext_unpack_start;
    conn->extension_api.pack_close = __wt_ext_pack_close;
    conn->extension_api.pack_item = __wt_ext_pack_item;
    conn->extension_api.pack_int = __wt_ext_pack_int;
    conn->extension_api.pack_str = __wt_ext_pack_str;
    conn->extension_api.pack_uint = __wt_ext_pack_uint;
    conn->extension_api.unpack_item = __wt_ext_unpack_item;
    conn->extension_api.unpack_int = __wt_ext_unpack_int;
    conn->extension_api.unpack_str = __wt_ext_unpack_str;
    conn->extension_api.unpack_uint = __wt_ext_unpack_uint;

    return (&conn->extension_api);
}

/*
 * __conn_builtin_init --
 *     Initialize and configure a builtin extension.
 */
static int
__conn_builtin_init(WT_CONNECTION_IMPL *conn, const char *name,
  int (*extension_init)(WT_CONNECTION *, WT_CONFIG_ARG *), const char *cfg[])
{
    WT_CONFIG_ITEM all_configs, cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    char *config;
    const char *ext_cfg[] = {NULL, NULL};

    session = conn->default_session;

    WT_RET(__wt_config_gets(session, cfg, "builtin_extension_config", &all_configs));
    WT_CLEAR(cval);
    WT_RET_NOTFOUND_OK(__wt_config_subgets(session, &all_configs, name, &cval));
    WT_RET(__wt_strndup(session, cval.str, cval.len, &config));
    ext_cfg[0] = config;

    ret = extension_init(&conn->iface, (WT_CONFIG_ARG *)ext_cfg);
    __wt_free(session, config);

    return (ret);
}

#ifdef HAVE_BUILTIN_EXTENSION_LZ4
extern int lz4_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif
#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
extern int snappy_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZLIB
extern int zlib_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZSTD
extern int zstd_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif

/*
 * __conn_builtin_extensions --
 *     Load extensions that are enabled via --with-builtins
 */
static int
__conn_builtin_extensions(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
#ifdef HAVE_BUILTIN_EXTENSION_LZ4
    WT_RET(__conn_builtin_init(conn, "lz4", lz4_extension_init, cfg));
#endif
#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
    WT_RET(__conn_builtin_init(conn, "snappy", snappy_extension_init, cfg));
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZLIB
    WT_RET(__conn_builtin_init(conn, "zlib", zlib_extension_init, cfg));
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZSTD
    WT_RET(__conn_builtin_init(conn, "zstd", zstd_extension_init, cfg));
#endif

    /* Avoid warnings if no builtin extensions are configured. */
    WT_UNUSED(conn);
    WT_UNUSED(cfg);
    WT_UNUSED(__conn_builtin_init);

    return (0);
}

/*
 * __conn_load_extension_int --
 *     Internal extension load interface
 */
static int
__conn_load_extension_int(
  WT_SESSION_IMPL *session, const char *path, const char *cfg[], bool early_load)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_DLH *dlh;
    int (*load)(WT_CONNECTION *, WT_CONFIG_ARG *);
    const char *ext_cfg[2];
    const char *ext_config, *init_name, *terminate_name;
    bool is_local;

    dlh = NULL;
    ext_config = init_name = terminate_name = NULL;
    is_local = strcmp(path, "local") == 0;

    /* Ensure that the load matches the phase of startup we are in. */
    WT_ERR(__wt_config_gets(session, cfg, "early_load", &cval));
    if ((cval.val == 0 && early_load) || (cval.val != 0 && !early_load))
        return (0);

    /*
     * This assumes the underlying shared libraries are reference counted, that is, that re-opening
     * a shared library simply increments a ref count, and closing it simply decrements the ref
     * count, and the last close discards the reference entirely -- in other words, we do not check
     * to see if we've already opened this shared library.
     */
    WT_ERR(__wt_dlopen(session, is_local ? NULL : path, &dlh));

    /*
     * Find the load function, remember the unload function for when we close.
     */
    WT_ERR(__wt_config_gets(session, cfg, "entry", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &init_name));
    WT_ERR(__wt_dlsym(session, dlh, init_name, true, &load));

    WT_ERR(__wt_config_gets(session, cfg, "terminate", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &terminate_name));
    WT_ERR(__wt_dlsym(session, dlh, terminate_name, false, &dlh->terminate));

    WT_CLEAR(cval);
    WT_ERR_NOTFOUND_OK(__wt_config_gets(session, cfg, "config", &cval), false);
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &ext_config));
    ext_cfg[0] = ext_config;
    ext_cfg[1] = NULL;

    /* Call the load function last, it simplifies error handling. */
    WT_ERR(load(&S2C(session)->iface, (WT_CONFIG_ARG *)ext_cfg));

    /* Link onto the environment's list of open libraries. */
    __wt_spin_lock(session, &S2C(session)->api_lock);
    TAILQ_INSERT_TAIL(&S2C(session)->dlhqh, dlh, q);
    __wt_spin_unlock(session, &S2C(session)->api_lock);
    dlh = NULL;

err:
    if (dlh != NULL)
        WT_TRET(__wt_dlclose(session, dlh));
    __wt_free(session, ext_config);
    __wt_free(session, init_name);
    __wt_free(session, terminate_name);
    return (ret);
}

/*
 * __conn_load_extension --
 *     WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(WT_CONNECTION *wt_conn, const char *path, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, load_extension, config, cfg);

    ret = __conn_load_extension_int(session, path, cfg, false);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_load_extensions --
 *     Load the list of application-configured extensions.
 */
static int
__conn_load_extensions(WT_SESSION_IMPL *session, const char *cfg[], bool early_load)
{
    WT_CONFIG subconfig;
    WT_CONFIG_ITEM cval, skey, sval;
    WT_DECL_ITEM(exconfig);
    WT_DECL_ITEM(expath);
    WT_DECL_RET;
    const char *sub_cfg[] = {WT_CONFIG_BASE(session, WT_CONNECTION_load_extension), NULL, NULL};

    WT_ERR(__wt_config_gets(session, cfg, "extensions", &cval));
    __wt_config_subinit(session, &subconfig, &cval);
    while ((ret = __wt_config_next(&subconfig, &skey, &sval)) == 0) {
        if (expath == NULL)
            WT_ERR(__wt_scr_alloc(session, 0, &expath));
        WT_ERR(__wt_buf_fmt(session, expath, "%.*s", (int)skey.len, skey.str));
        if (sval.len > 0) {
            if (exconfig == NULL)
                WT_ERR(__wt_scr_alloc(session, 0, &exconfig));
            WT_ERR(__wt_buf_fmt(session, exconfig, "%.*s", (int)sval.len, sval.str));
        }
        sub_cfg[1] = sval.len > 0 ? exconfig->data : NULL;
        WT_ERR(__conn_load_extension_int(session, expath->data, sub_cfg, early_load));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &expath);
    __wt_scr_free(session, &exconfig);

    return (ret);
}

/*
 * __conn_get_home --
 *     WT_CONNECTION.get_home method.
 */
static const char *
__conn_get_home(WT_CONNECTION *wt_conn)
{
    return (((WT_CONNECTION_IMPL *)wt_conn)->home);
}

/*
 * __conn_configure_method --
 *     WT_CONNECTION.configure_method method.
 */
static int
__conn_configure_method(WT_CONNECTION *wt_conn, const char *method, const char *uri,
  const char *config, const char *type, const char *check)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL_NOCONF(conn, session, configure_method);

    ret = __wt_configure_method(session, method, uri, config, type, check);

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_is_new --
 *     WT_CONNECTION->is_new method.
 */
static int
__conn_is_new(WT_CONNECTION *wt_conn)
{
    return (((WT_CONNECTION_IMPL *)wt_conn)->is_new);
}

/*
 * __conn_close --
 *     WT_CONNECTION->close method.
 */
static int
__conn_close(WT_CONNECTION *wt_conn, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *s, *session;
    uint32_t i;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, close, config, cfg);
err:

    /*
     * Ramp the eviction dirty target down to encourage eviction threads to clear dirty content out
     * of cache.
     */
    conn->cache->eviction_dirty_trigger = 1.0;
    conn->cache->eviction_dirty_target = 0.1;

    /*
     * Rollback all running transactions. We do this as a separate pass because an active
     * transaction in one session could cause trouble when closing a file, even if that session
     * never referenced that file.
     */
    for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i)
        if (s->active && !F_ISSET(s, WT_SESSION_INTERNAL) && F_ISSET(s->txn, WT_TXN_RUNNING)) {
            wt_session = &s->iface;
            WT_TRET(wt_session->rollback_transaction(wt_session, NULL));
        }

    /* Close open, external sessions. */
    for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i)
        if (s->active && !F_ISSET(s, WT_SESSION_INTERNAL)) {
            wt_session = &s->iface;
            /*
             * Notify the user that we are closing the session handle via the registered close
             * callback.
             */
            if (s->event_handler->handle_close != NULL)
                WT_TRET(s->event_handler->handle_close(s->event_handler, wt_session, NULL));
            WT_TRET(__wt_session_close_internal(s));
        }

    /* Wait for in-flight operations to complete. */
    WT_TRET(__wt_txn_activity_drain(session));

    /*
     * There should be no active transactions running now. Therefore, it's safe for operations to
     * proceed without doing snapshot visibility checks.
     */
    session->txn->isolation = WT_ISO_READ_UNCOMMITTED;

    WT_TRET(__wt_lsm_manager_destroy(session));

    /*
     * After the LSM threads have exited, we won't open more files for the application. However, the
     * sweep server is still running and it can close file handles at the same time the final
     * checkpoint is reviewing open data handles (forcing checkpoint to reopen handles). Shut down
     * the sweep server.
     */
    WT_TRET(__wt_sweep_destroy(session));
    WT_TRET(__wt_tiered_storage_destroy(session));

    /*
     * Shut down the checkpoint and capacity server threads: we don't want to throttle writes and
     * we're about to do a final checkpoint separately from the checkpoint server.
     */
    WT_TRET(__wt_capacity_server_destroy(session));
    WT_TRET(__wt_checkpoint_server_destroy(session));

    /* Perform a final checkpoint and shut down the global transaction state. */
    WT_TRET(__wt_txn_global_shutdown(session, cfg));

    if (ret != 0) {
        __wt_err(session, ret, "failure during close, disabling further writes");
        F_SET(conn, WT_CONN_PANIC);
    }

    /*
     * Now that the final checkpoint is complete, the shutdown process should not allocate a
     * significant amount of new memory. If a user configured leaking memory on shutdown, we will
     * avoid freeing memory at this time. This allows for faster shutdown as freeing all the content
     * of the cache can be slow.
     */
    WT_TRET(__wt_config_gets(session, cfg, "leak_memory", &cval));
    if (cval.val != 0)
        F_SET(conn, WT_CONN_LEAK_MEMORY);

    WT_TRET(__wt_connection_close(conn));

    /* We no longer have a session, don't try to update it. */
    session = NULL;

    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_debug_info --
 *     WT_CONNECTION->debug_info method.
 */
static int
__conn_debug_info(WT_CONNECTION *wt_conn, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, debug_info, config, cfg);

    WT_ERR(__wt_config_gets(session, cfg, "cache", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_cache(session));

    WT_ERR(__wt_config_gets(session, cfg, "cursors", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_sessions(session, true));

    WT_ERR(__wt_config_gets(session, cfg, "handles", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_handles(session));

    WT_ERR(__wt_config_gets(session, cfg, "log", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_log(session));

    WT_ERR(__wt_config_gets(session, cfg, "sessions", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_sessions(session, false));

    WT_ERR(__wt_config_gets(session, cfg, "txn", &cval));
    if (cval.val != 0)
        WT_ERR(__wt_verbose_dump_txn(session));
err:
    API_END_RET(session, ret);
}

/*
 * __conn_reconfigure --
 *     WT_CONNECTION->reconfigure method.
 */
static int
__conn_reconfigure(WT_CONNECTION *wt_conn, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, reconfigure, config, cfg);
    ret = __wt_conn_reconfig(session, cfg);
err:
    API_END_RET(session, ret);
}

/*
 * __conn_open_session --
 *     WT_CONNECTION->open_session method.
 */
static int
__conn_open_session(WT_CONNECTION *wt_conn, WT_EVENT_HANDLER *event_handler, const char *config,
  WT_SESSION **wt_sessionp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session, *session_ret;

    *wt_sessionp = NULL;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, open_session, config, cfg);
    WT_UNUSED(cfg);

    session_ret = NULL;
    WT_ERR(__wt_open_session(conn, event_handler, config, true, &session_ret));
    *wt_sessionp = &session_ret->iface;

err:
    API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_query_timestamp --
 *     WT_CONNECTION->query_timestamp method.
 */
static int
__conn_query_timestamp(WT_CONNECTION *wt_conn, char *hex_timestamp, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, query_timestamp, config, cfg);
    ret = __wt_txn_query_timestamp(session, hex_timestamp, cfg, true);
err:
    API_END_RET(session, ret);
}

/*
 * __conn_set_timestamp --
 *     WT_CONNECTION->set_timestamp method.
 */
static int
__conn_set_timestamp(WT_CONNECTION *wt_conn, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, set_timestamp, config, cfg);
    ret = __wt_txn_global_set_timestamp(session, cfg);
err:
    API_END_RET(session, ret);
}

/*
 * __conn_rollback_to_stable --
 *     WT_CONNECTION->rollback_to_stable method.
 */
static int
__conn_rollback_to_stable(WT_CONNECTION *wt_conn, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;

    CONNECTION_API_CALL(conn, session, rollback_to_stable, config, cfg);
    WT_STAT_CONN_INCR(session, txn_rts);
    ret = __wt_rollback_to_stable(session, cfg, false);
err:
    API_END_RET(session, ret);
}

/*
 * __conn_config_append --
 *     Append an entry to a config stack.
 */
static void
__conn_config_append(const char *cfg[], const char *config)
{
    while (*cfg != NULL)
        ++cfg;
    cfg[0] = config;
    cfg[1] = NULL;
}

/*
 * __conn_config_readonly --
 *     Append an entry to a config stack that overrides some settings when read-only is configured.
 */
static void
__conn_config_readonly(const char *cfg[])
{
    const char *readonly;

    /*
     * Override certain settings. In general we override the options whose default conflicts. Other
     * settings at odds will return an error and will be checked when those settings are processed.
     */
    readonly =
      "checkpoint=(wait=0),"
      "config_base=false,"
      "create=false,"
      "log=(archive=false,prealloc=false),"
      "lsm_manager=(merge=false),";
    __conn_config_append(cfg, readonly);
}

/*
 * __conn_config_check_version --
 *     Check if a configuration version isn't compatible.
 */
static int
__conn_config_check_version(WT_SESSION_IMPL *session, const char *config)
{
    WT_CONFIG_ITEM vmajor, vminor;

    /*
     * Version numbers aren't included in all configuration strings, but we check all of them just
     * in case. Ignore configurations without a version.
     */
    if (__wt_config_getones(session, config, "version.major", &vmajor) == WT_NOTFOUND)
        return (0);
    WT_RET(__wt_config_getones(session, config, "version.minor", &vminor));

    if (vmajor.val > WIREDTIGER_VERSION_MAJOR ||
      (vmajor.val == WIREDTIGER_VERSION_MAJOR && vminor.val > WIREDTIGER_VERSION_MINOR))
        WT_RET_MSG(session, ENOTSUP,
          "WiredTiger configuration is from an incompatible release of the WiredTiger engine, "
          "configuration major, minor of (%" PRId64 ", %" PRId64 "), with build (%d, %d)",
          vmajor.val, vminor.val, WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR);

    return (0);
}

/*
 * __conn_config_file --
 *     Read WiredTiger config files from the home directory.
 */
static int
__conn_config_file(
  WT_SESSION_IMPL *session, const char *filename, bool is_user, const char **cfg, WT_ITEM *cbuf)
{
    WT_DECL_RET;
    WT_FH *fh;
    wt_off_t size;
    size_t len;
    char *p, *t;
    bool exist, quoted;

    fh = NULL;

    /* Configuration files are always optional. */
    WT_RET(__wt_fs_exist(session, filename, &exist));
    if (!exist)
        return (0);

    /* Open the configuration file. */
    WT_RET(__wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, &fh));
    WT_ERR(__wt_filesize(session, fh, &size));
    if (size == 0)
        goto err;

    /*
     * Sanity test: a 100KB configuration file would be insane. (There's no practical reason to
     * limit the file size, but I can either limit the file size to something rational, or add code
     * to test if the wt_off_t size is larger than a uint32_t, which is more complicated and a waste
     * of time.)
     */
    if (size > 100 * 1024)
        WT_ERR_MSG(session, EFBIG, "Configuration file too big: %s", filename);
    len = (size_t)size;

    /*
     * Copy the configuration file into memory, with a little slop, I'm not interested in debugging
     * off-by-ones.
     *
     * The beginning of a file is the same as if we run into an unquoted newline character, simplify
     * the parsing loop by pretending that's what we're doing.
     */
    WT_ERR(__wt_buf_init(session, cbuf, len + 10));
    WT_ERR(__wt_read(session, fh, (wt_off_t)0, len, ((uint8_t *)cbuf->mem) + 1));
    ((uint8_t *)cbuf->mem)[0] = '\n';
    cbuf->size = len + 1;

    /*
     * Collapse the file's lines into a single string: newline characters are replaced with commas
     * unless the newline is quoted or backslash escaped. Comment lines (an unescaped newline where
     * the next non- white-space character is a hash), are discarded.
     */
    for (quoted = false, p = t = cbuf->mem; len > 0;) {
        /*
         * Backslash pairs pass through untouched, unless immediately preceding a newline, in which
         * case both the backslash and the newline are discarded. Backslash characters escape quoted
         * characters, too, that is, a backslash followed by a quote doesn't start or end a quoted
         * string.
         */
        if (*p == '\\' && len > 1) {
            if (p[1] != '\n') {
                *t++ = p[0];
                *t++ = p[1];
            }
            p += 2;
            len -= 2;
            continue;
        }

        /*
         * If we're in a quoted string, or starting a quoted string, take all characters, including
         * white-space and newlines.
         */
        if (quoted || *p == '"') {
            if (*p == '"')
                quoted = !quoted;
            *t++ = *p++;
            --len;
            continue;
        }

        /* Everything else gets taken, except for newline characters. */
        if (*p != '\n') {
            *t++ = *p++;
            --len;
            continue;
        }

        /*
         * Replace any newline characters with commas (and strings of commas are safe).
         *
         * After any newline, skip to a non-white-space character; if the next character is a hash
         * mark, skip to the next newline.
         */
        for (;;) {
            for (*t++ = ','; --len > 0 && __wt_isspace((u_char) * ++p);)
                ;
            if (len == 0)
                break;
            if (*p != '#')
                break;
            while (--len > 0 && *++p != '\n')
                ;
            if (len == 0)
                break;
        }
    }
    *t = '\0';
    cbuf->size = WT_PTRDIFF(t, cbuf->data);

    /* Check any version. */
    WT_ERR(__conn_config_check_version(session, cbuf->data));

    /* Check the configuration information. */
    WT_ERR(__wt_config_check(session,
      is_user ? WT_CONFIG_REF(session, wiredtiger_open_usercfg) :
                WT_CONFIG_REF(session, wiredtiger_open_basecfg),
      cbuf->data, 0));

    /* Append it to the stack. */
    __conn_config_append(cfg, cbuf->data);

err:
    WT_TRET(__wt_close(session, &fh));

    /**
     * Encountering an invalid configuration string from the base configuration file suggests
     * that there is corruption present in the file.
     */
    if (!is_user && ret == EINVAL) {
        F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
        return (WT_ERROR);
    }

    return (ret);
}

/*
 * __conn_env_var --
 *     Get an environment variable, but refuse to use it if running with additional privilege and
 *     "use_environment_priv" not configured.
 */
static int
__conn_env_var(WT_SESSION_IMPL *session, const char *cfg[], const char *name, const char **configp)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;

    *configp = NULL;

    /* Only use environment variables if "use_environment" is configured. */
    WT_RET(__wt_config_gets(session, cfg, "use_environment", &cval));
    if (cval.val == 0)
        return (0);

    /* Get a copy of the variable, if any. */
    WT_RET(__wt_getenv(session, name, configp));
    if (*configp == NULL)
        return (0);

    /*
     * Security stuff:
     *
     * Don't use the environment variable if the process has additional privileges, unless
     * "use_environment_priv" is configured.
     */
    if (!__wt_has_priv())
        return (0);

    WT_ERR(__wt_config_gets(session, cfg, "use_environment_priv", &cval));
    if (cval.val == 0)
        WT_ERR_MSG(session, WT_ERROR,
          "privileged process has %s environment variable set, without having "
          "\"use_environment_priv\" configured",
          name);
    return (0);

err:
    __wt_free(session, *configp);
    return (ret);
}

/*
 * __conn_config_env --
 *     Read configuration from an environment variable, if set.
 */
static int
__conn_config_env(WT_SESSION_IMPL *session, const char *cfg[], WT_ITEM *cbuf)
{
    WT_DECL_RET;
    const char *env_config;

    /* Get the WIREDTIGER_CONFIG environment variable. */
    WT_RET(__conn_env_var(session, cfg, "WIREDTIGER_CONFIG", &env_config));
    if (env_config == NULL)
        return (0);

    /* Check any version. */
    WT_ERR(__conn_config_check_version(session, env_config));

    /* Upgrade the configuration string. */
    WT_ERR(__wt_buf_setstr(session, cbuf, env_config));

    /* Check the configuration information. */
    WT_ERR(__wt_config_check(session, WT_CONFIG_REF(session, wiredtiger_open), env_config, 0));

    /* Append it to the stack. */
    __conn_config_append(cfg, cbuf->data);

err:
    __wt_free(session, env_config);

    return (ret);
}

/*
 * __conn_hash_config --
 *     Configure and allocate hash buckets in the connection.
 */
static int
__conn_hash_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint64_t i;

    conn = S2C(session);
    WT_RET(__wt_config_gets(session, cfg, "hash.buckets", &cval));
    if (!__wt_ispo2((uint32_t)cval.val))
        WT_RET_MSG(session, EINVAL, "Hash bucket size %" PRIu64 " invalid. Must be power of 2",
          (uint64_t)cval.val);
    conn->hash_size = (uint64_t)cval.val;
    WT_RET(__wt_config_gets(session, cfg, "hash.dhandle_buckets", &cval));
    if (!__wt_ispo2((uint32_t)cval.val))
        WT_RET_MSG(session, EINVAL,
          "Data handle hash bucket size %" PRIu64 " invalid. Must be power of 2",
          (uint64_t)cval.val);
    conn->dh_hash_size = (uint64_t)cval.val;
    /* Don't set the values in the statistics here. They're set after the connection is set up. */

    /* Hash bucket arrays. */
    WT_RET(__wt_calloc_def(session, conn->hash_size, &conn->blockhash));
    WT_RET(__wt_calloc_def(session, conn->hash_size, &conn->fhhash));
    for (i = 0; i < conn->hash_size; ++i) {
        TAILQ_INIT(&conn->blockhash[i]);
        TAILQ_INIT(&conn->fhhash[i]);
    }
    WT_RET(__wt_calloc_def(session, conn->dh_hash_size, &conn->dh_bucket_count));
    WT_RET(__wt_calloc_def(session, conn->dh_hash_size, &conn->dhhash));
    for (i = 0; i < conn->dh_hash_size; ++i)
        TAILQ_INIT(&conn->dhhash[i]);

    return (0);
}

/*
 * __conn_home --
 *     Set the database home directory.
 */
static int
__conn_home(WT_SESSION_IMPL *session, const char *home, const char *cfg[])
{
    /*
     * If the application specifies a home directory, use it. Else use the WIREDTIGER_HOME
     * environment variable. Else default to ".".
     */
    if (home == NULL) {
        WT_RET(__conn_env_var(session, cfg, "WIREDTIGER_HOME", &S2C(session)->home));
        if (S2C(session)->home != NULL)
            return (0);

        home = ".";
    }

    return (__wt_strdup(session, home, &S2C(session)->home));
}

/*
 * __conn_single --
 *     Confirm that no other thread of control is using this database.
 */
static int
__conn_single(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn, *t;
    WT_DECL_RET;
    WT_FH *fh;
    wt_off_t size;
    size_t len;
    char buf[256];
    bool bytelock, exist, is_create, match;

    conn = S2C(session);
    fh = NULL;

    WT_RET(__wt_config_gets(session, cfg, "create", &cval));
    is_create = cval.val != 0;

    if (F_ISSET(conn, WT_CONN_READONLY))
        is_create = false;

    bytelock = true;
    __wt_spin_lock(session, &__wt_process.spinlock);

    /*
     * We first check for other threads of control holding a lock on this database, because the
     * byte-level locking functions are based on the POSIX 1003.1 fcntl APIs, which require all
     * locks associated with a file for a given process are removed when any file descriptor for the
     * file is closed by that process. In other words, we can't open a file handle on the lock file
     * until we are certain that closing that handle won't discard the owning thread's lock.
     * Applications hopefully won't open a database in multiple threads, but we don't want to have
     * it fail the first time, but succeed the second.
     */
    match = false;
    TAILQ_FOREACH (t, &__wt_process.connqh, q)
        if (t->home != NULL && t != conn && strcmp(t->home, conn->home) == 0) {
            match = true;
            break;
        }
    if (match)
        WT_ERR_MSG(session, EBUSY,
          "WiredTiger database is already being managed by another thread in this process");

    /*
     * !!!
     * Be careful changing this code.
     *
     * We locked the WiredTiger file before release 2.3.2; a separate lock
     * file was added after 2.3.1 because hot backup has to copy the
     * WiredTiger file and system utilities on Windows can't copy locked
     * files.
     *
     * Additionally, avoid an upgrade race: a 2.3.1 release process might
     * have the WiredTiger file locked, and we're going to create the lock
     * file and lock it instead. For this reason, first acquire a lock on
     * the lock file and then a lock on the WiredTiger file, then release
     * the latter so hot backups can proceed.  (If someone were to run a
     * current release and subsequently a historic release, we could still
     * fail because the historic release will ignore our lock file and will
     * then successfully lock the WiredTiger file, but I can't think of any
     * way to fix that.)
     *
     * Open the WiredTiger lock file, optionally creating it if it doesn't
     * exist. The "optional" part of that statement is tricky: we don't want
     * to create the lock file in random directories when users mistype the
     * database home directory path, so we only create the lock file in two
     * cases: First, applications creating databases will configure create,
     * create the lock file. Second, after a hot backup, all of the standard
     * files will have been copied into place except for the lock file (see
     * above, locked files cannot be copied on Windows). If the WiredTiger
     * file exists in the directory, create the lock file, covering the case
     * of a hot backup.
     */
    exist = false;
    if (!is_create)
        WT_ERR(__wt_fs_exist(session, WT_WIREDTIGER, &exist));
    ret = __wt_open(session, WT_SINGLETHREAD, WT_FS_OPEN_FILE_TYPE_REGULAR,
      is_create || exist ? WT_FS_OPEN_CREATE : 0, &conn->lock_fh);

    /*
     * If this is a read-only connection and we cannot grab the lock file, check if it is because
     * there's no write permission or if the file does not exist. If so, then ignore the error. XXX
     * Ignoring the error does allow multiple read-only connections to exist at the same time on a
     * read-only directory.
     *
     * If we got an expected permission or non-existence error then skip the byte lock.
     */
    if (F_ISSET(conn, WT_CONN_READONLY) && (ret == EACCES || ret == ENOENT)) {
        bytelock = false;
        ret = 0;
    }

    /**
     * The WiredTiger lock file will not be created if the WiredTiger file does not exist in the
     * directory, suggesting possible corruption if the WiredTiger file was deleted. Suggest running
     * salvage.
     */
    if (ret == ENOENT) {
        WT_ERR(__wt_fs_exist(session, WT_WIREDTIGER, &exist));
        if (!exist) {
            F_SET(conn, WT_CONN_DATA_CORRUPTION);
            WT_ERR(WT_ERROR);
        }
    }

    WT_ERR(ret);
    if (bytelock) {
        /*
         * Lock a byte of the file: if we don't get the lock, some other process is holding it,
         * we're done. The file may be zero-length, and that's OK, the underlying call supports
         * locking past the end-of-file.
         */
        if (__wt_file_lock(session, conn->lock_fh, true) != 0)
            WT_ERR_MSG(
              session, EBUSY, "WiredTiger database is already being managed by another process");

/*
 * If the size of the lock file is non-zero, we created it (or won a locking race with the thread
 * that created it, it doesn't matter).
 *
 * Write something into the file, zero-length files make me nervous.
 *
 * The test against the expected length is sheer paranoia (the length should be 0 or correct), but
 * it shouldn't hurt.
 */
#define WT_SINGLETHREAD_STRING "WiredTiger lock file\n"
        WT_ERR(__wt_filesize(session, conn->lock_fh, &size));
        if ((size_t)size != strlen(WT_SINGLETHREAD_STRING))
            WT_ERR(__wt_write(session, conn->lock_fh, (wt_off_t)0, strlen(WT_SINGLETHREAD_STRING),
              WT_SINGLETHREAD_STRING));
    }

    /* We own the lock file, optionally create the WiredTiger file. */
    ret = __wt_open(
      session, WT_WIREDTIGER, WT_FS_OPEN_FILE_TYPE_REGULAR, is_create ? WT_FS_OPEN_CREATE : 0, &fh);

    /*
     * If we're read-only, check for handled errors. Even if able to open the WiredTiger file
     * successfully, we do not try to lock it. The lock file test above is the only one we do for
     * read-only.
     */
    if (F_ISSET(conn, WT_CONN_READONLY)) {
        if (ret == EACCES || ret == ENOENT)
            ret = 0;
        WT_ERR(ret);
    } else {
        if (ret == ENOENT) {
            F_SET(conn, WT_CONN_DATA_CORRUPTION);
            WT_ERR(WT_ERROR);
        }
        WT_ERR(ret);
        /*
         * Lock the WiredTiger file (for backward compatibility reasons as described above).
         * Immediately release the lock, it's just a test.
         */
        if (__wt_file_lock(session, fh, true) != 0) {
            WT_ERR_MSG(
              session, EBUSY, "WiredTiger database is already being managed by another process");
        }
        WT_ERR(__wt_file_lock(session, fh, false));
    }

    /*
     * We own the database home, figure out if we're creating it. There are a few files created when
     * initializing the database home and we could crash in-between any of them, so there's no
     * simple test. The last thing we do during initialization is rename a turtle file into place,
     * and there's never a database home after that point without a turtle file. If the turtle file
     * doesn't exist, it's a create.
     */
    WT_ERR(__wt_turtle_exists(session, &exist));
    conn->is_new = exist ? 0 : 1;

    if (conn->is_new) {
        if (F_ISSET(conn, WT_CONN_READONLY))
            WT_ERR_MSG(session, EINVAL,
              "The database directory is empty or needs recovery, cannot continue with a read only "
              "connection");
        WT_ERR(__wt_snprintf_len_set(
          buf, sizeof(buf), &len, "%s\n%s\n", WT_WIREDTIGER, WIREDTIGER_VERSION_STRING));
        WT_ERR(__wt_write(session, fh, (wt_off_t)0, len, buf));
        WT_ERR(__wt_fsync(session, fh, true));
    } else {
        /*
         * Although exclusive and the read-only configuration settings are at odds, we do not have
         * to check against read-only here because it falls out from earlier code in this function
         * preventing creation and confirming the database already exists.
         */
        WT_ERR(__wt_config_gets(session, cfg, "exclusive", &cval));
        if (cval.val != 0)
            WT_ERR_MSG(session, EEXIST,
              "WiredTiger database already exists and exclusive option configured");
    }

err:
    /*
     * We ignore the connection's lock file handle on error, it will be closed when the connection
     * structure is destroyed.
     */
    WT_TRET(__wt_close(session, &fh));

    __wt_spin_unlock(session, &__wt_process.spinlock);
    return (ret);
}

/*
 * __wt_debug_mode_config --
 *     Set debugging configuration.
 */
int
__wt_debug_mode_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CACHE *cache;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;

    conn = S2C(session);
    cache = conn->cache;
    txn_global = &conn->txn_global;

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.checkpoint_retention", &cval));

    /*
     * Checkpoint retention has some rules to avoid needing a lock to coordinate with the archive
     * thread and avoid memory issues. You can turn it on to some value. You can turn it off. You
     * can reconfigure to the same value again. You cannot change the non-zero value. Once it was on
     * in the past and then turned off, you cannot turn it back on again.
     */
    if (cval.val != 0) {
        if (conn->debug_ckpt_cnt != 0 && cval.val != conn->debug_ckpt_cnt)
            WT_RET_MSG(session, EINVAL, "Cannot change value for checkpoint retention");
        WT_RET(
          __wt_realloc_def(session, &conn->debug_ckpt_alloc, (size_t)cval.val, &conn->debug_ckpt));
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_CKPT_RETAIN);
    } else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_CKPT_RETAIN);
    /*
     * We need to make sure all writes to other fields are visible before setting the count because
     * the archive thread may walk the array using this value.
     */
    WT_PUBLISH(conn->debug_ckpt_cnt, (uint32_t)cval.val);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.corruption_abort", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_CORRUPTION_ABORT);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_CORRUPTION_ABORT);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.cursor_copy", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_CURSOR_COPY);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_CURSOR_COPY);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.eviction", &cval));
    if (cval.val)
        F_SET(cache, WT_CACHE_EVICT_DEBUG_MODE);
    else
        F_CLR(cache, WT_CACHE_EVICT_DEBUG_MODE);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.log_retention", &cval));
    conn->debug_log_cnt = (uint32_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.realloc_exact", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_REALLOC_EXACT);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_REALLOC_EXACT);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.rollback_error", &cval));
    txn_global->debug_rollback = (uint64_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.slow_checkpoint", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_SLOW_CKPT);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_SLOW_CKPT);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.table_logging", &cval));
    if (cval.val)
        FLD_SET(conn->log_flags, WT_CONN_LOG_DEBUG_MODE);
    else
        FLD_CLR(conn->log_flags, WT_CONN_LOG_DEBUG_MODE);

    WT_RET(__wt_config_gets(session, cfg, "debug_mode.update_restore_evict", &cval));
    if (cval.val)
        FLD_SET(conn->debug_flags, WT_CONN_DEBUG_UPDATE_RESTORE_EVICT);
    else
        FLD_CLR(conn->debug_flags, WT_CONN_DEBUG_UPDATE_RESTORE_EVICT);
    return (0);
}

/*
 * __wt_verbose_config --
 *     Set verbose configuration.
 */
int
__wt_verbose_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    static const WT_NAME_FLAG verbtypes[] = {{"api", WT_VERB_API}, {"backup", WT_VERB_BACKUP},
      {"block", WT_VERB_BLOCK}, {"block_cache", WT_VERB_BLKCACHE},
      {"checkpoint", WT_VERB_CHECKPOINT}, {"checkpoint_cleanup", WT_VERB_CHECKPOINT_CLEANUP},
      {"checkpoint_progress", WT_VERB_CHECKPOINT_PROGRESS}, {"compact", WT_VERB_COMPACT},
      {"compact_progress", WT_VERB_COMPACT_PROGRESS}, {"error_returns", WT_VERB_ERROR_RETURNS},
      {"evict", WT_VERB_EVICT}, {"evict_stuck", WT_VERB_EVICT_STUCK},
      {"evictserver", WT_VERB_EVICTSERVER}, {"fileops", WT_VERB_FILEOPS},
      {"handleops", WT_VERB_HANDLEOPS}, {"log", WT_VERB_LOG}, {"hs", WT_VERB_HS},
      {"history_store_activity", WT_VERB_HS_ACTIVITY}, {"lsm", WT_VERB_LSM},
      {"lsm_manager", WT_VERB_LSM_MANAGER}, {"metadata", WT_VERB_METADATA},
      {"mutex", WT_VERB_MUTEX}, {"overflow", WT_VERB_OVERFLOW}, {"read", WT_VERB_READ},
      {"reconcile", WT_VERB_RECONCILE}, {"recovery", WT_VERB_RECOVERY},
      {"recovery_progress", WT_VERB_RECOVERY_PROGRESS}, {"rts", WT_VERB_RTS},
      {"salvage", WT_VERB_SALVAGE}, {"shared_cache", WT_VERB_SHARED_CACHE},
      {"split", WT_VERB_SPLIT}, {"temporary", WT_VERB_TEMPORARY},
      {"thread_group", WT_VERB_THREAD_GROUP}, {"timestamp", WT_VERB_TIMESTAMP},
      {"tiered", WT_VERB_TIERED}, {"transaction", WT_VERB_TRANSACTION}, {"verify", WT_VERB_VERIFY},
      {"version", WT_VERB_VERSION}, {"write", WT_VERB_WRITE}, {NULL, 0}};
    WT_CONFIG_ITEM cval, sval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const WT_NAME_FLAG *ft;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "verbose", &cval));

    for (ft = verbtypes; ft->name != NULL; ft++) {
        ret = __wt_config_subgets(session, &cval, ft->name, &sval);
        WT_RET_NOTFOUND_OK(ret);

        if (ret == WT_NOTFOUND)
            /*
             * If the given event isn't specified in configuration string, default it to the
             * WT_VERBOSE_WARNING verbosity level.
             */
            conn->verbose[ft->flag] = WT_VERBOSE_WARNING;
        else if (sval.type == WT_CONFIG_ITEM_BOOL && sval.len == 0)
            /*
             * If no value is associated with the event (i.e passing verbose=[checkpoint]), default
             * the event to WT_VERBOSE_DEBUG. Correspondingly, all legacy uses of '__wt_verbose',
             * being messages without an explicit verbosity level, will default to
             * 'WT_VERBOSE_DEBUG'.
             */
            conn->verbose[ft->flag] = WT_VERBOSE_DEBUG;
        else if (sval.type == WT_CONFIG_ITEM_NUM && sval.val >= WT_VERBOSE_INFO &&
          sval.val <= WT_VERBOSE_DEBUG)
            conn->verbose[ft->flag] = (WT_VERBOSE_LEVEL)sval.val;
        else
            /*
             * We only support verbosity values in the form of positive numbers (representing
             * verbosity levels e.g. [checkpoint:1,rts:0]) and boolean expressions (e.g.
             * [checkpoint,rts]). Return error for all other unsupported verbosity values e.g
             * negative numbers and strings.
             */
            WT_RET_MSG(session, EINVAL, "Failed to parse verbose option '%s'", ft->name);
    }

    return (0);
}

/*
 * __wt_verbose_dump_sessions --
 *     Print out debugging information about sessions.
 */
int
__wt_verbose_dump_sessions(WT_SESSION_IMPL *session, bool show_cursors)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_SESSION_IMPL *s;
    uint32_t i, internal;

    conn = S2C(session);
    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    WT_RET(__wt_msg(session, "Active sessions: %" PRIu32 " Max: %" PRIu32, conn->session_cnt,
      conn->session_size));
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    internal = 0;
    for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i) {
        /*
         * If it is not active or it is an internal session it is not interesting.
         */
        if (!s->active)
            continue;
        if (F_ISSET(s, WT_SESSION_INTERNAL)) {
            ++internal;
            continue;
        }
        WT_ASSERT(session, i == s->id);
        WT_ERR(__wt_msg(session, "Session: ID: %" PRIu32 " @: 0x%p", i, (void *)s));
        WT_ERR(__wt_msg(session, "  Name: %s", s->name == NULL ? "EMPTY" : s->name));
        if (!show_cursors) {
            WT_ERR(
              __wt_msg(session, "  Last operation: %s", s->lastop == NULL ? "NONE" : s->lastop));
            WT_ERR(__wt_msg(
              session, "  Current dhandle: %s", s->dhandle == NULL ? "NONE" : s->dhandle->name));
            WT_ERR(
              __wt_msg(session, "  Backup in progress: %s", s->bkp_cursor == NULL ? "no" : "yes"));
            WT_ERR(__wt_msg(session, "  Compact state: %s",
              s->compact_state == WT_COMPACT_NONE ?
                "none" :
                (s->compact_state == WT_COMPACT_RUNNING ? "running" : "success")));
            WT_ERR(__wt_msg(session, "  Flags: 0x%" PRIx32, s->flags));
            WT_ERR(__wt_msg(session, "  Isolation level: %s",
              s->isolation == WT_ISO_READ_COMMITTED ?
                "read-committed" :
                (s->isolation == WT_ISO_READ_UNCOMMITTED ? "read-uncommitted" : "snapshot")));
            WT_ERR(__wt_msg(session, "  Transaction:"));
            WT_ERR(__wt_verbose_dump_txn_one(session, s, 0, NULL));
        } else {
            WT_ERR(__wt_msg(session, "  Number of positioned cursors: %u", s->ncursors));
            TAILQ_FOREACH (cursor, &s->cursors, q) {
                WT_ERR(__wt_msg(session, "Cursor @ %p:", (void *)cursor));
                WT_ERR(__wt_msg(session, "  URI: %s, Internal URI: %s",
                  cursor->uri == NULL ? "EMPTY" : cursor->uri,
                  cursor->internal_uri == NULL ? "EMPTY" : cursor->internal_uri));
                if (F_ISSET(cursor, WT_CURSTD_OPEN)) {
                    WT_ERR(__wt_buf_fmt(session, buf, "OPEN"));
                    if (F_ISSET(cursor, WT_CURSTD_KEY_SET) || F_ISSET(cursor, WT_CURSTD_VALUE_SET))
                        WT_ERR(__wt_buf_catfmt(session, buf, ", POSITIONED"));
                    else
                        WT_ERR(__wt_buf_catfmt(session, buf, ", RESET"));
                    if (F_ISSET(cursor, WT_CURSTD_APPEND))
                        WT_ERR(__wt_buf_catfmt(session, buf, ", APPEND"));
                    if (F_ISSET(cursor, WT_CURSTD_BULK))
                        WT_ERR(__wt_buf_catfmt(session, buf, ", BULK"));
                    if (F_ISSET(cursor, WT_CURSTD_META_INUSE))
                        WT_ERR(__wt_buf_catfmt(session, buf, ", META_INUSE"));
                    if (F_ISSET(cursor, WT_CURSTD_OVERWRITE))
                        WT_ERR(__wt_buf_catfmt(session, buf, ", OVERWRITE"));
                    WT_ERR(__wt_msg(session, "  %s", (const char *)buf->data));
                }
                WT_ERR(__wt_msg(session, "  Flags: 0x%" PRIx32, cursor->flags));
                WT_ERR(__wt_msg(session, "  Key_format: %s, Value_format: %s",
                  cursor->key_format == NULL ? "EMPTY" : cursor->key_format,
                  cursor->value_format == NULL ? "EMPTY" : cursor->value_format));
            }
        }
    }
    if (!show_cursors)
        WT_ERR(__wt_msg(session, "Internal sessions: %" PRIu32, internal));
err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_timing_stress_config --
 *     Set timing stress configuration. There are a places we optionally make threads sleep in order
 *     to stress the system and increase the likelihood of failure. For example, there are several
 *     places where page splits are delayed to make cursor iteration races more likely.
 */
int
__wt_timing_stress_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    /*
     * Each split race delay is controlled using a different flag to allow more effective race
     * condition detection, since enabling all delays at once can lead to an overall slowdown to the
     * point where race conditions aren't encountered.
     *
     * Fail points are also defined in this list and will occur randomly when enabled.
     */
    static const WT_NAME_FLAG stress_types[] = {
      {"aggressive_sweep", WT_TIMING_STRESS_AGGRESSIVE_SWEEP},
      {"backup_rename", WT_TIMING_STRESS_BACKUP_RENAME},
      {"checkpoint_reserved_txnid_delay", WT_TIMING_STRESS_CHECKPOINT_RESERVED_TXNID_DELAY},
      {"checkpoint_slow", WT_TIMING_STRESS_CHECKPOINT_SLOW},
      {"failpoint_history_delete_key_from_ts",
        WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS},
      {"history_store_checkpoint_delay", WT_TIMING_STRESS_HS_CHECKPOINT_DELAY},
      {"history_store_search", WT_TIMING_STRESS_HS_SEARCH},
      {"history_store_sweep_race", WT_TIMING_STRESS_HS_SWEEP},
      {"prepare_checkpoint_delay", WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY},
      {"split_1", WT_TIMING_STRESS_SPLIT_1}, {"split_2", WT_TIMING_STRESS_SPLIT_2},
      {"split_3", WT_TIMING_STRESS_SPLIT_3}, {"split_4", WT_TIMING_STRESS_SPLIT_4},
      {"split_5", WT_TIMING_STRESS_SPLIT_5}, {"split_6", WT_TIMING_STRESS_SPLIT_6},
      {"split_7", WT_TIMING_STRESS_SPLIT_7}, {NULL, 0}};
    WT_CONFIG_ITEM cval, sval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const WT_NAME_FLAG *ft;
    uint64_t flags;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "timing_stress_for_test", &cval));

    flags = 0;
    for (ft = stress_types; ft->name != NULL; ft++) {
        if ((ret = __wt_config_subgets(session, &cval, ft->name, &sval)) == 0 && sval.val != 0) {
            LF_SET(ft->flag);
        }
        WT_RET_NOTFOUND_OK(ret);
    }

    conn->timing_stress_flags = flags;
    return (0);
}

/*
 * __conn_write_base_config --
 *     Save the base configuration used to create a database.
 */
static int
__conn_write_base_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG parser;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_RET;
    WT_FSTREAM *fs;
    const char *base_config;
    bool exist;

    fs = NULL;
    base_config = NULL;

    /*
     * Discard any base configuration setup file left-over from previous runs. This doesn't matter
     * for correctness, it's just cleaning up random files.
     */
    WT_RET(__wt_remove_if_exists(session, WT_BASECONFIG_SET, false));

    /*
     * The base configuration file is only written if creating the database, and even then, a base
     * configuration file is optional.
     */
    if (!S2C(session)->is_new)
        return (0);
    WT_RET(__wt_config_gets(session, cfg, "config_base", &cval));
    if (!cval.val)
        return (0);

    /*
     * We don't test separately if we're creating the database in this run as we might have crashed
     * between creating the "WiredTiger" file and creating the base configuration file. If
     * configured, there's always a base configuration file, and we rename it into place, so it can
     * only NOT exist if we crashed before it was created; in other words, if the base configuration
     * file exists, we're done.
     */
    WT_RET(__wt_fs_exist(session, WT_BASECONFIG, &exist));
    if (exist)
        return (0);

    WT_RET(__wt_fopen(
      session, WT_BASECONFIG_SET, WT_FS_OPEN_CREATE | WT_FS_OPEN_EXCLUSIVE, WT_STREAM_WRITE, &fs));

    WT_ERR(__wt_fprintf(session, fs, "%s\n\n",
      "# Do not modify this file.\n"
      "#\n"
      "# WiredTiger created this file when the database was created,\n"
      "# to store persistent database settings.  Instead of changing\n"
      "# these settings, set a WIREDTIGER_CONFIG environment variable\n"
      "# or create a WiredTiger.config file to override them."));

    /*
     * The base configuration file contains all changes to default settings made at create, and we
     * include the user-configuration file in that list, even though we don't expect it to change.
     * Of course, an application could leave that file as it is right now and not remove a
     * configuration we need, but applications can also guarantee all database users specify
     * consistent environment variables and wiredtiger_open configuration arguments -- if we protect
     * against those problems, might as well include the application's configuration file in that
     * protection.
     *
     * We were passed the configuration items specified by the application. That list includes
     * configuring the default settings, presumably if the application configured it explicitly,
     * that setting should survive even if the default changes.
     *
     * When writing the base configuration file, we write the version and any configuration
     * information set by the application (in other words, the stack except for cfg[0]). However,
     * some configuration values need to be stripped out from the base configuration file; do that
     * now, and merge the rest to be written.
     */
    WT_ERR(__wt_config_merge(session, cfg + 1,
      "compatibility=(release=),"
      "config_base=,"
      "create=,"
      "encryption=(secretkey=),"
      "error_prefix=,"
      "exclusive=,"
      "in_memory=,"
      "log=(recover=),"
      "readonly=,"
      "timing_stress_for_test=,"
      "use_environment_priv=,"
      "verbose=,"
      "verify_metadata=,",
      &base_config));
    __wt_config_init(session, &parser, base_config);
    while ((ret = __wt_config_next(&parser, &k, &v)) == 0) {
        /* Fix quoting for non-trivial settings. */
        if (v.type == WT_CONFIG_ITEM_STRING) {
            --v.str;
            v.len += 2;
        }
        WT_ERR(__wt_fprintf(session, fs, "%.*s=%.*s\n", (int)k.len, k.str, (int)v.len, v.str));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Flush the stream and rename the file into place. */
    ret = __wt_sync_and_rename(session, &fs, WT_BASECONFIG_SET, WT_BASECONFIG);

    if (0) {
        /* Close open file handle, remove any temporary file. */
err:
        WT_TRET(__wt_fclose(session, &fs));
        WT_TRET(__wt_remove_if_exists(session, WT_BASECONFIG_SET, false));
    }

    __wt_free(session, base_config);

    return (ret);
}

/*
 * __conn_set_file_system --
 *     Configure a custom file system implementation on database open.
 */
static int
__conn_set_file_system(WT_CONNECTION *wt_conn, WT_FILE_SYSTEM *file_system, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    conn = (WT_CONNECTION_IMPL *)wt_conn;
    CONNECTION_API_CALL(conn, session, set_file_system, config, cfg);
    WT_UNUSED(cfg);

    /*
     * You can only configure a file system once, and attempting to do it again probably means the
     * extension argument didn't have early-load set and we've already configured the default file
     * system.
     */
    if (conn->file_system != NULL)
        WT_ERR_MSG(session, EPERM,
          "filesystem already configured; custom filesystems should enable \"early_load\" "
          "configuration");

    conn->file_system = file_system;

err:
    API_END_RET(session, ret);
}

/*
 * __conn_session_size --
 *     Return the session count for this run.
 */
static int
__conn_session_size(WT_SESSION_IMPL *session, const char *cfg[], uint32_t *vp)
{
    WT_CONFIG_ITEM cval;
    int64_t v;

/*
 * Start with 20 internal sessions to cover threads the application can't configure (for example,
 * checkpoint or statistics log server threads).
 */
#define WT_EXTRA_INTERNAL_SESSIONS 20
    v = WT_EXTRA_INTERNAL_SESSIONS;

    /* Then, add in the thread counts applications can configure. */
    WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
    v += cval.val;

    WT_RET(__wt_config_gets(session, cfg, "lsm_manager.worker_thread_max", &cval));
    v += cval.val;

    WT_RET(__wt_config_gets(session, cfg, "session_max", &cval));
    v += cval.val;

    *vp = (uint32_t)v;

    return (0);
}

/*
 * __conn_chk_file_system --
 *     Check the configured file system.
 */
static int
__conn_chk_file_system(WT_SESSION_IMPL *session, bool readonly)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

#define WT_CONN_SET_FILE_SYSTEM_REQ(name) \
    if (conn->file_system->name == NULL)  \
    WT_RET_MSG(session, EINVAL, "a WT_FILE_SYSTEM.%s method must be configured", #name)

    WT_CONN_SET_FILE_SYSTEM_REQ(fs_directory_list);
    WT_CONN_SET_FILE_SYSTEM_REQ(fs_directory_list_free);
    /* not required: directory_sync */
    WT_CONN_SET_FILE_SYSTEM_REQ(fs_exist);
    WT_CONN_SET_FILE_SYSTEM_REQ(fs_open_file);
    if (!readonly) {
        WT_CONN_SET_FILE_SYSTEM_REQ(fs_remove);
        WT_CONN_SET_FILE_SYSTEM_REQ(fs_rename);
    }
    WT_CONN_SET_FILE_SYSTEM_REQ(fs_size);

    /*
     * The lower-level API for returning the first matching entry was added later and not documented
     * because it's an optimization for high-end filesystems doing logging, specifically
     * pre-allocating log files. Check for the API and fall back to the standard API if not
     * available.
     */
    if (conn->file_system->fs_directory_list_single == NULL)
        conn->file_system->fs_directory_list_single = conn->file_system->fs_directory_list;

    return (0);
}

/*
 * wiredtiger_dummy_session_init --
 *     Initialize the connection's dummy session.
 */
static void
wiredtiger_dummy_session_init(WT_CONNECTION_IMPL *conn, WT_EVENT_HANDLER *event_handler)
{
    WT_SESSION_IMPL *session;

    session = &conn->dummy_session;

    /*
     * We use a fake session until we can allocate and initialize the real ones. Initialize the
     * necessary fields (unfortunately, the fields we initialize have been selected by core dumps,
     * we need to do better).
     */
    session->iface.connection = &conn->iface;
    session->name = "wiredtiger_open";

    /* Standard I/O and error handling first. */
    __wt_os_stdio(session);
    __wt_event_handler_set(session, event_handler);

    /* Statistics */
    session->stat_bucket = 0;

    /*
     * Set the default session's strerror method. If one of the extensions being loaded reports an
     * error via the WT_EXTENSION_API strerror method, but doesn't supply that method a WT_SESSION
     * handle, we'll use the WT_CONNECTION_IMPL's default session and its strerror method.
     */
    session->iface.strerror = __wt_session_strerror;

    /*
     * The dummy session should never be used to access data handles.
     */
    F_SET(session, WT_SESSION_NO_DATA_HANDLES);
}

/*
 * __conn_version_verify --
 *     Verify the versions before modifying the database.
 */
static int
__conn_version_verify(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    bool exist;

    conn = S2C(session);

    conn->recovery_major = 0;
    conn->recovery_minor = 0;
    conn->recovery_patch = 0;

    /* Always set the compatibility versions. */
    __wt_logmgr_compat_version(session);
    /*
     * If we're salvaging, don't verify now.
     */
    if (F_ISSET(conn, WT_CONN_SALVAGE))
        return (0);

    /*
     * Initialize the version variables. These aren't always populated since there are expected
     * cases where the turtle files doesn't exist (restoring from a backup, for example). All code
     * that deals with recovery versions must consider the case where they are default initialized
     * to zero.
     */
    WT_RET(__wt_fs_exist(session, WT_METADATA_TURTLE, &exist));
    if (exist)
        WT_RET(__wt_turtle_validate_version(session));

    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_CONFIG_ENABLED))
        WT_RET(__wt_log_compat_verify(session));

    return (0);
}

/*
 * wiredtiger_open --
 *     Main library entry point: open a new connection to a WiredTiger database.
 */
int
wiredtiger_open(const char *home, WT_EVENT_HANDLER *event_handler, const char *config,
  WT_CONNECTION **connectionp)
{
    static const WT_CONNECTION stdc = {__conn_close, __conn_debug_info, __conn_reconfigure,
      __conn_get_home, __conn_configure_method, __conn_is_new, __conn_open_session,
      __conn_query_timestamp, __conn_set_timestamp, __conn_rollback_to_stable,
      __conn_load_extension, __conn_add_data_source, __conn_add_collator, __conn_add_compressor,
      __conn_add_encryptor, __conn_add_extractor, __conn_set_file_system, __conn_add_storage_source,
      __conn_get_storage_source, __conn_get_extension_api};
    static const WT_NAME_FLAG file_types[] = {{"checkpoint", WT_DIRECT_IO_CHECKPOINT},
      {"data", WT_DIRECT_IO_DATA}, {"log", WT_DIRECT_IO_LOG}, {NULL, 0}};

    WT_CONFIG_ITEM cval, keyid, secretkey, sval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(encbuf);
    WT_DECL_ITEM(i1);
    WT_DECL_ITEM(i2);
    WT_DECL_ITEM(i3);
    WT_DECL_RET;
    const WT_NAME_FLAG *ft;
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    bool config_base_set, try_salvage, verify_meta;
    const char *enc_cfg[] = {NULL, NULL}, *merge_cfg;
    char version[64];

#if 0
    /* FIXME-WT-6263: Temporarily disable history store verification. */
    WT_SESSION_IMPL *verify_session;
#endif

    /* Leave lots of space for optional additional configuration. */
    const char *cfg[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};

    *connectionp = NULL;

    conn = NULL;
    session = NULL;
    merge_cfg = NULL;
    try_salvage = false;

    WT_RET(__wt_library_init());

    WT_RET(__wt_calloc_one(NULL, &conn));
    conn->iface = stdc;

    /*
     * Immediately link the structure into the connection structure list: the only thing ever looked
     * at on that list is the database name, and a NULL value is fine.
     */
    __wt_spin_lock(NULL, &__wt_process.spinlock);
    TAILQ_INSERT_TAIL(&__wt_process.connqh, conn, q);
    __wt_spin_unlock(NULL, &__wt_process.spinlock);

    /*
     * Initialize the fake session used until we can create real sessions.
     */
    wiredtiger_dummy_session_init(conn, event_handler);
    session = conn->default_session = &conn->dummy_session;

    /* Basic initialization of the connection structure. */
    WT_ERR(__wt_connection_init(conn));

    /* Check the application-specified configuration string. */
    WT_ERR(__wt_config_check(session, WT_CONFIG_REF(session, wiredtiger_open), config, 0));

    /*
     * Build the temporary, initial configuration stack, in the following
     * order (where later entries override earlier entries):
     *
     * 1. the base configuration for the wiredtiger_open call
     * 2. the config passed in by the application
     * 3. environment variable settings (optional)
     *
     * In other words, a configuration stack based on the application's
     * passed-in information and nothing else.
     */
    cfg[0] = WT_CONFIG_BASE(session, wiredtiger_open);
    cfg[1] = config;
    WT_ERR(__wt_scr_alloc(session, 0, &i1));
    WT_ERR(__conn_config_env(session, cfg, i1));

    /*
     * We need to know if configured for read-only or in-memory behavior before reading/writing the
     * filesystem. The only way the application can configure that before we touch the filesystem is
     * the wiredtiger config string or the WIREDTIGER_CONFIG environment variable.
     *
     * The environment isn't trusted by default, for security reasons; if the application wants us
     * to trust the environment before reading the filesystem, the wiredtiger_open config string is
     * the only way.
     */
    WT_ERR(__wt_config_gets(session, cfg, "in_memory", &cval));
    if (cval.val != 0)
        F_SET(conn, WT_CONN_IN_MEMORY);
    WT_ERR(__wt_config_gets(session, cfg, "readonly", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_READONLY);

    /* Configure error messages so we get them right early. */
    WT_ERR(__wt_config_gets(session, cfg, "error_prefix", &cval));
    if (cval.len != 0)
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->error_prefix));

    /* Set the database home so extensions have access to it. */
    WT_ERR(__conn_home(session, home, cfg));

    /*
     * Configure and allocate hash buckets. This must be done before the call to load extensions.
     * Some extensions like encryption or file systems may allocate hash arrays.
     */
    WT_ERR(__conn_hash_config(session, cfg));

    /*
     * Load early extensions before doing further initialization (one early extension is to
     * configure a file system).
     */
    WT_ERR(__conn_load_extensions(session, cfg, true));

    /*
     * If the application didn't configure its own file system, configure one of ours. Check to
     * ensure we have a valid file system.
     */
    if (conn->file_system == NULL) {
        if (F_ISSET(conn, WT_CONN_IN_MEMORY))
            WT_ERR(__wt_os_inmemory(session));
        else
#if defined(_MSC_VER)
            WT_ERR(__wt_os_win(session));
#else
            WT_ERR(__wt_os_posix(session));
#endif
    }
    WT_ERR(__conn_chk_file_system(session, F_ISSET(conn, WT_CONN_READONLY)));

    /* Make sure no other thread of control already owns this database. */
    WT_ERR(__conn_single(session, cfg));

    WT_ERR(__wt_conn_compat_config(session, cfg, false));

    /*
     * Capture the config_base setting file for later use. Again, if the application doesn't want us
     * to read the base configuration file, the WIREDTIGER_CONFIG environment variable or the
     * wiredtiger_open config string are the only ways.
     */
    WT_ERR(__wt_config_gets(session, cfg, "config_base", &cval));
    config_base_set = cval.val != 0;

    /*
     * Build the real configuration stack, in the following order (where
     * later entries override earlier entries):
     *
     * 1. all possible wiredtiger_open configurations
     * 2. the WiredTiger compilation version (expected to be overridden by
     *    any value in the base configuration file)
     * 3. base configuration file, created with the database (optional)
     * 4. the config passed in by the application
     * 5. user configuration file (optional)
     * 6. environment variable settings (optional)
     * 7. overrides for a read-only connection
     *
     * Clear the entries we added to the stack, we're going to build it in
     * order.
     */
    WT_ERR(__wt_scr_alloc(session, 0, &i2));
    WT_ERR(__wt_scr_alloc(session, 0, &i3));
    cfg[0] = WT_CONFIG_BASE(session, wiredtiger_open_all);
    cfg[1] = NULL;
    WT_ERR(__wt_snprintf(version, sizeof(version), "version=(major=%d,minor=%d)",
      conn->compat_major, conn->compat_minor));
    __conn_config_append(cfg, version);

    /* Ignore the base_config file if config_base_set is false. */
    if (config_base_set)
        WT_ERR(__conn_config_file(session, WT_BASECONFIG, false, cfg, i1));
    __conn_config_append(cfg, config);
    WT_ERR(__conn_config_file(session, WT_USERCONFIG, true, cfg, i2));
    WT_ERR(__conn_config_env(session, cfg, i3));

    /*
     * Merge the full configuration stack and save it for reconfiguration.
     */
    WT_ERR(__wt_config_merge(session, cfg, NULL, &merge_cfg));

    /*
     * Read-only and in-memory settings may have been set in a configuration file (not optimal, but
     * we can handle it). Get those settings again so we can override other configuration settings
     * as they are processed.
     */
    WT_ERR(__wt_config_gets(session, cfg, "in_memory", &cval));
    if (cval.val != 0)
        F_SET(conn, WT_CONN_IN_MEMORY);
    WT_ERR(__wt_config_gets(session, cfg, "readonly", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_READONLY);
    if (F_ISSET(conn, WT_CONN_READONLY)) {
        /*
         * Create a new stack with the merged configuration as the base. The read-only string will
         * use entry 1 and then we'll merge it again.
         */
        cfg[0] = merge_cfg;
        cfg[1] = NULL;
        cfg[2] = NULL;
        /*
         * We override some configuration settings for read-only. Other settings that conflict with
         * and are an error with read-only are tested in their individual locations later.
         */
        __conn_config_readonly(cfg);
        WT_ERR(__wt_config_merge(session, cfg, NULL, &conn->cfg));
    } else {
        conn->cfg = merge_cfg;
        merge_cfg = NULL;
    }

    /*
     * Configuration ...
     *
     * We can't open sessions yet, so any configurations that cause sessions to be opened must be
     * handled inside __wt_connection_open.
     *
     * The error message configuration might have changed (if set in a configuration file, and not
     * in the application's configuration string), get it again. Do it first, make error messages
     * correct. Ditto verbose configuration so we dump everything the application wants to see.
     */
    WT_ERR(__wt_config_gets(session, cfg, "error_prefix", &cval));
    if (cval.len != 0) {
        __wt_free(session, conn->error_prefix);
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->error_prefix));
    }
    WT_ERR(__wt_verbose_config(session, cfg));
    WT_ERR(__wt_timing_stress_config(session, cfg));
    WT_ERR(__wt_block_cache_setup(session, cfg, false));
    WT_ERR(__wt_conn_optrack_setup(session, cfg, false));
    WT_ERR(__conn_session_size(session, cfg, &conn->session_size));
    WT_ERR(__wt_config_gets(session, cfg, "session_scratch_max", &cval));
    conn->session_scratch_max = (size_t)cval.val;

    /*
     * If buffer alignment is not configured, use zero unless direct I/O is also configured, in
     * which case use the build-time default. The code to parse write through is also here because
     * it is nearly identical to direct I/O.
     */
    WT_ERR(__wt_config_gets(session, cfg, "direct_io", &cval));
    for (ft = file_types; ft->name != NULL; ft++) {
        ret = __wt_config_subgets(session, &cval, ft->name, &sval);
        if (ret == 0) {
            if (sval.val)
                FLD_SET(conn->direct_io, ft->flag);
        } else
            WT_ERR_NOTFOUND_OK(ret, false);
    }

    WT_ERR(__wt_config_gets(session, cfg, "write_through", &cval));
    for (ft = file_types; ft->name != NULL; ft++) {
        ret = __wt_config_subgets(session, &cval, ft->name, &sval);
        if (ret == 0) {
            if (sval.val)
                FLD_SET(conn->write_through, ft->flag);
        } else
            WT_ERR_NOTFOUND_OK(ret, false);
    }

    WT_ERR(__wt_config_gets(session, cfg, "buffer_alignment", &cval));
    if (cval.val == -1) {
        conn->buffer_alignment = 0;
        if (conn->direct_io != 0)
            conn->buffer_alignment = WT_BUFFER_ALIGNMENT_DEFAULT;
    } else
        conn->buffer_alignment = (size_t)cval.val;
#ifndef HAVE_POSIX_MEMALIGN
    if (conn->buffer_alignment != 0)
        WT_ERR_MSG(session, EINVAL, "buffer_alignment requires posix_memalign");
#endif

    WT_ERR(__wt_config_gets(session, cfg, "cache_cursors", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_CACHE_CURSORS);

    WT_ERR(__wt_config_gets(session, cfg, "checkpoint_sync", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_CKPT_SYNC);

    WT_ERR(__wt_config_gets(session, cfg, "file_close_sync", &cval));
    if (cval.val)
        F_SET(conn, WT_CONN_FILE_CLOSE_SYNC);

    WT_ERR(__wt_config_gets(session, cfg, "file_extend", &cval));
    /*
     * If the log extend length is not set use the default of the configured maximum log file size.
     * That size is not known until it is initialized as part of the log server initialization.
     */
    conn->log_extend_len = WT_CONFIG_UNSET;
    for (ft = file_types; ft->name != NULL; ft++) {
        ret = __wt_config_subgets(session, &cval, ft->name, &sval);
        if (ret == 0) {
            switch (ft->flag) {
            case WT_DIRECT_IO_DATA:
                conn->data_extend_len = sval.val;
                break;
            case WT_DIRECT_IO_LOG:
                /*
                 * When using "file_extend=(log=)", the val returned is 1. Unset the log extend
                 * length in that case to use the default.
                 */
                if (sval.val == 1)
                    conn->log_extend_len = WT_CONFIG_UNSET;
                else if (sval.val == 0 ||
                  (sval.val >= WT_LOG_FILE_MIN && sval.val <= WT_LOG_FILE_MAX))
                    conn->log_extend_len = sval.val;
                else
                    WT_ERR_MSG(session, EINVAL, "invalid log extend length: %" PRId64, sval.val);
                break;
            }
        } else
            WT_ERR_NOTFOUND_OK(ret, false);
    }

    WT_ERR(__wt_config_gets(session, cfg, "mmap", &cval));
    conn->mmap = cval.val != 0;
    WT_ERR(__wt_config_gets(session, cfg, "mmap_all", &cval));
    conn->mmap_all = cval.val != 0;
    if (conn->direct_io && conn->mmap_all)
        WT_ERR_MSG(
          session, EINVAL, "direct I/O configuration is incompatible with mmap_all configuration");

    WT_ERR(__wt_config_gets(session, cfg, "operation_timeout_ms", &cval));
    conn->operation_timeout_us = (uint64_t)(cval.val * WT_THOUSAND);

    WT_ERR(__wt_config_gets(session, cfg, "salvage", &cval));
    if (cval.val) {
        if (F_ISSET(conn, WT_CONN_READONLY))
            WT_ERR_MSG(session, EINVAL, "Readonly configuration incompatible with salvage");
        F_SET(conn, WT_CONN_SALVAGE);
    }

    WT_ERR(__wt_conn_statistics_config(session, cfg));
    WT_ERR(__wt_lsm_manager_config(session, cfg));
    WT_ERR(__wt_sweep_config(session, cfg));

    /* Initialize the OS page size for mmap */
    conn->page_size = __wt_get_vm_pagesize();

    /* Now that we know if verbose is configured, output the version. */
    __wt_verbose(session, WT_VERB_VERSION, "%s", WIREDTIGER_VERSION_STRING);

    /*
     * Open the connection, then reset the local session as the real one was allocated in
     * __wt_connection_open.
     */
    WT_ERR(__wt_connection_open(conn, cfg));
    session = conn->default_session;

#ifndef WT_STANDALONE_BUILD
    /* Explicitly set the flag to indicate whether the database that was not shutdown cleanly. */
    conn->unclean_shutdown = false;
#endif

    /*
     * This function expects the cache to be created so parse this after the rest of the connection
     * is set up.
     */
    WT_ERR(__wt_debug_mode_config(session, cfg));

    /*
     * Load the extensions after initialization completes; extensions expect everything else to be
     * in place, and the extensions call back into the library.
     */
    WT_ERR(__conn_builtin_extensions(conn, cfg));
    WT_ERR(__conn_load_extensions(session, cfg, false));

    /*
     * Do some early initialization for tiered storage, as this may affect our choice of file system
     * for some operations.
     */
    WT_ERR(__wt_tiered_conn_config(session, cfg, false));

    /*
     * The metadata/log encryptor is configured after extensions, since extensions may load
     * encryptors. We have to do this before creating the metadata file.
     *
     * The encryption customize callback needs the fully realized set of encryption args, as simply
     * grabbing "encryption" doesn't work. As an example, configuration for the current call may
     * just be "encryption=(secretkey=xxx)", with encryption.name, encryption.keyid being
     * 'inherited' from the stored base configuration.
     */
    WT_ERR(__wt_config_gets_none(session, cfg, "encryption.name", &cval));
    WT_ERR(__wt_config_gets_none(session, cfg, "encryption.keyid", &keyid));
    WT_ERR(__wt_config_gets_none(session, cfg, "encryption.secretkey", &secretkey));
    WT_ERR(__wt_scr_alloc(session, 0, &encbuf));
    WT_ERR(__wt_buf_fmt(session, encbuf, "(name=%.*s,keyid=%.*s,secretkey=%.*s)", (int)cval.len,
      cval.str, (int)keyid.len, keyid.str, (int)secretkey.len, secretkey.str));
    enc_cfg[0] = encbuf->data;
    WT_ERR(
      __wt_encryptor_config(session, &cval, &keyid, (WT_CONFIG_ARG *)enc_cfg, &conn->kencryptor));

    /*
     * We need to parse the logging configuration here to verify the compatibility settings because
     * we may need the log path and encryption and compression settings.
     */
    WT_ERR(__wt_logmgr_config(session, cfg, false));
    WT_ERR(__conn_version_verify(session));

    /*
     * Configuration completed; optionally write a base configuration file.
     */
    WT_ERR(__conn_write_base_config(session, cfg));

    /*
     * Check on the turtle and metadata files, creating them if necessary (which avoids application
     * threads racing to create the metadata file later). Once the metadata file exists, get a
     * reference to it in the connection's session.
     *
     * THE TURTLE FILE MUST BE THE LAST FILE CREATED WHEN INITIALIZING THE DATABASE HOME, IT'S WHAT
     * WE USE TO DECIDE IF WE'RE CREATING OR NOT.
     */
    WT_ERR(__wt_turtle_init(session));
    WT_ERR(__wt_config_gets(session, cfg, "verify_metadata", &cval));
    verify_meta = cval.val;

    /* Only verify the metadata file first. We will verify the history store table later. */
    if (verify_meta) {
        wt_session = &session->iface;
        ret = wt_session->verify(wt_session, WT_METAFILE_URI, NULL);
        WT_ERR(ret);
    }

    /*
     * If the user wants to salvage, do so before opening the metadata cursor. We do this after the
     * call to wt_turtle_init because that moves metadata files around from backups and would
     * overwrite any salvage we did if done before that call.
     */
    if (F_ISSET(conn, WT_CONN_SALVAGE)) {
        wt_session = &session->iface;
        WT_ERR(__wt_copy_and_sync(wt_session, WT_METAFILE, WT_METAFILE_SLVG));
        WT_ERR(wt_session->salvage(wt_session, WT_METAFILE_URI, NULL));
    }

    /* Initialize the connection's base write generation. */
    WT_ERR(__wt_metadata_init_base_write_gen(session));

    WT_ERR(__wt_metadata_cursor(session, NULL));
    /*
     * Load any incremental backup information. This reads the metadata so must be done after the
     * turtle file is initialized.
     */
    WT_ERR(__wt_backup_open(session));

    /* Start the worker threads and run recovery. */
    WT_ERR(__wt_connection_workers(session, cfg));

#if 0
    /*
     * If the user wants to verify WiredTiger metadata, verify the history store now that the
     * metadata table may have been salvaged and eviction has been started and recovery run.
     *
     * FIXME-WT-6682: temporarily disable history store verification.
     */
    if (verify_meta) {
        WT_ERR(__wt_open_internal_session(conn, "verify hs", false, 0, 0, &verify_session));
        ret = __wt_hs_verify(verify_session);
        WT_TRET(__wt_session_close_internal(verify_session));
        WT_ERR(ret);
    }
#endif

    /*
     * The hash array sizes needed to be set up very early. Set them in the statistics here. Setting
     * them in the early configuration function makes them get zeroed out.
     */
    WT_STAT_CONN_SET(session, buckets, conn->hash_size);
    WT_STAT_CONN_SET(session, buckets_dh, conn->dh_hash_size);

    /*
     * The default session should not open data handles after this point: since it can be shared
     * between threads, relying on session->dhandle is not safe.
     */
    F_SET(session, WT_SESSION_NO_DATA_HANDLES);

    WT_STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
    *connectionp = &conn->iface;

err:
    /* Discard the scratch buffers. */
    __wt_scr_free(session, &encbuf);
    __wt_scr_free(session, &i1);
    __wt_scr_free(session, &i2);
    __wt_scr_free(session, &i3);

    __wt_free(session, merge_cfg);
    /*
     * We may have allocated scratch memory when using the dummy session or the subsequently created
     * real session, and we don't want to tie down memory for the rest of the run in either of them.
     */
    if (session != &conn->dummy_session)
        __wt_scr_discard(session);
    __wt_scr_discard(&conn->dummy_session);

    if (ret != 0) {
        /*
         * Set panic if we're returning the run recovery error or if recovery did not complete so
         * that we don't try to checkpoint data handles. We need an explicit flag instead of
         * checking that WT_CONN_LOG_RECOVER_DONE is not set because other errors earlier than
         * recovery will not have that flag set.
         */
        if (ret == WT_RUN_RECOVERY || FLD_ISSET(conn->log_flags, WT_CONN_LOG_RECOVER_FAILED))
            F_SET(conn, WT_CONN_PANIC);
        /*
         * If we detected a data corruption issue, we really want to indicate the corruption instead
         * of whatever error was set. We cannot use standard return macros because we don't want to
         * generalize this. Record it here while we have the connection and set it after we destroy
         * the connection.
         */
        if (F_ISSET(conn, WT_CONN_DATA_CORRUPTION) && (ret == WT_PANIC || ret == WT_ERROR))
            try_salvage = true;
        WT_TRET(__wt_connection_close(conn));
        /*
         * Depending on the error, shutting down the connection may again return WT_PANIC. So if we
         * detected the corruption above, set it here after closing.
         */
        if (try_salvage)
            ret = WT_TRY_SALVAGE;
    }

    return (ret);
}
