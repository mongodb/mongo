/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "test_util.h"

#include <assert.h>

#define	DUMP	"__slvg.dump"			/* Dump file */
#define	LOAD	"__slvg.load"			/* Build file */
#define	RSLT	"__slvg.result"			/* Result file */
#define	SLVG	"__slvg.slvg"			/* Salvage file */

#define	PSIZE	(2 * 1024)
#define	OSIZE	(PSIZE / 20)

#define	CHECK(x) do {							\
	if (!(x)) {							\
		fprintf(stderr, "%s: failed\n", #x);			\
		assert(0);						\
	}								\
} while (0)

void build(int, int, int);
void copy(u_int, u_int);
void empty(int);
void print_res(int, int, int);
void process(void);
void run(int);
void t(int, u_int, int);
int  usage(void);

static const char *progname;			/* Program name */

static FILE	*res_fp;			/* Results file */
static u_int	 page_type;			/* File types */
static int	 value_unique;			/* Values are unique */
static int	 verbose;			/* -v flag */

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

int
main(int argc, char *argv[])
{
	u_int ptype;
	int ch, r;

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	r = 0;
	ptype = 0;
	while ((ch = __wt_getopt(progname, argc, argv, "r:t:v")) != EOF)
		switch (ch) {
		case 'r':
			r = atoi(__wt_optarg);
			if (r == 0)
				return (usage());
			break;
		case 't':
			if (strcmp(__wt_optarg, "fix") == 0)
				ptype = WT_PAGE_COL_FIX;
			else if (strcmp(__wt_optarg, "var") == 0)
				ptype = WT_PAGE_COL_VAR;
			else if (strcmp(__wt_optarg, "row") == 0)
				ptype = WT_PAGE_ROW_LEAF;
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
	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		return (usage());

	printf("salvage test run started\n");

	t(r, ptype, 1);
	t(r, ptype, 0);

	printf("salvage test run completed\n");
	return (EXIT_SUCCESS);
}

void
t(int r, u_int ptype, int unique)
{
	printf("%sunique values\n", unique ? "" : "non-");
	value_unique = unique;

#define	NTESTS	24
	if (r == 0) {
		if (ptype == 0) {
			page_type = WT_PAGE_COL_FIX;
			for (r = 1; r <= NTESTS; ++r)
				run(r);

			page_type = WT_PAGE_COL_VAR;
			for (r = 1; r <= NTESTS; ++r)
				run(r);

			page_type = WT_PAGE_ROW_LEAF;
			for (r = 1; r <= NTESTS; ++r)
				run(r);
		} else {
			page_type = ptype;
			for (r = 1; r <= NTESTS; ++r)
				run(r);
		}
	} else if (ptype == 0) {
		page_type = WT_PAGE_COL_FIX;
		run(r);
		page_type = WT_PAGE_COL_VAR;
		run(r);
		page_type = WT_PAGE_ROW_LEAF;
		run(r);
	}  else {
		page_type = ptype;
		run(r);
	}
}

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-v] [-r run] [-t fix|var|row]\n", progname);
	return (EXIT_FAILURE);
}

void
run(int r)
{
	char buf[128];

	printf("\t%s: run %d\n", __wt_page_type_string(page_type), r);

	CHECK(system("rm -f WiredTiger* __slvg.*") == 0);
	CHECK((res_fp = fopen(RSLT, "w")) != NULL);

	/*
	 * Each run builds the LOAD file, and then appends the first page of
	 * the LOAD file into the SLVG file.  The SLVG file is then salvaged,
	 * verified, and dumped into the DUMP file, which is compared to the
	 * results file, which are the expected results.
	 */
	switch (r) {
	case 1:
		/*
		 * Smoke test: empty files.
		 */
		build(0, 0, 0); copy(0, 0);
		break;
	case 2:
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
	case 3:
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
	case 4:
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
	case 5:
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
	case 6:
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
	case 7:
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
	case 8:
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
	case 9:
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
	case 10:
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
	case 11:
		/*
		 * Case #4:
		 * The second page is a prefix of the first page, and the first
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(7, 1);
		build(100, 200,  5); copy(6, 1);
		print_res(100, 100, 20);
		break;
	case 12:
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
	case 13:
		/*
		 * Case #5:
		 * The second page is in the middle of the first page, and the
		 * first page has a higher LSN.
		 */
		build(100, 100, 40); copy(7, 1);
		build(110, 200, 10); copy(6, 11);
		print_res(100, 100, 40);
		break;
	case 14:
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
	case 15:
		/*
		 * Case #6:
		 * The second page is a suffix of the first page, and the first
		 * page has a higher LSN.
		 */
		build(100, 100, 40); copy(7, 1);
		build(130, 200, 10); copy(6, 31);
		print_res(100, 100, 40);
		break;
	case 16:
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
	case 17:
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
	case 18:
		/*
		 * Case #9:
		 * The first page is a prefix of the second page, and the second
		 * page has a higher LSN.
		 */
		build(100, 100, 20); copy(6, 1);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 19:
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
	case 20:
		/*
		 * Case #10:
		 * The first page is a suffix of the second page, and the second
		 * page has a higher LSN.
		 */
		build(130, 100, 10); copy(6, 31);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 21:
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
	case 22:
		/*
		 * Case #11:
		 * The first page is in the middle of the second page, and the
		 * second page has a higher LSN.
		 */
		build(110, 100, 10); copy(6, 11);
		build(100, 200, 40); copy(7, 1);
		print_res(100, 200, 40);
		break;
	case 23:
		/*
		 * Column-store only: missing an initial key range of 99
		 * records.
		 */
		build(100, 100, 10); copy(1, 100);
		empty(99);
		print_res(100, 100, 10);
		break;
	case 24:
		/*
		 * Column-store only: missing a middle key range of 37
		 * records.
		 */
		build(100, 100, 10); copy(1, 1);
		build(138, 138, 10); copy(1, 48);
		print_res(100, 100, 10);
		empty(37);
		print_res(138, 138, 10);
		break;
	default:
		fprintf(stderr, "salvage: %d: no such test\n", r);
		exit(EXIT_FAILURE);
	}

	CHECK(fclose(res_fp) == 0);

	process();

	snprintf(buf, sizeof(buf), "cmp %s %s > /dev/null", DUMP, RSLT);
	if (system(buf)) {
		fprintf(stderr,
		    "check failed, salvage results were incorrect\n");
		exit(EXIT_FAILURE);
	}
}

