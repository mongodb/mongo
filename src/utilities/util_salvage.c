/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int usage(void);

int
util_salvage(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ch, ret, tret;
	char *name;

	conn = NULL;
	name = NULL;
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
	if ((name = util_name(*argv, "file", UTIL_FILE_OK)) == NULL)
		return (EXIT_FAILURE);

	if ((ret = wiredtiger_open(home,
	    verbose ? verbose_handler : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->salvage(session, name, NULL)) != 0) {
		fprintf(stderr, "%s: salvage(%s): %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}
	if (verbose)
		printf("\n");

	if (0) {
err:		ret = 1;
	}
	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

	if (name != NULL)
		free(name);

	return ((ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE);
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
