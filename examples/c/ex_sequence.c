/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_sequence.c
 *	This is an example demonstrating how to create and access a sequence.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

int main()
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	wiredtiger_recno_t recno;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	if (conn->is_new(conn)) {
		/*
		 * If we created the database, create the sequence by opening a
		 * cursor on the sequence view and inserting a new record.
		 */
		ret = session->open_cursor(session, "sequence:",
		    NULL, NULL, &cursor);
		cursor->set_key(cursor, "myseq");
		cursor->set_value(cursor, "cachesize=100,wrap");
		ret = cursor->insert(cursor);
		ret = cursor->close(cursor, NULL);
	}

	/* Use the sequence. */
	ret = session->open_cursor(session, "sequence:myseq",
	    NULL, NULL, &cursor);
	ret = cursor->insert(cursor);
	cursor->get_key(cursor, &recno);

	printf("Got record number: %d\n", (int)recno);

	ret = conn->close(conn, NULL);

	return (ret);
}
