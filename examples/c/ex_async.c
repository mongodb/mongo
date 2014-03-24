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
#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = NULL;
const char *uri = "table:async";
uint64_t insert_done, search_id;
int global_error = 0;

/*! [async example callback implementation] */
static int
cb_asyncop(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *op, int ret, uint32_t flags)
{
	const char *key, *value;
	int t_ret;

	(void)cb;
	t_ret = 0;
	if (ret != 0) {
		printf("ID %" PRIu64 " error %d\n", op->get_id, ret);
		global_error = ret;
		return (1);
	}
	if (op->get_id == search_id) {
		t_ret = op->get_key(cursor, &key);
		t_ret = op->get_value(cursor, &value);
		printf("Got record: %s : %s\n", key, value);
	} else
		insert_done = 1;
	return (t_ret);
}

static WT_ASYNC_CALLBACK cb = { cb_asyncop };
/*! [async example callback implementation] */

int main(void)
{
	/*! [async example connection] */
	WT_ASYNC_OP *op, *op1;
	WT_CONNECTION *wt_conn;
	WT_SESSION *session;
	int ret;

	if ((ret = wiredtiger_open(home, NULL,
	    "create,async=(enabled=true,ops_max=10,threads=2)", &conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}
	/*! [async example connection] */

	/*! [async example table create] */
	ret = conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri,
	    "key_format=S,value_format=S");
	/*! [async example table create] */

	/*! [async example open cursor] */
	ret = conn->new_async_op(conn, uri, NULL, &cb, &op);
	ret = conn->new_async_op(conn, uri, NULL, &cb, &op1);
	/*! [async example open cursor] */

	/*! [async example insert] */
	op->set_key(op, "key1");
	op->set_value(op, "value1");
	ret = op->insert(op);
	
	op1->set_key(op1, "key1");
	search_id = op1->get_id(op1);
	op1->search(op1);
	/*! [async example insert] */

	/*! [async example close] */
	ret = conn->close(conn, NULL);
	/*! [async example close] */

	return (ret);
}
