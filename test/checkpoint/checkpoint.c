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

#define	URI	"file:__ckpt"

struct L {
	int start, stop;			/* starting/stopping id */
	const char *name;			/* checkpoint name */
} list[] = {
	{ 100, 120, "checkpoint-1" },
	{ 200, 220, "checkpoint-2" },
	{ 300, 320, "checkpoint-3" },
	{ 400, 420, "checkpoint-4" },
	{ 500, 520, "checkpoint-5" },
	{ 100, 620, "checkpoint-6" },
	{ 200, 720, "checkpoint-7" },
	{ 300, 820, "checkpoint-8" },
	{ 400, 920, "checkpoint-9" },
	{ 500, 600, "checkpoint-a" },
	{ 0, 0, NULL }
};

void add(int, int);
void build(void);
void check(struct L *);
int  checkpoint(const char *, const char *);
void cursor_lock(void);
void delete(void);
void dump_cat(struct L *, const char *);
void dump_ckpt(struct L *, const char *);
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
	struct L *p;
	char config[128];

	/* Open the connection and create the file. */
	assert(wiredtiger_open(
	    NULL, NULL, "create,cache_size=100MB", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	(void)snprintf(config, sizeof(config),
	    "key_format=S,value_format=S,"
	    "internal_page_max=512,leaf_page_max=512");
	assert(session->create(session, URI, config) == 0);

	printf("building...\n");
	build();				/* Build a set of checkpoints */

	printf("checking build...\n");
	for (p = list; p->start != 0; ++p)
		check(p);			/* Check the contents */

	printf("checking cursor_lock...\n");
	cursor_lock();

	printf("checking delete...\n");
	delete();

	assert(conn->close(conn, 0) == 0);
}

/*
 * build --
 *	Build a file with a set of checkpoints.
 */
void
build(void)
{
	struct L *p;

	for (p = list; p->start != 0; ++p) {
		add(p->start, p->stop);
		assert (checkpoint(p->name, NULL) == 0);
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
 *	Check the contents of an individual checkpoint.
 */
void
check(struct L *ckpt)
{
	dump_cat(ckpt, "__dump.1");		/* Dump out the records */
	dump_ckpt(ckpt, "__dump.2");		/* Dump out the checkpoint */

	/*
	 * Sort the two versions of the checkpoint, discarding overlapping
	 * entries, and compare the results.
	 */
	if (system(
	    "sort -u -o __dump.1 __dump.1 && "
	    "sort -u -o __dump.2 __dump.2 && "
	    "cmp __dump.1 __dump.2 > /dev/null")) {
		fprintf(stderr,
		    "check failed, checkpoint results for %s were incorrect\n",
		    ckpt->name);
		exit(EXIT_FAILURE);
	 }
}

/*
 * dump_cat --
 *	Output the expected rows into a file.
 */
void
dump_cat(struct L *ckpt, const char *f)
{
	struct L *p;
	FILE *fp;
	int row;

	assert((fp = fopen(f, "w")) != NULL);

	for (p = list; p <= ckpt; ++p) {
		for (row = p->start; row < p->stop; ++row)
			fprintf(fp,
			    "%010d KEY------\n%010d VALUE----\n", row, row);
	}

	assert(fclose(fp) == 0);
}

/*
 * dump_ckpt --
 *	Dump a checkpoint into a file.
 */
void
dump_ckpt(struct L *ckpt, const char *f)
{
	FILE *fp;
	WT_CURSOR *cursor;
	int ret;
	const char *key, *value;
	char buf[64];

	assert((fp = fopen(f, "w")) != NULL);

	snprintf(buf, sizeof(buf), "checkpoint=%s", ckpt->name);
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

/*
 * cursor_lock --
 *	Check locking cases.
 */
void
cursor_lock(void)
{
	WT_CURSOR *cursor, *c1, *c2, *c3;
	char buf[64];

	/* Check that you can't drop a checkpoint if it's in use. */
	snprintf(buf, sizeof(buf), "checkpoint=%s", list[0].name);
	assert(session->open_cursor(session, URI, NULL, buf, &cursor) == 0);
	assert(checkpoint(list[0].name, NULL) != 0);
	assert(cursor->close(cursor) == 0);

	/* Check you can open two checkpoints at the same time. */
	snprintf(buf, sizeof(buf), "checkpoint=%s", list[0].name);
	assert(session->open_cursor(session, URI, NULL, buf, &c1) == 0);
	snprintf(buf, sizeof(buf), "checkpoint=%s", list[1].name);
	assert(session->open_cursor(session, URI, NULL, buf, &c2) == 0);
	assert(session->open_cursor(session, URI, NULL, NULL, &c3) == 0);
	assert(c2->close(c2) == 0);
	assert(c1->close(c1) == 0);
	assert(c3->close(c3) == 0);
}

/*
 * delete --
 *	Delete a checkpoint and verify the file.
 */
void
delete(void)
{
	struct L *p;

	for (p = list; p->start != 0; ++p)
		assert(checkpoint(NULL, p->name) == 0);
}

/*
 * checkpoint --
 *	Take a checkpoint, optionally naming and/or dropping other checkpoints.
 */
int
checkpoint(const char *name, const char *drop)
{
	int ret;
	char buf[128];

	snprintf(buf, sizeof(buf), "target=(\"%s\")%s%s%s%s%s",
	    URI,
	    name == NULL ? "" : ",name=",
	    name == NULL ? "" : name,
	    drop == NULL ? "" : ",drop=(\"",
	    drop == NULL ? "" : drop,
	    drop == NULL ? "" : "\")");

	ret = session->checkpoint(session, buf);

	assert(session->verify(session, URI, NULL) == 0);

	return (ret);
}
