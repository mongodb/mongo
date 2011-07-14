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
util_create(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ch, debug, ret, tret;
	const char *config, *uri;

	conn = NULL;
	config = NULL;
	debug = 0;

	while ((ch = getopt(argc, argv, "c:df:T")) != EOF)
		switch (ch) {
		case 'c':			/* command-line option */
			config = optarg;
			break;
		case 'd':			/* command-line option */
			debug = 1;
			break;
		case '?':
		default:
			return (usage());
		}

	argc -= optind;
	argv += optind;

	/* The remaining argument is the URI to create. */
	if (argc != 1)
		return (usage());

	uri = *argv;

	if ((ret = wiredtiger_open(home,
	    verbose ? verbose_handler : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->create(session, uri, config)) != 0)
		goto err;

	if (0) {
err:		ret = 1;
	}
	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

	return ((ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "create [-d] [-c configuration] uri\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
