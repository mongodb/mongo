/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_drop(WT_SESSION *session, int argc, char *argv[])
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

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());
	if ((uri = util_uri(session, *argv, "table")) == NULL)
		return (1);

	if ((ret = session->drop(session, uri, "force")) != 0)
		(void)util_err(session, ret, "session.drop: %s", uri);

	free(uri);
	return (ret);
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
