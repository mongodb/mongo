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
util_create(WT_SESSION *session, int argc, char *argv[])
{
	int ch;
	const char *config, *uri;

	config = NULL;

	while ((ch = getopt(argc, argv, "c:")) != EOF)
		switch (ch) {
		case 'c':			/* command-line configuration */
			config = optarg;
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

	return (session->create(session, uri, config));
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "create [-c configuration] uri\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
