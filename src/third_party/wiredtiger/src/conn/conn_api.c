/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __conn_statistics_config(WT_SESSION_IMPL *, const char *[]);

/*
 * ext_collate --
 *	Call the collation function (external API version).
 */
static int
ext_collate(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    WT_COLLATOR *collator, WT_ITEM *first, WT_ITEM *second, int *cmpp)
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
 *	Given a configuration, configure the collator (external API version).
 */
static int
ext_collator_config(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
    const char *uri, WT_CONFIG_ARG *cfg_arg, WT_COLLATOR **collatorp, int *ownp)
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
	WT_RET_NOTFOUND_OK(
	    __wt_config_gets_none(session, cfg, "collator", &cval));
	if (cval.len == 0)
		return (0);

	WT_CLEAR(metadata);
	WT_RET_NOTFOUND_OK(
	    __wt_config_gets(session, cfg, "app_metadata", &metadata));
	return (__wt_collator_config(
	    session, uri, &cval, &metadata, collatorp, ownp));
}

/*
 * __collator_confchk --
 *	Check for a valid custom collator.
 */
static int
__collator_confchk(
    WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cname, WT_COLLATOR **collatorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COLLATOR *ncoll;

	*collatorp = NULL;

	if (cname->len == 0 || WT_STRING_MATCH("none", cname->str, cname->len))
		return (0);

	conn = S2C(session);
	TAILQ_FOREACH(ncoll, &conn->collqh, q)
		if (WT_STRING_MATCH(ncoll->name, cname->str, cname->len)) {
			*collatorp = ncoll->collator;
			return (0);
		}
	WT_RET_MSG(session, EINVAL,
	    "unknown collator '%.*s'", (int)cname->len, cname->str);
}

/*
 * __wt_collator_config --
 *	Configure a custom collator.
 */
int
__wt_collator_config(WT_SESSION_IMPL *session, const char *uri,
    WT_CONFIG_ITEM *cname, WT_CONFIG_ITEM *metadata,
    WT_COLLATOR **collatorp, int *ownp)
{
	WT_COLLATOR *collator;

	*collatorp = NULL;
	*ownp = 0;

	WT_RET(__collator_confchk(session, cname, &collator));
	if (collator == NULL)
		return (0);

	if (collator->customize != NULL)
		WT_RET(collator->customize(collator,
		    &session->iface, uri, metadata, collatorp));

	if (*collatorp == NULL)
		*collatorp = collator;
	else
		*ownp = 1;

	return (0);
}

/*
 * __conn_add_collator --
 *	WT_CONNECTION->add_collator method.
 */
static int
__conn_add_collator(WT_CONNECTION *wt_conn,
    const char *name, WT_COLLATOR *collator, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_COLLATOR *ncoll;
	WT_SESSION_IMPL *session;

	ncoll = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);

	if (WT_STREQ(name, "none"))
		WT_ERR_MSG(session, EINVAL,
		    "invalid name for a collator: %s", name);

	WT_ERR(__wt_calloc_one(session, &ncoll));
	WT_ERR(__wt_strdup(session, name, &ncoll->name));
	ncoll->collator = collator;

	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->collqh, ncoll, q);
	ncoll = NULL;
	__wt_spin_unlock(session, &conn->api_lock);

err:	if (ncoll != NULL) {
		__wt_free(session, ncoll->name);
		__wt_free(session, ncoll);
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_collator --
 *	Remove collator added by WT_CONNECTION->add_collator, only used
 * internally.
 */
int
__wt_conn_remove_collator(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_COLLATOR *ncoll;

	conn = S2C(session);

	while ((ncoll = TAILQ_FIRST(&conn->collqh)) != NULL) {
		/* Call any termination method. */
		if (ncoll->collator->terminate != NULL)
			WT_TRET(ncoll->collator->terminate(
			    ncoll->collator, (WT_SESSION *)session));

		/* Remove from the connection's list, free memory. */
		TAILQ_REMOVE(&conn->collqh, ncoll, q);
		__wt_free(session, ncoll->name);
		__wt_free(session, ncoll);
	}

	return (ret);
}

/*
 * __compressor_confchk --
 *	Validate the compressor.
 */
static int
__compressor_confchk(
    WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_COMPRESSOR **compressorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COMPRESSOR *ncomp;

	*compressorp = NULL;

	if (cval->len == 0 || WT_STRING_MATCH("none", cval->str, cval->len))
		return (0);

	conn = S2C(session);
	TAILQ_FOREACH(ncomp, &conn->compqh, q)
		if (WT_STRING_MATCH(ncomp->name, cval->str, cval->len)) {
			*compressorp = ncomp->compressor;
			return (0);
		}
	WT_RET_MSG(session, EINVAL,
	    "unknown compressor '%.*s'", (int)cval->len, cval->str);
}

/*
 * __wt_compressor_config --
 *	Given a configuration, configure the compressor.
 */
int
__wt_compressor_config(
    WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_COMPRESSOR **compressorp)
{
	return (__compressor_confchk(session, cval, compressorp));
}

/*
 * __conn_add_compressor --
 *	WT_CONNECTION->add_compressor method.
 */
static int
__conn_add_compressor(WT_CONNECTION *wt_conn,
    const char *name, WT_COMPRESSOR *compressor, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_COMPRESSOR *ncomp;
	WT_SESSION_IMPL *session;

	WT_UNUSED(name);
	WT_UNUSED(compressor);
	ncomp = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_compressor, config, cfg);
	WT_UNUSED(cfg);

	if (WT_STREQ(name, "none"))
		WT_ERR_MSG(session, EINVAL,
		    "invalid name for a compressor: %s", name);

	WT_ERR(__wt_calloc_one(session, &ncomp));
	WT_ERR(__wt_strdup(session, name, &ncomp->name));
	ncomp->compressor = compressor;

	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->compqh, ncomp, q);
	ncomp = NULL;
	__wt_spin_unlock(session, &conn->api_lock);

