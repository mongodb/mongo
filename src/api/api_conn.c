/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __conn_config(WT_CONNECTION_IMPL *, const char **, WT_BUF **);
static int __conn_home(WT_CONNECTION_IMPL *, const char *, const char **);
static int __conn_single(WT_CONNECTION_IMPL *, const char **);

/*
 * api_err_printf --
 *	Extension API call to print to the error stream.
 */
static void
__api_err_printf(WT_SESSION *wt_session, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv((WT_SESSION_IMPL *)wt_session, 0, NULL, 0, fmt, ap);
	va_end(ap);
}

static WT_EXTENSION_API __api = {
	__api_err_printf,
	__wt_scr_alloc_ext,
	__wt_scr_free_ext
};

/*
 * __conn_load_extension --
 *	WT_CONNECTION->load_extension method.
 */
static int
__conn_load_extension(
    WT_CONNECTION *wt_conn, const char *path, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DLH *dlh;
	WT_SESSION_IMPL *session;
	int (*entry)(WT_SESSION *, WT_EXTENSION_API *, const char *);
	int ret;
	const char *entry_name;

	dlh = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, load_extension, config, cfg);

	entry_name = NULL;
	WT_ERR(__wt_config_gets(session, cfg, "entry", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &entry_name));

	/*
	 * This assumes the underlying shared libraries are reference counted,
	 * that is, that re-opening a shared library simply increments a ref
	 * count, and closing it simply decrements the ref count, and the last
	 * close discards the reference entirely -- in other words, we do not
	 * check to see if we've already opened this shared library.
	 */
	WT_ERR(__wt_dlopen(session, path, &dlh));
	WT_ERR(__wt_dlsym(session, dlh, entry_name, &entry));

	/* Call the entry function. */
	WT_ERR(entry(&session->iface, &__api, config));

	/* Link onto the environment's list of open libraries. */
	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->dlhqh, dlh, q);
	__wt_unlock(session, conn->mtx);

	if (0) {
err:		if (dlh != NULL)
			(void)__wt_dlclose(session, dlh);
	}
	__wt_free(session, entry_name);

	API_END(session);

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
	int ret;

	WT_UNUSED(prefix);
	WT_UNUSED(ctype);
	ret = ENOTSUP;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_cursor_type, config, cfg);
	WT_UNUSED(cfg);
err:	API_END(session);

	return (ret);
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
	int ret;

	WT_UNUSED(name);
	WT_UNUSED(collator);
	ret = ENOTSUP;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);
err:	API_END(session);

	return (ret);
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
	WT_NAMED_COMPRESSOR *ncomp;
	int ret;

	WT_UNUSED(name);
	WT_UNUSED(compressor);

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_compressor, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_calloc_def(session, 1, &ncomp));
	WT_ERR(__wt_strdup(session, name, &ncomp->name));
	ncomp->compressor = compressor;

	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->compqh, ncomp, q);
	__wt_unlock(session, conn->mtx);
	ncomp = NULL;
err:	API_END(session);
	__wt_free(session, ncomp);
	return (ret);
}

/*
 * __conn_remove_compressor --
 *	remove compressor added by WT_CONNECTION->add_compressor,
 *	only used internally.
 */
static int
__conn_remove_compressor(WT_CONNECTION *wt_conn, WT_COMPRESSOR *compressor)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	WT_NAMED_COMPRESSOR *ncomp;
	int ret;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session = &conn->default_session;

	/* Remove from the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_FOREACH(ncomp, &conn->compqh, q) {
		if (ncomp->compressor == compressor)
			break;
	}
	if (ncomp != NULL)
		TAILQ_REMOVE(&conn->compqh, ncomp, q);
	__wt_unlock(session, conn->mtx);

	/* Free associated memory */
	if (ncomp != NULL) {
		__wt_free(session, ncomp->name);
		__wt_free(session, ncomp);
		ret = 0;
	}
	else
		ret = ENOENT;

	return ret;
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
	int ret;

	WT_UNUSED(name);
	WT_UNUSED(extractor);
	ret = ENOTSUP;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_extractor, config, cfg);
	WT_UNUSED(cfg);
err:	API_END(session);

	return (ret);
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
	return (((WT_CONNECTION_IMPL *)wt_conn)->is_new);
}

/*
 * __conn_close --
 *	WT_CONNECTION->close method.
 */
