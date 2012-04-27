/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int list_print(WT_SESSION *, const char *, int, int);
static int list_print_snapshot(WT_SESSION *, const char *);
static int usage(void);

int
util_list(WT_SESSION *session, int argc, char *argv[])
{
	int ch, ret, sflag, vflag;
	char *name;

	sflag = vflag = 0;
	name = NULL;
	while ((ch = util_getopt(argc, argv, "sv")) != EOF)
		switch (ch) {
		case 's':
			sflag = 1;
			break;
		case 'v':
			vflag = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	switch (argc) {
	case 0:
		break;
	case 1:
		if ((name = util_name(
		    *argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
			return (1);
		break;
	default:
		return (usage());
	}

	ret = list_print(session, name, sflag, vflag);

	if (name != NULL)
		free(name);

	return (ret);
}

/*
 * list_print --
 *	List the high-level objects in the database.
 */
static int
list_print(WT_SESSION *session, const char *name, int sflag, int vflag)
{
	WT_CURSOR *cursor;
	int ret;
	const char *key, *value;

	ret = 0;

	/* Open the schema file. */
	if ((ret = session->open_cursor(
	    session, WT_SCHEMA_URI, NULL, NULL, &cursor)) != 0) {
		/*
		 * If there is no schema (yet), this will return ENOENT.
		 * Treat that the same as an empty schema.
		 */
		if (ret == ENOENT)
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
			
		/*
		 * If no object specified, show top-level objects (files and
		 * tables).
		 */
		if (name == NULL) {
			if (!MATCH(key, "table:") && !MATCH(key, "file:"))
				continue;
		} else
			if (!MATCH(key, name))
				continue;
		printf("%s\n", key);
		if (!sflag && !vflag)
			continue;
		if ((ret = cursor->get_value(cursor, &value)) != 0)
			return (util_cerr("schema", "get_value", ret));
		if (sflag && (ret = list_print_snapshot(session, value)) != 0)
			return (ret);
		if (vflag)
			printf("%s\n", value);

	}
	if (ret != WT_NOTFOUND)
		return (util_cerr("schema", "next", ret));

	return (0);
}

/*
 * list_print_snapshot --
 *	List the snapshot information.
 */
static int
list_print_snapshot(WT_SESSION *session, const char *config)
{
	WT_SNAPSHOT *snap, *snapbase;
	size_t len;
	time_t t;
	uint64_t v;
	int ret;
	char buf[256];

	/*
	 * We may not find any snapshots for this file, in which case we don't
	 * report an error, and continue our caller's loop.  Otherwise, report
	 * each snapshot's name and time.
	 */
	if ((ret =
	    __wt_snap_list_get(session, config, &snapbase)) == WT_NOTFOUND)
		return (0);
	WT_RET(ret);

	/* Find the longest name, so we can pretty-print. */
	len = 0;
	WT_SNAPSHOT_FOREACH(snapbase, snap)
		if (strlen(snap->name) > len)
			len = strlen(snap->name);
	++len;

	WT_SNAPSHOT_FOREACH(snapbase, snap) {
		t = (time_t)snap->sec;
		printf("\t%*s: %.24s", (int)len, snap->name, ctime_r(&t, buf));

		v = snap->snapshot_size;
		if (v >= WT_PETABYTE)
			printf(" (%" PRIu64 " PB)\n", v / WT_PETABYTE);
		else if (v >= WT_TERABYTE)
			printf(" (%" PRIu64 " TB)\n", v / WT_TERABYTE);
		else if (v >= WT_GIGABYTE)
			printf(" (%" PRIu64 " GB)\n", v / WT_GIGABYTE);
		else if (v >= WT_MEGABYTE)
			printf(" (%" PRIu64 " MB)\n", v / WT_MEGABYTE);
		else if (v >= WT_KILOBYTE)
			printf(" (%" PRIu64 " KB)\n", v / WT_KILOBYTE);
		else
			printf(" (%" PRIu64 " B)\n", v);
	}

	__wt_snap_list_free(session, snapbase);
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "list [-sv] [uri]\n",
	    progname, usage_prefix);
	return (1);
}
