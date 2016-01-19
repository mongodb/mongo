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
util_create(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	const char *config, *uri;

	config = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "c:")) != EOF)
		switch (ch) {
		case 'c':			/* command-line configuration */
			config = __wt_optarg;
			break;
		case '?':
		default:
			return (usage());
		}

	argc -= __wt_optind;
	argv += __wt_optind;

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());

	if ((uri = util_name(session, *argv, "table")) == NULL)
		return (1);

	if ((ret = session->create(session, uri, config)) != 0)
		return (util_err(session, ret, "%s: session.create", uri));
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "create [-c configuration] uri\n",
	    progname, usage_prefix);
	return (1);
}
