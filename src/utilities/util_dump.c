/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int usage(void);

static inline int
next(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
		if ((key.size != 0 && (
		    fwrite(key.data, 1, key.size, stdout) != key.size ||
		    fwrite("\n", 1, 1, stdout) != 1)) ||
		    fwrite(value.data, 1, value.size, stdout) != value.size ||
		    fwrite("\n", 1, 1, stdout) != 1)
			return (errno);
	}
	return (ret);
}

static inline int
prev(WT_CURSOR *cursor)
{
	WT_ITEM key, value;
	int ret;

	while ((ret = cursor->prev(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (ret);
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (ret);
		if ((key.size != 0 && (
		    fwrite(key.data, 1, key.size, stdout) != key.size ||
		    fwrite("\n", 1, 1, stdout) != 1)) ||
		    fwrite(value.data, 1, value.size, stdout) != value.size ||
		    fwrite("\n", 1, 1, stdout) != 1)
			return (errno);
	}
	return (ret);
}

int
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	int ch, printable, ret, reverse;
	char *name;

	name = NULL;
	printable = reverse = 0;
	while ((ch = util_getopt(argc, argv, "f:pr")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(util_optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'p':
			printable = 1;
			break;
		case 'r':
			reverse = 1;
			break;
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

	if ((ret = session->open_cursor(session, name, NULL,
	    printable ? "dump,printable" : "dump,raw", &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	ret = reverse ? prev(cursor) : next(cursor);
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

	if (name != NULL)
		free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "dump [-pr] [-f output-file] table\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
