/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __conn_verbose_config(WT_SESSION_IMPL *, const char *[]);

/*
 * api_err_printf --
 *	Extension API call to print to the error stream.
 */
static int
__api_err_printf(WT_SESSION *wt_session, const char *fmt, ...)
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_verrx((WT_SESSION_IMPL *)wt_session, fmt, ap);
	va_end(ap);
	return (ret);
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
	WT_DECL_RET;
	WT_DLH *dlh;
	WT_SESSION_IMPL *session;
	int (*entry)(WT_SESSION *, WT_EXTENSION_API *, const char *);
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
	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->dlhqh, dlh, q);
	__wt_spin_unlock(session, &conn->api_lock);

	if (0) {
err:		if (dlh != NULL)
			WT_TRET(__wt_dlclose(session, dlh));
	}
	__wt_free(session, entry_name);

	API_END_NOTFOUND_MAP(session, ret);
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

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_collator, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_calloc_def(session, 1, &ncoll));
	WT_ERR(__wt_strdup(session, name, &ncoll->name));
	ncoll->collator = collator;

	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->collqh, ncoll, q);
	__wt_spin_unlock(session, &conn->api_lock);
	ncoll = NULL;
err:	__wt_free(session, ncoll);

	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_remove_collator --
 *	remove collator added by WT_CONNECTION->add_collator,
 *	only used internally.
 */
static void
__conn_remove_collator(WT_CONNECTION_IMPL *conn, WT_NAMED_COLLATOR *ncoll)
{
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* Remove from the connection's list. */
	TAILQ_REMOVE(&conn->collqh, ncoll, q);

	/* Free associated memory */
	__wt_free(session, ncoll->name);
	__wt_free(session, ncoll);
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

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_compressor, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_calloc_def(session, 1, &ncomp));
	WT_ERR(__wt_strdup(session, name, &ncomp->name));
	ncomp->compressor = compressor;

	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->compqh, ncomp, q);
	__wt_spin_unlock(session, &conn->api_lock);
	ncomp = NULL;
err:	__wt_free(session, ncomp);

	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_remove_compressor --
 *	remove compressor added by WT_CONNECTION->add_compressor,
 *	only used internally.
 */
static void
__conn_remove_compressor(WT_CONNECTION_IMPL *conn, WT_NAMED_COMPRESSOR *ncomp)
{
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* Remove from the connection's list. */
	TAILQ_REMOVE(&conn->compqh, ncomp, q);

	/* Free associated memory */
	__wt_free(session, ncomp->name);
	__wt_free(session, ncomp);
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
	WT_SESSION_IMPL *session;
	WT_NAMED_DATA_SOURCE *ndsrc;

	ndsrc = NULL;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_data_source, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_calloc_def(session, 1, &ndsrc));
	WT_ERR(__wt_strdup(session, prefix, &ndsrc->prefix));
	ndsrc->dsrc = dsrc;

	/* Link onto the environment's list of data sources. */
	__wt_spin_lock(session, &conn->api_lock);
	TAILQ_INSERT_TAIL(&conn->dsrcqh, ndsrc, q);
	__wt_spin_unlock(session, &conn->api_lock);

	if (0) {
err:		if (ndsrc != NULL)
			__wt_free(session, ndsrc->prefix);
		__wt_free(session, ndsrc);
	}

	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_remove_compressor --
 *	remove compressor added by WT_CONNECTION->add_compressor,
 *	only used internally.
 */
