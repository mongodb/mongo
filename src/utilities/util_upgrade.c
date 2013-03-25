/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
	char *name;

	name = NULL;
	while ((ch = util_getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(*argv,
	    "table", UTIL_FILE_OK | UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	if ((ret = session->upgrade(session, name, NULL)) != 0) {
		fprintf(stderr, "%s: upgrade(%s): %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	/* Verbose configures a progress counter, move to the next line. */
	if (verbose)
		printf("\n");

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

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
