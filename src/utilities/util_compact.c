/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_compact(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	char *uri;

	uri = NULL;
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
	if ((uri = util_name(*argv, "table",
	    UTIL_COLGROUP_OK | UTIL_FILE_OK | UTIL_INDEX_OK |
	    UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	if ((ret = session->compact(session, uri, NULL)) != 0) {
		fprintf(stderr, "%s: compact(%s): %s\n",
		    progname, uri, wiredtiger_strerror(ret));
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
	    "compact uri\n",
	    progname, usage_prefix);
	return (1);
}
