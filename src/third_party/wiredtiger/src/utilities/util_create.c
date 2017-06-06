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
util_create(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	char *config, *uri;

	config = uri = NULL;
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

	if ((uri = util_uri(session, *argv, "table")) == NULL)
		return (1);

	if ((ret = session->create(session, uri, config)) != 0)
		(void)util_err(session, ret, "session.create: %s", uri);

	free(uri);
	return (ret);
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
