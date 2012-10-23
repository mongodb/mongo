/*-
 * Public Domain 2008-2012 WiredTiger, Inc.
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
 * ex_cache_pool.c
 * 	demonstrates how to create multiple databases using a cache pool
 */
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

#include <wiredtiger.h>

int populate_table(WT_SESSION *, const char *, uint64_t);

#define	home "WT_TEST"
#define	table_name "table:cache_pool"
const char *pool_name = "pool";

int main(void)
{
	WT_CONNECTION *conn1, *conn2;
	WT_CURSOR *cursor;
	WT_SESSION *session1, *session2;
	int i, ret;

	/* TODO: Is this OK? It's horrible. */
	ret = mkdir(home "/" home "1", 755);
	ret = mkdir(home "/" home "2", 755);

	/* Create cache that can be shared between multiple databases. */
	if ((ret = wiredtiger_open(
	    home "/" home "1", NULL,
	    "create,cache_pool=pool,cache_pool_size=250M,cache_pool_chunk=50M,"
	    "cache_pool_quota=150M,verbose=[cache_pool]",
	    &conn1)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home "1", wiredtiger_strerror(ret));

	if ((ret = conn1->open_session(conn1, NULL, NULL, &session1)) != 0)
		fprintf(stderr, "Error opening session for %s: %s\n",
		    home, wiredtiger_strerror(ret));

	ret = session1->create(session1,
	    table_name "1", "key_format=L,value_format=S");
	populate_table(session1, table_name "1", 100000);

	/* Create a second connection that shares the cache pool. */
	if ((ret = wiredtiger_open(
	    home "/" home "2", NULL,
	    "create,cache_pool=pool,verbose=[cache_pool]",
	    &conn2)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home "2", wiredtiger_strerror(ret));

	if ((ret = conn2->open_session(conn2, NULL, NULL, &session2)) != 0)
		fprintf(stderr, "Error opening session for %s: %s\n",
		    home "2", wiredtiger_strerror(ret));

	ret = session2->create(session2,
	    table_name "2", "key_format=L,value_format=S");
	populate_table(session2, table_name "2", 100000);

	/* Force session one to require more cache. */
	ret = session1->create(session1,
	    table_name "1", "key_format=L,value_format=S");
	populate_table(session1, table_name "1", 1000000);

	printf("Entering populate phase.\n");
	for (i = 0; i < 20; i++) {
		populate_table(session1, table_name "1", 250000);
		populate_table(session2, table_name "2", 250000);
	}
	/* Stop using the second connection - see if the cache is reduced. */
	printf("Entering single connection update phase.\n");
	for (i = 0; i < 15; i++)
		populate_table(session1, table_name "1", 250000);
	printf("Entering single connection read phase.\n");
	for (i = 0; i < 5; i++) {
		ret = session2->open_cursor(
		    session2, table_name "2", NULL, NULL, &cursor);
		while (cursor->next(cursor) == 0) {}
		cursor->close(cursor);
	}

	ret = conn1->close(conn1, NULL);
	ret = conn2->close(conn2, NULL);

	return (ret);
}

int populate_table(WT_SESSION *session, const char *table, uint64_t nops)
{
	WT_CURSOR *cursor;
	int ret;
	uint64_t i, start;

	ret = session->open_cursor(session, table, NULL, NULL, &cursor);
	start = 0;
	if (cursor->prev(cursor) != WT_NOTFOUND) {
		cursor->get_key(cursor, &start);
		cursor->reset(cursor);
	}

	cursor->set_value(cursor,
	    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
	for (i = start + 1; i < nops + start; i++) {
		cursor->set_key(cursor, i);
		ret = cursor->insert(cursor);
	}
	cursor->close(cursor);
	return (ret);
}
