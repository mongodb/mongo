/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_api_arg_min(SESSION *, const char *, uint64_t, uint64_t);

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

	SESSION_API_CALL(session, close, config);

	while ((cursor = TAILQ_FIRST(&session->cursors)) != NULL)
		WT_TRET(cursor->close(cursor, config));

	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL) {
		TAILQ_REMOVE(&session->btrees, btree_session, q);
		btree = btree_session->btree;
		__wt_free(session, btree_session);
		session->btree = btree;
		WT_TRET(__wt_btree_close(session));
	}

	__wt_lock(session, conn->mtx);
	if (!F_ISSET(session, WT_SESSION_INTERNAL))
		TAILQ_REMOVE(&conn->sessions_head, session, q);
	WT_TRET(__wt_session_close(session));

	session = &conn->default_session;
	__wt_unlock(session, conn->mtx);

	API_END();

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
	SESSION_API_CALL(session, open_cursor, config);

	if (strncmp(uri, "config:", 7) == 0)
		return (__wt_curconfig_open(session, uri, config, cursorp));
	if (strncmp(uri, "stat:", 5) == 0)
		return (__wt_curstat_open(session, uri, config, cursorp));
	if (strncmp(uri, "table:", 6) == 0)
		return (__wt_curbtree_open(session, uri, config, cursorp));

	__wt_errx(session, "Unknown cursor type '%s'", uri);

	API_END();
	return (EINVAL);
}

/*
 * __session_create --
 *	WT_SESSION->create method.
 */
static int
__session_create(WT_SESSION *wt_session, const char *name, const char *config)
{
	BTREE *btree;
	CONNECTION *conn;
	SESSION *session;
	WT_CONFIG_ITEM cval;
	const char *key_format, *value_format;

	session = (SESSION *)wt_session;
	conn = (CONNECTION *)wt_session->connection;

	SESSION_API_CALL(session, create, config);

	if (strncmp(name, "table:", 6) != 0) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}
	name += 6;

	/* XXX need check whether the table already exists. */

	/*
	 * Key / value formats.
	 *
	 * !!! TODO: these need to be saved in a table-of-tables.
	 * Also, avoiding copies / memory allocation at the moment by
	 * pointing to constant strings for the few cases we handle.
	 */
	WT_RET(__wt_config_gets(__cfg, "key_format", &cval));
	if (strncmp(cval.str, "r", cval.len) == 0)
		key_format = "r";
	else if (strncmp(cval.str, "S", cval.len) == 0)
		key_format = "S";
	else if (strncmp(cval.str, "u", cval.len) == 0)
		key_format = "u";
	else {
		__wt_errx(session, "Unknown key_format '%.*s'",
		    (int)cval.len, cval.str);
		return (EINVAL);
	}

	WT_RET(__wt_config_gets(__cfg, "value_format", &cval));
	if (strncmp(cval.str, "S", cval.len) == 0)
		value_format = "S";
	else if (strncmp(cval.str, "u", cval.len) == 0)
		value_format = "u";
	else if (cval.len > 1 && cval.str[cval.len - 1] == 'u')
		value_format = "u";
	else {
		__wt_errx(session, "Unknown value_format '%.*s'",
		    (int)cval.len, cval.str);
		return (EINVAL);
	}

	/* Allocate a BTREE handle. */
	WT_RET(__wt_connection_btree(conn, &btree));
	WT_RET(__wt_config_collapse(session, __cfg, &btree->config));

	session->btree = btree;
	WT_RET(__wt_btree_open(session, name, 0666, WT_CREATE));

	WT_RET(__wt_session_add_btree(
	    session, btree, key_format, value_format));

	API_END();
	return (0);
}

/*
 * __session_rename --
 *	WT_SESSION->rename method.
 */
static int
__session_rename(WT_SESSION *wt_session,
    const char *oldname, const char *newname, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(oldname);
	WT_UNUSED(newname);
	WT_UNUSED(config);

	return (ENOTSUP);
}

