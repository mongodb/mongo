/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int list_print(WT_SESSION *);
static int usage(void);

int
util_list(WT_SESSION *session, int argc, char *argv[])
{
	int ch;

	while ((ch = util_getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	if (argc != 0)
		return (usage());

	return (list_print(session));
}

/*
 * list_print --
 *	List the high-level objects in the database.
 */
static int
list_print(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;
	const char *key;

	ret = 0;

	/* Open the schema file. */
	if ((ret = session->open_cursor(
	    session, WT_SCHEMA_URI, NULL, NULL, &cursor)) != 0) {
		/*
		 * If there is no schema (yet), this will return WT_NOTFOUND.
		 * Treat that the same as an empty schema.
		 */
		if (ret == WT_NOTFOUND)
			return (0);

		fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
		    progname, WT_SCHEMA_URI, wiredtiger_strerror(ret));
		return (1);
	}

#define	MATCH(s, tag)							\
	(strncmp(s, tag, strlen(tag)) == 0)

	while ((ret = cursor->next(cursor)) == 0) {
		/* Get the key. */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr("schema", "get_key", ret));
			
		/* All we care about are top-level objects (files or tables). */
		if (MATCH(key, "table:") || MATCH(key, "file:"))
			printf("%s\n", key);
	}
	if (ret != WT_NOTFOUND)
		return (util_cerr("schema", "next", ret));

	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "list\n",
	    progname, usage_prefix);
	return (1);
}