static int
__conn_close(WT_CONNECTION *wt_conn, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *s, *session, **tp;
	WT_SESSION *wt_session;
	WT_NAMED_COMPRESSOR *ncomp;
	int ret;

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

	/* Free memory for compressors */
	while ((ncomp = TAILQ_FIRST(&conn->compqh)) != NULL)
		WT_TRET(__conn_remove_compressor(wt_conn, ncomp->compressor));

	WT_TRET(__wt_connection_close(conn));
	/* We no longer have a session, don't try to update it. */
	session = NULL;
err:	API_END(session);

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
	ret = 0;

	CONNECTION_API_CALL(conn, session, open_session, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_open_session(conn, 0, event_handler, config, &session_ret));

	*wt_sessionp = &session_ret->iface;
err:	API_END(session);

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
		{ "allocate",	WT_VERB_ALLOCATE },
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
	WT_BUF *cbuf, expath, exconfig;
	WT_CONFIG subconfig;
	WT_CONFIG_ITEM cval, skey, sval;
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	int ret;
	const char *cfg[] =
	    { __wt_confdfl_wiredtiger_open, config, NULL, NULL };

	*wt_connp = NULL;
	session = NULL;
	cbuf = NULL;
	WT_CLEAR(expath);
	WT_CLEAR(exconfig);

	WT_RET(__wt_library_init());

	WT_RET(__wt_calloc_def(NULL, 1, &conn));
	conn->iface = stdc;

	/*
	 * Immediately link the structure into the connection structure list:
	 * the only thing ever looked at on that list is the database name,
	 * and a NULL value is fine.
	 */
	__wt_lock(NULL, __wt_process.mtx);
	TAILQ_INSERT_TAIL(&__wt_process.connqh, conn, q);
	__wt_unlock(NULL, __wt_process.mtx);

	session = &conn->default_session;
	session->iface.connection = &conn->iface;
	session->name = "wiredtiger_open";

	/*
	 * Configure event handling as soon as possible so errors are handled
	 * correctly.  If the application didn't configure an event handler,
	 * use the default one, and use default entries for any entries not
	 * set by the application.
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
	session->event_handler = event_handler;

	/* Remaining basic initialization of the connection structure. */
	WT_ERR(__wt_connection_init(conn));

	/* Check the configuration strings. */
	WT_ERR(
	    __wt_config_check(session, __wt_confchk_wiredtiger_open, config));

	/* Get the database home. */
	WT_ERR(__conn_home(conn, home, cfg));

	/* Read the database-home configuration file. */
	WT_ERR(__conn_config(conn, cfg, &cbuf));

	/* Make sure no other thread of control already owns this database. */
	WT_ERR(__conn_single(conn, cfg));

	WT_ERR(__wt_config_gets(session, cfg, "cache_size", &cval));
	conn->cache_size = cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "hazard_max", &cval));
	conn->hazard_size = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "session_max", &cval));
	conn->session_size = (uint32_t)cval.val;

	WT_ERR(__wt_config_gets(session, cfg, "multithread", &cval));
	if (cval.val != 0)
		F_SET(conn, WT_MULTITHREAD);

	/* Configure verbose flags. */
	conn->verbose = 0;
#ifdef HAVE_VERBOSE
	WT_ERR(__wt_config_gets(session, cfg, "verbose", &cval));
	for (vt = verbtypes; vt->vname != NULL; vt++) {
		WT_ERR(__wt_config_subinit(session, &subconfig, &cval));
		skey.str = vt->vname;
		skey.len = strlen(vt->vname);
		ret = __wt_config_getraw(&subconfig, &skey, &sval);
		if (ret == 0 && sval.val)
			FLD_SET(conn->verbose, vt->vflag);
		else if (ret != WT_NOTFOUND)
			goto err;
	}
#endif

	WT_ERR(__wt_config_gets(session, cfg, "logging", &cval));
	if (cval.val != 0)
		WT_ERR(__wt_open(session, "__wt.log", 0666, 1, &conn->log_fh));

	/* Load any extensions referenced in the config. */
	WT_ERR(__wt_config_gets(session, cfg, "extensions", &cval));
	WT_ERR(__wt_config_subinit(session, &subconfig, &cval));
	while ((ret = __wt_config_next(&subconfig, &skey, &sval)) == 0) {
		__wt_buf_init(session, &expath, 0);
		WT_ERR(__wt_buf_sprintf(session, &expath,
		    "%.*s", (int)skey.len, skey.str));
		if (sval.len > 0)
			WT_ERR(__wt_buf_sprintf(session, &exconfig,
			    "entry=%.*s\n", (int)sval.len, sval.str));
		WT_ERR(conn->iface.load_extension(&conn->iface,
		    expath.data, (sval.len > 0) ? exconfig.data : NULL));
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	WT_ERR(ret);

	/*
	 * Open the connection; if that fails, the connection handle has been
	 * destroyed by the time the open function returns.
	 */
	if ((ret = __wt_connection_open(conn)) != 0) {
		conn = NULL;
		WT_ERR(ret);
	}

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*wt_connp = &conn->iface;

	if (0) {
err:		__wt_connection_destroy(conn);
	}
	if (cbuf != NULL)
		__wt_buf_free(session, cbuf);
	__wt_buf_free(session, &expath);
	__wt_buf_free(session, &exconfig);

	return (ret);
}

