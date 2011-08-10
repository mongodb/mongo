/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_config --
 *	Set configuration for a just-created WT_CONNECTION_IMPL handle.
 */
int
__wt_connection_config(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	session = &conn->default_session;

						/* Global mutex */
	WT_RET(__wt_mtx_alloc(session, "WT_CONNECTION_IMPL", 0, &conn->mtx));

	TAILQ_INIT(&conn->dbqh);		/* WT_BTREE list */
	TAILQ_INIT(&conn->dlhqh);		/* Library list */
	TAILQ_INIT(&conn->fhqh);		/* File list */
	TAILQ_INIT(&conn->compqh);		/* Compressor list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_conn_stats(session, &conn->stats));

	/* Diagnostic output separator. */
	conn->sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=";

	return (0);
}

/*
 * __wt_connection_destroy --
 *	Destroy the connection's underlying WT_CONNECTION_IMPL structure.
 */
int
__wt_connection_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;
	int ret;

	session = &conn->default_session;
	ret = 0;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return (0);

	if (conn->mtx != NULL)
		(void)__wt_mtx_destroy(session, conn->mtx);

	/* Free allocated memory. */
	__wt_free(session, conn->home);
	__wt_free(session, conn->sessions);
	__wt_free(session, conn->session_array);
	__wt_free(session, conn->hazard);
	__wt_free(session, conn->stats);

	__wt_free(NULL, conn);
	return (ret);
}
