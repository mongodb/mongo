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
	size_t len, urilen;
	int ch, objname_free, ret;
	const char *pval, *desc;
	char *objname, *pfx, *uri;

	objname_free = 0;
	objname = pfx = uri = NULL;
	while ((ch = util_getopt(argc, argv, "p:")) != EOF)
		switch (ch) {
		case 'p':
			pfx = util_optarg;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

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
			return (1);
		objname_free = 1;
		break;
	default:
		return (usage());
	}

	urilen = strlen("statistics:") + strlen(objname) + 1;
	if ((uri = calloc(urilen, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (1);
	}
	snprintf(uri, urilen, "statistics:%s", objname);

	if ((ret = session->open_cursor(session,
	    uri, NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, uri, wiredtiger_strerror(ret));
		goto err;
	}

	/* Optionally search for a specific value. */
	if (pfx == NULL) {
		while (
		    (ret = cursor->next(cursor)) == 0 &&
		    (ret = cursor->get_key(cursor, &desc)) == 0 &&
		    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
			if (printf("%s=%s\n", desc, pval) < 0) {
				ret = errno;
				break;
			}
	} else {
		cursor->set_key(cursor, pfx);
		if ((ret = cursor->search(cursor)) == 0 &&
		    (ret = cursor->get_key(cursor, &desc)) == 0 &&
		    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
			if (printf("%s=%s\n", desc, pval) < 0)
				ret = errno;
		if (ret == 0) {
			len = strlen(pfx);
			while ((ret = cursor->next(cursor)) == 0 &&
			    (ret = cursor->get_key(cursor, &desc)) == 0 &&
			    (ret = cursor->get_value(
			    cursor, &pval, &v)) == 0 &&
			    strncmp(pfx, desc, len) == 0)
				if (printf("%s=%s\n", desc, pval) < 0) {
					ret = errno;
					break;
				}
		}
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
	    "usage: %s%s "
	    "stat [-p prefix] [file]\n",
	    progname, usage_prefix);
	return (1);
}
