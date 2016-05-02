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

#include "thread.h"

static void  print_stats(u_int);
static void *reader(void *);
static void *writer(void *);

typedef struct {
	char *name;				/* object name */
	u_int nops;				/* Thread op count */

	WT_RAND_STATE rnd;			/* RNG */

	int remove;				/* cursor.remove */
	int update;				/* cursor.update */
	int reads;				/* cursor.search */
} INFO;

static INFO *run_info;

int
rw_start(u_int readers, u_int writers)
{
	struct timeval start, stop;
	double seconds;
	pthread_t *tids;
	u_int i, name_index, offset, total_nops;
	int ret;
	void *thread_ret;

	tids = NULL;	/* Keep GCC 4.1 happy. */
	total_nops = 0;

	/* Create per-thread structures. */
	run_info = dcalloc((size_t)(readers + writers), sizeof(*run_info));
	tids = dcalloc((size_t)(readers + writers), sizeof(*tids));

	/* Create the files and load the initial records. */
	for (i = 0; i < writers; ++i) {
		if (i == 0 || multiple_files) {
			run_info[i].name = dmalloc(64);
			snprintf(run_info[i].name, 64, FNAME, i);

			/* Vary by orders of magnitude */
			if (vary_nops)
				run_info[i].nops = WT_MAX(1000, max_nops >> i);
			load(run_info[i].name);
		} else
			run_info[i].name = run_info[0].name;

		/* Setup op count if not varying ops. */
		if (run_info[i].nops == 0)
			run_info[i].nops = max_nops;
		total_nops += run_info[i].nops;
	}

	/* Setup the reader configurations */
	for (i = 0; i < readers; ++i) {
		offset = i + writers;
		if (multiple_files) {
			run_info[offset].name = dmalloc(64);
			/* Have readers read from tables with writes. */
			name_index = i % writers;
			snprintf(
			    run_info[offset].name, 64, FNAME, name_index);

			/* Vary by orders of magnitude */
			if (vary_nops)
				run_info[offset].nops =
				    WT_MAX(1000, max_nops >> name_index);
		} else
			run_info[offset].name = run_info[0].name;

		/* Setup op count if not varying ops. */
		if (run_info[offset].nops == 0)
			run_info[offset].nops = max_nops;
		total_nops += run_info[offset].nops;
	}

	(void)gettimeofday(&start, NULL);

	/* Create threads. */
	for (i = 0; i < readers; ++i)
		if ((ret = pthread_create(
		    &tids[i], NULL, reader, (void *)(uintptr_t)i)) != 0)
			testutil_die(ret, "pthread_create");
	for (; i < readers + writers; ++i) {
		if ((ret = pthread_create(
		    &tids[i], NULL, writer, (void *)(uintptr_t)i)) != 0)
			testutil_die(ret, "pthread_create");
	}

	/* Wait for the threads. */
	for (i = 0; i < readers + writers; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	(void)gettimeofday(&stop, NULL);
	seconds = (stop.tv_sec - start.tv_sec) +
	    (stop.tv_usec - start.tv_usec) * 1e-6;
	fprintf(stderr, "timer: %.2lf seconds (%d ops/second)\n",
	    seconds, (int)(((readers + writers) * total_nops) / seconds));

	/* Verify the files. */
	for (i = 0; i < readers + writers; ++i) {
		verify(run_info[i].name);
		if (!multiple_files)
			break;
	}

	/* Output run statistics. */
	print_stats(readers + writers);

	/* Free allocated memory. */
	for (i = 0; i < readers + writers; ++i) {
		free(run_info[i].name);
		if (!multiple_files)
			break;
	}

	free(run_info);
	free(tids);

	return (0);
}

/*
 * reader_op --
 *	Read operation.
 */
static inline void
reader_op(WT_SESSION *session, WT_CURSOR *cursor, INFO *s)
{
	WT_ITEM *key, _key;
	u_int keyno;
	int ret;
	char keybuf[64];

	key = &_key;

	keyno = __wt_random(&s->rnd) % nkeys + 1;
	if (ftype == ROW) {
		key->data = keybuf;
		key->size = (uint32_t)
		    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
		cursor->set_key(cursor, key);
	} else
		cursor->set_key(cursor, (uint32_t)keyno);
	if ((ret = cursor->search(cursor)) != 0 && ret != WT_NOTFOUND)
		testutil_die(ret, "cursor.search");
	if (log_print)
		(void)session->log_printf(session,
		    "Reader Thread %p key %017u", pthread_self(), keyno);
}

/*
 * reader --
 *	Reader thread start function.
 */
static void *
reader(void *arg)
{
	INFO *s;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	u_int i;
	int id, ret;
	char tid[128];

	id = (int)(uintptr_t)arg;
	s = &run_info[id];
	__wt_thread_id(tid, sizeof(tid));
	__wt_random_init(&s->rnd);

	printf(" read thread %2d starting: tid: %s, file: %s\n",
	    id, tid, s->name);

	__wt_yield();		/* Get all the threads created. */

	if (session_per_op) {
		for (i = 0; i < s->nops; ++i, ++s->reads, __wt_yield()) {
			if ((ret = conn->open_session(
			    conn, NULL, NULL, &session)) != 0)
				testutil_die(ret, "conn.open_session");
			if ((ret = session->open_cursor(
			    session, s->name, NULL, NULL, &cursor)) != 0)
				testutil_die(ret, "session.open_cursor");
			reader_op(session, cursor, s);
			if ((ret = session->close(session, NULL)) != 0)
				testutil_die(ret, "session.close");
		}
	} else {
		if ((ret = conn->open_session(
		    conn, NULL, NULL, &session)) != 0)
			testutil_die(ret, "conn.open_session");
		if ((ret = session->open_cursor(
		    session, s->name, NULL, NULL, &cursor)) != 0)
			testutil_die(ret, "session.open_cursor");
		for (i = 0; i < s->nops; ++i, ++s->reads, __wt_yield())
			reader_op(session, cursor, s);
		if ((ret = session->close(session, NULL)) != 0)
			testutil_die(ret, "session.close");
	}

	printf(" read thread %2d stopping: tid: %s, file: %s\n",
	    id, tid, s->name);

	return (NULL);
}

/*
 * writer_op --
 *	Write operation.
 */
static inline void
writer_op(WT_SESSION *session, WT_CURSOR *cursor, INFO *s)
{
	WT_ITEM *key, _key, *value, _value;
	u_int keyno;
	int ret;
	char keybuf[64], valuebuf[64];

	key = &_key;
	value = &_value;

	keyno = __wt_random(&s->rnd) % nkeys + 1;
	if (ftype == ROW) {
		key->data = keybuf;
		key->size = (uint32_t)
		    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
		cursor->set_key(cursor, key);
	} else
		cursor->set_key(cursor, (uint32_t)keyno);
	if (keyno % 5 == 0) {
		++s->remove;
		if ((ret =
		    cursor->remove(cursor)) != 0 && ret != WT_NOTFOUND)
			testutil_die(ret, "cursor.remove");
	} else {
		++s->update;
		value->data = valuebuf;
		if (ftype == FIX)
			cursor->set_value(cursor, 0x10);
		else {
			value->size = (uint32_t)snprintf(
			    valuebuf, sizeof(valuebuf), "XXX %37u", keyno);
			cursor->set_value(cursor, value);
		}
		if ((ret = cursor->update(cursor)) != 0)
			testutil_die(ret, "cursor.update");
	}
	if (log_print)
		(void)session->log_printf(session,
		    "Writer Thread %p key %017u", pthread_self(), keyno);
}

/*
 * writer --
 *	Writer thread start function.
 */
static void *
writer(void *arg)
{
	INFO *s;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	u_int i;
	int id, ret;
	char tid[128];

	id = (int)(uintptr_t)arg;
	s = &run_info[id];
	__wt_thread_id(tid, sizeof(tid));
	__wt_random_init(&s->rnd);

	printf("write thread %2d starting: tid: %s, file: %s\n",
	    id, tid, s->name);

	__wt_yield();		/* Get all the threads created. */

	if (session_per_op) {
		for (i = 0; i < s->nops; ++i, __wt_yield()) {
			if ((ret = conn->open_session(
			    conn, NULL, NULL, &session)) != 0)
				testutil_die(ret, "conn.open_session");
			if ((ret = session->open_cursor(
			    session, s->name, NULL, NULL, &cursor)) != 0)
				testutil_die(ret, "session.open_cursor");
			writer_op(session, cursor, s);
			if ((ret = session->close(session, NULL)) != 0)
				testutil_die(ret, "session.close");
		}
	} else {
		if ((ret = conn->open_session(
		    conn, NULL, NULL, &session)) != 0)
			testutil_die(ret, "conn.open_session");
		if ((ret = session->open_cursor(
		    session, s->name, NULL, NULL, &cursor)) != 0)
			testutil_die(ret, "session.open_cursor");
		for (i = 0; i < s->nops; ++i, __wt_yield())
			writer_op(session, cursor, s);
		if ((ret = session->close(session, NULL)) != 0)
			testutil_die(ret, "session.close");
	}

	printf("write thread %2d stopping: tid: %s, file: %s\n",
	    id, tid, s->name);

	return (NULL);
}

/*
 * print_stats --
 *	Display reader/writer thread stats.
 */
static void
print_stats(u_int nthreads)
{
	INFO *s;
	u_int id;

	s = run_info;
	for (id = 0; id < nthreads; ++id, ++s)
		printf("%3d: read %6d, remove %6d, update %6d\n",
		    id, s->reads, s->remove, s->update);
}