err:	if (ncomp != NULL) {
		__wt_free(session, ncomp->name);
		__wt_free(session, ncomp);
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_compressor --
 *	remove compressor added by WT_CONNECTION->add_compressor, only used
 * internally.
 */
int
__wt_conn_remove_compressor(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_COMPRESSOR *ncomp;

	conn = S2C(session);

	while ((ncomp = TAILQ_FIRST(&conn->compqh)) != NULL) {
		/* Call any termination method. */
		if (ncomp->compressor->terminate != NULL)
			WT_TRET(ncomp->compressor->terminate(
			    ncomp->compressor, (WT_SESSION *)session));

		/* Remove from the connection's list, free memory. */
		TAILQ_REMOVE(&conn->compqh, ncomp, q);
		__wt_free(session, ncomp->name);
		__wt_free(session, ncomp);
	}

	return (ret);
}

/*
 * __conn_add_data_source --
 *	WT_CONNECTION->add_data_source method.
 */
static int
__conn_add_data_source(WT_CONNECTION *wt_conn,
    const char *prefix, WT_DATA_SOURCE *dsrc, const char *config)
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

err:	if (ndsrc != NULL) {
		__wt_free(session, ndsrc->prefix);
		__wt_free(session, ndsrc);
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_data_source --
 *	Remove data source added by WT_CONNECTION->add_data_source.
 */
int
__wt_conn_remove_data_source(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_DATA_SOURCE *ndsrc;

	conn = S2C(session);

	while ((ndsrc = TAILQ_FIRST(&conn->dsrcqh)) != NULL) {
		/* Call any termination method. */
		if (ndsrc->dsrc->terminate != NULL)
			WT_TRET(ndsrc->dsrc->terminate(
			    ndsrc->dsrc, (WT_SESSION *)session));

		/* Remove from the connection's list, free memory. */
		TAILQ_REMOVE(&conn->dsrcqh, ndsrc, q);
		__wt_free(session, ndsrc->prefix);
		__wt_free(session, ndsrc);
	}

	return (ret);
}

/*
 * __encryptor_confchk --
 *	Validate the encryptor.
 */
static int
__encryptor_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval,
    WT_NAMED_ENCRYPTOR **nencryptorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_ENCRYPTOR *nenc;

	if (nencryptorp != NULL)
		*nencryptorp = NULL;

	if (cval->len == 0 || WT_STRING_MATCH("none", cval->str, cval->len))
		return (0);

	conn = S2C(session);
	TAILQ_FOREACH(nenc, &conn->encryptqh, q)
		if (WT_STRING_MATCH(nenc->name, cval->str, cval->len)) {
			if (nencryptorp != NULL)
				*nencryptorp = nenc;
			return (0);
		}

	WT_RET_MSG(session, EINVAL,
	    "unknown encryptor '%.*s'", (int)cval->len, cval->str);
}

/*
 * __wt_encryptor_config --
 *	Given a configuration, configure the encryptor.
 */
int
__wt_encryptor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval,
    WT_CONFIG_ITEM *keyid, WT_CONFIG_ARG *cfg_arg,
    WT_KEYED_ENCRYPTOR **kencryptorp)
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
			WT_ERR_MSG(session, EINVAL, "encryption.keyid "
			    "requires encryption.name to be set");
		goto out;
	}

	/*
	 * Check if encryption is set on the connection.  If
	 * someone wants encryption on a table, it needs to be
	 * configured on the database as well.
	 */
	if (conn->kencryptor == NULL && kencryptorp != &conn->kencryptor)
		WT_ERR_MSG(session, EINVAL, "table encryption "
		    "requires connection encryption to be set");
	hash = __wt_hash_city64(keyid->str, keyid->len);
	bucket = hash % WT_HASH_ARRAY_SIZE;
	TAILQ_FOREACH(kenc, &nenc->keyedhashqh[bucket], q)
		if (WT_STRING_MATCH(kenc->keyid, keyid->str, keyid->len))
			goto out;

	WT_ERR(__wt_calloc_one(session, &kenc));
	WT_ERR(__wt_strndup(session, keyid->str, keyid->len, &kenc->keyid));
	encryptor = nenc->encryptor;
	if (encryptor->customize != NULL) {
		custom = NULL;
		WT_ERR(encryptor->customize(encryptor, &session->iface,
		    cfg_arg, &custom));
		if (custom != NULL) {
			kenc->owned = 1;
			encryptor = custom;
		}
	}
	WT_ERR(encryptor->sizing(encryptor, &session->iface,
	    &kenc->size_const));
	kenc->encryptor = encryptor;
	TAILQ_INSERT_HEAD(&nenc->keyedqh, kenc, q);
	TAILQ_INSERT_HEAD(&nenc->keyedhashqh[bucket], kenc, hashq);

out:	__wt_spin_unlock(session, &conn->encryptor_lock);
	*kencryptorp = kenc;
	return (0);

err:	if (kenc != NULL) {
		__wt_free(session, kenc->keyid);
		__wt_free(session, kenc);
	}
	__wt_spin_unlock(session, &conn->encryptor_lock);
	return (ret);
}

/*
 * __conn_add_encryptor --
 *	WT_CONNECTION->add_encryptor method.
 */
static int
__conn_add_encryptor(WT_CONNECTION *wt_conn,
    const char *name, WT_ENCRYPTOR *encryptor, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_ENCRYPTOR *nenc;
	WT_SESSION_IMPL *session;
	int i;

	nenc = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_encryptor, config, cfg);
	WT_UNUSED(cfg);

	if (WT_STREQ(name, "none"))
		WT_ERR_MSG(session, EINVAL,
		    "invalid name for an encryptor: %s", name);

	if (encryptor->encrypt == NULL || encryptor->decrypt == NULL ||
	    encryptor->sizing == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "encryptor: %s: required callbacks not set", name);

	/*
	 * Verify that terminate is set if customize is set. We could relax this
	 * restriction and give an error if customize returns an encryptor and
	 * terminate is not set. That seems more prone to mistakes.
	 */
	if (encryptor->customize != NULL && encryptor->terminate == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "encryptor: %s: has customize but no terminate", name);

	WT_ERR(__wt_calloc_one(session, &nenc));
	WT_ERR(__wt_strdup(session, name, &nenc->name));
	nenc->encryptor = encryptor;
	TAILQ_INIT(&nenc->keyedqh);
	for (i = 0; i < WT_HASH_ARRAY_SIZE; i++)
		TAILQ_INIT(&nenc->keyedhashqh[i]);

	TAILQ_INSERT_TAIL(&conn->encryptqh, nenc, q);
	nenc = NULL;

err:	if (nenc != NULL) {
		__wt_free(session, nenc->name);
		__wt_free(session, nenc);
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_conn_remove_encryptor --
 *	remove encryptors added by WT_CONNECTION->add_encryptor, only used
 * internally.
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
		while ((kenc = TAILQ_FIRST(&nenc->keyedqh)) != NULL) {
			/* Call any termination method. */
			if (kenc->owned && kenc->encryptor->terminate != NULL)
				WT_TRET(kenc->encryptor->terminate(
				    kenc->encryptor, (WT_SESSION *)session));

			/* Remove from the connection's list, free memory. */
			TAILQ_REMOVE(&nenc->keyedqh, kenc, q);
			__wt_free(session, kenc->keyid);
			__wt_free(session, kenc);
		}

		/* Call any termination method. */
		if (nenc->encryptor->terminate != NULL)
			WT_TRET(nenc->encryptor->terminate(
			    nenc->encryptor, (WT_SESSION *)session));

		/* Remove from the connection's list, free memory. */
		TAILQ_REMOVE(&conn->encryptqh, nenc, q);
		__wt_free(session, nenc->name);
		__wt_free(session, nenc);
	}
	return (ret);
}

/*
 * __conn_add_extractor --
 *	WT_CONNECTION->add_extractor method.
 */
static int
__conn_add_extractor(WT_CONNECTION *wt_conn,
    const char *name, WT_EXTRACTOR *extractor, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_EXTRACTOR *nextractor;
	WT_SESSION_IMPL *session;

	nextractor = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_extractor, config, cfg);
	WT_UNUSED(cfg);

	if (WT_STREQ(name, "none"))
		WT_ERR_MSG(session, EINVAL,
		    "invalid name for an extractor: %s", name);

	WT_ERR(__wt_calloc_one(session, &nextractor));
	WT_ERR(__wt_strdup(session, name, &nextractor->name));
	nextractor->extractor = extractor;

	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->extractorqh, nextractor, q);
	nextractor = NULL;
	__wt_spin_unlock(session, &conn->api_lock);

err:	if (nextractor != NULL) {
		__wt_free(session, nextractor->name);
		__wt_free(session, nextractor);
	}

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __extractor_confchk --
 *	Check for a valid custom extractor.
 */
static int
__extractor_confchk(
    WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cname, WT_EXTRACTOR **extractorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_EXTRACTOR *nextractor;

	*extractorp = NULL;

	if (cname->len == 0 || WT_STRING_MATCH("none", cname->str, cname->len))
		return (0);

	conn = S2C(session);
	TAILQ_FOREACH(nextractor, &conn->extractorqh, q)
		if (WT_STRING_MATCH(nextractor->name, cname->str, cname->len)) {
			*extractorp = nextractor->extractor;
			return (0);
		}
	WT_RET_MSG(session, EINVAL,
	    "unknown extractor '%.*s'", (int)cname->len, cname->str);
}

/*
 * __wt_extractor_config --
 *	Given a configuration, configure the extractor.
 */
int
__wt_extractor_config(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_EXTRACTOR **extractorp, int *ownp)
{
	WT_CONFIG_ITEM cname;
	WT_EXTRACTOR *extractor;

	*extractorp = NULL;
	*ownp = 0;

	WT_RET_NOTFOUND_OK(
	    __wt_config_getones_none(session, config, "extractor", &cname));
	if (cname.len == 0)
		return (0);

	WT_RET(__extractor_confchk(session, &cname, &extractor));
	if (extractor == NULL)
		return (0);

	if (extractor->customize != NULL) {
		WT_RET(__wt_config_getones(session,
		    config, "app_metadata", &cname));
		WT_RET(extractor->customize(extractor, &session->iface,
		    uri, &cname, extractorp));
	}

	if (*extractorp == NULL)
		*extractorp = extractor;
	else
		*ownp = 1;

	return (0);
}

