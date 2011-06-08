/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "util.h"

static int usage(void);

int
util_verify(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	const char *tablename;
	int ch, ret, tret;

	while ((ch = getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the file name. */
	if (argc != 1)
		return (usage());
	tablename = *argv;

	if ((ret = wiredtiger_open(".", verbose ?
	    __wt_event_handler_verbose : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->verify(session, tablename, NULL)) != 0) {
		fprintf(stderr, "%s: salvage(%s): %s\n",
		    progname, tablename, wiredtiger_strerror(ret));
		goto err;
	}
	if (verbose)
		printf("\n");

	if (0) {
err:		ret = 1;
	}
	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;
	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "verify file\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
