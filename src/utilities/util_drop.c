/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "util.h"

static int usage(void);

int
util_drop(WT_SESSION *session, int argc, char *argv[])
{
	int ch;
	const char *name;

	while ((ch = util_getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}

	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());
	if ((name =
	    util_name(*argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	return (session->drop(session, name, "force"));
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "drop uri\n",
	    progname, usage_prefix);
	return (1);
}
