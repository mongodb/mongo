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

#include "cursor_order.h"

static void *append_insert(void *);
static void  print_stats(SHARED_CONFIG *);
static void *reverse_scan(void *);

typedef struct {
	char *name;				/* object name */
	uint64_t nops;				/* Thread op count */

	WT_RAND_STATE rnd;			/* RNG */

	int append_insert;			/* cursor.insert */
	int reverse_scans;			/* cursor.prev sequences */
	SHARED_CONFIG *cfg;
} INFO;

static INFO *run_info;

int
ops_start(SHARED_CONFIG *cfg)
{
	struct timeval start, stop;
	double seconds;
	pthread_t *tids;
	uint64_t i, name_index, offset, total_nops;
	int ret;
	void *thread_ret;

	tids = NULL;	/* Keep GCC 4.1 happy. */
	total_nops = 0;

	/* Create per-thread structures. */
	run_info = dcalloc((size_t)
	    (cfg->reverse_scanners + cfg->append_inserters), sizeof(*run_info));
	tids = dcalloc((size_t)
	    (cfg->reverse_scanners + cfg->append_inserters), sizeof(*tids));

	/* Create the files and load the initial records. */
	for (i = 0; i < cfg->append_inserters; ++i) {
		run_info[i].cfg = cfg;
		if (i == 0 || cfg->multiple_files) {
			run_info[i].name = dmalloc(64);
			snprintf(run_info[i].name, 64, FNAME, (int)i);

			/* Vary by orders of magnitude */
			if (cfg->vary_nops)
				run_info[i].nops =
				    WT_MAX(1000, cfg->max_nops >> i);
			load(cfg, run_info[i].name);
		} else
			run_info[i].name = run_info[0].name;

		/* Setup op count if not varying ops. */
		if (run_info[i].nops == 0)
			run_info[i].nops = cfg->max_nops;
		total_nops += run_info[i].nops;
	}

	/* Setup the reverse scanner configurations */
	for (i = 0; i < cfg->reverse_scanners; ++i) {
		offset = i + cfg->append_inserters;
		run_info[offset].cfg = cfg;
		if (cfg->multiple_files) {
			run_info[offset].name = dmalloc(64);
			/* Have reverse scans read from tables with writes. */
			name_index = i % cfg->append_inserters;
			snprintf(
			    run_info[offset].name, 64, FNAME, (int)name_index);

			/* Vary by orders of magnitude */
			if (cfg->vary_nops)
				run_info[offset].nops =
				    WT_MAX(1000, cfg->max_nops >> name_index);
		} else
			run_info[offset].name = run_info[0].name;

		/* Setup op count if not varying ops. */
		if (run_info[offset].nops == 0)
			run_info[offset].nops = cfg->max_nops;
		total_nops += run_info[offset].nops;
	}

	(void)gettimeofday(&start, NULL);

	/* Create threads. */
	for (i = 0; i < cfg->reverse_scanners; ++i)
		if ((ret = pthread_create(
		    &tids[i], NULL, reverse_scan, (void *)(uintptr_t)i)) != 0)
			testutil_die(ret, "pthread_create");
	for (; i < cfg->reverse_scanners + cfg->append_inserters; ++i) {
		if ((ret = pthread_create(
		    &tids[i], NULL, append_insert, (void *)(uintptr_t)i)) != 0)
			testutil_die(ret, "pthread_create");
	}

	/* Wait for the threads. */
	for (i = 0; i < cfg->reverse_scanners + cfg->append_inserters; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	(void)gettimeofday(&stop, NULL);
	seconds = (stop.tv_sec - start.tv_sec) +
	    (stop.tv_usec - start.tv_usec) * 1e-6;
	fprintf(stderr, "timer: %.2lf seconds (%d ops/second)\n",
	    seconds, (int)(((cfg->reverse_scanners + cfg->append_inserters) *
	    total_nops) / seconds));

	/* Verify the files. */
	for (i = 0; i < cfg->reverse_scanners + cfg->append_inserters; ++i) {
		verify(cfg, run_info[i].name);
		if (!cfg->multiple_files)
			break;
	}

	/* Output run statistics. */
	print_stats(cfg);

	/* Free allocated memory. */
	for (i = 0; i < cfg->reverse_scanners + cfg->append_inserters; ++i) {
		free(run_info[i].name);
		if (!cfg->multiple_files)
			break;
	}

	free(run_info);
	free(tids);

	return (0);
}

/*
 * reverse_scan_op --
 *	Walk a cursor back from the end of the file.
 */
static inline void
reverse_scan_op(
    SHARED_CONFIG *cfg, WT_SESSION *session, WT_CURSOR *cursor, INFO *s)
{
	uint64_t i, initial_key_range, prev_key, this_key;
	int ret;
	char *strkey;

	WT_UNUSED(session);
	WT_UNUSED(s);

	/* Make GCC 4.1 happy */
	prev_key = this_key = 0;

	/* Reset the cursor */
	if ((ret = cursor->reset(cursor)) != 0)
		testutil_die(ret, "cursor.reset");

	/* Save the key range. */
	initial_key_range = cfg->key_range - cfg->append_inserters;

	for (i = 0; i < cfg->reverse_scan_ops; i++) {
		if ((ret = cursor->prev(cursor)) != 0) {
			if (ret == WT_NOTFOUND)
				break;
			testutil_die(ret, "cursor.prev");
		}

		if (cfg->ftype == ROW) {
			if ((ret = cursor->get_key(cursor, &strkey)) != 0)
				testutil_die(ret, "cursor.get_key");
			this_key = (uint64_t)atol(strkey);
		} else
			if ((ret = cursor->get_key(
			    cursor, (uint64_t *)&this_key)) != 0)
				testutil_die(ret, "cursor.get_key");

		if (i == 0 && this_key < initial_key_range)
			testutil_die(ret,
			    "cursor scan start range wrong first prev %" PRIu64
			    " initial range: %" PRIu64,
			    this_key, initial_key_range);
		if (i != 0 && this_key >= prev_key)
			testutil_die(ret,
			    "cursor scan out of order this: %" PRIu64
			    " prev: %" PRIu64,
			    this_key, prev_key);
		prev_key = this_key;
	}
}

/*
 * reverse_scan --
 *	Reader thread start function.
 */
static void *
reverse_scan(void *arg)
{
	INFO *s;
	SHARED_CONFIG *cfg;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uintmax_t id;
	uint64_t i;
	int ret;
	char tid[128];

	id = (uintmax_t)arg;
	s = &run_info[id];
	cfg = s->cfg;
	__wt_thread_id(tid, sizeof(tid));
	__wt_random_init(&s->rnd);

	printf(" reverse scan thread %2" PRIuMAX
	    " starting: tid: %s, file: %s\n",
	    id, tid, s->name);

	__wt_yield();		/* Get all the threads created. */

	if ((ret = cfg->conn->open_session(
	    cfg->conn, NULL, "isolation=snapshot", &session)) != 0)
		testutil_die(ret, "conn.open_session");
	if ((ret = session->open_cursor(
	    session, s->name, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "session.open_cursor");
	for (i = 0; i < s->nops && !cfg->thread_finish;
	    ++i, ++s->reverse_scans, __wt_yield())
		reverse_scan_op(cfg, session, cursor, s);
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");

	printf(" reverse scan thread %2" PRIuMAX
	    " stopping: tid: %s, file: %s\n",
	    id, tid, s->name);

	/* Notify all other threads to finish once the first thread is done */
	cfg->thread_finish = true;

	return (NULL);
}

/*
 * append_insert_op --
 *	Write operation.
 */
static inline void
append_insert_op(
    SHARED_CONFIG *cfg, WT_SESSION *session, WT_CURSOR *cursor, INFO *s)
{
	WT_ITEM *value, _value;
	uint64_t keyno;
	int ret;
	char keybuf[64], valuebuf[64];

	WT_UNUSED(session);

	value = &_value;

	keyno = __wt_atomic_add64(&cfg->key_range, 1);
	if (cfg->ftype == ROW) {
		snprintf(keybuf, sizeof(keybuf), "%016u", (u_int)keyno);
		cursor->set_key(cursor, keybuf);
	} else
		cursor->set_key(cursor, (uint32_t)keyno);

	++s->append_insert;
	value->data = valuebuf;
	if (cfg->ftype == FIX)
		cursor->set_value(cursor, 0x10);
	else {
		value->size = (uint32_t)snprintf(
		    valuebuf, sizeof(valuebuf), "XXX %37u", (u_int)keyno);
		cursor->set_value(cursor, value);
	}
	if ((ret = cursor->insert(cursor)) != 0)
		testutil_die(ret, "cursor.insert");
}

/*
 * append_insert --
 *	Writer thread start function.
 */
static void *
append_insert(void *arg)
{
	INFO *s;
	SHARED_CONFIG *cfg;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uintmax_t id;
	uint64_t i;
	int ret;
	char tid[128];

	id = (uintmax_t)arg;
	s = &run_info[id];
	cfg = s->cfg;
	__wt_thread_id(tid, sizeof(tid));
	__wt_random_init(&s->rnd);

	printf("write thread %2" PRIuMAX " starting: tid: %s, file: %s\n",
	    id, tid, s->name);

	__wt_yield();		/* Get all the threads created. */

	if ((ret = cfg->conn->open_session(
	    cfg->conn, NULL, "isolation=snapshot", &session)) != 0)
		testutil_die(ret, "conn.open_session");
	if ((ret = session->open_cursor(
	    session, s->name, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "session.open_cursor");
	for (i = 0; i < s->nops && !cfg->thread_finish; ++i, __wt_yield())
		append_insert_op(cfg, session, cursor, s);
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");

	printf("write thread %2" PRIuMAX " stopping: tid: %s, file: %s\n",
	    id, tid, s->name);

	/* Notify all other threads to finish once the first thread is done */
	cfg->thread_finish = true;

	return (NULL);
}

/*
 * print_stats --
 *	Display reverse scan/writer thread stats.
 */
static void
print_stats(SHARED_CONFIG *cfg)
{
	INFO *s;
	uint64_t id, total_threads;

	total_threads = cfg->reverse_scanners + cfg->append_inserters;
	s = run_info;
	for (id = 0; id < total_threads; ++id, ++s)
		printf("%3d: reverse scans %6d, append inserts %6d\n",
		    (int)id, (int)s->reverse_scans, (int)s->append_insert);
}
