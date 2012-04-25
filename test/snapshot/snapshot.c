/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wiredtiger.h>

#define	URI	"file:__snap"

struct L {
	int start, stop;			/* starting/stopping id */
	const char *name;			/* snapshot name */
} list[] = {
	{ 100, 120, "snapshot-1" },
	{ 200, 220, "snapshot-2" },
	{ 300, 320, "snapshot-3" },
	{ 400, 420, "snapshot-4" },
	{ 500, 520, "snapshot-5" },
	{ 100, 620, "snapshot-6" },
	{ 200, 720, "snapshot-7" },
	{ 300, 820, "snapshot-8" },
	{ 400, 920, "snapshot-9" },
	{ 500, 600, "snapshot-a" },
	{ 0, 0, NULL }
};

void add(int, int);
void build(void);
void check(struct L *);
void dump_cat(struct L *, const char *);
void dump_snap(struct L *, const char *);
void run(void);
int  usage(void);

WT_CONNECTION *conn;
WT_SESSION *session;
const char *progname;

int
main(int argc, char *argv[])
{
	int ch;

	(void)system("rm -f WiredTiger* __*");

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	while ((ch = getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	run();

	return (EXIT_SUCCESS);
}

int
usage(void)
{
	(void)fprintf(stderr, "usage: %s\n", progname);
	return (EXIT_FAILURE);
}

/*
 * run --
 *	Worker function.
 */
void
run(void)
{
	char config[128];

	/* Open the connection and create the file. */
	assert(wiredtiger_open(
	    NULL, NULL, "create,cache_size=100MB", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	(void)snprintf(config, sizeof(config),
	    "key_format=S,value_format=S,"
	    "internal_page_max=512,leaf_page_max=512");
	assert(session->create(session, URI, config) == 0);

	build();				/* Build a set of snapshots */
#if 0
	for (p = list; p->start != 0; ++p)
		check(p);			/* Check the contents */
#endif

	assert(conn->close(conn, 0) == 0);
}

/*
 * build --
 *	Build a file with a set of snapshots.
 */
void
build(void)
{
	struct L *p;
	char buf[64];

	for (p = list; p->start != 0; ++p) {
		add(p->start, p->stop);

		snprintf(buf, sizeof(buf), "snapshot=%s", p->name);
		assert(session->sync(session, URI, buf) == 0);

		assert(session->verify(session, URI, NULL) == 0);
	}
}

/*
 * add --
 *	Add records.
 */
void
add(int start, int stop)
{
	WT_CURSOR *cursor;
	int ret;
	char kbuf[64], vbuf[64];

	assert(session->open_cursor(
	    session, URI, NULL, "overwrite", &cursor) == 0);

	/* Insert the key/value pairs. */
	for (; start < stop; ++start) {
		snprintf(kbuf, sizeof(kbuf), "%010d KEY------", start);
		cursor->set_key(cursor, kbuf);
		snprintf(vbuf, sizeof(vbuf), "%010d VALUE----", start);
		cursor->set_value(cursor, vbuf);

		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr,
			    "cursor->insert: %s\n", wiredtiger_strerror(ret));
			exit(EXIT_FAILURE);
		}
	}

	assert(cursor->close(cursor) == 0);
}

/*
 * check --
 *	Check the contents of an individual snapshot.
 */
void
check(struct L *snap)
{
	dump_cat(snap, "__dump.1");		/* Dump out the records */
	dump_snap(snap, "__dump.2");		/* Dump out the snapshot */

	/*
	 * Sort the two versions of the snapshot, discarding overlapping
	 * entries, and compare the results.
	 */
	if (system(
	    "sort -u -o __dump.1 __dump.1 && "
	    "sort -u -o __dump.2 __dump.2 && "
	    "cmp __dump.1 __dump.2 > /dev/null")) {
		fprintf(stderr,
		    "check failed, snapshot results for %s were incorrect\n",
		    snap->name);
		exit(EXIT_FAILURE);
	 }
}

/*
 * dump_cat --
 *	Output the expected rows into a file.
 */
void
dump_cat(struct L *snap, const char *f)
{
	struct L *p;
	FILE *fp;
	int row;

	assert((fp = fopen(f, "w")) != NULL);

	for (p = list;; ++p) {
		for (row = p->start; row < p->stop; ++row)
			fprintf(fp,
			    "%010d KEY------\n%010d VALUE----\n", row, row);
		if (p == snap)
			break;
	}

	assert(fclose(fp) == 0);
}

/*
 * dump_snap --
 *	Dump a snapshot into a file.
 */
void
dump_snap(struct L *snap, const char *f)
{
	FILE *fp;
	WT_CURSOR *cursor;
	int ret;
	const char *key, *value;
	char buf[64];

	assert((fp = fopen(f, "w")) != NULL);

	snprintf(buf, sizeof(buf), "snapshot=%s", snap->name);
	assert(session->open_cursor(session, URI, NULL, buf, &cursor) == 0);

	while ((ret = cursor->next(cursor)) == 0) {
		assert(cursor->get_key(cursor, &key) == 0);
		assert(cursor->get_value(cursor, &value) == 0);
		fprintf(fp, "%s\n%s\n", key, value);
	}
	assert(ret == WT_NOTFOUND);

	assert(cursor->close(cursor) == 0);
	assert(fclose(fp) == 0);
}
