/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_pack.c
 *	This is an example demonstrating basic packing and unpacking of fields.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

int main(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	char buf[50];
	size_t size;
	int i, j, k, ret;

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	size = wiredtiger_struct_size("iii", 42, 1000, -9);
	if (size > sizeof (buf)) {
		/* Allocate a bigger buffer. */
	}
	wiredtiger_struct_pack(buf, size, "iii", 42, 1000, -9);
	wiredtiger_struct_unpack(buf, size, "iii", &i, &j, &k);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
