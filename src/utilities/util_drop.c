/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_drop(WT_SESSION *session, int argc, char *argv[])
{
	size_t len;
	WT_DECL_RET;
	int ch;
	const char *snapshot;
	char *name, *config;

	config = NULL;
	snapshot = NULL;
	while ((ch = util_getopt(argc, argv, "s:")) != EOF)
		switch (ch) {
		case 's':
			snapshot = util_optarg;
			break;
		case '?':
		default:
			return (usage());
		}

	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the uri. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(*argv, "table", UTIL_ALL_OK)) == NULL)
		return (1);

	if (snapshot == NULL)
		ret = session->drop(session, name, "force");
	else {
		len = strlen(snapshot) +
		    strlen("snapshot=") + strlen("force") + 10;
		if ((config = malloc(len)) == NULL)
			goto err;
		(void)snprintf(config, len, "snapshot=%s,force", snapshot);
		ret = session->drop(session, name, config);
	}

	if (0) {
err:		ret = 1;
	}
	if (config != NULL)
		free(config);
	if (name != NULL)
		free(name);
	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "drop [-s snapshot] uri\n",
	    progname, usage_prefix);
	return (1);
}
