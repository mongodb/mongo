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
 */
#include "test_util.h"

/*
 * JIRA ticket reference: WT-1965
 * Test case description: The reported issue was that column store tables
 * exhibit high CPU usage when populated with sparse record IDs.
 * Failure mode: It isn't simple to make this test case failure explicit since
 * it is demonstrating an inefficiency rather than a correctness bug.
 */

void (*custom_die)(void) = NULL;

/* If changing field count also need to change set_value and get_value calls */
#define	NR_FIELDS 8
#define	NR_OBJECTS 100
#define	NR_THREADS 4

static uint64_t g_ts = 0;

/*
 * Each thread inserts a set of keys into the record store database. The keys
 * are generated in such a way that there are large gaps in the key range.
 */
static void *
thread_func(void *arg)
{
	TEST_OPTS *opts;
	WT_CURSOR *cursor, *idx_cursor;
	WT_SESSION *session;
	uint64_t i, ins_rotor, ins_thr_idx, thr_idx, ts;
	uint64_t *obj_data;

	opts = (TEST_OPTS *)arg;
	thr_idx = __wt_atomic_fetch_addv64(&opts->next_threadid, 1);
	ts = g_ts;
	obj_data = dcalloc(
	    (NR_OBJECTS/NR_THREADS + 1) * NR_FIELDS, sizeof(*obj_data));

	testutil_check(opts->conn->open_session(
	    opts->conn, NULL, NULL, &session));

	testutil_check(session->open_cursor(
	    session, opts->uri, NULL, NULL, &cursor));
	testutil_check(session->open_cursor(
	    session, "table:index", NULL, NULL, &idx_cursor));

	for (ins_rotor = 1; ins_rotor < 10; ++ins_rotor) {
		for (ins_thr_idx = thr_idx, i = 0; ins_thr_idx < NR_OBJECTS;
		    ins_thr_idx += NR_THREADS, i += NR_FIELDS) {

			testutil_check(
			    session->begin_transaction(session, "sync=false"));

			cursor->set_key(cursor, ins_thr_idx << 40 | ins_rotor);
			cursor->set_value(cursor, ts,
			    obj_data[i+0], obj_data[i+1], obj_data[i+2],
			    obj_data[i+3], obj_data[i+4], obj_data[i+5],
			    obj_data[i+6], obj_data[i+7]);
			testutil_check(cursor->insert(cursor));

			idx_cursor->set_key(
			    idx_cursor, ins_thr_idx << 40 | ts);
			idx_cursor->set_value(idx_cursor, ins_rotor);
			testutil_check(idx_cursor->insert(idx_cursor));

			testutil_check(
			    session->commit_transaction(session, NULL));

			/* change object fields */
			++obj_data[i + ((ins_thr_idx + ins_rotor) % NR_FIELDS)];
			++obj_data[i +
			    ((ins_thr_idx + ins_rotor + 1) % NR_FIELDS)];

			++g_ts;
			/* 5K updates/sec */
			(void)usleep(1000000ULL * NR_THREADS / 5000);
		}
	}

	testutil_check(session->close(session, NULL));
	free(obj_data);
	return (NULL);
}

int
main(int argc, char *argv[])
{
	TEST_OPTS *opts, _opts;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	pthread_t thr[NR_THREADS];
	size_t t;
	uint64_t f[NR_FIELDS], r, ts;
	int i, ret;
	char table_format[256];

	opts = &_opts;
	memset(opts, 0, sizeof(*opts));
	testutil_check(testutil_parse_opts(argc, argv, opts));
	testutil_make_work_dir(opts->home);

	testutil_check(wiredtiger_open(opts->home, NULL,
	    "create,cache_size=1G,checkpoint=(wait=30),"
	    "eviction_trigger=80,eviction_target=64,eviction_dirty_target=65,"
	    "log=(enabled,file_max=10M),"
	    "transaction_sync=(enabled=true,method=none)", &opts->conn));
	testutil_check(opts->conn->open_session(
	    opts->conn, NULL, NULL, &session));

	sprintf(table_format, "key_format=r,value_format=");
	for (i = 0; i < NR_FIELDS; i++)
		strcat(table_format, "Q");

	/* recno -> timestamp + NR_FIELDS * Q */
	testutil_check(session->create(
	    session, opts->uri, table_format));
	/* timestamp -> recno */
	testutil_check(session->create(session,
	    "table:index", "key_format=Q,value_format=Q"));

	testutil_check(session->close(session, NULL));

	for (t = 0; t < NR_THREADS; ++t)
		testutil_check(pthread_create(
		    &thr[t], NULL, thread_func, (void *)opts));

	for (t = 0; t < NR_THREADS; ++t)
		(void)pthread_join(thr[t], NULL);

	testutil_check(opts->conn->open_session(
	    opts->conn, NULL, NULL, &session));

	/* recno -> timestamp + NR_FIELDS * Q */
	testutil_check(session->create(session, opts->uri, table_format));

	testutil_check(session->open_cursor(
	    session, opts->uri, NULL, NULL, &cursor));

	while ((ret = cursor->next(cursor)) == 0) {
		testutil_check(cursor->get_key(cursor, &r));
		testutil_check(cursor->get_value(cursor, &ts,
		    &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7]));

		if (!opts->verbose)
			continue;

		printf("(%" PRIu64 ",%llu)\t\t%" PRIu64,
		    (r >> 40), r & ((1ULL << 40) - 1), ts);

		for (i = 0; i < NR_FIELDS; i++)
			printf("\t%" PRIu64, f[i]);
		printf("\n");
	}
	testutil_assert(ret == WT_NOTFOUND);

	testutil_cleanup(opts);

	return (0);
}
