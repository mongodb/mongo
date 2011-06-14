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
util_salvage(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	size_t len;
	int ch, ret, tret;
	char *tablename;

	conn = NULL;
	tablename = NULL;

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

	len = sizeof("table:") + strlen(*argv);
	if ((tablename = calloc(len, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (EXIT_FAILURE);
	}
	snprintf(tablename, len, "table:%s", *argv);

	if ((ret = wiredtiger_open(".", verbose ?
	    __wt_event_handler_verbose : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->salvage(session, tablename, NULL)) != 0) {
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

	if (tablename != NULL)
		free(tablename);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "salvage file\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
