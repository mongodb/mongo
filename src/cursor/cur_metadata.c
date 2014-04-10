/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curmetadata_metadata_search --
 *	Retrieve the metadata for the metadata table
 */
static int
__curmetadata_metadata_search(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
	WT_CURSOR_METADATA *mdc;
	char *value;

	mdc = (WT_CURSOR_METADATA *)cursor;
	WT_RET(__wt_metadata_search(session,
	    "file:metadata", (const char **)&value));
	/*
	 * Copy the value in the underlying btree cursors tmp item
	 * which will be free'd when the cursor is closed.
	 */
	if (F_ISSET(mdc, WT_MDC_TMP_USED))
		__wt_buf_free(session, &mdc->tmp_val);
	mdc->tmp_val.data = mdc->tmp_val.mem = value;
	mdc->tmp_val.size = mdc->tmp_val.memsize = strlen(value);
	/* TODO: Is this assignment OK? */
	cursor->value = mdc->tmp_val;
	F_SET(mdc, WT_MDC_ONMETADATA | WT_MDC_POSITIONED | WT_MDC_TMP_USED);
	return (0);
}

/*
 * __curmetadata_compare --
 *	WT_CURSOR->compare method for the metadata cursor type.
 */
static int
__curmetadata_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_CURSOR *a_file_cursor, *b_file_cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	a_file_cursor = ((WT_CURSOR_METADATA *)a)->file_cursor;
	b_file_cursor = ((WT_CURSOR_METADATA *)b)->file_cursor;

	CURSOR_API_CALL(a, session,
	    compare, ((WT_CURSOR_BTREE *)a_file_cursor)->btree);

	if (b->compare != __curmetadata_compare)
		WT_ERR_MSG(session, EINVAL,
		    "Can only compare cursors of the same type");

	WT_CURSOR_NEEDKEY(a);
	WT_CURSOR_NEEDKEY(b);

	/* TODO: Compare if both cursors are positioned on metadata table */

	ret = a_file_cursor->compare(a_file_cursor, b_file_cursor, cmpp);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_next --
 *	WT_CURSOR->next method for the metadata cursor type.
 */
static int
__curmetadata_next(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    next, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	if (!F_ISSET(mdc, WT_MDC_POSITIONED))
		WT_ERR(__curmetadata_metadata_search(session, cursor));
	else {
		ret = file_cursor->next(mdc->file_cursor);
		if (ret == 0)
			F_CLR(mdc, WT_MDC_ONMETADATA);
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_prev --
 *	WT_CURSOR->prev method for the metadata cursor type.
 */
static int
__curmetadata_prev(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    prev, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	if (F_ISSET(mdc, WT_MDC_ONMETADATA))
		ret = WT_NOTFOUND;
	else if ((ret =
	    file_cursor->prev(file_cursor)) == WT_NOTFOUND)
		WT_ERR(__curmetadata_metadata_search(session, cursor));

	if (ret != 0)
		F_CLR(mdc,
		    WT_MDC_POSITIONED |
		    WT_MDC_ONMETADATA);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_reset --
 *	WT_CURSOR->reset method for the metadata cursor type.
 */
static int
__curmetadata_reset(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    reset, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	if (F_ISSET(mdc, WT_MDC_POSITIONED) &&
	    !F_ISSET(mdc, WT_MDC_ONMETADATA))
	    ret = file_cursor->reset(file_cursor);
	F_CLR(mdc,
	    WT_MDC_POSITIONED | WT_MDC_ONMETADATA);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_search --
 *	WT_CURSOR->search method for the metadata cursor type.
 */
static int
__curmetadata_search(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    search, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	WT_CURSOR_NEEDKEY(cursor);

	if (cursor->key.size == strlen("metadata") &&
	    strncmp(cursor->key.data, "metadata", strlen("metadata")) == 0)
		WT_ERR(__curmetadata_metadata_search(session, cursor));
	else {
		WT_ERR(file_cursor->search(file_cursor));
		F_SET(mdc, WT_MDC_POSITIONED);
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_search_near --
 *	WT_CURSOR->search_near method for the metadata cursor type.
 */
static int
__curmetadata_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    search_near, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	WT_CURSOR_NEEDKEY(cursor);

	if (cursor->key.size == strlen("metadata") &&
	    strncmp(cursor->key.data, "metadata", strlen("metadata")) == 0) {
		WT_ERR(__curmetadata_metadata_search(session, cursor));
		*exact = 1;
	} else {
		WT_ERR(file_cursor->search_near(file_cursor, exact));
		F_SET(mdc, WT_MDC_POSITIONED);
	}

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_insert --
 *	WT_CURSOR->insert method for the metadata cursor type.
 */
static int
__curmetadata_insert(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    insert, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);

	/* TODO: Copy key/value so we know they are NULL terminated */
	ret =__wt_metadata_insert(session,
	    (const char *)cursor->key.data, (const char *)cursor->value.data);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_update --
 *	WT_CURSOR->update method for the metadata cursor type.
 */
static int
__curmetadata_update(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    update, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);

	/* TODO: Copy key/value so we know they are NULL terminated */
	ret = __wt_metadata_update(session,
	    (const char *)cursor->key.data, (const char *)cursor->value.data);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_remove --
 *	WT_CURSOR->remove method for the metadata cursor type.
 */
static int
__curmetadata_remove(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    remove, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	WT_CURSOR_NEEDKEY(cursor);

	/* TODO: Copy key so we know it is NULL terminated */
	ret = __wt_metadata_remove(session, (const char *)cursor->key.data);

err:	API_END(session);
	return (ret);
}

/*
 * __curmetadata_close --
 *	WT_CURSOR->close method for the metadata cursor type.
 */
static int
__curmetadata_close(WT_CURSOR *cursor)
{
	WT_CURSOR *file_cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	mdc = (WT_CURSOR_METADATA *)cursor;
	file_cursor = mdc->file_cursor;
	CURSOR_API_CALL(cursor, session,
	    close, ((WT_CURSOR_BTREE *)file_cursor)->btree);

	ret = file_cursor->close(file_cursor);
	if (F_ISSET(mdc, WT_MDC_TMP_USED))
		__wt_buf_free(session, &mdc->tmp_val);

	WT_ERR(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curmetadata_open --
 *	WT_SESSION->open_cursor method for metadata cursors.
 *
 * Metadata cursors are a similar to a file cursor on the special metadata
 * table, except that the metadata for the metadata table (which is stored
 * in the turtle file) can also be queried.
 * Metadata cursors are read-only default.
 * updateable.
 */
int
__wt_curmetadata_open(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    __curmetadata_compare,	/* compare */
	    __curmetadata_next,		/* next */
	    __curmetadata_prev,		/* prev */
	    __curmetadata_reset,	/* reset */
	    __curmetadata_search,	/* search */
	    __curmetadata_search_near,	/* search-near */
	    __curmetadata_insert,	/* insert */
	    __curmetadata_update,	/* update */
	    __curmetadata_remove,	/* remove */
	    __curmetadata_close);	/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_METADATA *mdc;
	WT_DECL_RET;

	WT_RET(__wt_calloc(session, 1, sizeof(WT_CURSOR_METADATA), &mdc));

	cursor = &mdc->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->key_format = "S";
	cursor->value_format = "S";

	/* Open the file cursor for operations on the regular metadata */
	WT_ERR(__wt_metadata_cursor(session, cfg[1], &mdc->file_cursor));

	WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

	/* Metadata cursors default to read only. */
	WT_ERR(__wt_cursor_config_readonly(cursor, cfg, 1));

	if (0) {
err:		__wt_free(session, mdc);
	}
	return (ret);
}
