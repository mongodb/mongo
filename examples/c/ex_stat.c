/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_stat.c
 *	This is an example demonstrating how to query a database's statistics.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

int
display(WT_CURSOR *cursor)
{
	const char *description, *pvalue;
	uint64_t value;
	int ret;

	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_value(
		    cursor, &value, &pvalue, &description)) != 0)
			return (ret);
		printf("%s=%s\n", description, pvalue);
	}

	return (ret == WT_NOTFOUND ? 0 : ret);
}

int 
print_database_stats(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	if ((ret = session->open_cursor(
	    session, "statistics:", NULL, NULL, &cursor)) != 0)
		return (ret);

	return (display(cursor));
}

int 
print_file_stats(WT_SESSION *session, const char *filename)
{
	WT_CURSOR *cursor;
	size_t len;
	char *uri;
	int ret;

	len = strlen("statistics:") + strlen(filename) + 1;
	if ((uri = malloc(len)) == NULL)
		return (ENOMEM);
	(void)snprintf(uri, len, "statistics:%s", filename);
	if ((ret = session->open_cursor(
	    session, uri, NULL, NULL, &cursor)) != 0)
		return (ret);

	ret = display(cursor);

	free(uri);

	return (ret);
}

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	const char *f = "foo";
	int ret;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = print_database_stats(session);
	ret = print_file_stats(session, f);

	return (conn->close(conn, NULL) == 0 ? ret : EXIT_FAILURE);
}
