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

#include "thread.h"

/*
 * stats
 *	Dump the database/file statistics.
 */
void
stats(void)
{
	FILE *fp;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t v;
	int ret;
	char name[64];
	const char *pval, *desc;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((fp = fopen(FNAME_STAT, "w")) == NULL)
		testutil_die(errno, "fopen " FNAME_STAT);

	/* Connection statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "session.open_cursor");

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		(void)fprintf(fp, "%s=%s\n", desc, pval);

	if (ret != WT_NOTFOUND)
		testutil_die(ret, "cursor.next");
	if ((ret = cursor->close(cursor)) != 0)
		testutil_die(ret, "cursor.close");

	/* File statistics. */
	if (!multiple_files) {
		(void)snprintf(name, sizeof(name), "statistics:" FNAME, 0);
		if ((ret = session->open_cursor(
		    session, name, NULL, NULL, &cursor)) != 0)
			testutil_die(ret, "session.open_cursor");

		while ((ret = cursor->next(cursor)) == 0 &&
		    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
			(void)fprintf(fp, "%s=%s\n", desc, pval);

		if (ret != WT_NOTFOUND)
			testutil_die(ret, "cursor.next");
		if ((ret = cursor->close(cursor)) != 0)
			testutil_die(ret, "cursor.close");

		if ((ret = session->close(session, NULL)) != 0)
			testutil_die(ret, "session.close");
	}
	(void)fclose(fp);
}
