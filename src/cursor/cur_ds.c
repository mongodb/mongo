/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curds_txn_enter --
 *	Do transactional initialization when starting an operation.
 */
static int
__curds_txn_enter(WT_SESSION_IMPL *session, int update)
{
	/* Check if we need to start an autocommit transaction. */
	if (update)
		WT_RET(__wt_txn_autocommit_check(session));

	if (session->ncursors++ == 0)			/* XXX */
		__wt_txn_read_first(session);

	return (0);
}

/*
 * __curds_txn_leave --
 *	Do transactional cleanup when ending an operation.
 */
static void
__curds_txn_leave(WT_SESSION_IMPL *session)
{
	if (--session->ncursors == 0)			/* XXX */
		__wt_txn_read_last(session);
}

/*
 * __curds_key_get -
 *	Get the key from the data-source.
 */
static inline void
__curds_key_get(WT_CURSOR *cursor)
{
	WT_CURSOR *source;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	cursor->recno = source->recno;
	cursor->key.data = source->key.data;
	cursor->key.size = source->key.size;
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
	WT_CURSOR *source;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	cursor->value.data = source->value.data;
	cursor->value.size = source->value.size;
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
	WT_CURSOR *source;
	WT_DECL_RET;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	WT_CURSOR_NEEDKEY(cursor);

	source->recno = cursor->recno;
	source->key.data = cursor->key.data;
	source->key.size = cursor->key.size;

err:	return (ret);
}

/*
 * __curds_value_set -
 *	Set the value for the data-source.
 */
static inline int
__curds_value_set(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	WT_CURSOR_NEEDVALUE(cursor);

	source->value.data = cursor->value.data;
	source->value.size = cursor->value.size;

err:	return (ret);
}

/*
 * __curds_compare --
 *	WT_CURSOR.compare method for the data-source cursor type.
 */
static int
__curds_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_COLLATOR *collator;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(a, session, compare, NULL);

	/*
	 * Confirm both cursors refer to the same source and have keys, then
	 * compare them.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "Cursors must reference the same object");

	WT_CURSOR_NEEDKEY(a);
	WT_CURSOR_NEEDKEY(b);

	/*
	 * The assumption is data-sources don't need to provide WiredTiger with
	 * WT_CURSOR.compare methods, instead, we'll copy the key/value out of
	 * the underlying data-source cursor and any comparison to be done can
	 * be done at this level.
	 */
	collator = ((WT_CURSOR_DATA_SOURCE *)a)->collator;
	if (collator == NULL)
		*cmpp = __wt_btree_lex_compare(&a->key, &b->key);
	else
		ret = collator->compare(
		    collator, &session->iface, &a->key, &b->key, cmpp);

err:	API_END(session);
	return (ret);
}

/*
 * __curds_next --
 *	WT_CURSOR.next method for the data-source cursor type.
 */
static int
__curds_next(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, next, NULL);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(source->next(source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	__curds_txn_leave(session);

	API_END(session);
	return (ret);
}

/*
 * __curds_prev --
 *	WT_CURSOR.prev method for the data-source cursor type.
 */
static int
__curds_prev(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, prev, NULL);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(source->prev(source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	__curds_txn_leave(session);

	API_END(session);
	return (ret);
}

/*
 * __curds_reset --
 *	WT_CURSOR.reset method for the data-source cursor type.
 */
static int
__curds_reset(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, reset, NULL);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	WT_ERR(source->reset(source));

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
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, search, NULL);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(source->search(source));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	__curds_txn_leave(session);

	API_END(session);
	return (ret);
}

/*
 * __curds_search_near --
 *	WT_CURSOR.search_near method for the data-source cursor type.
 */
static int
__curds_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, search_near, NULL);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(source->search_near(source, exact));
	__curds_key_get(cursor);
	__curds_value_get(cursor);

err:	__curds_txn_leave(session);

	API_END(session);
	return (ret);
}

