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
util_verify(WT_SESSION *session, int argc, char *argv[])
{
	int ch, ret;
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
	if ((name = util_name(
	    *argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		return (EXIT_FAILURE);

	if ((ret = session->verify(session, name, NULL)) != 0) {
		fprintf(stderr, "%s: verify(%s): %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}
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
	    "usage: %s%s "
	    "verify file\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
