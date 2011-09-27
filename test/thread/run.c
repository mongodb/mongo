/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "thread.h"

static void  print_stats(int);
static void *reader(void *);
static void *writer(void *);

typedef struct {
	int remove;				/* cursor.remove */
	int update;				/* cursor.update */
	int reads;				/* cursor.search */
} STATS;

static STATS *run_stats;

/*
 * r --
 *	Return a 32-bit pseudo-random number.
 *
 * This is an implementation of George Marsaglia's multiply-with-carry pseudo-
 * random number generator.  Computationally fast, with reasonable randomness
 * properties.
 */
static inline uint32_t
r(void)
{
	static uint32_t m_w = 0, m_z = 0;

	if (m_w == 0) {
		struct timeval t;
		(void)gettimeofday(&t, NULL);
		m_w = (uint32_t)t.tv_sec;
		m_z = (uint32_t)t.tv_usec;
	}

	m_z = 36969 * (m_z & 65535) + (m_z >> 16);
	m_w = 18000 * (m_w & 65535) + (m_w >> 16);
	return (m_z << 16) + (m_w & 65535);
}

int
run(int readers, int writers)
{
	clock_t start, stop;
	double seconds;
	pthread_t *tids;
	int i, ret;
	void *thread_ret;

	/* Create statistics and thread structures. */
	if ((run_stats = calloc(
	    (size_t)(readers + writers), sizeof(*run_stats))) == NULL ||
	    (tids = calloc((size_t)(readers + writers), sizeof(*tids))) == NULL)
		die("calloc", errno);

	start = clock();

	/* Create threads. */
	for (i = 0; i < readers; ++i)
		if ((ret = pthread_create(
		    &tids[i], NULL, reader, (void *)(uintptr_t)i)) != 0)
			die("pthread_create", ret);
	for (; i < readers + writers; ++i) {
		if ((ret = pthread_create(
		    &tids[i], NULL, writer, (void *)(uintptr_t)i)) != 0)
			die("pthread_create", ret);
	}

	/* Wait for the threads. */
	for (i = 0; i < readers + writers; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	stop = clock();
	seconds = (stop - start) / (double)CLOCKS_PER_SEC;
	fprintf(stderr, "timer: %.2lf seconds (%d ops/second)\n",
	    seconds, (int)(((readers + writers) * nops) / seconds));

	print_stats(readers + writers);

	free(run_stats);
	free(tids);

	return (0);
}

/*
 * reader --
 *	Reader thread start function.
 */
static void *
reader(void *arg)
{
	STATS *s;
	WT_CURSOR *cursor;
	WT_ITEM *key, _key;
	WT_SESSION *session;
	pthread_t tid;
	u_int i, keyno;
	int id, ret;
	char keybuf[64];

	id = (int)(uintptr_t)arg;
	tid = pthread_self();
	printf(" read thread %2d starting: tid: %p\n", id, tid);
	sched_yield();		/* Get all the threads created. */

	key = &_key;
	s = &run_stats[id];

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.open_session", ret);

	if ((ret =
	    session->open_cursor(session, FNAME, NULL, NULL, &cursor)) != 0)
		die("session.open_cursor", ret);

	for (i = 0; i < nops; ++i, ++s->reads, sched_yield()) {
		keyno = r() % nkeys;
		if (ftype == ROW) {
			key->data = keybuf;
			key->size = (uint32_t)
			    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
			cursor->set_key(cursor, key);
		} else
			cursor->set_key(cursor, (uint32_t)keyno);
		if ((ret = cursor->search(cursor)) != 0 && ret != WT_NOTFOUND)
			die("cursor.search", ret);
	}

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);

	return (NULL);
}

/*
 * writer --
 *	Writer thread start function.
 */
static void *
writer(void *arg)
{
	STATS *s;
	WT_CURSOR *cursor;
	WT_ITEM *key, _key, *value, _value;
	WT_SESSION *session;
	pthread_t tid;
	u_int i, keyno;
	int id, ret;
	char keybuf[64], valuebuf[64];

	id = (int)(uintptr_t)arg;
	tid = pthread_self();
	printf("write thread %2d starting: tid: %p\n", id, tid);
	sched_yield();		/* Get all the threads created. */

	key = &_key;
	value = &_value;
	s = &run_stats[id];

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.open_session", ret);

	if ((ret =
	    session->open_cursor(session, FNAME, NULL, NULL, &cursor)) != 0)
		die("session.open_cursor", ret);

	for (i = 0; i < nops; ++i, sched_yield()) {
		keyno = r() % nkeys;
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
				die("cursor.remove", ret);
		} else {
			++s->update;
			value->data = valuebuf;
			if (ftype == FIX)
				cursor->set_value(cursor, 0x10);
			else {
				value->size = (uint32_t)
				    snprintf(valuebuf,
					sizeof(valuebuf), "XXX %37u", keyno);
				cursor->set_value(cursor, value);
			}
			if ((ret = cursor->update(cursor)) != 0)
				die("cursor.update", ret);
		}
	}

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);

	return (NULL);
}

/*
 * print_stats --
 *	Display reader/writer thread stats.
 */
static void
print_stats(int nthreads)
{
	STATS *s;
	int id;

	s = run_stats;
	for (id = 0; id < nthreads; ++id, ++s)
		printf("%2d: read: %6d, remove: %6d, update: %6d\n",
		    id, s->reads, s->remove, s->update);
}
