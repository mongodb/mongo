/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "util.h"

const char *progname;

int	usage(void);

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	const char *home, *tablename;
	int ch, debug, ret, tret, verbose;

	WT_UTILITY_INTRO(progname, argv);

	debug = verbose = 0;
	while ((ch = getopt(argc, argv, "dh:Vv")) != EOF)
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'h':			/* home directory */
			home = optarg;
			break;
		case 'v':			/* verbose */
			verbose = 1;
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
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

	if ((ret = wiredtiger_open(home, verbose ?
	    __wt_event_handler_verbose : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->verify_table(session,
	    tablename, "salvage")) != 0) {
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

int
usage(void)
{
	(void)fprintf(stderr, "usage: %s [-dVv] [-h home] file\n", progname);
	return (EXIT_FAILURE);
}