/*
 * __conn_home --
 *	Set the database home directory.
 */
static int
__conn_home(WT_CONNECTION_IMPL *conn, const char *home, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	WT_SESSION_IMPL *session;

	session = &conn->default_session;

	/* If the application specifies a home directory, use it. */
	if (home != NULL)
		goto copy;

	/* If there's no WIREDTIGER_HOME environment variable, use ".". */
	if ((home = getenv("WIREDTIGER_HOME")) == NULL) {
		home = ".";
		goto copy;
	}

	/*
	 * Security stuff:
	 *
	 * If the "home_environment" configuration string is set, use the
	 * environment variable for all processes.
	 */
	WT_RET(__wt_config_gets(session, cfg, "home_environment", &cval));
	if (cval.val != 0)
		goto copy;

	/*
	 * If the "home_environment_priv" configuration string is set, use the
	 * environment variable if the process has appropriate privileges.
	 */
	WT_RET(__wt_config_gets(session, cfg, "home_environment_priv", &cval));
	if (cval.val == 0) {
		__wt_errx(session, "%s",
		    "WIREDTIGER_HOME environment variable set but WiredTiger "
		    "not configured to use that environment variable");
		return (WT_ERROR);
	}

	if (!__wt_has_priv()) {
		__wt_errx(session, "%s",
		    "WIREDTIGER_HOME environment variable set but process "
		    "lacks privileges to use that environment variable");
		return (WT_ERROR);
	}

copy:	return (__wt_strdup(session, home, &conn->home));
}

/*
 * __conn_single --
 *	Confirm that no other thread of control is using this database.
 */
static int
__conn_single(WT_CONNECTION_IMPL *conn, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *t;
	WT_SESSION_IMPL *session;
	off_t size;
	uint32_t len;
	int created, exist, ret;
	char buf[256];

	session = &conn->default_session;

#define	WT_FLAGFILE	"WiredTiger"
	/*
	 * We need to check that no other process, or thread in this process,
	 * "owns" this database.
	 *
	 * Check for exclusive creation.
	 */
	WT_RET(__wt_config_gets(session, cfg, "exclusive", &cval));
	if (cval.val) {
		WT_RET(__wt_exist(session, WT_FLAGFILE, &exist));
		if (exist) {
			__wt_errx(session,
			    "%s", "WiredTiger database already exists");
			return (EEXIST);
		}
	}

	/*
	 * Optionally create the wiredtiger flag file if it doesn't already
	 * exist.  We don't actually care if we create it or not, the "am I
	 * the only locker" tests are all that matter.  The tricky part is
	 * the "exclusive" flag, but if another thread, with which we are
	 * racing, does the actual create, and we still win the eventual "am
	 * I the only locker" test, we don't care that we didn't do the actual
	 * physical create of the file, we are the final owner and get to act
	 * as if we created the database.
	 */
	WT_RET(__wt_config_gets(session, cfg, "create", &cval));
	WT_RET(__wt_open(session,
	    WT_FLAGFILE, 0666, cval.val == 0 ? 0 : 1, &conn->lock_fh));

	/*
	 * Lock a byte of the file: if we don't get the lock, some other process
	 * is holding it, we're done.  Note the file may be zero-length length,
	 * and that's OK, the underlying call supports acquisition of locks past
	 * the end-of-file.
	 */
	if (__wt_bytelock(conn->lock_fh, (off_t)0, 1) != 0) {
		__wt_errx(session, "%s",
		    "WiredTiger database is already being managed by another "
		    "process");
		WT_ERR(EBUSY);
	}

	/* Check to see if another thread of control has this database open. */
	ret = 0;
	__wt_lock(session, __wt_process.mtx);
	TAILQ_FOREACH(t, &__wt_process.connqh, q)
		if (t->home != NULL &&
		    t != conn && strcmp(t->home, conn->home) == 0) {
			ret = EBUSY;
			break;
		}
	__wt_unlock(session, __wt_process.mtx);
	if (ret != 0) {
		__wt_errx(session, "%s",
		    "WiredTiger database is already being managed by another "
		    "thread in this process");
		WT_ERR(EBUSY);
	}

	/*
	 * If the size of the file is 0, we created it (or we're racing with
	 * the thread that created it, it doesn't matter), write some bytes
	 * into the file.  Strictly speaking, this isn't even necessary, but
	 * zero-length files always make me nervous.
	 */
	WT_ERR(__wt_filesize(session, conn->lock_fh, &size));
	if (size == 0) {
		len = (uint32_t)snprintf(buf, sizeof(buf), "%s\n%s\n",
		    WT_FLAGFILE, wiredtiger_version(NULL, NULL, NULL));
		WT_ERR(__wt_write(
		    session, conn->lock_fh, (off_t)0, (uint32_t)len, buf));
		created = 1;
	} else
		created = 0;

	/*
	 * If we found a zero-length WiredTiger lock file, and eventually ended
	 * as the database owner, return that we created the database.  (There
	 * is a theoretical chance that another process created the WiredTiger
	 * lock file but we won the race to add the WT_CONNECTION_IMPL structure
	 * to the process' list.  It doesn't much matter, only one thread will
	 * be told it created the database.)
	 */
	conn->is_new = created;

	return (0);

err:	if (conn->lock_fh != NULL) {
		(void)__wt_close(session, conn->lock_fh);
		conn->lock_fh = NULL;
	}
	return (ret);
}

