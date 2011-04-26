/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include <stdlib.h>

#include "wt_internal.h"

static WT_CONNECTION *wt_conn;
static WT_SESSION *wt_session;

/*
 * wiredtiger_simple_setup --
 *	Standard setup for simple applications.
 */
int
wiredtiger_simple_setup(const char *progname,
    WT_EVENT_HANDLER *handler, const char *config, BTREE **dbp)
{
	BTREE *btree;
	CONNECTION *conn;
	int ret;

	btree = *dbp = NULL;

	if ((ret = wiredtiger_open(NULL, handler, config, &wt_conn)) != 0 ||
	    (ret = wt_conn->open_session(wt_conn,
	    NULL, NULL, &wt_session)) != 0) {
		fprintf(stderr, "%s: wiredtiger_open: %s\n",
		    progname, wiredtiger_strerror(ret));
		return (ret);
	}
	conn = (CONNECTION *)wt_conn;

	if ((ret = conn->btree(conn, 0, &btree)) != 0) {
		fprintf(stderr, "%s: conn.btree: %s\n",
		    progname, wiredtiger_strerror(ret));
		goto err;
	}

	*dbp = btree;
	return (EXIT_SUCCESS);

err:	wiredtiger_simple_teardown(progname, btree);
	return (ret);
}

/*
 * wiredtiger_simple_teardown --
 *	Standard teardown for simple applications.
 */
int
wiredtiger_simple_teardown(const char *progname, BTREE *btree)
{
	int ret, tret;

	ret = 0;
	if (btree != NULL &&
	    (tret = btree->close(btree, (SESSION *)wt_session, 0)) != 0) {
		fprintf(stderr, "%s: Db.close: %s\n",
		    progname, wiredtiger_strerror(ret));
		if (ret == 0)
			ret = tret;
	}

	if (wt_conn != NULL) {
		if ((tret = wt_conn->close(wt_conn, NULL)) != 0) {
			fprintf(stderr, "%s: conn.close: %s\n",
			    progname, wiredtiger_strerror(ret));
			if (ret == 0)
				ret = tret;
		}
		wt_conn = NULL;
	}

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
