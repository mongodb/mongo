/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __conn_load_extension --
 *	WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(WT_CONNECTION *wt_conn,
    const char *path, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	const char *entry_name;
	char namebuf[100];
	int (*entry)(WT_CONNECTION *, const char *);
	void *p;
	WT_CONFIG_ITEM cval;
	WT_DLH *dlh;
	int ret;

	WT_UNUSED(path);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, load_extension, config, cfg);

	entry_name = "wiredtiger_extension_init";
	WT_ERR(__wt_config_gets(session, cfg, "entry", &cval));
	if (cval.len > 0) {
		if (snprintf(namebuf, sizeof(namebuf), "%.*s",
		    (int)cval.len, cval.str) >= (int)sizeof (namebuf)) {
			__wt_errx(session,
			    "extension entry name too long: %.*s",
			    (int)cval.len, cval.str);
			ret = EINVAL;
			goto err;
		} else
			entry_name = namebuf;
	}

	WT_ERR(__wt_dlopen(session, path, &dlh));
	WT_ERR(__wt_dlsym(session, dlh, entry_name, &p));
	entry = p;
	entry(wt_conn, config);
err:	API_END(session);

	return (ret);
}

/*
 * __conn_add_cursor_type --
 *	WT_CONNECTION->add_cursor_type method.
 */
static int
__conn_add_cursor_type(WT_CONNECTION *wt_conn,
    const char *prefix, WT_CURSOR_TYPE *ctype, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	WT_UNUSED(prefix);
	WT_UNUSED(ctype);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_cursor_type, config, cfg);
	WT_UNUSED(cfg);
	API_END(session);

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
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	WT_UNUSED(name);
	WT_UNUSED(collator);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);
	API_END(session);

	return (ENOTSUP);
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
	WT_SESSION_IMPL *session;

	WT_UNUSED(name);
	WT_UNUSED(compressor);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);
	API_END(session);

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
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;

	WT_UNUSED(name);
	WT_UNUSED(extractor);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);
	API_END(session);

	return (ENOTSUP);
}

static const char *
__conn_get_home(WT_CONNECTION *wt_conn)
{
	return (((WT_CONNECTION_IMPL *)wt_conn)->home);
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
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *s, *session, **tp;
	WT_SESSION *wt_session;

	ret = 0;
	conn = (WT_CONNECTION_IMPL *)wt_conn;

	CONNECTION_API_CALL(conn, session, close, config, cfg);
	WT_UNUSED(cfg);

	/* Close open sessions. */
	for (tp = conn->sessions; (s = *tp) != NULL;) {
		if (!F_ISSET(s, WT_SESSION_INTERNAL)) {
			wt_session = &s->iface;
			WT_TRET(wt_session->close(wt_session, config));

			/*
			 * We closed a session, which has shuffled pointers
			 * around.  Restart the search.
			 */
			tp = conn->sessions;
		} else
			++tp;
	}

	WT_TRET(__wt_connection_close(conn));
	/* We no longer have a session, don't try to update it. */
	session = NULL;
	API_END(session);

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
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session, *session_ret;
	int ret;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session_ret = NULL;
	CONNECTION_API_CALL(conn, session, open_session, config, cfg);
	WT_UNUSED(cfg);

	ret = 0;
	__wt_lock(session, conn->mtx);
	WT_TRET(__wt_open_session(conn, event_handler, config, &session_ret));
	__wt_unlock(session, conn->mtx);

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*wt_sessionp = &session_ret->iface;
	API_END(session);

	return (ret);
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
		__conn_add_compressor,
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
		{ "evictserver",WT_VERB_EVICTSERVER },
		{ "fileops",	WT_VERB_FILEOPS },
		{ "hazard",	WT_VERB_HAZARD },
		{ "mutex",	WT_VERB_MUTEX },
		{ "read",	WT_VERB_READ },
		{ "readserver",	WT_VERB_READSERVER },
		{ "reconcile",	WT_VERB_RECONCILE },
		{ "salvage",	WT_VERB_SALVAGE },
		{ "write",	WT_VERB_WRITE },
		{ NULL, 0 }
	};
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	WT_CONFIG subconfig;
	WT_CONFIG_ITEM cval, skey, sval;
	const char *cfg[] = { __wt_confdfl_wiredtiger_open, config, NULL };
	char expath[256], exconfig[100];
	int opened, ret;

	opened = 0;
	*wt_connp = NULL;

	/*
	 * If the application didn't configure an event handler, use the default
	 * one, use the default entries for any not set by the application.
	 */
	if (event_handler == NULL)
		event_handler = __wt_event_handler_default;
	else {
		if (event_handler->handle_error == NULL)
			event_handler->handle_error =
			    __wt_event_handler_default->handle_error;
		if (event_handler->handle_message == NULL)
			event_handler->handle_message =
			    __wt_event_handler_default->handle_message;
		if (event_handler->handle_progress == NULL)
			event_handler->handle_progress =
			    __wt_event_handler_default->handle_progress;
	}

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
	WT_RET(__wt_calloc_def(NULL, 1, &conn));
	conn->iface = stdc;
	WT_ERR(__wt_strdup(NULL, home, &conn->home));

	session = &conn->default_session;
	session->iface.connection = &conn->iface;
	session->event_handler = event_handler;
	session->name = "wiredtiger_open";

	WT_ERR(__wt_connection_config(conn));

	WT_ERR(
	   __wt_config_check(session, __wt_confchk_wiredtiger_open, config));

	WT_ERR(__wt_config_gets(session, cfg, "cache_size", &cval));
	conn->cache_size = cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "hazard_max", &cval));
	conn->hazard_size = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "session_max", &cval));
	conn->session_size = (uint32_t)cval.val;

	conn->verbose = 0;
