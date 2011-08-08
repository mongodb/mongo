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
util_stat(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	uint64_t v;
	const char *name, *pval, *desc;
	char *objname, *uri;
	size_t urilen;
	int ch, objname_free, ret;

	objname = uri = NULL;
	objname_free = 0;
	while ((ch = getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/*
	 * If there are no arguments, the statistics cursor operates on the
	 * connection, otherwise, the optional remaining argument is a file
	 * name.
	 */
	switch (argc) {
	case 0:
		objname = (char *)"";
		break;
	case 1:
		if ((objname = util_name(*argv, "file", UTIL_FILE_OK)) == NULL)
			return (EXIT_FAILURE);
		objname_free = 1;
		break;
	default:
		return (usage());
	}

	urilen = strlen("statistics:") + strlen(objname) + 1;
	if ((uri = calloc(urilen, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (EXIT_FAILURE);
	}
	snprintf(uri, urilen, "statistics:%s", objname);

	if ((ret = session->open_cursor(session,
	    uri, NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, uri, wiredtiger_strerror(ret));
		goto err;
	}

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &name)) != 0 ||
		    (ret = cursor->get_value(cursor, &v, &pval, &desc)) != 0)
			break;
		if (printf("%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	else {
		fprintf(stderr, "%s: cursor get(%s) failed: %s\n",
		    progname, objname, wiredtiger_strerror(ret));
		goto err;
	}

	if (0) {
err:		ret = 1;
	}

	if (objname_free)
		free(objname);
	free(uri);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "stat [file]\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
