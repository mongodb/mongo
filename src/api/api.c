/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __session_close --
 *	WT_SESSION->close method.
 */
static int
__session_close(WT_SESSION *wt_session, const char *config)
{
	BTREE *btree;
	BTREE_SESSION *btree_session;
	CONNECTION *conn;
	SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	conn = (CONNECTION *)wt_session->connection;
	session = (SESSION *)wt_session;
	ret = 0;

	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL)
		WT_TRET(cursor->close(cursor, config));

	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL) {
		TAILQ_REMOVE(&session->btrees, btree_session, q);
		btree = btree_session->btree;
		WT_TRET(btree->close(btree, session, 0));
		__wt_free(session, btree_session);
	}

	TAILQ_REMOVE(&conn->sessions_head, session, q);
	WT_TRET(session->close(session, 0));

	return (0);
}

/*
 * __session_open_cursor --
 *	WT_SESSION->open_cursor method.
 */
static int
__session_open_cursor(WT_SESSION *wt_session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	SESSION *session;

	WT_UNUSED(to_dup);

	session = (SESSION *)wt_session;

	if (strncmp(uri, "config:", 6) == 0)
		return (__wt_curconfig_open(session, uri, config, cursorp));
	if (strncmp(uri, "table:", 6) == 0)
		return (__wt_cursor_open(session, uri, config, cursorp));

	__wt_err(session, 0, "Unknown cursor type '%s'\n", uri);
	return (EINVAL);
}

/*
 * __session_create_table --
 *	WT_SESSION->create_table method.
 */
static int
__session_create_table(WT_SESSION *wt_session,
    const char *name, const char *config)
{
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	WT_CONFIG_ITEM cval;
	const char *key_format, *value_format;
	uint32_t column_flags, fixed_len, huffman_flags;
	uint32_t intl_node_max, intl_node_min, leaf_node_max, leaf_node_min;
	const char *cfg[] = { __wt_config_def_create_table, config, NULL };

	WT_UNUSED(config);

	session = (SESSION *)wt_session;
	conn = (CONNECTION *)wt_session->connection;

	WT_RET(__wt_config_checkone(session,
	    __wt_config_def_create_table, config));

	/*
	 * Key / value formats.
	 *
	 * !!! TODO: these need to be saved in a table-of-tables.
	 * Also, avoiding copies / memory allocation at the moment by
	 * pointing to constant strings for the few cases we handle.
	 */
	fixed_len = 0;

	WT_RET(__wt_config_gets(cfg, "key_format", &cval));
	if (strncmp(cval.str, "r", cval.len) == 0)
		key_format = "r";
	else if (strncmp(cval.str, "S", cval.len) == 0)
		key_format = "S";
	else if (strncmp(cval.str, "u", cval.len) == 0)
		key_format = "u";
	else {
		__wt_err(session, 0, "Unknown key_format '%.*s'\n",
		    cval.len, cval.str);
		return (EINVAL);
	}

	WT_RET(__wt_config_gets(cfg, "value_format", &cval));
	if (strncmp(cval.str, "S", cval.len) == 0)
		value_format = "S";
	else if (strncmp(cval.str, "u", cval.len) == 0)
		value_format = "u";
	else if (cval.len > 1 && cval.str[cval.len - 1] == 'u') {
		fixed_len = (uint32_t)strtol(cval.str, NULL, 10);
		value_format = "u";
	} else {
		__wt_err(session, 0, "Unknown value_format '%.*s'\n",
		    cval.len, cval.str);
		return (EINVAL);
	}

	column_flags = 0;
	WT_RET(__wt_config_gets(cfg, "runlength_encoding", &cval));
	if (cval.val != 0)
		column_flags |= WT_RLE;

	huffman_flags = 0;
	WT_RET(__wt_config_gets(cfg, "huffman_key", &cval));
	if (cval.len > 0 && strncasecmp(cval.str, "english", cval.len) == 0)
		huffman_flags |= WT_ASCII_ENGLISH | WT_HUFFMAN_KEY;
	WT_RET(__wt_config_gets(cfg, "huffman_value", &cval));
	if (cval.len > 0 && strncasecmp(cval.str, "english", cval.len) == 0)
		huffman_flags |= WT_ASCII_ENGLISH | WT_HUFFMAN_VALUE;

	WT_RET(__wt_config_gets(cfg, "intl_node_max", &cval));
	intl_node_max = (uint32_t)cval.val;
	WT_RET(__wt_config_gets(cfg, "intl_node_min", &cval));
	intl_node_min = (uint32_t)cval.val;
	WT_RET(__wt_config_gets(cfg, "leaf_node_max", &cval));
	leaf_node_max = (uint32_t)cval.val;
	WT_RET(__wt_config_gets(cfg, "leaf_node_min", &cval));
	leaf_node_min = (uint32_t)cval.val;

	WT_RET(conn->btree(conn, 0, &btree));
	WT_RET(btree->btree_pagesize_set(btree, 0,
	    intl_node_min, intl_node_max, leaf_node_min, leaf_node_max));
	if (key_format[0] == 'r')
		WT_RET(btree->column_set(btree, fixed_len, NULL, column_flags));
	if (huffman_flags != 0)
		WT_RET(btree->huffman_set(btree, NULL, 0, huffman_flags));
	WT_RET(btree->open(btree, session, name, 0666, WT_CREATE));

	WT_RET(__wt_session_add_btree(session,
	    btree, key_format, value_format));

	return (0);
}

