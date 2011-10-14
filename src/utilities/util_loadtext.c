/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int load_text(WT_SESSION *, const char *);
static int usage(void);

int
util_loadtext(WT_SESSION *session, int argc, char *argv[])
{
	int ch;
	const char *uri;

	while ((ch = util_getopt(argc, argv, "f:")) != EOF)
		switch (ch) {
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL)
				return (
				    util_err(errno, "%s: reopen", util_optarg));
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
	if ((uri =
	    util_name(*argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	return (load_text(session, uri));
}

/*
 * load_text --
 *	Load flat-text into a file/table.
 */
static int
load_text(WT_SESSION *session, const char *uri)
{
	WT_CURSOR *cursor;
	int readkey, ignorekey, ret, tret;

	/* Open the insert cursor. */
	if ((ret = session->open_cursor(
	    session, uri, NULL, "dump=print,overwrite", &cursor)) != 0)
		return (util_err(ret, "%s: session.open", uri));

	/* Row-store tables have keys. */
	if (strcmp(cursor->key_format, "r") == 0) {
		readkey = 0;
		ignorekey = 1;
	} else {
		readkey = 1;
		ignorekey = 0;
	}

	/* Insert the records */
	ret = util_insert(cursor, uri, readkey, ignorekey);

	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if ((tret = cursor->close(cursor, NULL)) != 0) {
		tret = util_err(tret, "%s: cursor.close", uri);
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0 && (ret = session->sync(session, uri, NULL)) != 0)
		ret = util_err(ret, "%s: session.sync", uri);
		
	return (ret == 0 ? 0 : 1);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "loadtext [-f input-file] uri\n",
	    progname, usage_prefix);
	return (1);
}
