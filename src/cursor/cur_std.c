/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_notsup --
 *	Unsupported cursor actions.
 */
int
__wt_cursor_notsup(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (ENOTSUP);
}

/*
 * __wt_cursor_noop --
 *	Cursor noop.
 */
int
__wt_cursor_noop(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);

	return (0);
}

/*
 * __wt_cursor_set_notsup --
 *	Reset the cursor methods to not-supported.
 */
void
__wt_cursor_set_notsup(WT_CURSOR *cursor)
{
	/*
	 * Set all of the cursor methods (except for close and reset), to fail.
	 * Close is unchanged so the cursor can be discarded, reset defaults to
	 * a no-op because session transactional operations reset all of the
	 * cursors in a session, and random cursors shouldn't block transactions
	 * or checkpoints.
	 */
	cursor->compare =
	    (int (*)(WT_CURSOR *, WT_CURSOR *, int *))__wt_cursor_notsup;
	cursor->next = __wt_cursor_notsup;
	cursor->prev = __wt_cursor_notsup;
	cursor->reset = __wt_cursor_noop;
	cursor->search = __wt_cursor_notsup;
	cursor->search_near = (int (*)(WT_CURSOR *, int *))__wt_cursor_notsup;
	cursor->insert = __wt_cursor_notsup;
	cursor->update = __wt_cursor_notsup;
	cursor->remove = __wt_cursor_notsup;
}

/*
 * __wt_cursor_get_key --
 *	WT_CURSOR->get_key default implementation.
 */
int
__wt_cursor_get_key(WT_CURSOR *cursor, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_key, NULL);
	ret = __wt_kv_get_keyv(session, cursor, cursor->flags, ap);
err:	API_END(session);
	va_end(ap);
	return (ret);
}

/*
 * __wt_cursor_set_key --
 *	WT_CURSOR->set_key default implementation.
 */
void
__wt_cursor_set_key(WT_CURSOR *cursor, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, get_key, NULL);
	__wt_kv_set_keyv(session, cursor, cursor->flags, ap);
err:	API_END(session);
	va_end(ap);
}

/*
 * __wt_cursor_get_value --
 *	WT_CURSOR->get_value default implementation.
 */
int
__wt_cursor_get_value(WT_CURSOR *cursor, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;
	const char *fmt;

	CURSOR_API_CALL(cursor, session, get_value, NULL);

	va_start(ap, cursor);

	if (!F_ISSET(cursor, WT_CURSTD_VALUE_EXT | WT_CURSTD_VALUE_INT))
		WT_ERR(__wt_kv_not_set(session, 0, cursor->saved_err));

	fmt = F_ISSET(cursor, WT_CURSOR_RAW_OK) ? "u" : cursor->value_format;
	WT_ERR(__wt_kv_get_value(session, &cursor->value, fmt, ap));

err:	va_end(ap);
	API_END(session);
	return (ret);
}

/*
 * __wt_cursor_set_value --
 *	WT_CURSOR->set_value default implementation.
 */
void
__wt_cursor_set_value(WT_CURSOR *cursor, ...)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	va_start(ap, cursor);
	CURSOR_API_CALL(cursor, session, set_value, NULL);
	__wt_kv_set_value(cursor, ap);
err:	va_end(ap);
	API_END(session);
}

/*
 * __cursor_search --
 *	WT_CURSOR->search default implementation.
 */
static int
__cursor_search(WT_CURSOR *cursor)
{
	int exact;

	WT_RET(cursor->search_near(cursor, &exact));
	return ((exact == 0) ? 0 : WT_NOTFOUND);
}

/*
 * __wt_cursor_close --
 *	WT_CURSOR->close default implementation.
 */
int
__wt_cursor_close(WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cursor->session;
	__wt_buf_free(session, &cursor->key);
	__wt_buf_free(session, &cursor->value);

	if (F_ISSET(cursor, WT_CURSTD_OPEN)) {
		TAILQ_REMOVE(&session->cursors, cursor, q);

		WT_STAT_FAST_DATA_DECR(session, session_cursor_open);
		WT_STAT_FAST_CONN_ATOMIC_DECR(session, session_cursor_open);
	}

	__wt_free(session, cursor->uri);
	__wt_overwrite_and_free(session, cursor);
	return (ret);
}

/*
 * __cursor_runtime_config --
 *	Set runtime-configurable settings.
 */
static int
__cursor_runtime_config(WT_CURSOR *cursor, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cursor->session;

	/* 
	 * !!!
	 * There's no way yet to reconfigure cursor flags at runtime; if, in
	 * the future there is a way to do that, similar support needs to be
	 * added for data-source cursors, or, this call needs to return an
	 * error in the case of a data-source cursor.
	 */
	WT_RET(__wt_config_gets_def(session, cfg, "overwrite", 1, &cval));
	if (cval.val)
		F_SET(cursor, WT_CURSTD_OVERWRITE);
	else
		F_CLR(cursor, WT_CURSTD_OVERWRITE);

	return (0);
}

/*
 * __wt_cursor_dup_position --
 *	Set a cursor to another cursor's position.
 */
