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
util_write(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	uint64_t recno;
	int ch;
	bool append, overwrite, rkey;
	const char *uri;
	char config[100];

	append = overwrite = false;
	while ((ch = __wt_getopt(progname, argc, argv, "ao")) != EOF)
		switch (ch) {
		case 'a':
			append = true;
			break;
		case 'o':
			overwrite = true;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/*
	 * The remaining arguments are a uri followed by a list of values (if
	 * append is set), or key/value pairs (if append is not set).
	 */
	if (append) {
		if (argc < 2)
			return (usage());
	} else
		if (argc < 3 || ((argc - 1) % 2 != 0))
			return (usage());
	if ((uri = util_name(session, *argv, "table")) == NULL)
		return (1);

	/* Open the object. */
	(void)snprintf(config, sizeof(config), "%s,%s",
	    append ? "append=true" : "", overwrite ? "overwrite=true" : "");
	if ((ret = session->open_cursor(
	    session, uri, NULL, config, &cursor)) != 0)
		return (util_err(session, ret, "%s: session.open", uri));

	/*
	 * A simple search only makes sense if the key format is a string or a
	 * record number, and the value format is a single string.
	 */
	if (strcmp(cursor->key_format, "r") != 0 &&
	    strcmp(cursor->key_format, "S") != 0) {
		fprintf(stderr,
		    "%s: write command only possible when the key format is "
		    "a record number or string\n",
		    progname);
		return (1);
	}
	rkey = strcmp(cursor->key_format, "r") == 0;
	if (strcmp(cursor->value_format, "S") != 0) {
		fprintf(stderr,
		    "%s: write command only possible when the value format is "
		    "a string\n",
		    progname);
		return (1);
	}

	/* Run through the values or key/value pairs. */
	while (*++argv != NULL) {
		if (!append) {
			if (rkey) {
				if (util_str2recno(session, *argv, &recno))
					return (1);
				cursor->set_key(cursor, recno);
			} else
				cursor->set_key(cursor, *argv);
			++argv;
		}
		cursor->set_value(cursor, *argv);

		if ((ret = cursor->insert(cursor)) != 0)
			return (util_cerr(cursor, "search", ret));
	}

	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "write [-ao] uri key ...\n",
	    progname, usage_prefix);
	return (1);
}
