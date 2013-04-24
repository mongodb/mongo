/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_rename(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	char *uri, *newuri;

	uri = NULL;
	while ((ch = util_getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining arguments are the object uri and new name. */
	if (argc != 2)
		return (usage());
	if ((uri = util_name(*argv,
	    "table", UTIL_FILE_OK | UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
		return (1);
	newuri = argv[1];

	if ((ret = session->rename(session, uri, newuri, NULL)) != 0) {
		fprintf(stderr, "%s: rename %s to %s: %s\n",
		    progname, uri, newuri, wiredtiger_strerror(ret));
		goto err;
	}

	if (0) {
err:		ret = 1;
	}

	if (uri != NULL)
		free(uri);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "rename uri newuri\n",
	    progname, usage_prefix);
	return (1);
}
