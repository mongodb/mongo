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
 * ex_async.c
 * 	demonstrates how to use the asynchronous API.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>

const char *home = NULL;
const char *uri = "table:async";
int global_error = 0;

/*! [example callback implementation] */
static int
cb_asyncop(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *op, int ret, uint32_t flags)
{
	WT_ASYNC_OPTYPE type;
	WT_ITEM k, v;
	const char *key, *value;
	uint64_t id;
	int t_ret;

	(void)cb;
	(void)flags;
	/*! [Get type] */
	type = op->get_type(op);
	/*! [Get type] */
	/*! [Get identifier] */
	id = op->get_id(op);
	/*! [Get identifier] */
	t_ret = 0;
	if (ret != 0) {
		printf("ID %" PRIu64 " error %d\n", id, ret);
		global_error = ret;
		return (1);
	}
	if (type == WT_AOP_SEARCH) {
		/*! [Get the operation's string key] */
		t_ret = op->get_key(op, &k);
		key = k.data;
		/*! [Get the operation's string key] */
		/*! [Get the operation's string value] */
		t_ret = op->get_value(op, &v);
		value = v.data;
		/*! [Get the operation's string value] */
		printf("Id %" PRIu64 " got record: %s : %s\n", id, key, value);
	}
	return (t_ret);
}

WT_ASYNC_CALLBACK cb = { cb_asyncop };
/*! [example callback implementation] */
#define	MAX_KEYS	15

int main(void)
{
	/*! [example connection] */
	WT_ASYNC_OP *op;
	WT_CONNECTION *wt_conn;
	WT_SESSION *session;
	int i, ret;
	char k[MAX_KEYS][16], v[MAX_KEYS][16];

	if ((ret = wiredtiger_open(home, NULL,
	    "create,cache_size=100MB,async=(enabled=true,ops_max=10,threads=2)",
	    &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}
	/*! [example connection] */

	/*! [example table create] */
	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri,
	    "key_format=S,value_format=S");
	/*! [example table create] */

	for (i = 0; i < MAX_KEYS; i++) {
		/*! [Allocate a handle] */
		op = NULL;
retry:
		ret = wt_conn->async_new_op(wt_conn, uri, NULL, &cb, &op);
		if (ret != 0) {
			/*
			 * If we used up all the ops, pause and retry to
			 * give the workers a chance to process them.
			 */
			fprintf(stderr,
			    "Iteration %d: async_new_op ret %d\n",i,ret);
			sleep(1);
			goto retry;
		}
		/*! [Allocate a handle] */
		snprintf(k[i], sizeof(k), "key%d", i);
		snprintf(v[i], sizeof(v), "value%d", i);
		/*! [Set the operation's string key] */
		op->set_key(op, k[i]);
		/*! [Set the operation's string key] */
		/*! [Set the operation's string value] */
		op->set_value(op, v[i]);
		/*! [Set the operation's string value] */
		/*! [example insert] */
		ret = op->insert(op);
		/*! [example insert] */
	}

	/*! [flush] */
	wt_conn->async_flush(wt_conn);
	/*! [flush] */
	/*! [Compact a table] */
	ret = wt_conn->async_new_op(wt_conn, uri, "timeout=10", &cb, &op);
	op->compact(op);
	/*! [Compact a table] */

	for (i = 0; i < MAX_KEYS; i++) {
		op = NULL;
retry2:
		ret = wt_conn->async_new_op(wt_conn, uri, NULL, &cb, &op);
		if (ret != 0) {
			/*
			 * If we used up all the ops, pause and retry to
			 * give the workers a chance to process them.
			 */
			fprintf(stderr,
			    "Iteration %d: async_new_op ret %d\n",i,ret);
			sleep(1);
			goto retry2;
		}
		snprintf(k[i], sizeof(k), "key%d", i);
		op->set_key(op, k[i]);
		/*! [search] */
		op->search(op);
		/*! [search] */
	}

	/*! [example close] */
	/*
	 * Connection close automatically does an async_flush so it will
	 * allow all queued search operations to complete.
	 */
	ret = wt_conn->close(wt_conn, NULL);
	/*! [example close] */

	return (ret);
}