/*
 * __wt_conn_remove_extractor --
 *	Remove extractor added by WT_CONNECTION->add_extractor, only used
 * internally.
 */
int
__wt_conn_remove_extractor(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_NAMED_EXTRACTOR *nextractor;

	conn = S2C(session);

	while ((nextractor = TAILQ_FIRST(&conn->extractorqh)) != NULL) {
		/* Call any termination method. */
		if (nextractor->extractor->terminate != NULL)
			WT_TRET(nextractor->extractor->terminate(
			    nextractor->extractor, (WT_SESSION *)session));

		/* Remove from the connection's list, free memory. */
		TAILQ_REMOVE(&conn->extractorqh, nextractor, q);
		__wt_free(session, nextractor->name);
		__wt_free(session, nextractor);
	}

	return (ret);
}

/*
 * __conn_async_flush --
 *	WT_CONNECTION.async_flush method.
 */
static int
__conn_async_flush(WT_CONNECTION *wt_conn)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL_NOCONF(conn, session, async_flush);
	WT_ERR(__wt_async_flush(session));

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_async_new_op --
 *	WT_CONNECTION.async_new_op method.
 */
static int
__conn_async_new_op(WT_CONNECTION *wt_conn, const char *uri, const char *config,
    WT_ASYNC_CALLBACK *callback, WT_ASYNC_OP **asyncopp)
{
	WT_ASYNC_OP_IMPL *op;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, async_new_op, config, cfg);
	WT_ERR(__wt_async_new_op(session, uri, config, cfg, callback, &op));

	*asyncopp = &op->iface;

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_get_extension_api --
 *	WT_CONNECTION.get_extension_api method.
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
	conn->extension_api.config_parser_open = __wt_ext_config_parser_open;
	conn->extension_api.config_get = __wt_ext_config_get;
	conn->extension_api.metadata_insert = __wt_ext_metadata_insert;
	conn->extension_api.metadata_remove = __wt_ext_metadata_remove;
	conn->extension_api.metadata_search = __wt_ext_metadata_search;
	conn->extension_api.metadata_update = __wt_ext_metadata_update;
	conn->extension_api.struct_pack = __wt_ext_struct_pack;
	conn->extension_api.struct_size = __wt_ext_struct_size;
	conn->extension_api.struct_unpack = __wt_ext_struct_unpack;
	conn->extension_api.transaction_id = __wt_ext_transaction_id;
	conn->extension_api.transaction_isolation_level =
	    __wt_ext_transaction_isolation_level;
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

#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
	extern int snappy_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZLIB
	extern int zlib_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif
#ifdef HAVE_BUILTIN_EXTENSION_LZ4
	extern int lz4_extension_init(WT_CONNECTION *, WT_CONFIG_ARG *);
#endif

/*
 * __conn_load_default_extensions --
 *	Load extensions that are enabled via --with-builtins
 */
static int
__conn_load_default_extensions(WT_CONNECTION_IMPL *conn)
{
	WT_UNUSED(conn);

#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
	WT_RET(snappy_extension_init(&conn->iface, NULL));
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZLIB
	WT_RET(zlib_extension_init(&conn->iface, NULL));
#endif
#ifdef HAVE_BUILTIN_EXTENSION_LZ4
	WT_RET(lz4_extension_init(&conn->iface, NULL));
#endif
	return (0);
}

/*
 * __conn_load_extension_int --
 *	Internal extension load interface
 */
static int
__conn_load_extension_int(WT_SESSION_IMPL *session,
    const char *path, const char *cfg[], bool early_load)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_DLH *dlh;
	int (*load)(WT_CONNECTION *, WT_CONFIG_ARG *);
	bool is_local;
	const char *init_name, *terminate_name;

	dlh = NULL;
	init_name = terminate_name = NULL;
	is_local = strcmp(path, "local") == 0;

	/* Ensure that the load matches the phase of startup we are in. */
	WT_ERR(__wt_config_gets(session, cfg, "early_load", &cval));
	if ((cval.val == 0 && early_load) || (cval.val != 0 && !early_load))
		return (0);

	/*
	 * This assumes the underlying shared libraries are reference counted,
	 * that is, that re-opening a shared library simply increments a ref
	 * count, and closing it simply decrements the ref count, and the last
	 * close discards the reference entirely -- in other words, we do not
	 * check to see if we've already opened this shared library.
	 */
	WT_ERR(__wt_dlopen(session, is_local ? NULL : path, &dlh));

	/*
	 * Find the load function, remember the unload function for when we
	 * close.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "entry", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &init_name));
	WT_ERR(__wt_dlsym(session, dlh, init_name, true, &load));

	WT_ERR(__wt_config_gets(session, cfg, "terminate", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &terminate_name));
	WT_ERR(
	    __wt_dlsym(session, dlh, terminate_name, false, &dlh->terminate));

	/* Call the load function last, it simplifies error handling. */
	WT_ERR(load(&S2C(session)->iface, (WT_CONFIG_ARG *)cfg));

	/* Link onto the environment's list of open libraries. */
	__wt_spin_lock(session, &S2C(session)->api_lock);
	TAILQ_INSERT_TAIL(&S2C(session)->dlhqh, dlh, q);
	__wt_spin_unlock(session, &S2C(session)->api_lock);
	dlh = NULL;

err:	if (dlh != NULL)
		WT_TRET(__wt_dlclose(session, dlh));
	__wt_free(session, init_name);
	__wt_free(session, terminate_name);
	return (ret);
}

/*
 * __conn_load_extension --
 *	WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(
    WT_CONNECTION *wt_conn, const char *path, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, load_extension, config, cfg);

	ret = __conn_load_extension_int(session, path, cfg, false);

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_load_extensions --
 *	Load the list of application-configured extensions.
 */
static int
__conn_load_extensions(
    WT_SESSION_IMPL *session, const char *cfg[], bool early_load)
{
	WT_CONFIG subconfig;
	WT_CONFIG_ITEM cval, skey, sval;
	WT_DECL_ITEM(exconfig);
	WT_DECL_ITEM(expath);
	WT_DECL_RET;
	const char *sub_cfg[] = {
	    WT_CONFIG_BASE(session, WT_CONNECTION_load_extension), NULL, NULL };

	WT_ERR(__wt_config_gets(session, cfg, "extensions", &cval));
	WT_ERR(__wt_config_subinit(session, &subconfig, &cval));
	while ((ret = __wt_config_next(&subconfig, &skey, &sval)) == 0) {
		if (expath == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &expath));
		WT_ERR(__wt_buf_fmt(
		    session, expath, "%.*s", (int)skey.len, skey.str));
		if (sval.len > 0) {
			if (exconfig == NULL)
				WT_ERR(__wt_scr_alloc(session, 0, &exconfig));
			WT_ERR(__wt_buf_fmt(session,
			    exconfig, "%.*s", (int)sval.len, sval.str));
		}
		sub_cfg[1] = sval.len > 0 ? exconfig->data : NULL;
		WT_ERR(__conn_load_extension_int(
		    session, expath->data, sub_cfg, early_load));
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	__wt_scr_free(session, &expath);
	__wt_scr_free(session, &exconfig);

	return (ret);
}

/*
 * __conn_get_home --
 *	WT_CONNECTION.get_home method.
 */
static const char *
__conn_get_home(WT_CONNECTION *wt_conn)
{
	return (((WT_CONNECTION_IMPL *)wt_conn)->home);
}

/*
 * __conn_configure_method --
 *	WT_CONNECTION.configure_method method.
 */
static int
__conn_configure_method(WT_CONNECTION *wt_conn, const char *method,
    const char *uri, const char *config, const char *type, const char *check)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL_NOCONF(conn, session, configure_method);

	ret = __wt_configure_method(session, method, uri, config, type, check);

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_is_new --
 *	WT_CONNECTION->is_new method.
 */
static int
__conn_is_new(WT_CONNECTION *wt_conn)
{
	return (((WT_CONNECTION_IMPL *)wt_conn)->is_new);
}

