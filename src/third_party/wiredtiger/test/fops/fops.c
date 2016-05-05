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

static void *fop(void *);
static void  print_stats(u_int);

typedef struct {
	int bulk;				/* bulk load */
	int bulk_unique;			/* bulk load of new file */
	int ckpt;				/* session.checkpoint */
	int create;				/* session.create */
	int create_unique;			/* session.create of new file */
	int cursor;				/* session.open_cursor */
	int drop;				/* session.drop */
	int rebalance;				/* session.rebalance */
	int upgrade;				/* session.upgrade */
	int verify;				/* session.verify */
} STATS;

static STATS *run_stats;

int
fop_start(u_int nthreads)
{
	struct timeval start, stop;
	double seconds;
	pthread_t *tids;
	u_int i;
	int ret;
	void *thread_ret;

	tids = NULL; /* Silence GCC 4.1 warning. */

	/* Create statistics and thread structures. */
	run_stats = dcalloc((size_t)(nthreads), sizeof(*run_stats));
	tids = dcalloc((size_t)(nthreads), sizeof(*tids));

	(void)gettimeofday(&start, NULL);

	/* Create threads. */
	for (i = 0; i < nthreads; ++i)
		if ((ret = pthread_create(
		    &tids[i], NULL, fop, (void *)(uintptr_t)i)) != 0)
			testutil_die(ret, "pthread_create");

	/* Wait for the threads. */
	for (i = 0; i < nthreads; ++i)
		(void)pthread_join(tids[i], &thread_ret);

	(void)gettimeofday(&stop, NULL);
	seconds = (stop.tv_sec - start.tv_sec) +
	    (stop.tv_usec - start.tv_usec) * 1e-6;

	print_stats(nthreads);
	printf("timer: %.2lf seconds (%d ops/second)\n",
	    seconds, (int)((nthreads * nops) / seconds));

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
	uintptr_t id;
	WT_RAND_STATE rnd;
	u_int i;

	id = (uintptr_t)arg;
	__wt_yield();		/* Get all the threads created. */

	s = &run_stats[id];
	__wt_random_init(&rnd);

	for (i = 0; i < nops; ++i, __wt_yield())
		switch (__wt_random(&rnd) % 10) {
		case 0:
			++s->bulk;
			obj_bulk();
			break;
		case 1:
			++s->create;
			obj_create();
			break;
		case 2:
			++s->cursor;
			obj_cursor();
			break;
		case 3:
			++s->drop;
			obj_drop(__wt_random(&rnd) & 1);
			break;
		case 4:
			++s->ckpt;
			obj_checkpoint();
			break;
		case 5:
			++s->upgrade;
			obj_upgrade();
			break;
		case 6:
			++s->rebalance;
			obj_rebalance();
			break;
		case 7:
			++s->verify;
			obj_verify();
			break;
		case 8:
			++s->bulk_unique;
			obj_bulk_unique(__wt_random(&rnd) & 1);
			break;
		case 9:
			++s->create_unique;
			obj_create_unique(__wt_random(&rnd) & 1);
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
		    "%2d:"
		    "\t" "bulk %3d, checkpoint %3d, create %3d, cursor %3d,\n"
		    "\t" "drop %3d, rebalance %3d, upgrade %3d, verify %3d\n",
		    id, s->bulk + s->bulk_unique, s->ckpt,
		    s->create + s->create_unique, s->cursor,
		    s->drop, s->rebalance, s->upgrade, s->verify);
}
