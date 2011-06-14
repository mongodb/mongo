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
util_printlog(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	int ch, debug, printable, ret, tret;
	char cursor_config[100], datasrc[100];

	conn = NULL;
	debug = printable = 0;

	while ((ch = getopt(argc, argv, "df:p")) != EOF)
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'f':			/* output file */
			if (freopen(optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'p':
			printable = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* There should not be any more arguments. */
	if (argc != 0)
		return (usage());

	if ((ret = wiredtiger_open(".", NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	snprintf(cursor_config, sizeof(cursor_config), "dump=%s%s",
	    printable ? "print" : "raw", debug ? ",debug" : "");

	if ((ret = session->open_cursor(session, "log:",
	    NULL, cursor_config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, datasrc, wiredtiger_strerror(ret));
		goto err;
	}

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			break;
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			break;
		if (fwrite(key.data, key.size, 1, stdout) != key.size ||
		    fwrite("\n", 1, 1, stdout) != 1 ||
		    fwrite(value.data, value.size, 1, stdout) != value.size ||
		    fwrite("\n", 1, 1, stdout) != 1) {
			ret = errno;
			break;
		}
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	else {
		fprintf(stderr, "%s: cursor get(%s) failed: %s\n",
		    progname, datasrc, wiredtiger_strerror(ret));
		goto err;
	}

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
	    "printlog [-dp] [-f output-file]\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
