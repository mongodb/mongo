/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_connection_config --
 *	Set configuration for a just-created CONNECTION handle.
 */
int
__wt_connection_config(CONNECTION *conn)
{
	SESSION *session;

	session = &conn->default_session;

	__wt_methods_connection_config_default(conn);
	__wt_methods_connection_lockout(conn);
	__wt_methods_connection_init_transition(conn);

						/* Global mutex */
	WT_RET(__wt_mtx_alloc(session, "CONNECTION", 0, &conn->mtx));

	TAILQ_INIT(&conn->dbqh);		/* BTREE list */
	TAILQ_INIT(&conn->fhqh);		/* File list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_conn_stats(session, &conn->stats));

	/* Diagnostic output separator. */
	conn->sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=";

	return (0);
}

/*
 * __wt_connection_destroy --
 *	Destroy the CONNECTION's underlying CONNECTION structure.
 */
int
__wt_connection_destroy(CONNECTION *conn)
{
	SESSION *session;
	int ret;

	session = &conn->default_session;
	ret = 0;

	/* Check there's something to destroy. */
	if (conn == NULL)
		return (0);

	/* Diagnostic check: check flags against approved list. */
	WT_CONN_FCHK_RET(conn, "Env.close", conn->flags, WT_APIMASK_CONN, ret);

	if (conn->mtx != NULL)
		(void)__wt_mtx_destroy(session, conn->mtx);

	/* Free allocated memory. */
	__wt_free(session, conn->home);
	__wt_free(session, conn->sessions);
	__wt_free(session, conn->toc_array);
	__wt_free(session, conn->hazard);
	__wt_free(session, conn->stats);

	__wt_free(NULL, conn);
	return (ret);
}
