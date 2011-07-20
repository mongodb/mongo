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
util_dump(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	char cursor_config[100];
	int ch, printable, ret;
	char *name;

	name = NULL;
	printable = 0;

	while ((ch = getopt(argc, argv, "f:p")) != EOF)
		switch (ch) {
		case 'f':			/* output file */
			if (freopen(optarg, "w", stdout) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'p':
			printable = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(
	    *argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		return (EXIT_FAILURE);

	snprintf(cursor_config, sizeof(cursor_config),
	    "dump,%s", printable ? "printable" : "raw");
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
		if ((key.size != 0 && (
		    fwrite(key.data, 1, key.size, stdout) != key.size ||
		    fwrite("\n", 1, 1, stdout) != 1)) ||
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

	if (name != NULL)
		free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "dump [-p] [-f output-file] table\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
