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

#include "format.h"

/*
 * salvage --
 *	A single salvage.
 */
static void
salvage(void)
{
	WT_CONNECTION *conn;
	WT_DECL_RET;
	WT_SESSION *session;

	conn = g.wts_conn;
	track("salvage", 0ULL, NULL);

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	if ((ret = session->salvage(session, g.uri, "force=true")) != 0)
		testutil_die(ret, "session.salvage: %s", g.uri);
	testutil_check(session->close(session, NULL));
}

/*
 * corrupt --
 *	Corrupt the file in a random way.
 */
static int
corrupt(void)
{
	FILE *fp;
	struct stat sb;
	size_t len, nw;
	wt_off_t offset;
	int fd, ret;
	char buf[8 * 1024], copycmd[2 * 1024];

	/*
	 * If it's a single Btree file (not LSM), open the file, and corrupt
	 * roughly 2% of the file at a random spot, including the beginning
	 * of the file and overlapping the end.
	 *
	 * It's a little tricky: if the data source is a file, we're looking
	 * for "wt", if the data source is a table, we're looking for "wt.wt".
	 */
	(void)snprintf(buf, sizeof(buf), "%s/%s", g.home, WT_NAME);
	if ((fd = open(buf, O_RDWR)) != -1) {
#ifdef _WIN32
		(void)snprintf(copycmd, sizeof(copycmd),
		    "copy %s\\%s %s\\slvg.copy\\%s.corrupted",
		    g.home, WT_NAME, g.home, WT_NAME);
#else
		(void)snprintf(copycmd, sizeof(copycmd),
		    "cp %s/%s %s/slvg.copy/%s.corrupted",
		    g.home, WT_NAME, g.home, WT_NAME);
#endif
		goto found;
	}
	(void)snprintf(buf, sizeof(buf), "%s/%s.wt", g.home, WT_NAME);
	if ((fd = open(buf, O_RDWR)) != -1) {
#ifdef _WIN32
		(void)snprintf(copycmd, sizeof(copycmd),
		    "copy %s\\%s.wt %s\\slvg.copy\\%s.wt.corrupted",
		    g.home, WT_NAME, g.home, WT_NAME);
#else
		(void)snprintf(copycmd, sizeof(copycmd),
		    "cp %s/%s.wt %s/slvg.copy/%s.wt.corrupted",
		    g.home, WT_NAME, g.home, WT_NAME);
#endif
		goto found;
	}
	return (0);

found:	if (fstat(fd, &sb) == -1)
		testutil_die(errno, "salvage-corrupt: fstat");

	offset = mmrand(NULL, 0, (u_int)sb.st_size);
	len = (size_t)(20 + (sb.st_size / 100) * 2);
	(void)snprintf(buf, sizeof(buf), "%s/slvg.corrupt", g.home);
	if ((fp = fopen(buf, "w")) == NULL)
		testutil_die(errno, "salvage-corrupt: open: %s", buf);
	(void)fprintf(fp,
	    "salvage-corrupt: offset %" PRIuMAX ", length " SIZET_FMT "\n",
	    (uintmax_t)offset, len);
	fclose_and_clear(&fp);

	if (lseek(fd, offset, SEEK_SET) == -1)
		testutil_die(errno, "salvage-corrupt: lseek");

	memset(buf, 'z', sizeof(buf));
	for (; len > 0; len -= nw) {
		nw = (size_t)(len > sizeof(buf) ? sizeof(buf) : len);
		if (write(fd, buf, nw) == -1)
			testutil_die(errno, "salvage-corrupt: write");
	}

	if (close(fd) == -1)
		testutil_die(errno, "salvage-corrupt: close");

	/*
	 * Save a copy of the corrupted file so we can replay the salvage step
	 * as necessary.
	 */
	if ((ret = system(copycmd)) != 0)
		testutil_die(ret, "salvage corrupt copy step failed");

	return (1);
}

/*
 * wts_salvage --
 *	Salvage testing.
 */
void
wts_salvage(void)
{
	WT_DECL_RET;

	/* Some data-sources don't support salvage. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		return;

	if (g.c_salvage == 0)
		return;

	/*
	 * Save a copy of the interesting files so we can replay the salvage
	 * step as necessary.
	 */
	if ((ret = system(g.home_salvage_copy)) != 0)
		testutil_die(ret, "salvage copy step failed");

	/* Salvage, then verify. */
	wts_open(g.home, true, &g.wts_conn);
	salvage();
	wts_verify("post-salvage verify");
	wts_close();

	/*
	 * If no records were deleted, dump and compare against Berkeley DB.
	 * (The problem with deleting records is salvage restores deleted
	 * records if a page splits leaving a deleted record on one side of
	 * the split, so we cannot depend on correctness in that case.)
	 */
	if (g.c_delete_pct == 0)
		wts_dump("salvage", SINGLETHREADED);

	/* Corrupt the file randomly, salvage, then verify. */
	if (corrupt()) {
		wts_open(g.home, true, &g.wts_conn);
		salvage();
		wts_verify("post-corrupt-salvage verify");
		wts_close();
	}
}