/*
 * file_exists --
 *	Return if the file exists.
 */
static int
file_exists(const char *path)
{
	struct stat sb;

	return (stat(path, &sb) == 0);
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
	int new_slvg;

	/*
	 * Disable logging: we're modifying files directly, we don't want to
	 * run recovery.
	 */
	CHECK(wiredtiger_open(
	    NULL, NULL, "create,log=(enabled=false)", &conn) == 0);
	CHECK(conn->open_session(conn, NULL, NULL, &session) == 0);
	CHECK(session->drop(session, "file:" LOAD, "force") == 0);

	switch (page_type) {
	case WT_PAGE_COL_FIX:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,value_format=7t,"
		    "allocation_size=%d,"
		    "internal_page_max=%d,internal_item_max=%d,"
		    "leaf_page_max=%d,leaf_item_max=%d",
		    PSIZE, PSIZE, OSIZE, PSIZE, OSIZE);
		break;
	case WT_PAGE_COL_VAR:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,"
		    "allocation_size=%d,"
		    "internal_page_max=%d,internal_item_max=%d,"
		    "leaf_page_max=%d,leaf_item_max=%d",
		    PSIZE, PSIZE, OSIZE, PSIZE, OSIZE);
		break;
	case WT_PAGE_ROW_LEAF:
		(void)snprintf(config, sizeof(config),
		    "key_format=u,"
		    "allocation_size=%d,"
		    "internal_page_max=%d,internal_item_max=%d,"
		    "leaf_page_max=%d,leaf_item_max=%d",
		    PSIZE, PSIZE, OSIZE, PSIZE, OSIZE);
		break;
	default:
		assert(0);
	}
	CHECK(session->create(session, "file:" LOAD, config) == 0);
	CHECK(session->open_cursor(
	    session, "file:" LOAD, NULL, "bulk,append", &cursor) == 0);
	for (; cnt > 0; --cnt, ++ikey, ++ivalue) {
		switch (page_type) {			/* Build the key. */
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			break;
		case WT_PAGE_ROW_LEAF:
			snprintf(kbuf, sizeof(kbuf), "%010d KEY------", ikey);
			key.data = kbuf;
			key.size = 20;
			cursor->set_key(cursor, &key);
			break;
		}

		switch (page_type) {			/* Build the value. */
		case WT_PAGE_COL_FIX:
			cursor->set_value(cursor, ivalue & 0x7f);
			break;
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			snprintf(vbuf, sizeof(vbuf),
			    "%010d VALUE----", value_unique ? ivalue : 37);
			value.data = vbuf;
			value.size = 20;
			cursor->set_value(cursor, &value);
		}
		CHECK(cursor->insert(cursor) == 0);
	}

	/*
	 * The first time through this routine we create the salvage file and
	 * then remove it (all we want is the appropriate schema entry, we're
	 * creating the salvage file itself by hand).
	 */
	new_slvg = !file_exists(SLVG);
	if (new_slvg) {
		CHECK(session->drop(session, "file:" SLVG, "force") == 0);
		CHECK(session->create(session, "file:" SLVG, config) == 0);
	}
	CHECK(conn->close(conn, 0) == 0);
	if (new_slvg)
		(void)remove(SLVG);
}

/*
 * copy --
 *	Copy the created page to the end of the salvage file.
 */