/*
 * __conn_close --
 *	WT_CONNECTION->close method.
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

	WT_TRET(__wt_config_gets(session, cfg, "leak_memory", &cval));
	if (cval.val != 0)
		F_SET(conn, WT_CONN_LEAK_MEMORY);

err:	/*
	 * Rollback all running transactions.
	 * We do this as a separate pass because an active transaction in one
	 * session could cause trouble when closing a file, even if that
	 * session never referenced that file.
	 */
	for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i)
		if (s->active && !F_ISSET(s, WT_SESSION_INTERNAL) &&
		    F_ISSET(&s->txn, WT_TXN_RUNNING)) {
			wt_session = &s->iface;
			WT_TRET(wt_session->rollback_transaction(
			    wt_session, NULL));
		}

	/* Release all named snapshots. */
	WT_TRET(__wt_txn_named_snapshot_destroy(session));

	/* Close open, external sessions. */
	for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i)
		if (s->active && !F_ISSET(s, WT_SESSION_INTERNAL)) {
			wt_session = &s->iface;
			/*
			 * Notify the user that we are closing the session
			 * handle via the registered close callback.
			 */
			if (s->event_handler->handle_close != NULL)
				WT_TRET(s->event_handler->handle_close(
				    s->event_handler, wt_session, NULL));
			WT_TRET(wt_session->close(wt_session, config));
		}

	WT_TRET(__wt_connection_close(conn));

	/* We no longer have a session, don't try to update it. */
	session = NULL;

	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_reconfigure --
 *	WT_CONNECTION->reconfigure method.
 */
static int
__conn_reconfigure(WT_CONNECTION *wt_conn, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	const char *p;

	conn = (WT_CONNECTION_IMPL *)wt_conn;

	CONNECTION_API_CALL(conn, session, reconfigure, config, cfg);

	/* Serialize reconfiguration. */
	__wt_spin_lock(session, &conn->reconfig_lock);

	/*
	 * The configuration argument has been checked for validity, update the
	 * previous connection configuration.
	 *
	 * DO NOT merge the configuration before the reconfigure calls.  Some
	 * of the underlying reconfiguration functions do explicit checks with
	 * the second element of the configuration array, knowing the defaults
	 * are in slot #1 and the application's modifications are in slot #2.
	 *
	 * First, replace the base configuration set up by CONNECTION_API_CALL
	 * with the current connection configuration, otherwise reconfiguration
	 * functions will find the base value instead of previously configured
	 * value.
	 */
	cfg[0] = conn->cfg;
	cfg[1] = config;

	/* Second, reconfigure the system. */
	WT_ERR(__conn_statistics_config(session, cfg));
	WT_ERR(__wt_async_reconfig(session, cfg));
	WT_ERR(__wt_cache_config(session, true, cfg));
	WT_ERR(__wt_checkpoint_server_create(session, cfg));
	WT_ERR(__wt_logmgr_reconfig(session, cfg));
	WT_ERR(__wt_lsm_manager_reconfig(session, cfg));
	WT_ERR(__wt_statlog_create(session, cfg));
	WT_ERR(__wt_sweep_config(session, cfg));
	WT_ERR(__wt_verbose_config(session, cfg));

	/* Third, merge everything together, creating a new connection state. */
	WT_ERR(__wt_config_merge(session, cfg, NULL, &p));
	__wt_free(session, conn->cfg);
	conn->cfg = p;

err:	__wt_spin_unlock(session, &conn->reconfig_lock);

	API_END_RET(session, ret);
}

/*
 * __conn_open_session --
 *	WT_CONNECTION->open_session method.
 */
