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
util_dumpfile(WT_SESSION *session, int argc, char *argv[])
{
	int ch, ret;
	char *name;

	name = NULL;
	while ((ch = util_getopt(argc, argv, "f:")) != EOF)
		switch (ch) {
		case 'f':				/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: %s\n",
				    progname, util_optarg, strerror(errno));
				return (1);
			}
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the file name. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(*argv, "file", UTIL_FILE_OK)) == NULL)
		return (1);

	if ((ret = session->dumpfile(session, name, NULL)) != 0) {
		fprintf(stderr, "%s: dumpfile(%s): %s\n",
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
	    "usage: %s %s "
	    "dumpfile [-f output-file] file\n",
	    progname, usage_prefix);
	return (1);
}
