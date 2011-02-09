/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__session_close(WT_SESSION *session, const char *config)
{
	DB *db;
	ICONNECTION *iconn;
	ISESSION *isession;
	WT_CURSOR_STD *cstd;
	int ret;

	printf("WT_SESSION->close\n");
	iconn = (ICONNECTION *)session->connection;
	isession = (ISESSION *)session;
	ret = 0;

	while ((cstd = TAILQ_FIRST(&isession->cursors)) != NULL)
		WT_TRET(cstd->iface.close(&cstd->iface, config));

	while ((db = TAILQ_FIRST(&isession->btrees)) != NULL) {
		TAILQ_REMOVE(&isession->btrees, db, q);
		WT_TRET(db->close(db, 0));
	}

	if (isession->toc != NULL)
		WT_TRET(__wt_wt_toc_close(isession->toc));

	TAILQ_REMOVE(&iconn->sessions, isession, q);
	__wt_free(NULL, session, sizeof(ISESSION));
	return (0);
}

static int
__session_open_cursor(WT_SESSION *session,
    const char *uri, WT_CURSOR *to_dup, const char *config, WT_CURSOR **cursorp)
{
	ISESSION *isession;

	WT_UNUSED(to_dup);

	isession = (ISESSION *)session;

	if (strncmp(uri, "table:", 6) == 0)
		return (__wt_curtable_open(session, uri, config, cursorp));

	__wt_err(NULL, isession, 0, "Unknown cursor type '%s'\n", uri);
	return (EINVAL);
}

static int
__session_create_table(WT_SESSION *session,
    const char *name, const char *config)
{
	DB *db;
	ENV *env;
	ISESSION *isession;

	WT_UNUSED(config);

	isession = (ISESSION *)session;
	env = ((ICONNECTION *)session->connection)->env;

	WT_RET(env->db(env, 0, &db));
	WT_RET(db->open(db, name, 0, WT_CREATE));

	TAILQ_INSERT_HEAD(&isession->btrees, db, q);

	return (0);
}