static int
__conn_open_session(WT_CONNECTION *wt_conn,
    WT_EVENT_HANDLER *event_handler, const char *config,
    WT_SESSION **wt_sessionp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session, *session_ret;

	*wt_sessionp = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session_ret = NULL;

	CONNECTION_API_CALL(conn, session, open_session, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_open_session(
	    conn, event_handler, config, true, &session_ret));
	*wt_sessionp = &session_ret->iface;

err:	API_END_RET_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_config_append --
 *	Append an entry to a config stack.
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
 *	Append an entry to a config stack that overrides some settings
 *	when read-only is configured.
 */
static void
__conn_config_readonly(const char *cfg[])
{
	const char *readonly;

	/*
	 * Override certain settings.  In general we override the options
	 * whose default conflicts.  Other settings at odds will return
	 * an error and will be checked when those settings are processed.
	 */
	readonly="checkpoint=(wait=0),"
	    "config_base=false,"
	    "create=false,"
	    "log=(archive=false,prealloc=false),"
	    "lsm_manager=(merge=false),";
	__conn_config_append(cfg, readonly);
}

/*
 * __conn_config_check_version --
 *	Check if a configuration version isn't compatible.
 */
static int
__conn_config_check_version(WT_SESSION_IMPL *session, const char *config)
{
	WT_CONFIG_ITEM vmajor, vminor;

	/*
	 * Version numbers aren't included in all configuration strings, but
	 * we check all of them just in case. Ignore configurations without
	 * a version.
	 */
	 if (__wt_config_getones(
	     session, config, "version.major", &vmajor) == WT_NOTFOUND)
		return (0);
	 WT_RET(__wt_config_getones(session, config, "version.minor", &vminor));

	 if (vmajor.val > WIREDTIGER_VERSION_MAJOR ||
	     (vmajor.val == WIREDTIGER_VERSION_MAJOR &&
	     vminor.val > WIREDTIGER_VERSION_MINOR))
		WT_RET_MSG(session, ENOTSUP,
		    "WiredTiger configuration is from an incompatible release "
		    "of the WiredTiger engine");

	return (0);
}

/*
 * __conn_config_file --
 *	Read WiredTiger config files from the home directory.
 */
static int
__conn_config_file(WT_SESSION_IMPL *session,
    const char *filename, bool is_user, const char **cfg, WT_ITEM *cbuf)
{
	WT_DECL_RET;
	WT_FH *fh;
	size_t len;
	wt_off_t size;
	bool exist, quoted;
	char *p, *t;

	fh = NULL;

	/* Configuration files are always optional. */
	WT_RET(__wt_fs_exist(session, filename, &exist));
	if (!exist)
		return (0);

	/* Open the configuration file. */
	WT_RET(__wt_open(session, filename, WT_OPEN_FILE_TYPE_REGULAR, 0, &fh));
	WT_ERR(__wt_filesize(session, fh, &size));
	if (size == 0)
		goto err;

	/*
	 * Sanity test: a 100KB configuration file would be insane.  (There's
	 * no practical reason to limit the file size, but I can either limit
	 * the file size to something rational, or add code to test if the
	 * wt_off_t size is larger than a uint32_t, which is more complicated
	 * and a waste of time.)
	 */
	if (size > 100 * 1024)
		WT_ERR_MSG(
		    session, EFBIG, "Configuration file too big: %s", filename);
	len = (size_t)size;

	/*
	 * Copy the configuration file into memory, with a little slop, I'm not
	 * interested in debugging off-by-ones.
	 *
	 * The beginning of a file is the same as if we run into an unquoted
	 * newline character, simplify the parsing loop by pretending that's
	 * what we're doing.
	 */
	WT_ERR(__wt_buf_init(session, cbuf, len + 10));
	WT_ERR(__wt_read(
	    session, fh, (wt_off_t)0, len, ((uint8_t *)cbuf->mem) + 1));
	((uint8_t *)cbuf->mem)[0] = '\n';
	cbuf->size = len + 1;

	/*
	 * Collapse the file's lines into a single string: newline characters
	 * are replaced with commas unless the newline is quoted or backslash
	 * escaped.  Comment lines (an unescaped newline where the next non-
	 * white-space character is a hash), are discarded.
	 */
	for (quoted = false, p = t = cbuf->mem; len > 0;) {
		/*
		 * Backslash pairs pass through untouched, unless immediately
		 * preceding a newline, in which case both the backslash and
		 * the newline are discarded.  Backslash characters escape
		 * quoted characters, too, that is, a backslash followed by a
		 * quote doesn't start or end a quoted string.
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
		 * If we're in a quoted string, or starting a quoted string,
		 * take all characters, including white-space and newlines.
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
		 * Replace any newline characters with commas (and strings of
		 * commas are safe).
		 *
		 * After any newline, skip to a non-white-space character; if
		 * the next character is a hash mark, skip to the next newline.
		 */
		for (;;) {
			for (*t++ = ',';
			    --len > 0 && __wt_isspace((u_char)*++p);)
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

	/* Upgrade the configuration string. */
	WT_ERR(__wt_config_upgrade(session, cbuf));

	/* Check the configuration information. */
	WT_ERR(__wt_config_check(session, is_user ?
	    WT_CONFIG_REF(session, wiredtiger_open_usercfg) :
	    WT_CONFIG_REF(session, wiredtiger_open_basecfg), cbuf->data, 0));

	/* Append it to the stack. */
	__conn_config_append(cfg, cbuf->data);

err:	WT_TRET(__wt_close(session, &fh));
	return (ret);
}

/*
 * __conn_config_env --
 *	Read configuration from an environment variable, if set.
 */
static int
__conn_config_env(WT_SESSION_IMPL *session, const char *cfg[], WT_ITEM *cbuf)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *env_config;
	size_t len;

	/* Only use the environment variable if configured. */
	WT_RET(__wt_config_gets(session, cfg, "use_environment", &cval));
	if (cval.val == 0)
		return (0);

	ret = __wt_getenv(session, "WIREDTIGER_CONFIG", &env_config);
	if (ret == WT_NOTFOUND)
		return (0);
	WT_ERR(ret);

	len = strlen(env_config);
	if (len == 0)
		goto err;			/* Free the memory. */
	WT_ERR(__wt_buf_set(session, cbuf, env_config, len + 1));

	/*
	 * Security stuff:
	 *
	 * If the "use_environment_priv" configuration string is set, use the
	 * environment variable if the process has appropriate privileges.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "use_environment_priv", &cval));
	if (cval.val == 0 && __wt_has_priv())
		WT_ERR_MSG(session, WT_ERROR, "%s",
		    "WIREDTIGER_CONFIG environment variable set but process "
		    "lacks privileges to use that environment variable");

	/* Check any version. */
	WT_ERR(__conn_config_check_version(session, env_config));

	/* Upgrade the configuration string. */
	WT_ERR(__wt_config_upgrade(session, cbuf));

	/* Check the configuration information. */
	WT_ERR(__wt_config_check(session,
	    WT_CONFIG_REF(session, wiredtiger_open), env_config, 0));

	/* Append it to the stack. */
	__conn_config_append(cfg, cbuf->data);

err:	__wt_free(session, env_config);

      return (ret);
}

/*
 * __conn_home --
 *	Set the database home directory.
 */
static int
__conn_home(WT_SESSION_IMPL *session, const char *home, const char *cfg[])
{
	WT_CONFIG_ITEM cval;

	/* If the application specifies a home directory, use it. */
	if (home != NULL)
		goto copy;

	/* Only use the environment variable if configured. */
	WT_RET(__wt_config_gets(session, cfg, "use_environment", &cval));
	if (cval.val != 0 &&
	    __wt_getenv(session, "WIREDTIGER_HOME", &S2C(session)->home) == 0)
		return (0);

	/* If there's no WIREDTIGER_HOME environment variable, use ".". */
	home = ".";

	/*
	 * Security stuff:
	 *
	 * Unless the "use_environment_priv" configuration string is set,
	 * fail if the process is running with special privileges.
	 */
	WT_RET(__wt_config_gets(session, cfg, "use_environment_priv", &cval));
	if (cval.val == 0 && __wt_has_priv())
		WT_RET_MSG(session, WT_ERROR, "%s",
		    "WIREDTIGER_HOME environment variable set but process "
		    "lacks privileges to use that environment variable");

copy:	return (__wt_strdup(session, home, &S2C(session)->home));
}

/*
 * __conn_single --
 *	Confirm that no other thread of control is using this database.
 */
static int
__conn_single(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn, *t;
	WT_DECL_RET;
	WT_FH *fh;
	size_t len;
	wt_off_t size;
	bool bytelock, exist, is_create;
	char buf[256];

	conn = S2C(session);
	fh = NULL;

	WT_RET(__wt_config_gets(session, cfg, "create", &cval));
	is_create = cval.val != 0;

	if (F_ISSET(conn, WT_CONN_READONLY))
		is_create = false;

	bytelock = true;
	__wt_spin_lock(session, &__wt_process.spinlock);

	/*
	 * We first check for other threads of control holding a lock on this
	 * database, because the byte-level locking functions are based on the
	 * POSIX 1003.1 fcntl APIs, which require all locks associated with a
	 * file for a given process are removed when any file descriptor for
	 * the file is closed by that process. In other words, we can't open a
	 * file handle on the lock file until we are certain that closing that
	 * handle won't discard the owning thread's lock. Applications hopefully
	 * won't open a database in multiple threads, but we don't want to have
	 * it fail the first time, but succeed the second.
	 */
	TAILQ_FOREACH(t, &__wt_process.connqh, q)
		if (t->home != NULL &&
		    t != conn && strcmp(t->home, conn->home) == 0) {
			ret = EBUSY;
			break;
		}
	if (ret != 0)
		WT_ERR_MSG(session, EBUSY,
		    "WiredTiger database is already being managed by another "
		    "thread in this process");

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
	ret = __wt_open(session, WT_SINGLETHREAD, WT_OPEN_FILE_TYPE_REGULAR,
	    is_create || exist ? WT_OPEN_CREATE : 0, &conn->lock_fh);

	/*
	 * If this is a read-only connection and we cannot grab the lock
	 * file, check if it is because there is not write permission or
	 * if the file does not exist.  If so, then ignore the error.
	 * XXX Ignoring the error does allow multiple read-only
	 * connections to exist at the same time on a read-only directory.
	 *
	 * If we got an expected permission or non-existence error then skip
	 * the byte lock.
	 */
	if (F_ISSET(conn, WT_CONN_READONLY) &&
	    (ret == EACCES || ret == ENOENT)) {
		bytelock = false;
		ret = 0;
	}
	WT_ERR(ret);
	if (bytelock) {
		/*
		 * Lock a byte of the file: if we don't get the lock, some other
		 * process is holding it, we're done.  The file may be
		 * zero-length, and that's OK, the underlying call supports
		 * locking past the end-of-file.
		 */
		if (__wt_file_lock(session, conn->lock_fh, true) != 0)
			WT_ERR_MSG(session, EBUSY,
			    "WiredTiger database is already being managed by "
			    "another process");

		/*
		 * If the size of the lock file is non-zero, we created it (or
		 * won a locking race with the thread that created it, it
		 * doesn't matter).
		 *
		 * Write something into the file, zero-length files make me
		 * nervous.
		 *
		 * The test against the expected length is sheer paranoia (the
		 * length should be 0 or correct), but it shouldn't hurt.
		 */
#define	WT_SINGLETHREAD_STRING	"WiredTiger lock file\n"
		WT_ERR(__wt_filesize(session, conn->lock_fh, &size));
		if (size != strlen(WT_SINGLETHREAD_STRING))
			WT_ERR(__wt_write(session, conn->lock_fh, (wt_off_t)0,
			    strlen(WT_SINGLETHREAD_STRING),
			    WT_SINGLETHREAD_STRING));

	}

	/* We own the lock file, optionally create the WiredTiger file. */
	ret = __wt_open(session, WT_WIREDTIGER,
	    WT_OPEN_FILE_TYPE_REGULAR, is_create ? WT_OPEN_CREATE : 0, &fh);

	/*
	 * If we're read-only, check for handled errors. Even if able to open
	 * the WiredTiger file successfully, we do not try to lock it.  The
	 * lock file test above is the only one we do for read-only.
	 */
	if (F_ISSET(conn, WT_CONN_READONLY)) {
		if (ret == EACCES || ret == ENOENT)
			ret = 0;
		WT_ERR(ret);
	} else {
		WT_ERR(ret);
		/*
		 * Lock the WiredTiger file (for backward compatibility reasons
		 * as described above).  Immediately release the lock, it's
		 * just a test.
		 */
		if (__wt_file_lock(session, fh, true) != 0) {
			WT_ERR_MSG(session, EBUSY,
			    "WiredTiger database is already being managed by "
			    "another process");
		}
		WT_ERR(__wt_file_lock(session, fh, false));
	}

	/*
	 * We own the database home, figure out if we're creating it. There are
	 * a few files created when initializing the database home and we could
	 * crash in-between any of them, so there's no simple test. The last
	 * thing we do during initialization is rename a turtle file into place,
	 * and there's never a database home after that point without a turtle
	 * file. If the turtle file doesn't exist, it's a create.
	 */
	WT_ERR(__wt_fs_exist(session, WT_METADATA_TURTLE, &exist));
	conn->is_new = exist ? 0 : 1;

	if (conn->is_new) {
		if (F_ISSET(conn, WT_CONN_READONLY))
			WT_ERR_MSG(session, EINVAL,
			    "Creating a new database is incompatible with "
			    "read-only configuration");
		len = (size_t)snprintf(buf, sizeof(buf),
		    "%s\n%s\n", WT_WIREDTIGER, WIREDTIGER_VERSION_STRING);
		WT_ERR(__wt_write(session, fh, (wt_off_t)0, len, buf));
		WT_ERR(__wt_fsync(session, fh, true));
	} else {
		/*
		 * Although exclusive and the read-only configuration settings
		 * are at odds, we do not have to check against read-only here
		 * because it falls out from earlier code in this function
		 * preventing creation and confirming the database
		 * already exists.
		 */
		WT_ERR(__wt_config_gets(session, cfg, "exclusive", &cval));
		if (cval.val != 0)
			WT_ERR_MSG(session, EEXIST,
			    "WiredTiger database already exists and exclusive "
			    "option configured");
	}

err:	/*
	 * We ignore the connection's lock file handle on error, it will be
	 * closed when the connection structure is destroyed.
	 */
	WT_TRET(__wt_close(session, &fh));

	__wt_spin_unlock(session, &__wt_process.spinlock);
	return (ret);
}

/*
 * __conn_statistics_config --
 *	Set statistics configuration.
 */
static int
__conn_statistics_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	uint32_t flags;
	int set;

	conn = S2C(session);

	WT_RET(__wt_config_gets(session, cfg, "statistics", &cval));

	flags = 0;
	set = 0;
	if ((ret = __wt_config_subgets(
	    session, &cval, "none", &sval)) == 0 && sval.val != 0) {
		LF_SET(WT_CONN_STAT_NONE);
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "fast", &sval)) == 0 && sval.val != 0) {
		LF_SET(WT_CONN_STAT_FAST);
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "all", &sval)) == 0 && sval.val != 0) {
		LF_SET(WT_CONN_STAT_ALL | WT_CONN_STAT_FAST);
		++set;
	}
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_subgets(
	    session, &cval, "clear", &sval)) == 0 && sval.val != 0)
		LF_SET(WT_CONN_STAT_CLEAR);
	WT_RET_NOTFOUND_OK(ret);

	if (set > 1)
		WT_RET_MSG(session, EINVAL,
		    "only one statistics configuration value may be specified");

	/* Configuring statistics clears any existing values. */
	conn->stat_flags = flags;

	return (0);
}

