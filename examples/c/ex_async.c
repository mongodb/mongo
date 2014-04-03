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
uint64_t search_id = -1;
int global_error = 0;

/*! [example callback implementation] */
static int
cb_asyncop(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *op, int ret, uint32_t flags)
{
	WT_ITEM k, v;
	const char *key, *value;
	int t_ret;

	(void)cb;
	t_ret = 0;
	if (ret != 0) {
		/*! [Get identifier] */
		printf("ID %" PRIu64 " error %d\n", op->get_id(op), ret);
		/*! [Get identifier] */
		global_error = ret;
		return (1);
	}
	if (op->get_id(op) == search_id) {
		/*! [Get the op's string key] */
		t_ret = op->get_key(op, &k);
		key = k.data;
		/*! [Get the op's string key] */
		/*! [Get the op's string value] */
		t_ret = op->get_value(op, &v);
		value = v.data;
		/*! [Get the op's string value] */
		printf("Got record: %s : %s\n", key, value);
	}
	return (t_ret);
}

static WT_ASYNC_CALLBACK cb = { cb_asyncop };
/*! [example callback implementation] */
#define	MAX_KEYS	15

int main(void)
{
	/*! [example connection] */
	WT_ASYNC_OP *op, *opget;
	WT_CONNECTION *wt_conn;
	WT_ITEM key, value;
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
			fprintf(stderr, "Iter %d: table 1 ret %d\n",i,ret);
			sleep(1);
			goto retry;
		}
		/*! [Allocate a handle] */
		snprintf(k[i], sizeof(k), "key%d", i);
		snprintf(v[i], sizeof(v), "value%d", i);
		/*! [Set the op's string key] */
		key.data = k[i];
		key.size = sizeof(k[i]);
		value.data = v[i];
		value.size = sizeof(v[i]);
		op->set_key(op, &key);
		/*! [Set the op's string key] */
		/*! [Set the op's string value] */
		op->set_value(op, &value);
		/*! [Set the op's string value] */
		/*! [example insert] */
		ret = op->insert(op);
		/*! [example insert] */
	}

	ret = wt_conn->async_new_op(wt_conn, uri, "raw", &cb, &opget);
	snprintf(k[0], sizeof(k[0]), "key1");
	key.data = k[0];
	key.size = sizeof(k[0]);
	opget->set_key(opget, &key);
	search_id = opget->get_id(opget);
	opget->search(opget);

	/*! [flush] */
	wt_conn->async_flush(wt_conn);
	/*! [flush] */

	/*! [example close] */
	ret = wt_conn->close(wt_conn, NULL);
	/*! [example close] */

	return (ret);
}