void
copy(u_int gen, u_int recno)
{
	FILE *ifp, *ofp;
	WT_PAGE_HEADER *dsk;
	WT_BLOCK_HEADER *blk;
	char buf[PSIZE];

	CHECK((ifp = fopen(LOAD, "r")) != NULL);

	/*
	 * If the salvage file doesn't exist, then we're creating it:
	 * copy the first sector (the file description).
	 * Otherwise, we are appending to an existing file.
	 */
	if (file_exists(SLVG))
		CHECK((ofp = fopen(SLVG, "a")) != NULL);
	else {
		CHECK((ofp = fopen(SLVG, "w")) != NULL);
		CHECK(fread(buf, 1, PSIZE, ifp) == PSIZE);
		CHECK(fwrite(buf, 1, PSIZE, ofp) == PSIZE);
	}

	/*
	 * If there's data, copy/update the first formatted page.
	 */
	if (gen != 0) {
		CHECK(fseek(ifp, (long)PSIZE, SEEK_SET) == 0);
		CHECK(fread(buf, 1, PSIZE, ifp) == PSIZE);
		dsk = (void *)buf;
		if (page_type != WT_PAGE_ROW_LEAF)
			dsk->recno = recno;
		dsk->write_gen = gen;
		blk = WT_BLOCK_HEADER_REF(buf);
		blk->cksum = 0;
		blk->cksum = __wt_cksum(dsk, PSIZE);
		CHECK(fwrite(buf, 1, PSIZE, ofp) == PSIZE);
	}

	CHECK(fclose(ifp) == 0);
	CHECK(fclose(ofp) == 0);
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
	const char *key, *value;
	WT_SESSION *session;
	char config[100];

	/* Salvage. */
	config[0] = '\0';
	if (verbose)
		snprintf(config, sizeof(config),
		    "error_prefix=\"%s\",verbose=[salvage,verify],",
		    progname);
	strcat(config, "log=(enabled=false),");

	CHECK(wiredtiger_open(NULL, NULL, config, &conn) == 0);
	CHECK(conn->open_session(conn, NULL, NULL, &session) == 0);
	CHECK(session->salvage(session, "file:" SLVG, 0) == 0);
	CHECK(conn->close(conn, 0) == 0);

	/* Verify. */
	CHECK(wiredtiger_open(NULL, NULL, config, &conn) == 0);
	CHECK(conn->open_session(conn, NULL, NULL, &session) == 0);
	CHECK(session->verify(session, "file:" SLVG, 0) == 0);
	CHECK(conn->close(conn, 0) == 0);

	/* Dump. */
	CHECK((fp = fopen(DUMP, "w")) != NULL);
	CHECK(wiredtiger_open(NULL, NULL, config, &conn) == 0);
	CHECK(conn->open_session(conn, NULL, NULL, &session) == 0);
	CHECK(session->open_cursor(
	    session, "file:" SLVG, NULL, "dump=print", &cursor) == 0);
	while (cursor->next(cursor) == 0) {
		if (page_type == WT_PAGE_ROW_LEAF) {
			CHECK(cursor->get_key(cursor, &key) == 0);
			CHECK(fputs(key, fp) >= 0);
			CHECK(fputc('\n', fp) >= 0);
		}
		CHECK(cursor->get_value(cursor, &value) == 0);
		CHECK(fputs(value, fp) >= 0);
		CHECK(fputc('\n', fp) >= 0);
	}
	CHECK(conn->close(conn, 0) == 0);
	CHECK(fclose(fp) == 0);
}

/*
 * empty --
 *	Print empty print_res, for fixed-length column-store files.
 */
void
empty(int cnt)
{
	int i;

	if (page_type == WT_PAGE_COL_FIX)
		for (i = 0; i < cnt; ++i)
			fputs("\\00\n", res_fp);
}

/*
 * print_res --
 *	Write results file.
 */
void
print_res(int key, int value, int cnt)
{
	static const char hex[] = "0123456789abcdef";
	int ch;

	for (; cnt > 0; ++key, ++value, --cnt) {
		switch (page_type) {			/* Print key */
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			break;
		case WT_PAGE_ROW_LEAF:
			fprintf(res_fp, "%010d KEY------\n", key);
			break;
		}

		switch (page_type) {			/* Print value */
		case WT_PAGE_COL_FIX:
			ch = value & 0x7f;
			if (__wt_isprint((u_char)ch)) {
				if (ch == '\\')
					fputc('\\', res_fp);
				fputc(ch, res_fp);
			} else {
				fputc('\\', res_fp);
				fputc(hex[(ch & 0xf0) >> 4], res_fp);
				fputc(hex[ch & 0x0f], res_fp);
			}
			fputc('\n', res_fp);
			break;
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			fprintf(res_fp,
			    "%010d VALUE----\n", value_unique ? value : 37);
			break;
		}
	}
}
