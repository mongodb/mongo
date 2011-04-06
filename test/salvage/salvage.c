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
int  bulk(BTREE *, WT_ITEM **, WT_ITEM **);
void copy(int, int);
void print_res(int, int, int);
void process(void);
void run(int);

#define	OP_APPEND	1
#define	OP_FIRST	2

FILE *res_fp;					/* Results file */

int gkey, gvalue, gcnt;				/* Records to build */
int page_type;					/* Types of records */

int
main(void)
{
	int r;

	page_type = WT_PAGE_COL_FIX;
	for (r = 1; r <= 21; ++r)
		run(r);

	page_type = WT_PAGE_COL_VAR;
	for (r = 1; r <= 21; ++r)
		run(r);

	page_type = WT_PAGE_ROW_LEAF;
	for (r = 1; r <= 21; ++r)
		run(r);

	printf("salvage test run completed\n");
	return (EXIT_SUCCESS);
}

void
run(int r)
{
	char buf[128];

	printf("%s: run %d\n", __wt_page_type_string(page_type), r);

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
build(int key, int value, int cnt)
{
	BTREE *btree;
	SESSION *session;

	(void)remove(LOAD);

	gvalue = value;
	gkey = key;
	gcnt = cnt;
	
	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	if (page_type != WT_PAGE_ROW_LEAF)
		assert(btree->column_set(btree,
		    page_type == WT_PAGE_COL_FIX ? 20 : 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE) == 0);
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
	char buf[PSIZE];

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
		assert(fread(buf, 1, PSIZE, ifp) == PSIZE);
		assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);
	}

	/* Copy/update the first formatted page. */
	assert(fseek(ifp, (long)PSIZE, SEEK_SET) == 0);
	assert(fread(buf, 1, PSIZE, ifp) == PSIZE);
	dsk = (WT_PAGE_DISK *)buf;
	dsk->lsn = (uint64_t)lsn;
	if (page_type != WT_PAGE_ROW_LEAF)
		dsk->recno = (uint64_t)recno;
	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, PSIZE);
	assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);

#if 0
	/* Throw some random garbage into the file. */
	memset(buf, 'a', sizeof(buf));
	assert(fwrite(buf, 1, PSIZE, ofp) == PSIZE);
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
	if (page_type != WT_PAGE_ROW_LEAF)
		assert(btree->column_set(btree,
		    page_type == WT_PAGE_COL_FIX ? 20 : 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE) == 0);
	assert(btree->open(btree, SLVG, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert(btree->salvage(btree, NULL, 0) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);

	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	if (page_type != WT_PAGE_ROW_LEAF)
		assert(btree->column_set(btree,
		    page_type == WT_PAGE_COL_FIX ? 20 : 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE) == 0);
	assert(btree->open(btree, SLVG, 0660, WT_CREATE) == 0);
	assert(btree->conn->session(btree->conn, 0, &session) == 0);
	assert(btree->verify(btree, NULL, 0) == 0);
	assert(session->close(session, 0) == 0);
	assert(wiredtiger_simple_teardown("salvage", btree) == 0);

	assert(wiredtiger_simple_setup("salvage", NULL, &btree) == 0);
	if (page_type != WT_PAGE_ROW_LEAF)
		assert(btree->column_set(btree,
		    page_type == WT_PAGE_COL_FIX ? 20 : 0, NULL, 0) == 0);
	assert(btree->btree_pagesize_set(
	    btree, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE) == 0);
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
print_res(int key, int value, int cnt)
{
	for (; cnt > 0; ++key, ++value, --cnt) {
		if (page_type == WT_PAGE_ROW_LEAF)
			fprintf(res_fp, "%010d KEY------\n", key);
		fprintf(res_fp, "%010d VALUE----\n", value);
	}
}

/*
 * bulk --
 *	Bulk load records.
 */
int
bulk(BTREE *btree, WT_ITEM **keyp, WT_ITEM **valuep)
{
	static WT_ITEM key, value;
	static char kbuf[64], vbuf[64];

	if (gcnt == 0)
		return (1);
	--gcnt;

	/* Build the key. */
	switch (page_type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		*keyp = NULL;
		break;
	case WT_PAGE_ROW_LEAF:
		snprintf(kbuf, sizeof(kbuf), "%010d KEY------", gkey);
		key.data = kbuf;
		key.size = 20;
		*keyp = &key;
		break;
	}

	/* Build the value. */
	snprintf(vbuf, sizeof(vbuf), "%010d VALUE----", gvalue);
	value.data = vbuf;
	value.size = 20;
	*valuep = &value;

	++gkey;
	++gvalue;

	btree = NULL;
	return (0);
}
