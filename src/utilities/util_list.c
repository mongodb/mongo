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
	WT_DECL_RET;
	int ch, sflag, vflag;
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
	WT_DECL_RET;
	int found;
	const char *key, *value, *uri;

	/*
	 * XXX
	 * Normally, we don't say anything about the WiredTiger metadata file,
	 * it's not an "object" in the database.  I'm making an exception for
	 * -s and -v, the snapshot and verbose options.
	 */
	if (sflag || vflag) {
		uri = WT_METADATA_URI;
		printf("%s\n", uri);
		if (sflag && (ret = list_print_snapshot(session, uri)) != 0)
			return (ret);
		if (vflag) {
			if ((ret =
			    __wt_file_metadata(session, uri, &value)) != 0)
				return (
				    util_err(ret, "metadata read: %s", uri));
			printf("%s\n", value);
		}
	}

	/* Open the metadata file. */
	if ((ret = session->open_cursor(
	    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
		/*
		 * If there is no metadata (yet), this will return ENOENT.
		 * Treat that the same as an empty metadata.
		 */
		if (ret == ENOENT)
			return (0);

		fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
		    progname, WT_METADATA_URI, wiredtiger_strerror(ret));
		return (1);
	}

#define	MATCH(s, tag)							\
	(strncmp(s, tag, strlen(tag)) == 0)

	found = name == NULL;
	while ((ret = cursor->next(cursor)) == 0) {
		/* Get the key. */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr("metadata", "get_key", ret));
			
		/*
		 * If no object specified, show top-level objects (files and
		 * tables).
		 */
		if (name == NULL) {
			if (!MATCH(key, "file:") && !MATCH(key, "table:"))
				continue;
		} else {
			if (!MATCH(key, name))
				continue;
			found = 1;
		}
		printf("%s\n", key);
		if (!sflag && !vflag)
			continue;

		if (sflag && (ret = list_print_snapshot(session, key)) != 0)
			return (ret);
		if (vflag) {
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				return (
				    util_cerr("metadata", "get_value", ret));
			printf("%s\n", value);
		}
	}
	if (ret != WT_NOTFOUND)
		return (util_cerr("metadata", "next", ret));
	if (!found) {
		fprintf(stderr, "%s: %s: not found\n", progname, name);
		return (1);
	}

	return (0);
}

/*
 * list_print_snapshot --
 *	List the snapshot information.
 */
static int
list_print_snapshot(WT_SESSION *session, const char *key)
{
	WT_DECL_RET;
	WT_SNAPSHOT *snap, *snapbase;
	size_t len;
	time_t t;
	uint64_t v;
	char buf[256];

	/*
	 * We may not find any snapshots for this file, in which case we don't
	 * report an error, and continue our caller's loop.  Otherwise, report
	 * each snapshot's name and time.
	 */
	if ((ret = __wt_snaplist_get(session, key, &snapbase)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

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

	__wt_snaplist_free(session, snapbase);
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
