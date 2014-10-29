/*-
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
 *
 * ex_scope.c
 * 	demonstrates the scope of buffers holding cursor keys and values.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

#ifdef _WIN32
/* snprintf is not supported on <= VS2013 */
#define	snprintf _snprintf
#endif

static const char *home;

static int
cursor_scope_ops(WT_CURSOR *cursor)
{
	struct {
		const char *key;
		const char *value;
		int (*apply)(WT_CURSOR *);
	} *op, ops[] = {
		{ "key1", "value1", cursor->insert, },
		{ "key1", "value2", cursor->update, },
		{ "key1", "value2", cursor->search, },
		{ "key1", "value2", cursor->remove, },
		{ NULL, NULL, NULL }
	};
	const char *key, *value;
	char keybuf[10], valuebuf[10];
	int ret;

	for (op = ops; op->key != NULL; op++) {
		key = value = NULL;

		/*! [cursor scope operation] */
		(void)snprintf(keybuf, sizeof(keybuf), "%s", op->key);
		cursor->set_key(cursor, keybuf);
		(void)snprintf(valuebuf, sizeof(valuebuf), "%s", op->value);
		cursor->set_value(cursor, valuebuf);

		/*
		 * The application must keep the key and value memory valid
		 * until the next operation that positions the cursor.
		 * Modifying either the key or value buffers is not permitted.
		 */

		/* Apply the operation (insert, update, search or remove). */
		if ((ret = op->apply(cursor)) != 0) {
			fprintf(stderr, "Error performing the operation: %s\n",
			    wiredtiger_strerror(ret));
			return (ret);
		}

		/*
		 * Except for WT_CURSOR::insert, the cursor has been positioned
		 * and no longer references application memory, so application
		 * buffers can be safely overwritten.
		 */
		if (op->apply != cursor->insert) {
			strcpy(keybuf, "no key");
			strcpy(valuebuf, "no value");
		}

		/*
		 * Check that get_key/value behave as expected after the
		 * operation.
		 */
		if ((ret = cursor->get_key(cursor, &key)) != 0 ||
		    (op->apply != cursor->remove &&
		    (ret = cursor->get_value(cursor, &value)) != 0)) {
			fprintf(stderr, "Error in get_key/value: %s\n",
			    wiredtiger_strerror(ret));
			return (ret);
		}

		/*
		 * Except for WT_CURSOR::insert (which does not position the
		 * cursor), the application now has pointers to memory owned
		 * by the cursor.  Modifying the memory referenced by either
		 * key or value is not permitted.
		 */

		/* Check that the cursor's key and value are what we expect. */
		if (op->apply != cursor->insert)
			if (key == keybuf ||
			    (op->apply != cursor->remove &&
			    value == valuebuf)) {
				fprintf(stderr,
				    "Cursor points at application memory!\n");
				return (EINVAL);
			}

		if (strcmp(key, op->key) != 0 ||
		    (op->apply != cursor->remove &&
		    strcmp(value, op->value) != 0)) {
			fprintf(stderr, "Unexpected key / value!\n");
			return (EINVAL);
		}
		/*! [cursor scope operation] */
	}

	return (0);
}

int
main(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int ret, tret;

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	/* Open a connection, create a simple table, open a cursor. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}

	ret = session->create(session,
	    "table:scope", "key_format=S,value_format=S,columns=(k,v)");

	ret = session->open_cursor(session,
	    "table:scope", NULL, NULL, &cursor);

	ret = cursor_scope_ops(cursor);

	/* Close the connection and clean up. */
	if ((tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}
