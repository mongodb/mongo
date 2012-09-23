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
 * ex_rand_read.c
 *	This is an application that executes parallel random read workload.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>

#include <wiredtiger.h>

int populate(WT_CONNECTION *);
void *read_thread(void *arg);

const char *home = "WT_TEST";
const char *table_name = "table:home";
const uint32_t key_count = 2000000;
const uint32_t data_size = 100;
const uint32_t NUM_THREADS = 5;
const uint32_t RUNTIME = 20;		/* Seconds. */
const uint32_t rand_seed = 14023954;
uint64_t nops;

int running;

void *
read_thread(void *arg)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	conn = (WT_CONNECTION *)arg;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		printf("open_session failed in read thread: %d\n", ret);
		return NULL;
	}
	if ((ret = session->open_cursor(session, table_name,
	    NULL, NULL, &cursor)) != 0) {
		printf("open_cursor failed in read thread: %d\n", ret);
		return NULL;
	}

	while (running) {
		++nops;
		/*
		if (nops % 1000000 == 0)
			printf(".");
		if (nops % 60000000 == 0)
			printf("\r");
		*/
		cursor->set_key(cursor, rand() % key_count);
		cursor->search(cursor);
	}
	session->close(session, NULL);
	return (arg);
}

int populate(WT_CONNECTION *conn)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *dbuf;
	int ret;
	uint32_t i;

	dbuf = calloc(data_size, 1);
	if (dbuf == NULL)
		return (ENOMEM);

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	ret = session->create(session, table_name,
	    "key_format=L,value_format=S,lsm_chunk_size=16MB");

	ret = session->open_cursor(
	    session, table_name, NULL, "bulk", &cursor);

	memset(dbuf, 'a', data_size - 1);
	cursor->set_value(cursor, dbuf);
	/* Populate the database. */
	for (i = 0; i < key_count; i++) {
		cursor->set_key(cursor, i);
		cursor->insert(cursor);
	}
	cursor->close(cursor);
	session->close(session, NULL);
	printf("Finished bulk load of %d items, waiting on merges\n", key_count);

	sleep(20);
	return (ret);
}

int main(int argc, char **argv)
{
	WT_CONNECTION *conn;
	int ch, ret;
	pthread_t threads[NUM_THREADS];
	uint32_t create, i;

	create = 0;
	while ((ch = getopt(argc, argv, "c")) != EOF)
		switch (ch) {
		case 'c':
			create = 1;
			break;
		case '?':
		default:
			printf("Invalid option\n");
			return (EINVAL);
		}

	srand(rand_seed);

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create,cache_size=50MB,verbose=[lsm]", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	if (create)
		populate(conn);

	printf("Starting read threads\n");
	running = 1;
	nops = 0;
	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_create(&threads[i], NULL, read_thread, conn);

	sleep(RUNTIME);
	running = 0;

	for (i = 0; i < NUM_THREADS; i++)
		ret = pthread_join(threads[i], NULL);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	printf("Ran random read test with %d threads for %d seconds.\n",
	    NUM_THREADS, RUNTIME);
	printf("Executed %" PRIu64 " operations\n", nops); 
	return (ret);
}
