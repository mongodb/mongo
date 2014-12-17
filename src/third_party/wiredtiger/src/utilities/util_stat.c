/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_stat(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	size_t urilen;
	int all, ch, objname_free;
	const char *pval, *desc;
	char *objname, *uri;

	all = objname_free = 0;
	objname = uri = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "a")) != EOF)
		switch (ch) {
		case 'a':
			all = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/*
	 * If there are no arguments, the statistics cursor operates on the
	 * connection, otherwise, the optional remaining argument is a file
	 * or LSM name.
	 */
	switch (argc) {
	case 0:
		objname = (char *)"";
		break;
	case 1:
		if ((objname = util_name(*argv, "table")) == NULL)
			return (1);
		objname_free = 1;
		break;
	default:
		return (usage());
	}

	urilen = strlen("statistics:") + strlen(objname) + 1;
	if ((uri = calloc(urilen, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		goto err;
	}
	snprintf(uri, urilen, "statistics:%s", objname);

	if ((ret = session->open_cursor(session, uri, NULL,
	    all ? "statistics=(all)" : NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, uri, wiredtiger_strerror(ret));
		goto err;
	}

	/* List the statistics. */
	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, NULL)) == 0)
		if (printf("%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}
	if (ret == WT_NOTFOUND)
		ret = 0;

	if (ret != 0) {
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
	    "usage: %s %s "
	    "stat -a [uri]\n",
	    progname, usage_prefix);
	return (1);
}
