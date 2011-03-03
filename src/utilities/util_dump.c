/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

const char *progname;

int	usage(void);

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	const char *tablename, *home;
	char cursor_config[100], datasrc[100];
	int ch, debug, printable, ret, tret;

	WT_UTILITY_INTRO(progname, argv);

	conn = NULL;
	home = NULL;
	debug = printable = 0;

	while ((ch = getopt(argc, argv, "df:h:p")) != EOF)
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
		case 'h':			/* home directory */
			home = optarg;
			break;
		case 'p':
			printable = 1;
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

	if ((ret = wiredtiger_open(home, NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	snprintf(datasrc, sizeof(datasrc), "table:%s", tablename);
	snprintf(cursor_config, sizeof(cursor_config), "dump=%s%s",
	    printable ? "print" : "raw", debug ? ",debug" : "");

	if ((ret = session->open_cursor(session, datasrc, NULL,
	    cursor_config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, datasrc, wiredtiger_strerror(ret));
		goto err;
	}

	while ((ret = cursor->next(cursor)) == 0) {
		cursor->get_key(cursor, &key);
		fwrite(key.data, key.size, 1, stdout);
		fwrite("\n", 1, 1, stdout);
		cursor->get_value(cursor, &value);
		fwrite(value.data, value.size, 1, stdout);
		fwrite("\n", 1, 1, stdout);
	}

	if (ret != WT_NOTFOUND) {
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

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-dpV] [-f output-file] file\n", progname);
	return (EXIT_FAILURE);
}
