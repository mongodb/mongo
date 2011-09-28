/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "thread.h"

/*
 * stats
 *	Dump the database/file statistics.
 */
void
stats(void)
{
	WT_CURSOR *cursor;
	uint64_t v;
	const char *pval, *desc;
	WT_SESSION *session;
	FILE *fp;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((fp = fopen(FNAME_STAT, "w")) == NULL)
		die("fopen " FNAME_STAT , errno);

	/* Connection statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0)
		die("session.open_cursor", ret);

	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0 &&
	    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
		(void)fprintf(fp, "%s=%s\n", desc, pval);

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor, NULL)) != 0)
		die("cursor.close", ret);
	
	/* File statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:" FNAME, NULL, NULL, &cursor)) != 0)
		die("session.open_cursor", ret);

	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &desc)) == 0 &&
	    (ret = cursor->get_value(cursor, &pval, &v)) == 0)
		(void)fprintf(fp, "%s=%s\n", desc, pval);

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor, NULL)) != 0)
		die("cursor.close", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);

	(void)fclose(fp);
}
