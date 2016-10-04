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
 *
 * ex_async.c
 * 	demonstrates how to use the asynchronous API.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include "windows_shim.h"
#endif

#include <wiredtiger.h>

#if defined(_lint)
#define	ATOMIC_ADD(v, val)      ((v) += (val), (v))
#elif defined(_WIN32)
#define	ATOMIC_ADD(v, val)      (_InterlockedExchangeAdd(&(v), val) + val)
#else
#define	ATOMIC_ADD(v, val)      __sync_add_and_fetch(&(v), val)
#endif

static int global_error = 0;

/*! [async example callback implementation] */
typedef struct {
	WT_ASYNC_CALLBACK iface;
	uint32_t num_keys;
} ASYNC_KEYS;

static int
async_callback(WT_ASYNC_CALLBACK *cb,
    WT_ASYNC_OP *op, int wiredtiger_error, uint32_t flags)
{
	ASYNC_KEYS *asynckey = (ASYNC_KEYS *)cb;
	WT_ASYNC_OPTYPE type;
	WT_ITEM k, v;
	const char *key, *value;
	uint64_t id;
	int ret;

	(void)flags;				/* Unused */

	ret = 0;

	/*! [async get type] */
	/* Retrieve the operation's WT_ASYNC_OPTYPE type. */
	type = op->get_type(op);
	/*! [async get type] */

	/*! [async get identifier] */
	/* Retrieve the operation's 64-bit identifier. */
	id = op->get_id(op);
	/*! [async get identifier] */

	/* Check for a WiredTiger error. */
	if (wiredtiger_error != 0) {
		fprintf(stderr,
		    "ID %" PRIu64 " error %d: %s\n",
		    id, wiredtiger_error,
		    wiredtiger_strerror(wiredtiger_error));
		global_error = wiredtiger_error;
		return (1);
	}

	/* If doing a search, retrieve the key/value pair. */
	if (type == WT_AOP_SEARCH) {
		/*! [async get the operation's string key] */
		ret = op->get_key(op, &k);
		key = k.data;
		/*! [async get the operation's string key] */
		/*! [async get the operation's string value] */
		ret = op->get_value(op, &v);
		value = v.data;
		/*! [async get the operation's string value] */
		ATOMIC_ADD(asynckey->num_keys, 1);
		printf("Id %" PRIu64 " got record: %s : %s\n", id, key, value);
	}
	return (ret);
}
/*! [async example callback implementation] */

static ASYNC_KEYS ex_asynckeys = { {async_callback}, 0 };

#define	MAX_KEYS	15

int
main(void)
{
	WT_ASYNC_OP *op;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int i, ret;
	const char *home;
	char k[MAX_KEYS][16], v[MAX_KEYS][16];

	/*
	 * Create a clean test directory for this run of the test program if the
	 * environment variable isn't already set (as is done by make check).
	 */
	if (getenv("WIREDTIGER_HOME") == NULL) {
		home = "WT_HOME";
		ret = system("rm -rf WT_HOME && mkdir WT_HOME");
	} else
		home = NULL;

	/*! [async example connection] */
	ret = wiredtiger_open(home, NULL,
	    "create,cache_size=100MB,"
	    "async=(enabled=true,ops_max=20,threads=2)", &conn);
	/*! [async example connection] */

	/*! [async example table create] */
	ret = conn->open_session(conn, NULL, NULL, &session);
	ret = session->create(
	    session, "table:async", "key_format=S,value_format=S");
	/*! [async example table create] */

	/* Insert a set of keys asynchronously. */
	for (i = 0; i < MAX_KEYS; i++) {
		/*! [async handle allocation] */
		while ((ret = conn->async_new_op(conn,
		    "table:async", NULL, &ex_asynckeys.iface, &op)) != 0) {
			/*
			 * If we used up all the handles, pause and retry to
			 * give the workers a chance to catch up.
			 */
			fprintf(stderr,
			    "asynchronous operation handle not available\n");
			if (ret == EBUSY)
				sleep(1);
			else
				return (EXIT_FAILURE);
		}
		/*! [async handle allocation] */

		/*! [async insert] */
		/*
		 * Set the operation's string key and value, and then do
		 * an asynchronous insert.
		 */
		/*! [async set the operation's string key] */
		snprintf(k[i], sizeof(k), "key%d", i);
		op->set_key(op, k[i]);
		/*! [async set the operation's string key] */

		/*! [async set the operation's string value] */
		snprintf(v[i], sizeof(v), "value%d", i);
		op->set_value(op, v[i]);
		/*! [async set the operation's string value] */

		ret = op->insert(op);
		/*! [async insert] */
	}

	/*! [async flush] */
	/* Wait for all outstanding operations to complete. */
	ret = conn->async_flush(conn);
	/*! [async flush] */

	/*! [async compaction] */
	/*
	 * Compact a table asynchronously, limiting the run-time to 5 minutes.
	 */
	ret = conn->async_new_op(
	    conn, "table:async", "timeout=300", &ex_asynckeys.iface, &op);
	ret = op->compact(op);
	/*! [async compaction] */

	/* Search for the keys we just inserted, asynchronously. */
	for (i = 0; i < MAX_KEYS; i++) {
		while ((ret = conn->async_new_op(conn,
		    "table:async", NULL, &ex_asynckeys.iface, &op)) != 0) {
			/*
			 * If we used up all the handles, pause and retry to
			 * give the workers a chance to catch up.
			 */
			fprintf(stderr,
			    "asynchronous operation handle not available\n");
			if (ret == EBUSY)
				sleep(1);
			else
				return (EXIT_FAILURE);
		}

		/*! [async search] */
		/*
		 * Set the operation's string key and value, and then do
		 * an asynchronous search.
		 */
		snprintf(k[i], sizeof(k), "key%d", i);
		op->set_key(op, k[i]);
		ret = op->search(op);
		/*! [async search] */
	}

	/*
	 * Connection close automatically does an async_flush so it will wait
	 * for all queued search operations to complete.
	 */
	ret = conn->close(conn, NULL);

	printf("Searched for %" PRIu32 " keys\n", ex_asynckeys.num_keys);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
