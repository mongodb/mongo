/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wt_internal.h"

#define	DUMP	"__slvg.dump"			/* Dump file */
#define	LOAD	"__slvg.load"			/* Build file */
#define	RSLT	"__slvg.result"			/* Result file */
#define	SLVG	"__slvg.build"			/* Salvage file */

#define	PSIZE	(2 * 1024)

void build(int, int, int);
void copy(u_int, u_int);
void print_res(int, int, int);
void process(void);
void run(int);
int  usage(void);

u_int	 page_type;				/* Types of records */
FILE	*res_fp;				/* Results file */
int	 verbose;				/* -v flag */

const char *progname;				/* Program name */

int
main(int argc, char *argv[])
{
	int ch, r;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	r = 0;
	while ((ch = getopt(argc, argv, "r:t:v")) != EOF)
		switch (ch) {
		case 'r':
			r = atoi(optarg);
			if (r == 0)
				return (usage());
			break;
		case 't':
			if (strcmp(optarg, "fix") == 0)
				page_type = WT_PAGE_COL_FIX;
			else if (strcmp(optarg, "rle") == 0)
				page_type = WT_PAGE_COL_RLE;
			else if (strcmp(optarg, "var") == 0)
				page_type = WT_PAGE_COL_VAR;
			else if (strcmp(optarg, "row") == 0)
				page_type = WT_PAGE_ROW_LEAF;
			else
				return (usage());
			break;
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	printf("salvage test run started\n");
	if (r == 0) {
		page_type = WT_PAGE_COL_FIX;
		for (r = 1; r <= 23; ++r)
			run(r);

		page_type = WT_PAGE_COL_RLE;
		for (r = 1; r <= 23; ++r)
			run(r);

		page_type = WT_PAGE_COL_VAR;
		for (r = 1; r <= 23; ++r)
			run(r);

		page_type = WT_PAGE_ROW_LEAF;
		for (r = 1; r <= 21; ++r)
			run(r);
	} else
		run (r);

	printf("salvage test run completed\n");
	return (EXIT_SUCCESS);
}

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-v] [-r run] [-t fix|rle|var|row]\n", progname);
	return (EXIT_FAILURE);
}