/* Simple structure for name and flag configuration searches. */
typedef struct {
	const char *name;
	uint32_t flag;
} WT_NAME_FLAG;

/*
 * __wt_verbose_config --
 *	Set verbose configuration.
 */
int
__wt_verbose_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	static const WT_NAME_FLAG verbtypes[] = {
		{ "api",		WT_VERB_API },
		{ "block",		WT_VERB_BLOCK },
		{ "checkpoint",		WT_VERB_CHECKPOINT },
		{ "compact",		WT_VERB_COMPACT },
		{ "evict",		WT_VERB_EVICT },
		{ "evictserver",	WT_VERB_EVICTSERVER },
		{ "fileops",		WT_VERB_FILEOPS },
		{ "handleops",		WT_VERB_HANDLEOPS },
		{ "log",		WT_VERB_LOG },
		{ "lsm",		WT_VERB_LSM },
		{ "lsm_manager",	WT_VERB_LSM_MANAGER },
		{ "metadata",		WT_VERB_METADATA },
		{ "mutex",		WT_VERB_MUTEX },
		{ "overflow",		WT_VERB_OVERFLOW },
		{ "read",		WT_VERB_READ },
		{ "rebalance",		WT_VERB_REBALANCE },
		{ "reconcile",		WT_VERB_RECONCILE },
		{ "recovery",		WT_VERB_RECOVERY },
		{ "salvage",		WT_VERB_SALVAGE },
		{ "shared_cache",	WT_VERB_SHARED_CACHE },
		{ "split",		WT_VERB_SPLIT },
		{ "temporary",		WT_VERB_TEMPORARY },
		{ "transaction",	WT_VERB_TRANSACTION },
		{ "verify",		WT_VERB_VERIFY },
		{ "version",		WT_VERB_VERSION },
		{ "write",		WT_VERB_WRITE },
		{ NULL, 0 }
	};
	WT_CONFIG_ITEM cval, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const WT_NAME_FLAG *ft;
	uint32_t flags;

	conn = S2C(session);

	WT_RET(__wt_config_gets(session, cfg, "verbose", &cval));

	flags = 0;
	for (ft = verbtypes; ft->name != NULL; ft++) {
		if ((ret = __wt_config_subgets(
		    session, &cval, ft->name, &sval)) == 0 && sval.val != 0) {
#ifdef HAVE_VERBOSE
			LF_SET(ft->flag);
#else
			WT_RET_MSG(session, EINVAL,
			    "Verbose option specified when WiredTiger built "
			    "without verbose support. Add --enable-verbose to "
			    "configure command and rebuild to include support "
			    "for verbose messages");
#endif
		}
		WT_RET_NOTFOUND_OK(ret);
	}

	conn->verbose = flags;
	return (0);
}

/*
 * __conn_write_base_config --
 *	Save the base configuration used to create a database.
 */
static int
__conn_write_base_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_FSTREAM *fs;
	WT_CONFIG parser;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_RET;
	bool exist;
	const char *base_config;

	fs = NULL;
	base_config = NULL;

	/*
	 * Discard any base configuration setup file left-over from previous
	 * runs.  This doesn't matter for correctness, it's just cleaning up
	 * random files.
	 */
	WT_RET(__wt_remove_if_exists(session, WT_BASECONFIG_SET));

	/*
	 * The base configuration file is only written if creating the database,
	 * and even then, a base configuration file is optional.
	 */
	if (!S2C(session)->is_new)
		return (0);
	WT_RET(__wt_config_gets(session, cfg, "config_base", &cval));
	if (!cval.val)
		return (0);

	/*
	 * We don't test separately if we're creating the database in this run
	 * as we might have crashed between creating the "WiredTiger" file and
	 * creating the base configuration file. If configured, there's always
	 * a base configuration file, and we rename it into place, so it can
	 * only NOT exist if we crashed before it was created; in other words,
	 * if the base configuration file exists, we're done.
	 */
	WT_RET(__wt_fs_exist(session, WT_BASECONFIG, &exist));
	if (exist)
		return (0);

	WT_RET(__wt_fopen(session, WT_BASECONFIG_SET,
	    WT_OPEN_CREATE | WT_OPEN_EXCLUSIVE, WT_STREAM_WRITE, &fs));

	WT_ERR(__wt_fprintf(session, fs, "%s\n\n",
	    "# Do not modify this file.\n"
	    "#\n"
	    "# WiredTiger created this file when the database was created,\n"
	    "# to store persistent database settings.  Instead of changing\n"
	    "# these settings, set a WIREDTIGER_CONFIG environment variable\n"
	    "# or create a WiredTiger.config file to override them."));

	/*
	 * The base configuration file contains all changes to default settings
	 * made at create, and we include the user-configuration file in that
	 * list, even though we don't expect it to change. Of course, an
	 * application could leave that file as it is right now and not remove
	 * a configuration we need, but applications can also guarantee all
	 * database users specify consistent environment variables and
	 * wiredtiger_open configuration arguments -- if we protect against
	 * those problems, might as well include the application's configuration
	 * file in that protection.
	 *
	 * We were passed the configuration items specified by the application.
	 * That list includes configuring the default settings, presumably if
	 * the application configured it explicitly, that setting should survive
	 * even if the default changes.
	 *
	 * When writing the base configuration file, we write the version and
	 * any configuration information set by the application (in other words,
	 * the stack except for cfg[0]). However, some configuration values need
	 * to be stripped out from the base configuration file; do that now, and
	 * merge the rest to be written.
	 */
	WT_ERR(__wt_config_merge(session, cfg + 1,
	    "config_base=,"
	    "create=,"
	    "encryption=(secretkey=),"
	    "exclusive=,"
	    "in_memory=,"
	    "log=(recover=),"
	    "readonly=,"
	    "use_environment_priv=,"
	    "verbose=,", &base_config));
	WT_ERR(__wt_config_init(session, &parser, base_config));
	while ((ret = __wt_config_next(&parser, &k, &v)) == 0) {
		/* Fix quoting for non-trivial settings. */
		if (v.type == WT_CONFIG_ITEM_STRING) {
			--v.str;
			v.len += 2;
		}
		WT_ERR(__wt_fprintf(session, fs,
		    "%.*s=%.*s\n", (int)k.len, k.str, (int)v.len, v.str));
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Flush the stream and rename the file into place. */
	ret = __wt_sync_and_rename(
	    session, &fs, WT_BASECONFIG_SET, WT_BASECONFIG);

	if (0) {
		/* Close open file handle, remove any temporary file. */
err:		WT_TRET(__wt_fclose(session, &fs));
		WT_TRET(__wt_remove_if_exists(session, WT_BASECONFIG_SET));
	}

	__wt_free(session, base_config);

	return (ret);
}

