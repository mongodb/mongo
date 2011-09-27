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

int print_cursor(WT_CURSOR *);
int print_database_stats(WT_SESSION *);
int print_file_stats(WT_SESSION *);

const char *home = "WT_TEST";

int
print_cursor(WT_CURSOR *cursor)
{
	const char *desc, *pvalue;
	uint64_t value;
	int ret;

	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pvalue, &value)) == 0)
		printf("%s=%s\n", desc, pvalue);

	return (ret == WT_NOTFOUND ? 0 : ret);
}

int 
print_database_stats(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0)
		return (ret);

	return (print_cursor(cursor));
}

int 
print_file_stats(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	if ((ret = session->open_cursor(session,
	    "statistics:file:foo.wt", NULL, NULL, &cursor)) != 0)
		return (ret);

	return (print_cursor(cursor));
}

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = print_database_stats(session);
	ret = print_file_stats(session);

	return (conn->close(conn, NULL) == 0 ? ret : EXIT_FAILURE);
}
