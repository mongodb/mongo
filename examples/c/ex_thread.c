/*
 * ex_thread.c
 * Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demonstrating how to create and access a simple table.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";
#define	NUM_THREADS	10

WT_CONNECTION *conn;

void *scan_thread(void *arg) {
	WT_SESSION *session;
	WT_CURSOR *cursor;
	const char *key, *value;
	int ret;

	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->open_cursor(session, "table:access", NULL, NULL, &cursor);

	/* Show all records. */
	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);

		printf("Got record: %s : %s\n", key, value);
	}

	return (arg);
}

int main()
{
	WT_SESSION *session;
	WT_CURSOR *cursor;
	pthread_t threads[NUM_THREADS];
	int i, ret;

	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
	/* Note: further error checking omitted for clarity. */

	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->create_table(session, "access",
	    "key_format=S,value_format=S");
	ret = session->open_cursor(session, "table:access", NULL,
	    "overwrite", &cursor);
	cursor->set_key(cursor, "key1");
	cursor->set_value(cursor, "value1");
	ret = cursor->insert(cursor);
	ret = session->close(session, NULL);

	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_create(&threads[i], NULL, scan_thread, NULL);

	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_join(threads[i], NULL);

	ret = conn->close(conn, NULL);

	return (ret);
}