static void
__conn_remove_data_source(
    WT_CONNECTION_IMPL *conn, WT_NAMED_DATA_SOURCE *ndsrc)
{
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	/* Remove from the connection's list. */
	TAILQ_REMOVE(&conn->dsrcqh, ndsrc, q);
	__wt_free(session, ndsrc->prefix);
	__wt_free(session, ndsrc);
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
	WT_SESSION_IMPL *session;

	WT_UNUSED(name);
	WT_UNUSED(extractor);
	ret = ENOTSUP;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	CONNECTION_API_CALL(conn, session, add_extractor, config, cfg);
	WT_UNUSED(cfg);

err:	API_END_NOTFOUND_MAP(session, ret);
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
	WT_DECL_RET;
	WT_NAMED_COLLATOR *ncoll;
	WT_NAMED_COMPRESSOR *ncomp;
	WT_NAMED_DATA_SOURCE *ndsrc;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *s, *session;
	uint32_t i;

	conn = (WT_CONNECTION_IMPL *)wt_conn;

	CONNECTION_API_CALL(conn, session, close, config, cfg);
	WT_UNUSED(cfg);

	/*
	 * Close open, external sessions.
	 * Additionally, the session's hazard pointer memory isn't discarded
	 * during normal session close because access to it isn't serialized.
	 * Discard it now.  Note the loop for the hazard pointer memory, it's
	 * the entire session array, not only the active session count, as the
	 * active session count may be less than the maximum session count.
	 */
	for (s = conn->sessions, i = 0; i < conn->session_cnt; ++s, ++i)
		if (s->active && !F_ISSET(s, WT_SESSION_INTERNAL)) {
			wt_session = &s->iface;
			WT_TRET(wt_session->close(wt_session, config));
		}
	for (s = conn->sessions, i = 0; i < conn->session_size; ++s, ++i)
		if (!F_ISSET(s, WT_SESSION_INTERNAL))
			__wt_free(session, s->hazard);

	/* Clean up open LSM handles. */
	WT_ERR(__wt_lsm_cleanup(&conn->iface));

	/* Close open data handles. */
	WT_TRET(__wt_conn_dhandle_discard(conn));

	/* Free memory for collators */
	while ((ncoll = TAILQ_FIRST(&conn->collqh)) != NULL)
		__conn_remove_collator(conn, ncoll);

	/* Free memory for compressors */
	while ((ncomp = TAILQ_FIRST(&conn->compqh)) != NULL)
		__conn_remove_compressor(conn, ncomp);

	/* Free memory for data sources */
	while ((ndsrc = TAILQ_FIRST(&conn->dsrcqh)) != NULL)
		__conn_remove_data_source(conn, ndsrc);

	WT_TRET(__wt_connection_close(conn));
	/* We no longer have a session, don't try to update it. */
	session = NULL;

err:	API_END_NOTFOUND_MAP(session, ret);
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
	const char *raw_cfg[] = { config, NULL };

	conn = (WT_CONNECTION_IMPL *)wt_conn;

	CONNECTION_API_CALL(conn, session, reconfigure, config, cfg);
	WT_UNUSED(cfg);

	/*
	 * Don't include the default config: only override values the
	 * application sets explicitly.
	 */
	WT_ERR(__wt_conn_cache_pool_config(session, cfg));
	WT_ERR(__wt_cache_config(conn, raw_cfg));
	WT_ERR(__conn_verbose_config(session, cfg));
	/* Wake up the cache pool server so any changes are noticed. */
	if (F_ISSET(conn, WT_CONN_CACHE_POOL))
		WT_ERR(__wt_cond_signal(
		    session, __wt_process.cache_pool->cache_pool_cond));

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
	WT_DECL_RET;
	WT_SESSION_IMPL *session, *session_ret;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session_ret = NULL;

	CONNECTION_API_CALL(conn, session, open_session, config, cfg);
	WT_UNUSED(cfg);

	WT_ERR(__wt_open_session(conn, 0, event_handler, config, &session_ret));

	*wt_sessionp = &session_ret->iface;

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __conn_config_file --
 *	Read in any WiredTiger_config file in the home directory.
 */
static int
__conn_config_file(WT_SESSION_IMPL *session, const char **cfg, WT_ITEM **cbufp)
{
	WT_DECL_ITEM(cbuf);
	WT_DECL_RET;
	WT_FH *fh;
	off_t size;
	uint32_t len;
	int exist, quoted;
	uint8_t *p, *t;

	*cbufp = NULL;				/* Returned buffer */

	fh = NULL;

	/* Check for an optional configuration file. */
#define	WT_CONFIGFILE	"WiredTiger.config"
	WT_RET(__wt_exist(session, WT_CONFIGFILE, &exist));
	if (!exist)
		return (0);

	/* Open the configuration file. */
	WT_RET(__wt_open(session, WT_CONFIGFILE, 0, 0, 0, &fh));
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
	if (size > 100 * 1024)
		WT_ERR_MSG(session, EFBIG, WT_CONFIGFILE);
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
	 * Collapse the file's lines into a single string: newline characters
	 * are replaced with commas unless the newline is quoted or backslash
	 * escaped.  Comment lines (an unescaped newline where the next non-
	 * white-space character is a hash), are discarded.
	 */
	for (quoted = 0, p = t = cbuf->mem; len > 0;) {
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
	}
	*t = '\0';

#if 0
	fprintf(stderr, "file config: {%s}\n", (const char *)cbuf->data);
	exit(0);
#endif

	/* Check the configuration string. */
	WT_ERR(__wt_config_check(
	    session, __wt_confchk_wiredtiger_open, cbuf->data, 0));

	/*
	 * The configuration file falls between the default configuration and
	 * the wiredtiger_open() configuration, overriding the defaults but not
	 * overriding the wiredtiger_open() configuration.
	 */
	while (cfg[1] != NULL)
		++cfg;
	cfg[1] = cfg[0];
	cfg[0] = cbuf->data;

	*cbufp = cbuf;

	if (0) {
err:		if (cbuf != NULL)
			__wt_buf_free(session, cbuf);
	}
	if (fh != NULL)
		WT_TRET(__wt_close(session, fh));
	return (ret);
}

/*
 * __conn_config_env --
 *	Read configuration from an environment variable, if set.
 */
static int
__conn_config_env(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	const char *env_config;

	if ((env_config = getenv("WIREDTIGER_CONFIG")) == NULL ||
	    strlen(env_config) == 0)
		return (0);

	/*
	 * Security stuff:
	 *
	 * If the "use_environment_priv" configuration string is set, use the
	 * environment variable if the process has appropriate privileges.
	 */
	WT_RET(__wt_config_gets(session, cfg, "use_environment_priv", &cval));
	if (cval.val == 0 && __wt_has_priv())
		WT_RET_MSG(session, WT_ERROR, "%s",
		    "WIREDTIGER_CONFIG environment variable set but process "
		    "lacks privileges to use that environment variable");

	/* Check the configuration string. */
	WT_RET(__wt_config_check(
	    session, __wt_confchk_wiredtiger_open, env_config, 0));

	/*
	 * The environment setting comes second-to-last: it overrides the
	 * WiredTiger.config file (if any), but not the config passed by the
	 * application.
	 */
	while (cfg[1] != NULL)
		++cfg;
	cfg[1] = cfg[0];
	cfg[0] = env_config;

	return (0);
}

/*
 * __conn_home --
 *	Set the database home directory.
 */
static int
__conn_home(WT_SESSION_IMPL *session, const char *home, const char **cfg)
{
	WT_CONFIG_ITEM cval;

	/* If the application specifies a home directory, use it. */
	if (home != NULL)
		goto copy;

	/* If there's no WIREDTIGER_HOME environment variable, use ".". */
	if ((home = getenv("WIREDTIGER_HOME")) == NULL || strlen(home) == 0) {
		home = ".";
		goto copy;
	}

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
__conn_single(WT_SESSION_IMPL *session, const char **cfg)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn, *t;
	WT_DECL_RET;
	off_t size;
	uint32_t len;
	int created;
	char buf[256];

	conn = S2C(session);

	/*
	 * Optionally create the wiredtiger flag file if it doesn't already
	 * exist.  We don't actually care if we create it or not, the "am I the
	 * only locker" tests are all that matter.
	 */
	WT_RET(__wt_config_gets(session, cfg, "create", &cval));
	WT_RET(__wt_open(session,
	    WT_SINGLETHREAD, cval.val == 0 ? 0 : 1, 0, 0, &conn->lock_fh));

	/*
	 * Lock a byte of the file: if we don't get the lock, some other process
	 * is holding it, we're done.  Note the file may be zero-length length,
	 * and that's OK, the underlying call supports acquisition of locks past
	 * the end-of-file.
	 */
	if (__wt_bytelock(conn->lock_fh, (off_t)0, 1) != 0)
		WT_ERR_MSG(session, EBUSY, "%s",
		    "WiredTiger database is already being managed by another "
		    "process");

	/* Check to see if another thread of control has this database open. */
	__wt_spin_lock(session, &__wt_process.spinlock);
	TAILQ_FOREACH(t, &__wt_process.connqh, q)
		if (t->home != NULL &&
		    t != conn && strcmp(t->home, conn->home) == 0) {
			ret = EBUSY;
			break;
		}
	__wt_spin_unlock(session, &__wt_process.spinlock);
	if (ret != 0)
		WT_ERR_MSG(session, EBUSY, "%s",
		    "WiredTiger database is already being managed by another "
		    "thread in this process");

	/*
	 * If the size of the file is 0, we created it (or we're racing with
	 * the thread that created it, it doesn't matter), write some bytes
	 * into the file.  Strictly speaking, this isn't even necessary, but
	 * zero-length files always make me nervous.
	 */
	WT_ERR(__wt_filesize(session, conn->lock_fh, &size));
	if (size == 0) {
		len = (uint32_t)snprintf(buf, sizeof(buf), "%s\n%s\n",
		    WT_SINGLETHREAD, wiredtiger_version(NULL, NULL, NULL));
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
		WT_TRET(__wt_close(session, conn->lock_fh));
		conn->lock_fh = NULL;
	}
	return (ret);
}

/*
 * __conn_verbose_config --
 *	Set verbose configuration.
 */
static int
__conn_verbose_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	static struct {
		const char *name;
		uint32_t flag;
	} *ft, verbtypes[] = {
		{ "block",	WT_VERB_block },
		{ "shared_cache",WT_VERB_shared_cache },
		{ "ckpt",	WT_VERB_ckpt },
		{ "evict",	WT_VERB_evict },
		{ "evictserver",WT_VERB_evictserver },
		{ "fileops",	WT_VERB_fileops },
		{ "hazard",	WT_VERB_hazard },
		{ "lsm",	WT_VERB_lsm },
		{ "mutex",	WT_VERB_mutex },
		{ "read",	WT_VERB_read },
		{ "reconcile",	WT_VERB_reconcile },
		{ "salvage",	WT_VERB_salvage },
		{ "verify",	WT_VERB_verify },
		{ "write",	WT_VERB_write },
		{ NULL, 0 }
	};

	conn = S2C(session);

	WT_RET_NOTFOUND_OK(__wt_config_gets(session, cfg, "verbose", &cval));
	for (ft = verbtypes; ft->name != NULL; ft++) {
		if ((ret = __wt_config_subgets(
		    session, &cval, ft->name, &sval)) == 0 && sval.val != 0)
			FLD_SET(conn->verbose, ft->flag);
		else
			FLD_CLR(conn->verbose, ft->flag);

		WT_RET_NOTFOUND_OK(ret);
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
	static WT_CONNECTION stdc = {
		__conn_close,
		__conn_reconfigure,
		__conn_get_home,
		__conn_is_new,
		__conn_open_session,
		__conn_load_extension,
		__conn_add_data_source,
		__conn_add_collator,
		__conn_add_compressor,
		__conn_add_extractor
	};
	static struct {
		const char *name;
		uint32_t flag;
	} *ft, directio_types[] = {
		{ "data",	WT_DIRECTIO_DATA },
		{ "log",	WT_DIRECTIO_LOG },
		{ NULL, 0 }
	};
	WT_CONFIG subconfig;
	WT_CONFIG_ITEM cval, skey, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_ITEM *cbuf, expath, exconfig;
	WT_SESSION_IMPL *session;
	const char *cfg[] =
	    { __wt_confdfl_wiredtiger_open, config, NULL, NULL, NULL };
	int exist;

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
	__wt_spin_lock(NULL, &__wt_process.spinlock);
	TAILQ_INSERT_TAIL(&__wt_process.connqh, conn, q);
	__wt_spin_unlock(NULL, &__wt_process.spinlock);

	session = conn->default_session = &conn->dummy_session;
	session->iface.connection = &conn->iface;
	session->name = "wiredtiger_open";
	__wt_event_handler_set(session, event_handler);

	/* Remaining basic initialization of the connection structure. */
	WT_ERR(__wt_connection_init(conn));

	/* Check the configuration strings. */
	WT_ERR(__wt_config_check(
	    session, __wt_confchk_wiredtiger_open, config, 0));

	/* Get the database home. */
	WT_ERR(__conn_home(session, home, cfg));

	/* Make sure no other thread of control already owns this database. */
	WT_ERR(__conn_single(session, cfg));

	/* Read the database-home configuration file. */
	WT_ERR(__conn_config_file(session, cfg, &cbuf));

	/* Read the environment variable configuration. */
	WT_ERR(__conn_config_env(session, cfg));

	WT_ERR(__wt_config_gets(session, cfg, "hazard_max", &cval));
	conn->hazard_max = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "session_max", &cval));
	conn->session_size = (uint32_t)cval.val + WT_NUM_INTERNAL_SESSIONS;
	WT_ERR(__wt_config_gets(session, cfg, "lsm_merge", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_LSM_MERGE);
	WT_ERR(__wt_config_gets(session, cfg, "sync", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_SYNC);
	WT_ERR(__wt_config_gets(session, cfg, "transactional", &cval));
	if (cval.val)
		F_SET(conn, WT_CONN_TRANSACTIONAL);

	/* Configure verbose flags. */
	WT_ERR(__conn_verbose_config(session, cfg));

	WT_ERR(__wt_conn_cache_pool_config(session, cfg));

	WT_ERR(__wt_config_gets(session, cfg, "logging", &cval));
	if (cval.val != 0)
		WT_ERR(__wt_open(
		   session, WT_LOG_FILENAME, 1, 0, 0, &conn->log_fh));

	/* Configure direct I/O and buffer alignment. */
	WT_ERR(__wt_config_gets(session, cfg, "buffer_alignment", &cval));
	if (cval.val == -1)
		conn->buffer_alignment = WT_BUFFER_ALIGNMENT_DEFAULT;
	else
		conn->buffer_alignment = (size_t)cval.val;
#ifndef HAVE_POSIX_MEMALIGN
	if (conn->buffer_alignment != 0)
		WT_ERR_MSG(session, EINVAL,
		    "buffer_alignment requires posix_memalign");
#endif

	WT_ERR(__wt_config_gets(session, cfg, "direct_io", &cval));
	for (ft = directio_types; ft->name != NULL; ft++) {
		ret = __wt_config_subgets(session, &cval, ft->name, &sval);
		if (ret == 0) {
			if (sval.val)
				FLD_SET(conn->direct_io, ft->flag);
		} else if (ret != WT_NOTFOUND)
			goto err;
	}

	/* Configure mmap. */
	WT_ERR(__wt_config_gets(session, cfg, "mmap", &cval));
	conn->mmap = cval.val == 0 ? 0 : 1;

	/* Load any extensions referenced in the config. */
	WT_ERR(__wt_config_gets(session, cfg, "extensions", &cval));
	WT_ERR(__wt_config_subinit(session, &subconfig, &cval));
	while ((ret = __wt_config_next(&subconfig, &skey, &sval)) == 0) {
		WT_ERR(__wt_buf_fmt(
		    session, &expath, "%.*s", (int)skey.len, skey.str));
		if (sval.len > 0)
			WT_ERR(__wt_buf_fmt(session, &exconfig,
			    "entry=%.*s\n", (int)sval.len, sval.str));
		WT_ERR(conn->iface.load_extension(&conn->iface,
		    expath.data, (sval.len > 0) ? exconfig.data : NULL));
	}
	WT_ERR_NOTFOUND_OK(ret);

	/*
	 * Open the connection; if that fails, the connection handle has been
	 * destroyed by the time the open function returns.
	 */
	if ((ret = __wt_connection_open(conn, cfg)) != 0) {
		conn = NULL;
		WT_ERR(ret);
	}

	/* Open the default session. */
	WT_ERR(__wt_open_session(conn, 1, NULL, NULL, &conn->default_session));
	session = conn->default_session;

	/*
	 * Check on the turtle and metadata files, creating them if necessary
	 * (which avoids application threads racing to create the metadata file
	 * later).
	 */
	WT_ERR(__wt_meta_turtle_init(session, &exist));
	if (!exist) {
		/*
		 * We're single-threaded, but acquire the schema lock
		 * regardless: the lower level code checks that it is
		 * appropriately synchronized.
		 */
		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_schema_create(session, WT_METADATA_URI, NULL));
		WT_ERR(ret);
	}
	WT_ERR(__wt_metadata_open(session));

	/* If there's a hot-backup file, load it. */
	WT_ERR(__wt_metadata_load_backup(session));

	/*
	 * XXX LSM initialization.
	 * This is structured so that it could be moved to an extension.
	 */
	WT_ERR(__wt_lsm_init(&conn->iface, NULL));

	STATIC_ASSERT(offsetof(WT_CONNECTION_IMPL, iface) == 0);
	*wt_connp = &conn->iface;

	/*
	 * Destroying the connection on error will destroy our session handle,
	 * cleanup using the session handle first, then discard the connection.
	 */
err:	if (cbuf != NULL)
		__wt_buf_free(session, cbuf);
	__wt_buf_free(session, &expath);
	__wt_buf_free(session, &exconfig);

	if (ret != 0 && conn != NULL)
		WT_TRET(__wt_connection_destroy(conn));

	return (ret);
}