#ifdef HAVE_VERBOSE
	WT_ERR(__wt_config_gets(session, cfg, "verbose", &cval));
	for (vt = verbtypes; vt->vname != NULL; vt++) {
		WT_ERR(
		    __wt_config_subinit(session, &subconfig, &cval));
		skey.str = vt->vname;
		skey.len = strlen(vt->vname);
		ret = __wt_config_getraw(&subconfig, &skey, &sval);
		if (ret == 0 && sval.val)
			FLD_SET(conn->verbose, vt->vflag);
		else if (ret != WT_NOTFOUND)
			goto err;
	}
#endif

	WT_ERR(__wt_connection_open(conn, home, 0644));
	opened = 1;

	WT_ERR(__wt_config_gets(session, cfg, "logging", &cval));
	if (cval.val != 0)
		WT_ERR(__wt_open(session, "__wt.log", 0666, 1, &conn->log_fh));

	/* Load any extensions referenced in the config. */
	WT_ERR(__wt_config_gets(session, cfg, "extensions", &cval));
	WT_ERR(__wt_config_subinit(session, &subconfig, &cval));
	while ((ret = __wt_config_next(&subconfig, &skey, &sval)) == 0) {
		if (snprintf(expath, sizeof(expath), "%.*s",
		    (int)skey.len, skey.str) >= (int)sizeof (expath)) {
			__wt_err(session, ret = EINVAL,
			    "extension filename too long: %.*s",
			    (int)skey.len, skey.str);
			goto err;
		}
		if (sval.len > 0 &&
		    snprintf(exconfig, sizeof(exconfig), "entry=%.*s\n",
		    (int)sval.len, sval.str) > (int)sizeof (exconfig)) {
			__wt_err(session, ret = EINVAL,
			    "extension name too long: %.*s",
			    (int)skey.len, skey.str);
			goto err;
		}
		WT_ERR(conn->iface.load_extension(&conn->iface, expath,
		    (sval.len > 0) ? exconfig : NULL));
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	else if (ret != 0)
		goto err;

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*wt_connp = &conn->iface;

	if (0) {
err:		if (opened)
			__wt_connection_close(conn);
		else
			__wt_connection_destroy(conn);
	}

	return (ret);
}