static int
__session_rename_table(WT_SESSION *session,
    const char *oldname, const char *newname, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(oldname);
	WT_UNUSED(newname);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__session_drop_table(WT_SESSION *session, const char *name, const char *config)
{
	ISESSION *isession;
	WT_CONFIG_ITEM cvalue;
	int force, ret;

	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	isession = (ISESSION *)session;
	force = 0;

	CONFIG_LOOP(isession, config, cvalue)
		CONFIG_ITEM("force")
			force = (cvalue.val != 0);
	CONFIG_END(isession)

	/* TODO: Combine the table name with the env home to make a filename. */

	ret = remove(name);

	return (force ? 0 : ret);
}

static int
__session_truncate_table(WT_SESSION *session,
    const char *name, WT_CURSOR *start, WT_CURSOR *end, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(start);
	WT_UNUSED(end);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__session_verify_table(WT_SESSION *session, const char *name, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (0);
}

static int
__session_begin_transaction(WT_SESSION *session, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__session_commit_transaction(WT_SESSION *session, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__session_rollback_transaction(WT_SESSION *session, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__session_checkpoint(WT_SESSION *session, const char *config)
{
	WT_UNUSED(session);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__conn_load_extension(WT_CONNECTION *conn, const char *path, const char *config)
{
	WT_UNUSED(conn);
	WT_UNUSED(path);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__conn_add_cursor_factory(WT_CONNECTION *conn,
    const char *prefix, WT_CURSOR_FACTORY *factory, const char *config)
{
	WT_UNUSED(conn);
	WT_UNUSED(prefix);
	WT_UNUSED(factory);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__conn_add_collator(WT_CONNECTION *conn,
    const char *name, WT_COLLATOR *collator, const char *config)
{
	WT_UNUSED(conn);
	WT_UNUSED(name);
	WT_UNUSED(collator);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__conn_add_extractor(WT_CONNECTION *conn,
    const char *name, WT_EXTRACTOR *extractor, const char *config)
{
	WT_UNUSED(conn);
	WT_UNUSED(name);
	WT_UNUSED(extractor);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static const char *
__conn_get_home(WT_CONNECTION *conn)
{
	return (((ICONNECTION *)conn)->home);
}

static int
__conn_is_new(WT_CONNECTION *conn)
{
	WT_UNUSED(conn);

	return (0);
}

static int
__conn_close(WT_CONNECTION *conn, const char *config)
{
	int ret;
	ICONNECTION *iconn;
	ISESSION *isession;
	WT_SESSION *session;

	ret = 0;
	iconn = (ICONNECTION *)conn;

	while ((isession = TAILQ_FIRST(&iconn->sessions)) != NULL) {
		session = (WT_SESSION *)isession;
		WT_TRET(session->close(session, config));
	}

	__wt_free(iconn->env, iconn->home, 0);
	WT_TRET(iconn->env->close(iconn->env, 0));
	__wt_free(NULL, iconn, sizeof(ICONNECTION));
	return (ret);
}

static int
__conn_open_session(WT_CONNECTION *conn,
    WT_ERROR_HANDLER *error_handler, const char *config, WT_SESSION **sessionp)
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
	ICONNECTION *iconn;
	ISESSION *isession;
	int ret;

	WT_UNUSED(error_handler);
	WT_UNUSED(config);

	iconn = (ICONNECTION *)conn;

	WT_RET(__wt_calloc(iconn->env, 1, sizeof(ISESSION), &isession));
	TAILQ_INIT(&isession->cursors);
	TAILQ_INIT(&isession->btrees);

	isession->iface = stds;
	isession->iface.connection = conn;

	isession->error_handler = (error_handler != NULL) ?
	    error_handler : iconn->env->error_handler;

	WT_ERR(__wt_env_toc(iconn->env, &isession->toc));

	TAILQ_INSERT_HEAD(&iconn->sessions, isession, q);

	STATIC_ASSERT(offsetof(ICONNECTION, iface) == 0);
	*sessionp = &isession->iface;

	if (0) {
err:		if (isession->toc != NULL)
			(void)__wt_wt_toc_close(isession->toc);
		__wt_free(iconn->env, isession, sizeof(ISESSION));
	}

	return (0);
}

int
wiredtiger_open(const char *home, WT_ERROR_HANDLER *error_handler,
    const char *config, WT_CONNECTION **connectionp)
{
	static WT_CONNECTION stdc = {
		__conn_load_extension,
		__conn_add_cursor_factory,
		__conn_add_collator,
		__conn_add_extractor,
		__conn_close,
		__conn_get_home,
		__conn_is_new,
		__conn_open_session
	};
	ICONNECTION *iconn;
	int ret;

	WT_UNUSED(home);
	WT_UNUSED(config);

	WT_RET(__wt_calloc(NULL, 1, sizeof(ICONNECTION), &iconn));
	WT_ERR(__wt_strdup(NULL, home, &iconn->home));
	TAILQ_INIT(&iconn->sessions);

	/* XXX env flags? */
	WT_ERR(wiredtiger_env_init(&iconn->env, 0));

	iconn->env->error_handler = (error_handler != NULL) ?
	    error_handler : __wt_error_handler_default;

	/* XXX configure cache size */

	WT_ERR(iconn->env->open(iconn->env, home, 0, 0));

	STATIC_ASSERT(offsetof(ICONNECTION, iface) == 0);
	iconn->iface = stdc;
	*connectionp = &iconn->iface;

	if (0) {
err:		if (iconn->home != NULL)
			__wt_free(NULL, iconn, 0);
		if (iconn->env != NULL)
			iconn->env->close(iconn->env, 0);
		__wt_free(NULL, iconn, sizeof(ICONNECTION));
	}

	return (ret);
}
