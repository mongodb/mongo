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

void build(int, int);
int  bulk(BTREE *, WT_ITEM **, WT_ITEM **);
void copy(int, int);
void print_res(int, int);
void process(void);
void run(int);

#define	OP_APPEND	1
#define	OP_FIRST	2

FILE *res_fp;					/* Results file */

int __start, __stop;				/* Records in the page */
int __column_store = 1;				/* Is a column-store file */

int
main(int argc, char *argv[])
{
	int r;

	if (argc == 2 && isdigit(argv[1][0]))
		run (atoi(argv[1]));
	else
		for (r = 1; r <= 7; ++r)
			run(r);

	printf("salvage test run completed\n");
	return (EXIT_SUCCESS);
}

void
run(int r)
{
	char buf[128];

	printf("run %d\n", r);

	(void)remove(SLVG);

	assert((res_fp = fopen(RSLT, "w")) != NULL);
	fprintf(res_fp, "VERSION=1\n");
	fprintf(res_fp, "HEADER=END\n");

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
		build( 1, 20); copy(6,  1);
		build(21, 40); copy(7, 21);
		build(41, 60); copy(8, 41);
		print_res(1, 60);
		break;
	case 2:
		/*
		 * Smoke test:
		 * Sequential pages, all pages should be kept.
		 */
		build( 1, 20); copy(8,  1);
		build(21, 40); copy(7, 21);
		build(41, 60); copy(6, 41);
		print_res(1, 60);
		break;
	case 3:
		/*
		 * Case #1:
		 * 3 column-store pages, each with 20 records starting with
		 * record number 1, and sequential LSNs; salvage should leave
		 * the page with the largest LSN, records 41-60.
		 */
		build( 1, 20); copy(6, 1);
		build(21, 40); copy(7, 1);
		build(41, 60); copy(8, 1);
		print_res(41, 60);
		break;
	case 4:
		/*
		 * Case #1:
		 * 3 column-store pages, each with 20 records starting with
		 * record number 1, and sequential LSNs; salvage should leave
		 * the page with the largest LSN, records 41-60.
		 */
		build( 1, 20); copy(6, 1);
		build(41, 60); copy(8, 1);
		build(21, 40); copy(7, 1);
		print_res(41, 60);
		break;
	case 5:
		/*
		 * Case #1:
		 * 3 column-store pages, each with 20 records starting with
		 * record number 1, and sequential LSNs; salvage should leave
		 * the page with the largest LSN, records 41-60.
		 */
		build(41, 60); copy(8, 1);
		build(21, 40); copy(7, 1);
		build( 1, 20); copy(6, 1);
		print_res(41, 60);
		break;
	case 6:
		/*
		 * Case #2:
		 * 2 column-store pages, where the second page overlaps with
		 * the beginning of the first page.
		 */
		build( 1, 40); copy(6, 10);
		build(21, 30); copy(7, 1);
		print_res(21, 30);
		print_res(11, 40);
		break;
	default:
		fprintf(stderr, "salvage: %d: no such test\n", r);
		exit (EXIT_FAILURE);
	}

	fprintf(res_fp, "DATA=END\n");
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
build(int start, int stop)
{
	BTREE *btree;
	SESSION *session;

	(void)remove(LOAD);

	__start = start;
	__stop = stop;
	
	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	assert(btree->column_set(btree, 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, 1024, 1024, 1024, 1024, 1024) == 0);
	assert(btree->open(btree, LOAD, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert(btree->bulk_load(btree, NULL, bulk) == 0);
	assert(btree->sync(btree, NULL, 0) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);
}

/*
 * copy --
 *	Copy the created page to the end of the salvage file.
 */
void
copy(int lsn, int recno)
{
	FILE *ifp, *ofp;
	WT_PAGE_DISK *dsk;
	int first;
	char buf[1024];

	/*
	 * If the file doesn't exist, then we're creating it, copy a metadata
	 * page to the salvage file.
	 */
	assert((ifp = fopen(LOAD, "r")) != NULL);
	first = access(SLVG, F_OK) ? 1 : 0;
	assert((ofp = fopen(SLVG, "a")) != NULL);

	/* Copy the first "page" (the metadata description). */
	if (first) {
		assert((ofp = fopen(SLVG, "w")) != NULL);
		assert(fread(buf, 1, 1024, ifp) == 1024);
		assert(fwrite(buf, 1, 1024, ofp) == 1024);
	}

	/* Copy/update the first formatted page. */
	assert(fseek(ifp, 1024L, SEEK_SET) == 0);
	assert(fread(buf, 1, 1024, ifp) == 1024);
	dsk = (WT_PAGE_DISK *)buf;
	if (lsn != 0)
		dsk->lsn = (uint64_t)lsn;
	if (recno != 0)
		dsk->recno = (uint64_t)recno;
	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, 1024);
	assert(fwrite(buf, 1, 1024, ofp) == 1024);

#if 0
	/* Throw some random garbage into the file. */
	memset(buf, 'a', sizeof(buf));
	assert(fwrite(buf, 1, 1024, ofp) == 1024);
#endif

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
	BTREE *btree;
	FILE *fp;
	SESSION *session;

	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	assert(btree->column_set(btree, 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, 1024, 1024, 1024, 1024, 1024) == 0);
	assert(btree->open(btree, SLVG, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert(btree->salvage(btree, NULL, 0) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);

	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	assert(btree->column_set(btree, 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, 1024, 1024, 1024, 1024, 1024) == 0);
	assert(btree->open(btree, SLVG, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert(btree->verify(btree, NULL, 0) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);

	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	assert(btree->column_set(btree, 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, 1024, 1024, 1024, 1024, 1024) == 0);
	assert(btree->open(btree, SLVG, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert((fp = fopen(DUMP, "w")) != NULL);
	assert(btree->dump(btree, fp, NULL, WT_PRINTABLES) == 0);
	assert(fclose(fp) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);
}

/*
 * print_res --
 *	Build results file.
 */
void
print_res(int start, int stop)
{
	for (; start <= stop; ++start)
		fprintf(res_fp, "%010d VALUE----\n", start);
}

/*
 * bulk --
 *	Bulk load records.
 */
int
bulk(BTREE *btree, WT_ITEM **keyp, WT_ITEM **datap)
{
	static WT_ITEM key, data;
	char kbuf[64], dbuf[64];

	if (__start > __stop)
		return (1);

	if (__column_store)
		*keyp = NULL;
	else {
		snprintf(kbuf, sizeof(kbuf), "%010d KEY------", __start);
		key.data = kbuf;
		key.size = 20;
		*keyp = &key;
	}
	snprintf(dbuf, sizeof(dbuf), "%010d VALUE----", __start);
	data.data = dbuf;
	data.size = 20;
	*datap = &data;

	++__start;

	btree = NULL;
	return (0);
}
