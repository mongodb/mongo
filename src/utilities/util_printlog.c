/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_printlog(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM key, value;
	int ch, printable;

	printable = 0;
	while ((ch = util_getopt(argc, argv, "f:p")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (1);
			}
			break;
		case 'p':
			printable = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* There should not be any more arguments. */
	if (argc != 0)
		return (usage());

	if ((ret = session->open_cursor(session, "log",
	    NULL, printable ? "printable" : "raw", &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(log) failed: %s\n",
		    progname, wiredtiger_strerror(ret));
		goto err;
	}

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			break;
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			break;
		if (fwrite(key.data, 1, key.size, stdout) != key.size ||
		    fwrite("\n", 1, 1, stdout) != 1 ||
		    fwrite(value.data, 1, value.size, stdout) != value.size ||
		    fwrite("\n", 1, 1, stdout) != 1) {
			ret = errno;
			break;
		}
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	else {
		fprintf(stderr, "%s: cursor get(log) failed: %s\n",
		    progname, wiredtiger_strerror(ret));
		goto err;
	}

	if (0) {
err:		ret = 1;
	}
	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "printlog [-p] [-f output-file]\n",
	    progname, usage_prefix);
	return (1);
}
