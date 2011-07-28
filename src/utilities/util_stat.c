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
	WT_ITEM key, value;
	int ch, name_free, ret;
	const char *cursor_config;
	char *name;

	name = NULL;
	name_free = 0;
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
		name = (char *)"statistics:";
		cursor_config = "printable";
		break;
	case 1:
		if ((name = util_name(*argv, "file", UTIL_FILE_OK)) == NULL)
			return (EXIT_FAILURE);
		name_free = 1;
		cursor_config = "printable,statistics";
		break;
	default:
		return (usage());
	}

	if ((ret = session->open_cursor(
	    session, name, NULL, cursor_config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			break;
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			break;
		if (fwrite(key.data, 1, key.size, stdout) != key.size ||
		    fwrite("=", 1, 1, stdout) != 1 ||
		    fwrite(value.data, 1, value.size, stdout) != value.size ||
		    fwrite("\n", 1, 1, stdout) != 1) {
			ret = errno;
			break;
		}
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	else {
		fprintf(stderr, "%s: cursor get(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	if (0) {
err:		ret = 1;
	}

	if (name_free)
		free(name);

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
