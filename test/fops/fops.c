/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

static void *fop(void *);
static void  print_stats(u_int);

typedef struct {
	int create;				/* session.create */
	int drop;				/* session.drop */
	int ckpt;				/* session.checkpoint */
	int upgrade;				/* session.upgrade */
	int verify;				/* session.verify */
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
fop_start(u_int nthreads)
{
	clock_t start, stop;
	double seconds;
	pthread_t *tids;
	u_int i;
	int ret;
	void *thread_ret;

	/* Create statistics and thread structures. */
	if ((run_stats = calloc(
	    (size_t)(nthreads), sizeof(*run_stats))) == NULL ||
	    (tids = calloc((size_t)(nthreads), sizeof(*tids))) == NULL)
		die("calloc", errno);

	start = clock();

	/* Create threads. */
	for (i = 0; i < nthreads; ++i)
		if ((ret = pthread_create(
		    &tids[i], NULL, fop, (void *)(uintptr_t)i)) != 0)
			die("pthread_create", ret);

	/* Wait for the threads. */
	for (i = 0; i < nthreads; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	stop = clock();
	seconds = (stop - start) / (double)CLOCKS_PER_SEC;
	fprintf(stderr, "timer: %.2lf seconds (%d ops/second)\n",
	    seconds, (int)((nthreads * nops) / seconds));

	print_stats(nthreads);

	free(run_stats);
	free(tids);

	return (0);
}

/*
 * fop --
 *	File operation function.
 */
static void *
fop(void *arg)
{
	STATS *s;
	pthread_t tid;
	u_int i;
	int id;

	id = (int)(uintptr_t)arg;
	tid = pthread_self();
	printf(
	    "file operation thread %2d starting: tid: %p\n", id, (void *)tid);
	sched_yield();		/* Get all the threads created. */

	s = &run_stats[id];

	for (i = 0; i < nops; ++i, sched_yield())
		switch (r() % 5) {
		case 0:
			++s->create;
			obj_create();
			break;
		case 1:
			++s->drop;
			obj_drop();
			break;
		case 2:
			++s->ckpt;
			obj_checkpoint();
			break;
		case 3:
			++s->upgrade;
			obj_upgrade();
			break;
		case 4:
			++s->verify;
			obj_verify();
			break;
		default:
			break;
		}

	return (NULL);
}

/*
 * print_stats --
 *	Display file operation thread stats.
 */
static void
print_stats(u_int nthreads)
{
	STATS *s;
	u_int id;

	s = run_stats;
	for (id = 0; id < nthreads; ++id, ++s)
		printf(
		    "%3d: create %6d, drop %6d, ckpt %6d, upgrade %6d, "
		    "verify %6d\n",
		    id, s->create, s->drop, s->ckpt, s->upgrade, s->verify);
}
