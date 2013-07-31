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
 * __curds_key_set -
 *	Set the key for the data-source.
 */
static int
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
static int
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
 * __curds_cursor_resolve -
 *	Resolve cursor operation.
 */
static int
__curds_cursor_resolve(WT_SESSION_IMPL *session, WT_CURSOR *cursor, int ret)
{
	WT_CURSOR *source;
	size_t key_size, value_size;
	const void *key_data, *value_data;

	source = ((WT_CURSOR_DATA_SOURCE *)cursor)->source;

	if (ret == 0) {
		/*
		 * On success, retrieve the key and value and ensure we don't
		 * reference application memory.  WiredTiger guarantees that
		 * a successful cursor operation leaves the cursor referencing
		 * private memory, that is, the application's memory can be
		 * modified without concern.  File cursors do this in the btree
		 * layer; we don't complicate underlying data sources by making
		 * them implement that semantic, do it more generally here.
		 *
		 * Originally, we set the source's key/value to reference the
		 * original cursor's value.  If that's still true, make a local
		 * copy of any key or value so we can clean up on error, then
		 * do a copy in WiredTiger memory.  If that's not true, the data
		 * source set a return key/value and we can simply reference it.
		 */
		cursor->recno = source->recno;
		if (F_ISSET(cursor, WT_CURSTD_KEY_APP) &&
		    cursor->key.data == source->key.data) {
			key_data = cursor->key.data;
			key_size = cursor->key.size;
			WT_TRET(__wt_buf_set(session, &cursor->key,
			    cursor->key.data, cursor->key.size));
		} else {
			cursor->key.data = source->key.data;
			cursor->key.size = source->key.size;
		}
		if (F_ISSET(cursor, WT_CURSTD_VALUE_APP) &&
		    cursor->value.data == source->value.data) {
			value_data = cursor->value.data;
			value_size = cursor->value.size;
			WT_TRET(__wt_buf_set(session, &cursor->value,
			    cursor->value.data, cursor->value.size));
		} else {
			cursor->value.data = source->value.data;
			cursor->value.size = source->value.size;
		}

		/*
		 * If a copy failed, then the return value might no longer
		 * be 0, check again.
		 */
		if (ret == 0) {
			F_CLR(cursor, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
			F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
		} else {
			if (F_ISSET(cursor, WT_CURSTD_KEY_APP)) {
				cursor->key.data = key_data;
				cursor->key.size = key_size;
			}
			if (F_ISSET(cursor, WT_CURSTD_VALUE_APP)) {
				cursor->value.data = value_data;
				cursor->value.size = value_size;
			}
			F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
		}
	} else {
		/*
		 * WiredTiger's semantic is a cursor operation failure implies
		 * the cursor position is lost.  Simplify the underlying data
		 * source implementation and reset the cursor explicitly here.
		 */
		WT_TRET(source->reset(source));

		F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
	}
	return (ret);
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

	if (WT_CURSOR_RECNO(a)) {
		if (a->recno < b->recno)
			*cmpp = -1;
		else if (a->recno == b->recno)
			*cmpp = 0;
		else
			*cmpp = 1;
	} else {
		/*
		 * The assumption is data-sources don't provide WiredTiger with
		 * WT_CURSOR.compare methods, instead, we'll copy the key/value
		 * out of the underlying data-source cursor and any comparison
		 * to be done can be done at this level.
		 */
		collator = ((WT_CURSOR_DATA_SOURCE *)a)->collator;
		WT_ERR(WT_LEX_CMP(session, collator, &a->key, &b->key, *cmpp));
	}

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

	WT_CSTAT_INCR(session, cursor_next); 
	WT_DSTAT_INCR(session, cursor_next);

	/*
	 * XXX: In the current develop branch, cursor movement explicitly
	 * clears the all of the cursor key/value set flags.  That code
	 * is going away soon, this line corrects for now.
	 */
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	WT_ERR(__curds_txn_enter(session, 0));

	ret = source->next(source);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_prev);
	WT_DSTAT_INCR(session, cursor_prev);

	/*
	 * XXX: In the current develop branch, cursor movement explicitly
	 * clears the all of the cursor key/value set flags.  That code
	 * is going away soon, this line corrects for now.
	 */
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	WT_ERR(__curds_txn_enter(session, 0));

	ret = source->prev(source);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_reset);      
	WT_DSTAT_INCR(session, cursor_reset);

	WT_ERR(source->reset(source));

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

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

	WT_CSTAT_INCR(session, cursor_search);
	WT_DSTAT_INCR(session, cursor_search);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(__curds_key_set(cursor));
	ret = source->search(source);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_search_near);
	WT_DSTAT_INCR(session, cursor_search_near);

	WT_ERR(__curds_txn_enter(session, 0));

	WT_ERR(__curds_key_set(cursor));
	ret = source->search_near(source, exact);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_insert);     
	WT_DSTAT_INCR(session, cursor_insert);
	WT_DSTAT_INCRV(session,
	    cursor_insert_bytes, cursor->key.size + cursor->value.size);

	/* If not appending, we require a key. */
	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	ret = source->insert(source);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_update);     
	WT_DSTAT_INCR(session, cursor_update);
	WT_DSTAT_INCRV(session, cursor_update_bytes, cursor->value.size);

	WT_ERR(__curds_txn_enter(session, 1));

	WT_ERR(__curds_key_set(cursor));
	WT_ERR(__curds_value_set(cursor));
	ret = source->update(source);
	WT_TRET(__curds_cursor_resolve(session, cursor, ret));

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

	WT_CSTAT_INCR(session, cursor_remove);     
	WT_DSTAT_INCR(session, cursor_remove);
	    WT_DSTAT_INCRV(session, cursor_remove_bytes, cursor->key.size);

	WT_ERR(__curds_txn_enter(session, 1));

	WT_ERR(__curds_key_set(cursor));
	ret = source->remove(source);

	/* After a successful remove, the key and value are not available. */
	if (ret == 0)
		F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

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
