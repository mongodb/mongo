/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_upgrade(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	char *uri;

	uri = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());
	if ((uri = util_uri(session, *argv, "table")) == NULL)
		return (1);

	if ((ret = session->upgrade(session, uri, NULL)) != 0)
		(void)util_err(session, ret, "session.upgrade: %s", uri);
	else {
		/*
		 * Verbose configures a progress counter, move to the next
		 * line.
		 */
		if (verbose)
			printf("\n");
	}

	free(uri);
	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "upgrade uri\n",
	    progname, usage_prefix);
	return (1);
}
