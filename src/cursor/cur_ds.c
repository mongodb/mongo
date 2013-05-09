/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curds_key_get -
 *	Get the key from the data-source.
 */
static inline void
__curds_key_get(WT_CURSOR *cursor)
{
	cursor->recno = cursor->data_source->recno;
	cursor->key.data = cursor->data_source->key.data;
	cursor->key.size = cursor->data_source->key.size;
	F_CLR(cursor, WT_CURSTD_KEY_APP);
	F_SET(cursor, WT_CURSTD_KEY_RET);
}

/*
 * __curds_value_get -
 *	Get the value from the data-source.
 */
static inline void
__curds_value_get(WT_CURSOR *cursor)
{
	cursor->value.data = cursor->data_source->value.data;
	cursor->value.size = cursor->data_source->value.size;
	F_CLR(cursor, WT_CURSTD_VALUE_APP);
	F_SET(cursor, WT_CURSTD_VALUE_RET);
}

/*
 * __curds_key_set -
 *	Set the key for the data-source.
 */
static inline int
__curds_key_set(WT_CURSOR *cursor)
{
	WT_DECL_RET;

	WT_CURSOR_NEEDKEY(cursor);
	cursor->data_source->recno = cursor->recno;
	cursor->data_source->key.data = cursor->key.data;
	cursor->data_source->key.size = cursor->key.size;

err:	return (ret);
}

/*
 * __curds_value_set -
 *	Set the value for the data-source.
 */
static inline int
__curds_value_set(WT_CURSOR *cursor)
{
	WT_DECL_RET;

	WT_CURSOR_NEEDVALUE(cursor);
	cursor->data_source->value.data = cursor->value.data;
	cursor->data_source->value.size = cursor->value.size;

err:	return (ret);
}

/*
 * __curds_next --
 *	WT_CURSOR.next method for the data-source cursor type.
 */
static int
__curds_next(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, next, NULL);

	WT_ERR(cursor->data_source->next(cursor->data_source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	API_END(session);
	return (ret);
}

/*
 * __curds_prev --
 *	WT_CURSOR.prev method for the data-source cursor type.
 */
static int
__curds_prev(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, prev, NULL);

	WT_ERR(cursor->data_source->prev(cursor->data_source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	API_END(session);
	return (ret);
}

/*
 * __curds_reset --
 *	WT_CURSOR.reset method for the data-source cursor type.
 */
static int
__curds_reset(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, reset, NULL);

	WT_ERR(cursor->data_source->reset(cursor->data_source));

err:	API_END(session);
	return (ret);
}

/*
 * __curds_search --
 *	WT_CURSOR.search method for the data-source cursor type.
 */
static int
__curds_search(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, search, NULL);

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(cursor->data_source->search(cursor->data_source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	API_END(session);
	return (ret);
}

/*
 * __curds_search_near --
 *	WT_CURSOR.search_near method for the data-source cursor type.
 */
static int
__curds_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, search_near, NULL);

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(cursor->data_source->search_near(cursor->data_source, exact));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	API_END(session);
	return (ret);
}

/*
 * __curds_insert --
 *	WT_CURSOR.insert method for the data-source cursor type.
 */
static int
__curds_insert(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_UPDATE_API_CALL(cursor, session, insert, NULL);

	/* If not appending, we require a key. */
	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	WT_ERR(cursor->data_source->insert(cursor->data_source));

	/* If appending, we allocated a key. */
	if (F_ISSET(cursor, WT_CURSTD_APPEND))
		__curds_key_get(cursor);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_update --
 *	WT_CURSOR.update method for the data-source cursor type.
 */
static int
__curds_update(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_UPDATE_API_CALL(cursor, session, update, NULL);

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	WT_ERR(cursor->data_source->update(cursor->data_source));

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_remove --
 *	WT_CURSOR.remove method for the data-source cursor type.
*/
static int
__curds_remove(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_UPDATE_API_CALL(cursor, session, remove, NULL);

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(cursor->data_source->remove(cursor->data_source));

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_close --
 *	WT_CURSOR.close method for the data-source cursor type.
 */
static int
__curds_close(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, close, NULL);

	if (cursor->data_source != NULL)
		ret = cursor->data_source->close(cursor->data_source);

	/*
	 * The key/value formats are in allocated memory, which isn't standard
	 * behavior.
	 */
	__wt_free(session, cursor->key_format);
	__wt_free(session, cursor->value_format);

	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curds_create --
 *	Initialize a data-source cursor.
 */
int
__wt_curds_create(WT_SESSION_IMPL *session, const char *uri,
    const char *cfg[], WT_DATA_SOURCE *dsrc, WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    NULL,			/* compare */
	    __curds_next,		/* next */
	    __curds_prev,		/* prev */
	    __curds_reset,		/* reset */
	    __curds_search,		/* search */
	    __curds_search_near,	/* search-near */
	    __curds_insert,		/* insert */
	    __curds_update,		/* update */
	    __curds_remove,		/* remove */
	    __curds_close);		/* close */
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor, *dsc;
	WT_DECL_RET;
	const char **cfgp, *dscfg[5];
	const char *metaconf;

	metaconf = NULL;

	/* Open the WiredTiger cursor. */
	WT_RET(__wt_calloc_def(session, 1, &cursor));
	*cursor = iface;
	cursor->session = (WT_SESSION *)session;

	/*
	 * XXX
	 * We'll need the object's key and value formats.
	 */
	WT_ERR(__wt_metadata_read(session, uri, &metaconf));
	WT_ERR(__wt_config_getones(session, metaconf, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &cursor->key_format));
	WT_ERR(__wt_config_getones(session, metaconf, "value_format", &cval));
	WT_ERR(
	    __wt_strndup(session, cval.str, cval.len, &cursor->value_format));

	/*
	 * And, we'll need to pass that information down to the underlying
	 * data-source.
	 */
	cfgp = dscfg;
	if (cfg[0] != NULL) {
		*cfgp++ = cfg[0];
		if (cfg[1] != NULL) {
			*cfgp++ = cfg[1];
			if (cfg[2] != NULL)
				*cfgp++ = cfg[2];
		}
	}
	*cfgp++ = metaconf;
	*cfgp = NULL;

	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	WT_ERR(dsrc->open_cursor(dsrc,
	    &session->iface, uri, (WT_CONFIG_ARG *)dscfg, &dsc));
	memset(&dsc->q, 0, sizeof(dsc->q));
	dsc->recno = 0;
	memset(dsc->raw_recno_buf, 0, sizeof(dsc->raw_recno_buf));
	memset(&dsc->key, 0, sizeof(dsc->key));
	memset(&dsc->value, 0, sizeof(dsc->value));
	memset(&dsc->saved_err, 0, sizeof(dsc->saved_err));
	dsc->data_source = NULL;
	memset(&dsc->flags, 0, sizeof(dsc->flags));

	/* Reference the underlying application cursor. */
	cursor->data_source = dsc;

	if (0) {
err:		if (F_ISSET(cursor, WT_CURSTD_OPEN))
			WT_TRET(cursor->close(cursor));
		else
			__wt_free(session, cursor);
	}

	__wt_free(session, metaconf);
	return (ret);
}
