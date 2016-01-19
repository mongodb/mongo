/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_read(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	uint64_t recno;
	int ch;
	bool rkey, rval;
	const char *uri, *value;

	while ((ch = __wt_getopt(progname, argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/* The remaining arguments are a uri followed by a list of keys. */
	if (argc < 2)
		return (usage());
	if ((uri = util_name(session, *argv, "table")) == NULL)
		return (1);

	/* Open the object. */
	if ((ret = session->open_cursor(
	    session, uri, NULL, NULL, &cursor)) != 0)
		return (util_err(session, ret, "%s: session.open", uri));

	/*
	 * A simple search only makes sense if the key format is a string or a
	 * record number, and the value format is a single string.
	 */
	if (strcmp(cursor->key_format, "r") != 0 &&
	    strcmp(cursor->key_format, "S") != 0) {
		fprintf(stderr,
		    "%s: read command only possible when the key format is "
		    "a record number or string\n",
		    progname);
		return (1);
	}
	rkey = strcmp(cursor->key_format, "r") == 0;
	if (strcmp(cursor->value_format, "S") != 0) {
		fprintf(stderr,
		    "%s: read command only possible when the value format is "
		    "a string\n",
		    progname);
		return (1);
	}

	/*
	 * Run through the keys, returning non-zero on error or if any requested
	 * key isn't found.
	 */
	for (rval = false; *++argv != NULL;) {
		if (rkey) {
			if (util_str2recno(session, *argv, &recno))
				return (1);
			cursor->set_key(cursor, recno);
		} else
			cursor->set_key(cursor, *argv);

		switch (ret = cursor->search(cursor)) {
		case 0:
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				return (util_cerr(cursor, "get_value", ret));
			if (printf("%s\n", value) < 0)
				return (util_err(session, EIO, NULL));
			break;
		case WT_NOTFOUND:
			(void)util_err(session, 0, "%s: not found", *argv);
			rval = true;
			break;
		default:
			return (util_cerr(cursor, "search", ret));
		}
	}

	return (rval ? 1 : 0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "read uri key ...\n",
	    progname, usage_prefix);
	return (1);
}