int
__wt_cursor_dup_position(WT_CURSOR *to_dup, WT_CURSOR *cursor)
{
	WT_DECL_RET;
	WT_ITEM key;

	/*
	 * Get a copy of the cursor's raw key, and set it in the new cursor,
	 * then search for that key to position the cursor.
	 *
	 * We don't clear the WT_ITEM structure: all that happens when getting
	 * and setting the key is the data/size fields are reset to reference
	 * the original cursor's key.
	 *
	 * That said, we're playing games with the cursor flags: setting the key
	 * sets the key/value application-set flags in the new cursor, which may
	 * or may not be correct, but there's nothing simple that fixes it.  We
	 * depend on the subsequent cursor search to clean things up, as search
	 * is required to copy and/or reference private memory after success.
	 */

	WT_WITH_RAW(to_dup, WT_CURSTD_RAW, ret = to_dup->get_key(to_dup, &key));
	WT_RET(ret);
	WT_WITH_RAW(cursor, WT_CURSTD_RAW, cursor->set_key(cursor, &key));

	/*
	 * We now have a reference to the raw key, but we don't know anything
	 * about the memory in which it's stored, it could be btree/file page
	 * memory in the cache, application memory or the original cursor's
	 * key/value WT_ITEMs.  Memory allocated in support of another cursor
	 * could be discarded when that cursor is closed, so it's a problem.
	 * However, doing a search to position the cursor will fix the problem:
	 * cursors cannot reference application memory after cursor operations
	 * and that requirement will save the day.
	 */
	WT_RET(cursor->search(cursor));

	return (0);
}

/*
 * __wt_cursor_init --
 *	Default cursor initialization.
 */
int
__wt_cursor_init(WT_CURSOR *cursor,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR *cdump;
	WT_CONFIG_ITEM cval;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cursor->session;

	/*
	 * Fill in unspecified cursor methods: get/set key/value, position
	 * duplication, search and reconfiguration are all standard, else
	 * if the method isn't set, assume it's unsupported.
	 */
	if (cursor->get_key == NULL)
		cursor->get_key = __wt_cursor_get_key;
	if (cursor->get_value == NULL)
		cursor->get_value = __wt_cursor_get_value;
	if (cursor->set_key == NULL)
		cursor->set_key = __wt_cursor_set_key;
	if (cursor->set_value == NULL)
		cursor->set_value = __wt_cursor_set_value;
	if (cursor->compare == NULL)
		cursor->compare = (int (*)
		    (WT_CURSOR *, WT_CURSOR *, int *))__wt_cursor_notsup;
	if (cursor->next == NULL)
		cursor->next = __wt_cursor_notsup;
	if (cursor->prev == NULL)
		cursor->prev = __wt_cursor_notsup;
	if (cursor->reset == NULL)
		cursor->reset = __wt_cursor_noop;
	if (cursor->search == NULL)
		cursor->search = __cursor_search;
	if (cursor->search_near == NULL)
		cursor->search_near =
		    (int (*)(WT_CURSOR *, int *))__wt_cursor_notsup;
	if (cursor->insert == NULL)
		cursor->insert = __wt_cursor_notsup;
	if (cursor->update == NULL)
		cursor->update = __wt_cursor_notsup;
	if (cursor->remove == NULL)
		cursor->remove = __wt_cursor_notsup;
	if (cursor->close == NULL)
		WT_RET_MSG(session, EINVAL, "cursor lacks a close method");

	if (cursor->uri == NULL)
		WT_RET(__wt_strdup(session, uri, &cursor->uri));

	/* Set runtime-configurable settings. */
	WT_RET(__cursor_runtime_config(cursor, cfg));

	/*
	 * append
	 * The append flag is only relevant to column stores.
	 */
	if (WT_CURSOR_RECNO(cursor)) {
		WT_RET(__wt_config_gets_def(session, cfg, "append", 0, &cval));
		if (cval.val != 0)
			F_SET(cursor, WT_CURSTD_APPEND);
	}

	/*
	 * checkpoint
	 * Checkpoint cursors are read-only.
	 */
	WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
	if (cval.len != 0) {
		cursor->insert = __wt_cursor_notsup;
		cursor->update = __wt_cursor_notsup;
		cursor->remove = __wt_cursor_notsup;
	}

	/* dump */
	WT_RET(__wt_config_gets_def(session, cfg, "dump", 0, &cval));
	if (cval.len != 0) {
		/*
		 * Dump cursors should not have owners: only the top-level
		 * cursor should be wrapped in a dump cursor.
		 */
		WT_ASSERT(session, owner == NULL);

		F_SET(cursor,
		    WT_STRING_MATCH("print", cval.str, cval.len) ?
		    WT_CURSTD_DUMP_PRINT : WT_CURSTD_DUMP_HEX);
		WT_RET(__wt_curdump_create(cursor, owner, &cdump));
		owner = cdump;
	} else
		cdump = NULL;

	/* raw */
	WT_RET(__wt_config_gets_def(session, cfg, "raw", 0, &cval));
	if (cval.val != 0)
		F_SET(cursor, WT_CURSTD_RAW);

	/*
	 * Cursors that are internal to some other cursor (such as file cursors
	 * inside a table cursor) should be closed after the containing cursor.
	 * Arrange for that to happen by putting internal cursors after their
	 * owners on the queue.
	 */
	if (owner != NULL)
		TAILQ_INSERT_AFTER(&session->cursors, owner, cursor, q);
	else
		TAILQ_INSERT_HEAD(&session->cursors, cursor, q);

	F_SET(cursor, WT_CURSTD_OPEN);
	WT_STAT_FAST_DATA_INCR(session, session_cursor_open);
	WT_STAT_FAST_CONN_ATOMIC_INCR(session, session_cursor_open);

	*cursorp = (cdump != NULL) ? cdump : cursor;
	return (0);
}