void
run(int r)
{
	char buf[128];

	printf("\t%s: run %d\n", __wt_page_type_string(page_type), r);

	(void)remove(SLVG);

	assert((res_fp = fopen(RSLT, "w")) != NULL);

	/*
	 * Each run builds the LOAD file, and then appends the first page of
	 * the LOAD file into the SLVG file.  The SLVG file is then salvaged,
	 * verified, and dumped into the DUMP file, which is compared to the
	 * results file, which are the expected results.
	 */
	switch (r) {
	case 1:
		/*
		 * Smoke test:
		 * Sequential pages, all pages should be kept.
		 */
		build(100, 100, 20); copy(6,  1);
		build(200, 200, 20); copy(7, 21);
		build(300, 300, 20); copy(8, 41);
		print_res(100, 100, 20);
		print_res(200, 200, 20);
		print_res(300, 300, 20);
		break;
	case 2:
		/*
		 * Smoke test:
		 * Sequential pages, all pages should be kept.
		 */
		build(100, 100, 20); copy(8,  1);
		build(200, 200, 20); copy(7, 21);
		build(300, 300, 20); copy(6, 41);
		print_res(100, 100, 20);
		print_res(200, 200, 20);
		print_res(300, 300, 20);
		break;
	case 3:
		/*
		 * Case #1:
		 * 3 pages, each with 20 records starting with the same record
		 * and sequential LSNs; salvage should leave the page with the
		 * largest LSN.
		 */
		build(100, 100, 20); copy(6, 1);
		build(100, 200, 20); copy(7, 1);
		build(100, 300, 20); copy(8, 1);
		print_res(100, 300, 20);
		break;
	case 4:
		/*
		 * Case #1:
		 * 3 pages, each with 20 records starting with the same record
		 * and sequential LSNs; salvage should leave the page with the
		 * largest LSN.
		 */
		build(100, 100, 20); copy(6, 1);
		build(100, 200, 20); copy(8, 1);
		build(100, 300, 20); copy(7, 1);
		print_res(100, 200, 20);
		break;
	case 5:
		/*
		 * Case #1:
		 * 3 pages, each with 20 records starting with the same record
		 * and sequential LSNs; salvage should leave the page with the
		 * largest LSN.
		 */
		build(100, 100, 20); copy(8, 1);
		build(100, 200, 20); copy(7, 1);
		build(100, 300, 20); copy(6, 1);
		print_res(100, 100, 20);
		break;
	case 6:
		/*
		 * Case #2:
		 * The second page overlaps the beginning of the first page, and
		 * the first page has a higher LSN.
		 */
		build(110, 100, 20); copy(7, 11);
		build(100, 200, 20); copy(6,  1);
		print_res(100, 200, 10);
		print_res(110, 100, 20);
		break;
	case 7:
		/*
		 * Case #2:
		 * The second page overlaps the beginning of the first page, and
		 * the second page has a higher LSN.
		 */
		build(110, 100, 20); copy(6, 11);
		build(100, 200, 20); copy(7,  1);
		print_res(100, 200, 20);
		print_res(120, 110, 10);
		break;
	case 8:
		/*
		 * Case #3:
		 * The second page overlaps with the end of the first page, and
		 * the first page has a higher LSN.
		 */
		build(100, 100, 20); copy(7,  1);
		build(110, 200, 20); copy(6, 11);
		print_res(100, 100, 20);
		print_res(120, 210, 10);
		break;
	case 9:
		/*
		 * Case #3:
		 * The second page overlaps with the end of the first page, and
		 * the second page has a higher LSN.
		 */
		build(100, 100, 20); copy(6,  1);
		build(110, 200, 20); copy(7, 11);
		print_res(100, 100, 10);
		print_res(110, 200, 20);
		break;
	case 10:
		/*
		 * Case #4:
		 * The second page is a prefix of the first page, and the first
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(7, 1);
		build(100, 200,  5); copy(6, 1);
		print_res(100, 100, 20);
		break;
	case 11:
		/*
		 * Case #4:
		 * The second page is a prefix of the first page, and the second
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(6, 1);
		build(100, 200,  5); copy(7, 1);
		print_res(100, 200, 5);
		print_res(105, 105, 15);
		break;
	case 12:
		/*
		 * Case #5:
		 * The second page is in the middle of the first page, and the
		 * first page has a higher LSN.
		 */
		build(100, 100, 40); copy(7, 1);
		build(110, 200, 10); copy(6, 11);
		print_res(100, 100, 40);
		break;
	case 13:
		/*
		 * Case #5:
		 * The second page is in the middle of the first page, and the
		 * second page has a higher LSN.
		 */
		build(100, 100, 40); copy(6, 1);
		build(110, 200, 10); copy(7, 11);
		print_res(100, 100, 10);
		print_res(110, 200, 10);
		print_res(120, 120, 20);
		break;
	case 14:
		/*
		 * Case #6:
		 * The second page is a suffix of the first page, and the first
		 * page has a higher LSN.
		 */
		build(100, 100, 40); copy(7, 1);
		build(130, 200, 10); copy(6, 31);
		print_res(100, 100, 40);
		break;
	case 15:
		/*
		 * Case #6:
		 * The second page is a suffix of the first page, and the second
		 * page has a higher LSN.
		 */
		build(100, 100, 40); copy(6, 1);
		build(130, 200, 10); copy(7, 31);
		print_res(100, 100, 30);
		print_res(130, 200, 10);
		break;
	case 16:
		/*
		 * Case #9:
		 * The first page is a prefix of the second page, and the first
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(7, 1);
		build(100, 200, 40); copy(6, 1);
		print_res(100, 100, 20);
		print_res(120, 220, 20);
		break;
	case 17:
		/*
		 * Case #9:
		 * The first page is a prefix of the second page, and the second
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(6, 1);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 18:
		/*
		 * Case #10:
		 * The first page is a suffix of the second page, and the first
		 * page has a higher LSN.
		 */
		build(130, 100, 10); copy(7, 31);
		build(100, 200, 40); copy(6, 1);
		print_res(100, 200, 30);
		print_res(130, 100, 10);
		break;
	case 19:
		/*
		 * Case #10:
		 * The first page is a suffix of the second page, and the second
		 * page has a higher LSN.
		 */
		build(130, 100, 10); copy(6, 31);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 20:
		/*
		 * Case #11:
		 * The first page is in the middle of the second page, and the
		 * first page has a higher LSN.
		 */
		build(110, 100, 10); copy(7, 11);
		build(100, 200, 40); copy(6, 1);
		print_res(100, 200, 10);
		print_res(110, 100, 10);
		print_res(120, 220, 20);
		break;
	case 21:
		/*
		 * Case #11:
		 * The first page is in the middle of the second page, and the
		 * second page has a higher LSN.
		 */
		build(110, 100, 10); copy(6, 11);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 22:
		/*
		 * Column-store only: missing an initial key range of 99
		 * records.
		 */
		build(100, 100, 10); copy(1, 100);
		print_res(100, 100, 10);
		break;
	case 23:
		/*
		 * Column-store only: missing a middle key range of 37
		 * records.
		 */
		build(100, 100, 10); copy(1, 1);
		build(138, 138, 10); copy(1, 48);
		print_res(100, 100, 10);
		print_res(138, 138, 10);
		break;
	default:
		fprintf(stderr, "salvage: %d: no such test\n", r);
		exit (EXIT_FAILURE);
	}

	assert(fclose(res_fp) == 0);

	process();

	snprintf(buf, sizeof(buf), "cmp %s %s > /dev/null", DUMP, RSLT);
	if (system(buf)) {
		fprintf(stderr,
		    "check failed, salvage results were incorrect\n");
		exit (EXIT_FAILURE);
	}
}

/*
 * build --
 *	Build a row- or column-store page in a file.
 */