/*
 * __curds_insert --
 *	WT_CURSOR.insert method for the data-source cursor type.
 */
static int
__curds_insert(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_UPDATE_API_CALL(cursor, session, insert, NULL);

	WT_ERR(__curds_txn_enter(session, 1));

	/* If not appending, we require a key. */
	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	WT_ERR(source->insert(source));

	/* If appending, we allocated a key. */
	if (F_ISSET(cursor, WT_CURSTD_APPEND))
		__curds_key_get(cursor);

err:	__curds_txn_leave(session);

	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_update --
 *	WT_CURSOR.update method for the data-source cursor type.
 */
static int
__curds_update(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_UPDATE_API_CALL(cursor, session, update, NULL);

	WT_ERR(__curds_txn_enter(session, 1));

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	ret = source->update(source);

err:	__curds_txn_leave(session);

	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_remove --
 *	WT_CURSOR.remove method for the data-source cursor type.
*/
static int
__curds_remove(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_UPDATE_API_CALL(cursor, session, remove, NULL);

	WT_ERR(__curds_txn_enter(session, 1));

	WT_ERR(__curds_key_set(cursor));
	ret = source->remove(source);

err:	__curds_txn_leave(session);

	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curds_close --
 *	WT_CURSOR.close method for the data-source cursor type.
 */
static int
__curds_close(WT_CURSOR *cursor)
{
	WT_CURSOR *source;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	CURSOR_API_CALL(cursor, session, close, NULL);

	if (source != NULL)
		ret = source->close(source);

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
__wt_curds_create(
    WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
    const char *cfg[], WT_DATA_SOURCE *dsrc, WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    __curds_compare,		/* compare */
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
	WT_CURSOR *cursor, *source;
	WT_CURSOR_DATA_SOURCE *data_source;
	WT_DECL_RET;
	const char *metaconf;

	STATIC_ASSERT(offsetof(WT_CURSOR_DATA_SOURCE, iface) == 0);

	data_source = NULL;
	metaconf = NULL;

	WT_RET(__wt_calloc_def(session, 1, &data_source));
	cursor = &data_source->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	F_SET(cursor, WT_CURSTD_DATA_SOURCE);

	/*
	 * XXX
	 * The underlying data-source may require the object's key and value
	 * formats.  This isn't a particularly elegant way of getting that
	 * information to the data-source, this feels like a layering problem
	 * to me.
	 */
	WT_ERR(__wt_metadata_search(session, uri, &metaconf));
	WT_ERR(__wt_config_getones(session, metaconf, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &cursor->key_format));
	WT_ERR(__wt_config_getones(session, metaconf, "value_format", &cval));
	WT_ERR(
	    __wt_strndup(session, cval.str, cval.len, &cursor->value_format));

	/*
	 * The assumption is data-sources don't need to provide WiredTiger with
	 * cursor range truncation support, instead, we'll iterate the cursors
	 * removing key/value pairs one at a time.
	 */
	cursor->range_truncate = __wt_cursor_range_truncate;

	WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

	/* Data-source cursors have a collator reference. */
	WT_ERR(__wt_collator_config(session, cfg, &data_source->collator));

	WT_ERR(dsrc->open_cursor(dsrc,
	    &session->iface, uri, (WT_CONFIG_ARG *)cfg, &data_source->source));
	source = data_source->source;
	source->session = (WT_SESSION *)session;
	memset(&source->q, 0, sizeof(source->q));
	source->recno = 0;
	memset(source->raw_recno_buf, 0, sizeof(source->raw_recno_buf));
	memset(&source->key, 0, sizeof(source->key));
	memset(&source->value, 0, sizeof(source->value));
	source->saved_err = 0;
	source->flags = 0;

	if (0) {
err:		if (F_ISSET(cursor, WT_CURSTD_OPEN))
			WT_TRET(cursor->close(cursor));
		else
			__wt_free(session, data_source);
		*cursorp = NULL;
	}

	__wt_free(session, metaconf);
	return (ret);
}
