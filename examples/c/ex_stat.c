/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
int print_overflow_pages(WT_SESSION *);

const char *home = "WT_TEST";

/*! [statistics display function] */
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
/*! [statistics display function] */

int 
print_database_stats(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [statistics database function] */
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0)
		return (ret);

	return (print_cursor(cursor));
	/*! [statistics database function] */
}

int 
print_file_stats(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;

	/*! [statistics file function] */
	if ((ret = session->open_cursor(session,
	    "statistics:file:access.wt", NULL, NULL, &cursor)) != 0)
		return (ret);

	return (print_cursor(cursor));
	/*! [statistics file function] */
}

int 
print_overflow_pages(WT_SESSION *session)
{
	/*! [statistics retrieve by key] */
	WT_CURSOR *cursor;
	const char *desc, *pvalue;
	uint64_t value;
	int ret;

	if ((ret = session->open_cursor(session,
	    "statistics:file:access.wt", NULL, NULL, &cursor)) != 0)
		return (ret);

	cursor->set_key(cursor, WT_STAT_file_overflow);
	ret = cursor->search(cursor);
	ret = cursor->get_value(cursor, &desc, &pvalue, &value);
	printf("%s=%s\n", desc, pvalue);
	/*! [statistics retrieve by key] */

	return (ret);
}

int
main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	ret = wiredtiger_open(home, NULL, "create", &conn);
	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->create(
	    session, "table:access", "key_format=S,value_format=S");

	ret = print_database_stats(session);

	ret = print_file_stats(session);

	ret = print_overflow_pages(session);

	return (conn->close(conn, NULL) == 0 ? ret : EXIT_FAILURE);
}