void
build(int ikey, int ivalue, int cnt)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	char config[256], kbuf[64], vbuf[64];

	(void)remove(LOAD);

	assert(wiredtiger_open(NULL, NULL, "", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	switch (page_type) {
	case WT_PAGE_COL_FIX:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,value_format=\"20u\","
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case WT_PAGE_COL_RLE:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,value_format=\"20u\",runlength_encoding,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case WT_PAGE_COL_VAR:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case WT_PAGE_ROW_LEAF:
		(void)snprintf(config, sizeof(config),
		    "key_format=u,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	default:
		assert(0);
	}
	assert(session->create(session, "file:" LOAD, config) == 0);
	assert(session->open_cursor(
	    session, "file:" LOAD, NULL, "bulk", &cursor) == 0);
	for (; cnt > 0; --cnt, ++ikey, ++ivalue) {
		switch (page_type) {			/* Build the key. */
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_RLE:
		case WT_PAGE_COL_VAR:
			break;
		case WT_PAGE_ROW_LEAF:
			snprintf(kbuf, sizeof(kbuf), "%010d KEY------", ikey);
			key.data = kbuf;
			key.size = 20;
			cursor->set_key(cursor, &key);
			break;
		}
							/* Build the value. */
		snprintf(vbuf, sizeof(vbuf), "%010d VALUE----", ivalue);
		value.data = vbuf;
		value.size = 20;
		cursor->set_value(cursor, &value);
		assert(cursor->insert(cursor) == 0);
	}

	assert(session->sync(session, "file:" LOAD, NULL) == 0);
	assert(conn->close(conn, 0) == 0);
}

/*
 * copy --
 *	Copy the created page to the end of the salvage file.
 */
void
copy(u_int lsn, u_int recno)
{
	FILE *ifp, *ofp;
	WT_PAGE_DISK *dsk;
	char buf[PSIZE];

	assert((ifp = fopen(LOAD, "r")) != NULL);

	/*
	 * If the file doesn't exist, then we're creating it: copy the .conf
	 * file and the first sector (the file description).  Otherwise, we
	 * are appending to an existing file.
	 */
	if (access(SLVG, F_OK)) {
		assert(system("cp " LOAD ".conf " SLVG ".conf") == 0);

		assert((ofp = fopen(SLVG, "w")) != NULL);
		assert(fread(buf, 1, 512, ifp) == 512);
		assert(fwrite(buf, 1, 512, ofp) == 512);
	} else
		assert((ofp = fopen(SLVG, "a")) != NULL);

	/* Copy/update the first formatted page. */
	assert(fseek(ifp, (long)512, SEEK_SET) == 0);
	assert(fread(buf, 1, PSIZE, ifp) == PSIZE);
	dsk = (WT_PAGE_DISK *)buf;
	dsk->lsn = lsn;
	if (page_type != WT_PAGE_ROW_LEAF)
		dsk->recno = recno;
	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, PSIZE);
	assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);

	/*
	 * XXX
	 * The open checks that the root page isn't past EOF, and for some of
	 * these tests, it is.   We need a salvage force flag, but don't yet
	 * have one.  For now, extend the file with random garbage that salvage
	 * will ignore.
	 */
	memset(buf, 'a', sizeof(buf));
	assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);
	assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);

	assert(fclose(ifp) == 0);
	assert(fclose(ofp) == 0);
}	

/*
 * process --
 *	Salvage, verify and dump the created file.
 */
void
process(void)
{
	FILE *fp;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	char config[100];

	/* Salvage. */
	config[0] = '\0';
	if (verbose)
		snprintf(config, sizeof(config),
		    "error_prefix=\"%s\",verbose=[salvage]", progname);
	assert(wiredtiger_open(NULL, NULL, config, &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	assert(session->salvage(session, "file:" SLVG, 0) == 0);
	assert(conn->close(conn, 0) == 0);

	/* Verify. */
	assert(wiredtiger_open(NULL, NULL, "", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	assert(session->verify(session, "file:" SLVG, 0) == 0);
	assert(conn->close(conn, 0) == 0);

	/* Dump. */
	assert((fp = fopen(DUMP, "w")) != NULL);
	assert(wiredtiger_open(NULL, NULL, "", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	assert(session->create(session, "file:" SLVG, NULL) == 0);
	assert(session->open_cursor(
	    session, "file:" SLVG, NULL, "dump,printable", &cursor) == 0);
	while (cursor->next(cursor) == 0) {
		assert (cursor->get_key(cursor, &key) == 0);
		if (key.data != NULL) {
			fwrite(key.data, 1, key.size, fp);
			fwrite("\n", 1, 1, fp);
		}
		assert(cursor->get_value(cursor, &value) == 0);
		fwrite(value.data, 1, value.size, fp);
		fwrite("\n", 1, 1, fp);
	}
	assert(conn->close(conn, 0) == 0);
	assert(fclose(fp) == 0);
}

/*
 * print_res --
 *	Build results file.
 */
void
print_res(int key, int value, int cnt)
{
	for (; cnt > 0; ++key, ++value, --cnt) {
		if (page_type == WT_PAGE_ROW_LEAF)
			fprintf(res_fp, "%010d KEY------\n", key);
		fprintf(res_fp, "%010d VALUE----\n", value);
	}
}
