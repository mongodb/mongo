/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ckpt_server_config --
 *	Parse and setup the checkpoint server options.
 */
static int
__ckpt_server_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * The checkpoint configuration requires a wait time -- if it's not set,
	 * we're not running at all.
	 */
	WT_RET(__wt_config_gets(session, cfg, "checkpoint.wait", &cval));
	if (cval.val == 0) {
		*runp = 0;
		return (0);
	}
	conn->ckpt_usecs = (long)cval.val * 1000000;
	*runp = 1;

	WT_RET(__wt_config_gets(session, cfg, "checkpoint.name", &cval));

	if (!WT_STRING_MATCH(WT_CHECKPOINT, cval.str, cval.len)) {
		WT_RET(__wt_scr_alloc(session, cval.len + 20, &tmp));
		strcpy((char *)tmp->data, "name=");
		strncat((char *)tmp->data, cval.str, cval.len);
		ret = __wt_strndup(session,
		    tmp->data, strlen("name=") + cval.len, &conn->ckpt_config);
		__wt_scr_free(&tmp);
		WT_RET(ret);
	}

	return (0);
}

/*
 * __ckpt_server --
 *	The checkpoint server thread.
 */
static void *
__ckpt_server(void *arg)
{
	struct timespec ts;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);
	wt_session = (WT_SESSION *)session;

	/*
	 * The checkpoint server may be running before the database is created,
	 * and checkpoints would fail.   Wait for the wiredtiger_open call.
	 */
	while (!conn->connection_initialized)
		__wt_sleep(1, 0);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Get the current local time of day. */
		WT_ERR(__wt_epoch(session, &ts));

		/* Checkpoint the database. */
		WT_ERR(wt_session->checkpoint(wt_session, conn->ckpt_config));

		/* Wait... */
		WT_ERR(
		    __wt_cond_wait(session, conn->ckpt_cond, conn->ckpt_usecs));
	}

	if (0) {
err:		__wt_err(session, ret, "checkpoint server error");
	}
	return (NULL);
}

/*
 * __wt_checkpoint_create -
 *	Start the checkpoint server thread.
 */
int
__wt_checkpoint_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	int run;

	session = conn->default_session;

	/* Handle configuration. */
	WT_RET(__ckpt_server_config(session, cfg, &run));

	/* If not configured, we're done. */
	if (!run)
		return (0);

	/* The checkpoint server gets its own session. */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &conn->ckpt_session));
	conn->ckpt_session->name = "checkpoint-server";

	WT_RET(
	    __wt_cond_alloc(session, "checkpoint server", 0, &conn->ckpt_cond));

	/*
	 * Start the thread.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->ckpt_tid, __ckpt_server, conn->ckpt_session));
	conn->ckpt_tid_set = 1;

	return (0);
}

/*
 * __wt_checkpoint_destroy -
 *	Destroy the checkpoint server thread.
 */
int
__wt_checkpoint_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	if (conn->ckpt_tid_set) {
		WT_TRET(__wt_cond_signal(session, conn->ckpt_cond));
		WT_TRET(__wt_thread_join(session, conn->ckpt_tid));
		conn->ckpt_tid_set = 0;
	}
	WT_TRET(__wt_cond_destroy(session, &conn->ckpt_cond));

	__wt_free(session, conn->ckpt_config);

	/* Close the server thread's session, free its hazard array. */
	if (conn->ckpt_session != NULL) {
		wt_session = &conn->ckpt_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		__wt_free(session, conn->ckpt_session->hazard);
	}

	return (ret);
}