/*
 * __session_drop --
 *	WT_SESSION->drop method.
 */
static int
__session_drop(
    WT_SESSION *wt_session, const char *name, const char *config)
{
	SESSION *session;
	WT_CONFIG_ITEM cval;
	int force, ret;

	session = (SESSION *)wt_session;

	SESSION_API_CALL(session, drop, config);
	if (strncmp(name, "table:", 6) != 0) {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}
	name += strlen("table:");

	WT_RET(__wt_config_gets(__cfg, "force", &cval));
	force = (cval.val != 0);

	/* TODO: Combine the table name with the conn home to make a filename. */

	ret = remove(name);

	API_END();

	return (force ? 0 : ret);
}

/*
 * __session_salvage --
 *	WT_SESSION->salvage method.
 */
static int
__session_salvage(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (0);
}

/*
 * __session_sync --
 *	WT_SESSION->sync method.
 */
static int
__session_sync(WT_SESSION *wt_session, const char *name, const char *config)
{
	WT_UNUSED(wt_session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (0);
}

/*
 * __session_truncate --
 *	WT_SESSION->truncate method.
 */
static int
__session_truncate(WT_SESSION *wt_session,
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
 * __session_verify --
 *	WT_SESSION->verify method.
 */
static int
__session_verify(WT_SESSION *wt_session, const char *name, const char *config)
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
 * __session_log_printf --
 *	WT_SESSION->log_printf method.
 */
static int
__session_log_printf(WT_SESSION *wt_session, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = __wt_log_vprintf((SESSION *)wt_session, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __conn_load_extension --
 *	WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(WT_CONNECTION *wt_conn, const char *path, const char *config)
{
	CONNECTION *conn;
	SESSION *session;

	WT_UNUSED(path);

	conn = (CONNECTION *)wt_conn;
	CONNECTION_API_CALL(conn, session, load_extension, config);
	API_END();

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
	CONNECTION *conn;
	SESSION *session;

	WT_UNUSED(prefix);
	WT_UNUSED(ctype);

	conn = (CONNECTION *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_cursor_type, config);
	API_END();

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
	CONNECTION *conn;
	SESSION *session;

	WT_UNUSED(name);
	WT_UNUSED(collator);

	conn = (CONNECTION *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config);
	API_END();

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
	CONNECTION *conn;
	SESSION *session;

	WT_UNUSED(name);
	WT_UNUSED(extractor);

	conn = (CONNECTION *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config);
	API_END();

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
	SESSION *s, *session;
	WT_SESSION *wt_session;

	ret = 0;
	conn = (CONNECTION *)wt_conn;

	CONNECTION_API_CALL(conn, session, close, config);

	/* Close open sessions. */
	while ((s = TAILQ_FIRST(&conn->sessions_head)) != NULL) {
		if (F_ISSET(s, WT_SESSION_INTERNAL))
			TAILQ_REMOVE(&conn->sessions_head, s, q);
		else {
			wt_session = &s->iface;
			WT_TRET(wt_session->close(wt_session, config));
		}
	}

	WT_TRET(__wt_connection_close(conn));
	API_END();

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
		__session_create,
		__session_drop,
		__session_rename,
		__session_salvage,
		__session_sync,
		__session_truncate,
		__session_verify,
		__session_begin_transaction,
		__session_commit_transaction,
		__session_rollback_transaction,
		__session_checkpoint,
		__session_log_printf,
	};
	CONNECTION *conn;
	SESSION *session, *session_ret;
	int ret;

	conn = (CONNECTION *)wt_conn;
	session_ret = NULL;
	CONNECTION_API_CALL(conn, session, open_session, config);

	__wt_lock(session, conn->mtx);
	WT_ERR(__wt_connection_session(conn, &session_ret));

	/*
	 * XXX
	 * Kludge while there is a separate __wt_conection_session method.
	 * We shouldn't be overwriting the connection pointer, particularly not
	 * through a static struct that is shared between threads.
	 */
	stds.connection = wt_conn;
	session_ret->iface = stds;
	TAILQ_INIT(&session_ret->cursors);
	TAILQ_INIT(&session_ret->btrees);
	WT_ASSERT(NULL, conn->default_session.event_handler != NULL);
	if (event_handler != NULL)
		session_ret->event_handler = event_handler;

	TAILQ_INSERT_HEAD(&conn->sessions_head, session_ret, q);
	__wt_unlock(session, conn->mtx);

	STATIC_ASSERT(offsetof(CONNECTION, iface) == 0);
	*wt_sessionp = &session_ret->iface;

	if (0) {
err:		if (session_ret != NULL)
			(void)__wt_session_close(session_ret);
		__wt_unlock(session, conn->mtx);
		__wt_free(session, session_ret);
	}
	API_END();

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
	static struct {
		const char *vname;
		uint32_t vflag;
	} *vt, verbtypes[] = {
		{ "fileops", WT_VERB_FILEOPS },
		{ "hazard", WT_VERB_HAZARD },
		{ "mutex", WT_VERB_MUTEX },
		{ "read", WT_VERB_READ },
		{ "evict", WT_VERB_EVICT },
		{ NULL, 0 }
	};
	CONNECTION *conn;
	SESSION *session;
	WT_CONFIG vconfig;
	WT_CONFIG_ITEM cval, vkey, vval;
	const char *__cfg[] = { __wt_confdfl_wiredtiger_open, config, NULL };
	int opened, ret;

	opened = 0;
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

	session = &conn->default_session;
	session->iface.connection = &conn->iface;
	session->event_handler = event_handler;
	session->name = "wiredtiger_open";

	WT_ERR(__wt_connection_config(conn));

	WT_ERR(__wt_config_check(session, __cfg[0], config));

	WT_ERR(__wt_config_gets(__cfg, "cache_size", &cval));
	WT_ERR(__wt_api_arg_min(session, "cache size", cval.val,
	    1 * WT_MEGABYTE));
	conn->cache_size = cval.val;

	WT_ERR(__wt_config_gets(__cfg, "data_update_max", &cval));
	conn->data_update_max = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(__cfg, "data_update_min", &cval));
	conn->data_update_min = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(__cfg, "hazard_max", &cval));
	conn->hazard_size = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(__cfg, "session_max", &cval));
	conn->session_size = (uint32_t)cval.val;

	conn->verbose = 0;
#ifdef HAVE_VERBOSE
	WT_ERR(__wt_config_gets(__cfg, "verbose", &cval));
	for (vt = verbtypes; vt->vname != NULL; vt++) {
		WT_ERR(__wt_config_initn(&vconfig, cval.str, cval.len));
		vkey.str = vt->vname;
		vkey.len = strlen(vt->vname);
		ret = __wt_config_getraw(&vconfig, &vkey, &vval);
		if (ret == 0 && vval.val)
			FLD_SET(conn->verbose, vt->vflag);
		else if (ret != WT_NOTFOUND)
			goto err;
	}
#endif

	WT_ERR(__wt_connection_open(conn, home, 0644));
	opened = 1;

	WT_ERR(__wt_config_gets(__cfg, "logging", &cval));
	if (cval.val != 0)
		WT_ERR(__wt_open(session, "__wt.log", 0666, 1, &conn->log_fh));

	STATIC_ASSERT(offsetof(CONNECTION, iface) == 0);
	*wt_connp = &conn->iface;

	if (0) {
err:		if (opened)
			__wt_connection_close(conn);
		else
			__wt_connection_destroy(conn);
	}

	return (ret);
}

/*
 * __wt_api_arg_min --
 *	Print a standard error message when an API function is passed a
 *	too-small argument.
 */
static int
__wt_api_arg_min(
    SESSION *session, const char *arg_name, uint64_t v, uint64_t min)
{
	if (v >= min)
		return (0);

	__wt_errx(session, "%s argument %llu less than minimum value of %llu",
	    arg_name, (unsigned long long)v, (unsigned long long)min);
	return (WT_ERROR);
}