/*
 * __session_rename_table --
 *	WT_SESSION->rename_table method.
 */
static int
__session_rename_table(WT_SESSION *wt_session,
    const char *oldname, const char *newname, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(oldname);
	WT_UNUSED(newname);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_drop_table --
 *	WT_SESSION->drop_table method.
 */
static int
__session_drop_table(
    WT_SESSION *wt_session, const char *name, const char *config)
{
	SESSION *session;
	WT_CONFIG_ITEM cvalue;
	int force, ret;

	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	session = (SESSION *)wt_session;
	force = 0;

	CONFIG_LOOP(session, config, cvalue)
		CONFIG_ITEM("force")
			force = (cvalue.val != 0);
	CONFIG_END(session);

	/* TODO: Combine the table name with the conn home to make a filename. */

	ret = remove(name);

	return (force ? 0 : ret);
}

/*
 * __session_truncate_table --
 *	WT_SESSION->truncate_table method.
 */
static int
__session_truncate_table(WT_SESSION *wt_session,
    const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(start);
	WT_UNUSED(end);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_verify_table --
 *	WT_SESSION->verify_table method.
 */
static int
__session_verify_table(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (0);
}

/*
 * __session_begin_transaction --
 *	WT_SESSION->begin_transaction method.
 */
static int
__session_begin_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_commit_transaction --
 *	WT_SESSION->commit_transaction method.
 */
static int
__session_commit_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_rollback_transaction --
 *	WT_SESSION->rollback_transaction method.
 */
static int
__session_rollback_transaction(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_checkpoint --
 *	WT_SESSION->checkpoint method.
 */
static int
__session_checkpoint(WT_SESSION *wt_session, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __conn_load_extension --
 *	WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(WT_CONNECTION *wt_conn, const char *path, const char *config)
{
	WT_UNUSED(wt_conn);
	WT_UNUSED(path);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __conn_add_cursor_type --
 *	WT_CONNECTION->add_cursor_type method.
 */
static int
__conn_add_cursor_type(WT_CONNECTION *wt_conn,
    const char *prefix, WT_CURSOR_TYPE *ctype, const char *config)
{
	WT_UNUSED(wt_conn);
	WT_UNUSED(prefix);
	WT_UNUSED(ctype);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __conn_add_collator --
 *	WT_CONNECTION->add_collator method.
 */
static int
__conn_add_collator(WT_CONNECTION *wt_conn,
    const char *name, WT_COLLATOR *collator, const char *config)
{
	WT_UNUSED(wt_conn);
	WT_UNUSED(name);
	WT_UNUSED(collator);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __conn_add_extractor --
 *	WT_CONNECTION->add_extractor method.
 */
static int
__conn_add_extractor(WT_CONNECTION *wt_conn,
    const char *name, WT_EXTRACTOR *extractor, const char *config)
{
	WT_UNUSED(wt_conn);
	WT_UNUSED(name);
	WT_UNUSED(extractor);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static const char *
__conn_get_home(WT_CONNECTION *wt_conn)
{
	return (((CONNECTION *)wt_conn)->home);
}

/*
 * __conn_is_new --
 *	WT_CONNECTION->is_new method.
 */
static int
__conn_is_new(WT_CONNECTION *wt_conn)
{
	WT_UNUSED(wt_conn);

	return (0);
}

/*
 * __conn_close --
 *	WT_CONNECTION->close method.
 */
static int
__conn_close(WT_CONNECTION *wt_conn, const char *config)
{
	int ret;
	CONNECTION *conn;
	SESSION *session;
	WT_SESSION *wt_session;

	ret = 0;
	conn = (CONNECTION *)wt_conn;

	while ((session = TAILQ_FIRST(&conn->sessions_head)) != NULL) {
		wt_session = &session->iface;
		WT_TRET(wt_session->close(wt_session, config));
	}

	__wt_free(&conn->default_session, conn->home);
	if (conn->log_fh != NULL) {
		WT_TRET(__wt_close(&conn->default_session, conn->log_fh));
		conn->log_fh = NULL;
	}
	WT_TRET(conn->close(conn, 0));
	return (ret);
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
	static WT_SESSION stds = {
		NULL,
		__session_close,
		__session_open_cursor,
		__session_create_table,
		__session_rename_table,
		__session_drop_table,
		__session_truncate_table,
		__session_verify_table,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
	};
	CONNECTION *conn;
	SESSION *session;
	int ret;

	WT_UNUSED(config);

	conn = (CONNECTION *)wt_conn;

	WT_ERR(conn->session(conn, 0, &session));
	/*
	 * XXX
	 * Kludge while there is a separate __wt_conection_session method.
	 * We shouldn't be overwriting the connection pointer, particularly not
	 * through a static struct that is shared between threads.
	 */
	stds.connection = wt_conn;
	session->iface = stds;
	TAILQ_INIT(&session->cursors);
	TAILQ_INIT(&session->btrees);
	WT_ASSERT(NULL, conn->default_session.event_handler != NULL);
	if (event_handler != NULL)
		session->event_handler = event_handler;

	TAILQ_INSERT_HEAD(&conn->sessions_head, session, q);

	STATIC_ASSERT(offsetof(CONNECTION, iface) == 0);
	*wt_sessionp = &session->iface;

	if (0) {
err:		if (session != NULL)
			(void)__wt_session_close(session);
		__wt_free(&conn->default_session, session);
	}

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
	static int library_init = 0;
	static WT_CONNECTION stdc = {
		__conn_load_extension,
		__conn_add_cursor_type,
		__conn_add_collator,
		__conn_add_extractor,
		__conn_close,
		__conn_get_home,
		__conn_is_new,
		__conn_open_session
	};
	CONNECTION *conn;
	WT_CONFIG_ITEM cval;
	const char *cfg[] = { __wt_config_def_wiredtiger_open, config, NULL };
	int ret;

	*wt_connp = NULL;

	if (event_handler == NULL)
		event_handler = __wt_event_handler_default;

	/*
	 * We end up here before we do any real work.   Check the build itself,
	 * and do some global stuff.
	 */
	if (library_init == 0) {
		WT_RET(__wt_library_init());
		library_init = 1;
	}

	/*
	 * !!!
	 * We don't yet have a session handle to pass to the memory allocation
	 * functions.
	 */
	WT_RET(__wt_calloc(NULL, 1, sizeof(CONNECTION), &conn));
	conn->iface = stdc;
	WT_ERR(__wt_strdup(NULL, home, &conn->home));
	TAILQ_INIT(&conn->sessions_head);

	conn->default_session.iface.connection = &conn->iface;
	conn->default_session.event_handler = event_handler;

	/* XXX conn flags, including WT_MEMORY_CHECK */
	WT_ERR(__wt_connection_config(conn));

	WT_ERR(__wt_config_checkone(&conn->default_session,
	    __wt_config_def_wiredtiger_open, config));

	WT_ERR(__wt_config_gets(cfg, "cache_size", &cval));
	WT_ERR(conn->cache_size_set(conn, (uint32_t)cval.val));

	WT_ERR(conn->open(conn, home, 0644, 0));

	WT_ERR(__wt_config_gets(cfg, "logging", &cval));
	if (cval.val != 0)
		WT_ERR(__wt_open(&conn->default_session,
		    "__wt.log", 0666, 1, &conn->log_fh));

	STATIC_ASSERT(offsetof(CONNECTION, iface) == 0);
	*wt_connp = &conn->iface;

	if (0) {
err:		if (conn->home != NULL)
			__wt_free(NULL, conn);
		conn->close(conn, 0);
		__wt_free(NULL, conn);
	}

	return (ret);
}