/*
 * __conn_set_file_system --
 *	Configure a custom file system implementation on database open.
 */
static int
__conn_set_file_system(
    WT_CONNECTION *wt_conn, WT_FILE_SYSTEM *file_system, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, set_file_system, config, cfg);
	WT_UNUSED(cfg);

	conn->file_system = file_system;

err:	API_END_RET(session, ret);
}

/*
 * __conn_chk_file_system --
 *	Check the configured file system.
 */
static int
__conn_chk_file_system(WT_SESSION_IMPL *session, bool readonly)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

#define	WT_CONN_SET_FILE_SYSTEM_REQ(name)				\
	if (conn->file_system->name == NULL)				\
		WT_RET_MSG(session, EINVAL,				\
		    "a WT_FILE_SYSTEM.%s method must be configured", #name)

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

	return (0);
}

/*
 * wiredtiger_open --
 *	Main library entry point: open a new connection to a WiredTiger
 *	database.
 */
int
wiredtiger_open(const char *home, WT_EVENT_HANDLER *event_handler,
    const char *config, WT_CONNECTION **wt_connp)
{
	static const WT_CONNECTION stdc = {
		__conn_async_flush,
		__conn_async_new_op,
		__conn_close,
		__conn_reconfigure,
		__conn_get_home,
		__conn_configure_method,
		__conn_is_new,
		__conn_open_session,
		__conn_load_extension,
		__conn_add_data_source,
		__conn_add_collator,
		__conn_add_compressor,
		__conn_add_encryptor,
		__conn_add_extractor,
		__conn_set_file_system,
		__conn_get_extension_api
	};
	static const WT_NAME_FLAG file_types[] = {
		{ "checkpoint",	WT_DIRECT_IO_CHECKPOINT },
		{ "data",	WT_DIRECT_IO_DATA },
		{ "log",	WT_DIRECT_IO_LOG },
		{ NULL, 0 }
	};

	WT_CONFIG_ITEM cval, keyid, secretkey, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(encbuf);
	WT_DECL_ITEM(i1);
	WT_DECL_ITEM(i2);
	WT_DECL_ITEM(i3);
	WT_DECL_RET;
	const WT_NAME_FLAG *ft;
	WT_SESSION_IMPL *session;
	bool config_base_set;
	const char *enc_cfg[] = { NULL, NULL }, *merge_cfg;
	char version[64];

	/* Leave lots of space for optional additional configuration. */
	const char *cfg[] = {
	    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

	*wt_connp = NULL;

	conn = NULL;
	session = NULL;
	merge_cfg = NULL;

	WT_RET(__wt_library_init());

	WT_RET(__wt_calloc_one(NULL, &conn));
	conn->iface = stdc;

	/*
	 * Immediately link the structure into the connection structure list:
	 * the only thing ever looked at on that list is the database name,
	 * and a NULL value is fine.
	 */
	__wt_spin_lock(NULL, &__wt_process.spinlock);
	TAILQ_INSERT_TAIL(&__wt_process.connqh, conn, q);
	__wt_spin_unlock(NULL, &__wt_process.spinlock);

	session = conn->default_session = &conn->dummy_session;
	session->iface.connection = &conn->iface;
	session->name = "wiredtiger_open";

	/* Do standard I/O and error handling first. */
	WT_ERR(__wt_os_stdio(session));
	__wt_event_handler_set(session, event_handler);

	/*
	 * Set the default session's strerror method. If one of the extensions
	 * being loaded reports an error via the WT_EXTENSION_API strerror
	 * method, but doesn't supply that method a WT_SESSION handle, we'll
	 * use the WT_CONNECTION_IMPL's default session and its strerror method.
	 */
	conn->default_session->iface.strerror = __wt_session_strerror;

	/* Basic initialization of the connection structure. */
	WT_ERR(__wt_connection_init(conn));

	/* Check the application-specified configuration string. */
	WT_ERR(__wt_config_check(session,
	    WT_CONFIG_REF(session, wiredtiger_open), config, 0));

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
	 * We need to know if configured for read-only or in-memory behavior
	 * before reading/writing the filesystem. The only way the application
	 * can configure that before we touch the filesystem is the wiredtiger
	 * config string or the WIREDTIGER_CONFIG environment variable.
	 *
	 * The environment isn't trusted by default, for security reasons; if
	 * the application wants us to trust the environment before reading
	 * the filesystem, the wiredtiger_open config string is the only way.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "in_memory", &cval));
	if (cval.val != 0)
		F_SET(conn, WT_CONN_IN_MEMORY);
	WT_ERR(__wt_config_gets(session, cfg, "readonly", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_READONLY);

	/*
	 * Load early extensions before doing further initialization (one early
	 * extension is to configure a file system).
	 */
	WT_ERR(__conn_load_extensions(session, cfg, true));

	/*
	 * If the application didn't configure its own file system, configure
	 * one of ours. Check to ensure we have a valid file system.
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
	WT_ERR(
	    __conn_chk_file_system(session, F_ISSET(conn, WT_CONN_READONLY)));

	/*
	 * Capture the config_base setting file for later use. Again, if the
	 * application doesn't want us to read the base configuration file,
	 * the WIREDTIGER_CONFIG environment variable or the wiredtiger_open
	 * config string are the only ways.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "config_base", &cval));
	config_base_set = cval.val != 0;

	/* Configure error messages so we get them right early. */
	WT_ERR(__wt_config_gets(session, cfg, "error_prefix", &cval));
	if (cval.len != 0)
		WT_ERR(__wt_strndup(
		    session, cval.str, cval.len, &conn->error_prefix));

	/* Get the database home. */
	WT_ERR(__conn_home(session, home, cfg));

	/* Make sure no other thread of control already owns this database. */
	WT_ERR(__conn_single(session, cfg));

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
	WT_ERR_TEST(snprintf(version, sizeof(version),
	    "version=(major=%d,minor=%d)",
	    WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR) >=
	    (int)sizeof(version), ENOMEM);
	__conn_config_append(cfg, version);

	/* Ignore the base_config file if config_base_set is false. */
	if (config_base_set)
		WT_ERR(
		    __conn_config_file(session, WT_BASECONFIG, false, cfg, i1));
	__conn_config_append(cfg, config);
	WT_ERR(__conn_config_file(session, WT_USERCONFIG, true, cfg, i2));
	WT_ERR(__conn_config_env(session, cfg, i3));

	/*
	 * Merge the full configuration stack and save it for reconfiguration.
	 */
	WT_ERR(__wt_config_merge(session, cfg, NULL, &merge_cfg));

	/*
	 * Read-only and in-memory settings may have been set in a configuration
	 * file (not optimal, but we can handle it). Get those settings again so
	 * we can override other configuration settings as they are processed.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "in_memory", &cval));
	if (cval.val != 0)
		F_SET(conn, WT_CONN_IN_MEMORY);
	WT_ERR(__wt_config_gets(session, cfg, "readonly", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_READONLY);
	if (F_ISSET(conn, WT_CONN_READONLY)) {
		/*
		 * Create a new stack with the merged configuration as the
		 * base.  The read-only string will use entry 1 and then
		 * we'll merge it again.
		 */
		cfg[0] = merge_cfg;
		cfg[1] = NULL;
		cfg[2] = NULL;
		/*
		 * We override some configuration settings for read-only.
		 * Other settings that conflict with and are an error with
		 * read-only are tested in their individual locations later.
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
	 * We can't open sessions yet, so any configurations that cause
	 * sessions to be opened must be handled inside __wt_connection_open.
	 *
	 * The error message configuration might have changed (if set in a
	 * configuration file, and not in the application's configuration
	 * string), get it again. Do it first, make error messages correct.
	 * Ditto verbose configuration so we dump everything the application
	 * wants to see.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "error_prefix", &cval));
	if (cval.len != 0) {
		__wt_free(session, conn->error_prefix);
		WT_ERR(__wt_strndup(
		    session, cval.str, cval.len, &conn->error_prefix));
	}
	WT_ERR(__wt_verbose_config(session, cfg));

	WT_ERR(__wt_config_gets(session, cfg, "hazard_max", &cval));
	conn->hazard_max = (uint32_t)cval.val;

	WT_ERR(__wt_config_gets(session, cfg, "session_max", &cval));
	conn->session_size = (uint32_t)cval.val + WT_EXTRA_INTERNAL_SESSIONS;

	WT_ERR(__wt_config_gets(session, cfg, "session_scratch_max", &cval));
	conn->session_scratch_max = (size_t)cval.val;

	WT_ERR(__wt_config_gets(session, cfg, "checkpoint_sync", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_CKPT_SYNC);

	WT_ERR(__wt_config_gets(session, cfg, "direct_io", &cval));
	for (ft = file_types; ft->name != NULL; ft++) {
		ret = __wt_config_subgets(session, &cval, ft->name, &sval);
		if (ret == 0) {
			if (sval.val)
				FLD_SET(conn->direct_io, ft->flag);
		} else
			WT_ERR_NOTFOUND_OK(ret);
	}

	WT_ERR(__wt_config_gets(session, cfg, "write_through", &cval));
	for (ft = file_types; ft->name != NULL; ft++) {
		ret = __wt_config_subgets(session, &cval, ft->name, &sval);
		if (ret == 0) {
			if (sval.val)
				FLD_SET(conn->write_through, ft->flag);
		} else
			WT_ERR_NOTFOUND_OK(ret);
	}

	/*
	 * If buffer alignment is not configured, use zero unless direct I/O is
	 * also configured, in which case use the build-time default.
	 */
	WT_ERR(__wt_config_gets(session, cfg, "buffer_alignment", &cval));
	if (cval.val == -1)
		conn->buffer_alignment =
		    (conn->direct_io == 0) ? 0 : WT_BUFFER_ALIGNMENT_DEFAULT;
	else
		conn->buffer_alignment = (size_t)cval.val;
#ifndef HAVE_POSIX_MEMALIGN
	if (conn->buffer_alignment != 0)
		WT_ERR_MSG(session, EINVAL,
		    "buffer_alignment requires posix_memalign");
#endif

	WT_ERR(__wt_config_gets(session, cfg, "file_extend", &cval));
	for (ft = file_types; ft->name != NULL; ft++) {
		ret = __wt_config_subgets(session, &cval, ft->name, &sval);
		if (ret == 0) {
			switch (ft->flag) {
			case WT_DIRECT_IO_DATA:
				conn->data_extend_len = sval.val;
				break;
			case WT_DIRECT_IO_LOG:
				conn->log_extend_len = sval.val;
				break;
			}
		} else
			WT_ERR_NOTFOUND_OK(ret);
	}

	WT_ERR(__wt_config_gets(session, cfg, "mmap", &cval));
	conn->mmap = cval.val != 0;

	WT_ERR(__conn_statistics_config(session, cfg));
	WT_ERR(__wt_lsm_manager_config(session, cfg));
	WT_ERR(__wt_sweep_config(session, cfg));

	/* Initialize the OS page size for mmap */
	conn->page_size = __wt_get_vm_pagesize();

	/* Now that we know if verbose is configured, output the version. */
	WT_ERR(__wt_verbose(
	    session, WT_VERB_VERSION, "%s", WIREDTIGER_VERSION_STRING));

	/*
	 * Open the connection, then reset the local session as the real one
	 * was allocated in __wt_connection_open.
	 */
	WT_ERR(__wt_connection_open(conn, cfg));
	session = conn->default_session;

	/*
	 * Load the extensions after initialization completes; extensions expect
	 * everything else to be in place, and the extensions call back into the
	 * library.
	 */
	WT_ERR(__conn_load_default_extensions(conn));
	WT_ERR(__conn_load_extensions(session, cfg, false));

	/*
	 * The metadata/log encryptor is configured after extensions, since
	 * extensions may load encryptors.  We have to do this before creating
	 * the metadata file.
	 *
	 * The encryption customize callback needs the fully realized set of
	 * encryption args, as simply grabbing "encryption" doesn't work.
	 * As an example, configuration for the current call may just be
	 * "encryption=(secretkey=xxx)", with encryption.name,
	 * encryption.keyid being 'inherited' from the stored base
	 * configuration.
	 */
	WT_ERR(__wt_config_gets_none(session, cfg, "encryption.name", &cval));
	WT_ERR(__wt_config_gets_none(session, cfg, "encryption.keyid", &keyid));
	WT_ERR(__wt_config_gets_none(session, cfg, "encryption.secretkey",
	    &secretkey));
	WT_ERR(__wt_scr_alloc(session, 0, &encbuf));
	WT_ERR(__wt_buf_fmt(session, encbuf,
	    "(name=%.*s,keyid=%.*s,secretkey=%.*s)",
	    (int)cval.len, cval.str, (int)keyid.len, keyid.str,
	    (int)secretkey.len, secretkey.str));
	enc_cfg[0] = encbuf->data;
	WT_ERR(__wt_encryptor_config(session, &cval, &keyid,
	    (WT_CONFIG_ARG *)enc_cfg, &conn->kencryptor));

	/*
	 * Configuration completed; optionally write a base configuration file.
	 */
	WT_ERR(__conn_write_base_config(session, cfg));

	/*
	 * Check on the turtle and metadata files, creating them if necessary
	 * (which avoids application threads racing to create the metadata file
	 * later).  Once the metadata file exists, get a reference to it in
	 * the connection's session.
	 *
	 * THE TURTLE FILE MUST BE THE LAST FILE CREATED WHEN INITIALIZING THE
	 * DATABASE HOME, IT'S WHAT WE USE TO DECIDE IF WE'RE CREATING OR NOT.
	 */
	WT_ERR(__wt_turtle_init(session));

	WT_ERR(__wt_metadata_cursor(session, NULL));

	/* Start the worker threads and run recovery. */
	WT_ERR(__wt_connection_workers(session, cfg));

	WT_STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*wt_connp = &conn->iface;

err:	/* Discard the scratch buffers. */
	__wt_scr_free(session, &encbuf);
	__wt_scr_free(session, &i1);
	__wt_scr_free(session, &i2);
	__wt_scr_free(session, &i3);

	__wt_free(session, merge_cfg);
	/*
	 * We may have allocated scratch memory when using the dummy session or
	 * the subsequently created real session, and we don't want to tie down
	 * memory for the rest of the run in either of them.
	 */
	if (session != &conn->dummy_session)
		__wt_scr_discard(session);
	__wt_scr_discard(&conn->dummy_session);

	if (ret != 0)
		WT_TRET(__wt_connection_close(conn));

	return (ret);
}