/*
 * __conn_config --
 *	Read in any WiredTiger_config file in the home directory.
 */
static int
__conn_config(WT_CONNECTION_IMPL *conn, const char **cfg, WT_BUF **cbufp)
{
	WT_BUF *cbuf;
	WT_FH *fh;
	WT_SESSION_IMPL *session;
	off_t size;
	uint32_t len;
	int exist, quoted, ret;
	uint8_t *p, *t;

	*cbufp = NULL;				/* Returned buffer */

	cbuf = NULL;
	fh = NULL;
	session = &conn->default_session;
	ret = 0;

	/* Check for an optional configuration file. */
#define	WT_CONFIGFILE	"WiredTiger.config"
	WT_RET(__wt_exist(session, WT_CONFIGFILE, &exist));
	if (!exist)
		return (0);

	/* Open the configuration file. */
	WT_RET(__wt_open(session, WT_CONFIGFILE, 0444, 0, &fh));
	WT_ERR(__wt_filesize(session, fh, &size));
	if (size == 0)
		goto err;

	/*
	 * Sanity test: a 100KB configuration file would be insane.  (There's
	 * no practical reason to limit the file size, but I can either limit
	 * the file size to something rational, or I can add code to test if
	 * the off_t size is larger than a uint32_t, which is more complicated
	 * and a waste of time.)
	 */
	if (size > 100 * 1024) {
		__wt_err(session, EFBIG, WT_CONFIGFILE);
		WT_ERR(EFBIG);
	}
	len = (uint32_t)size;

	/*
	 * Copy the configuration file into memory, with a little slop, I'm not
	 * interested in debugging off-by-ones.
	 *
	 * The beginning of a file is the same as if we run into an unquoted
	 * newline character, simplify the parsing loop by pretending that's
	 * what we're doing.
	 */
	WT_ERR(__wt_scr_alloc(session, len + 10,  &cbuf));
	WT_ERR(
	    __wt_read(session, fh, (off_t)0, len, ((uint8_t *)cbuf->mem) + 1));
	((uint8_t *)cbuf->mem)[0] = '\n';
	cbuf->size = len + 1;

	/*
	 * Collapse the string by replacing newlines with commas, and discarding
	 * any lines where the first non-white-space character is a #.  The only
	 * override is quoted strings; text in double-quotes gets copied without
	 * change.
	 */
	for (quoted = 0, p = t = cbuf->mem; len > 0;) {
		if (quoted || *p == '"') {		/* Quoted strings */
			if (*p == '"')
				quoted = !quoted;
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
		while (*p == '\n') {
			for (*t++ = ','; --len > 0 && isspace(*++p);)
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
		if (len > 0) {
			*t++ = *p++;
			--len;
		}
	}
	*t = '\0';

#if 0
	fprintf(stderr, "file config: {%s}\n", (char *)cbuf->data);
#endif

	/* Check the configuration string. */
	WT_ERR(__wt_config_check(
	    session, __wt_confchk_wiredtiger_open, cbuf->data));

	/*
	 * The configuration file falls between the default configuration and
	 * the wiredtiger_open() configuration, overriding the defaults but not
	 * overriding the wiredtiger_open() configuration.
	 */
	cfg[2] = cfg[1];
	cfg[1] = cbuf->data;

	*cbufp = cbuf;

	if (0) {
err:		if (cbuf != NULL)
			__wt_buf_free(session, cbuf);
	}
	if (fh != NULL)
		WT_TRET(__wt_close(session, fh));
	return (ret);
}
