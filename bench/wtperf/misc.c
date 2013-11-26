/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

#include "wtperf.h"

/*
 * Return total operations count for a group of threads.
 */
static uint64_t
sum_ops(CONFIG *cfg, size_t field_offset)
{
	CONFIG_THREAD *thread;
	uint64_t total;
	u_int i;

	total = 0;

	for (i = 0, thread = cfg->ckptthreads;
	    thread != NULL && i < cfg->checkpoint_threads;
	    ++i, ++thread)
		total += ((TRACK *)((uint8_t *)thread + field_offset))->ops;
	for (i = 0, thread = cfg->ithreads;
	    thread != NULL && i < cfg->insert_threads;
	    ++i, ++thread)
		total += ((TRACK *)((uint8_t *)thread + field_offset))->ops;
	for (i = 0, thread = cfg->rthreads;
	    thread != NULL && i < cfg->read_threads;
	    ++i, ++thread)
		total += ((TRACK *)((uint8_t *)thread + field_offset))->ops;
	for (i = 0, thread = cfg->uthreads;
	    thread != NULL && i < cfg->update_threads;
	    ++i, ++thread)
		total += ((TRACK *)((uint8_t *)thread + field_offset))->ops;

	return (total);
}

/*
 * Return total insert operations for the populate phase.
 */
uint64_t
sum_pop_ops(CONFIG *cfg)
{
	CONFIG_THREAD *thread;
	uint64_t total;
	u_int i;

	total = 0;

	for (i = 0, thread = cfg->popthreads;
	    thread != NULL && i < cfg->populate_threads; ++i, ++thread)
		total += thread->insert.ops;
	return (total);
}

uint64_t
sum_ckpt_ops(CONFIG *cfg)
{
	return (sum_ops(cfg, offsetof(CONFIG_THREAD, ckpt)));
}
uint64_t
sum_insert_ops(CONFIG *cfg)
{
	return (sum_ops(cfg, offsetof(CONFIG_THREAD, insert)));
}
uint64_t
sum_read_ops(CONFIG *cfg)
{
	return (sum_ops(cfg, offsetof(CONFIG_THREAD, read)));
}
uint64_t
sum_update_ops(CONFIG *cfg)
{
	return (sum_ops(cfg, offsetof(CONFIG_THREAD, update)));
}

/*
 * latency_monitor --
 *	Get average, minimum and maximum latency for this period.
 */
void
latency_monitor(CONFIG *cfg, uint32_t *avgp, uint32_t *minp, uint32_t *maxp)
{
	static uint32_t last_avg = 0, last_max = 0, last_min = 0;
	CONFIG_THREAD *thread;
	uint64_t ops, latency, tmp;
	uint32_t max, min;
	u_int i;

	ops = latency = 0;
	max = 0;
	min = UINT32_MAX;
	for (i = 0, thread = cfg->ithreads;
	    thread != NULL && i < cfg->insert_threads; ++i, ++thread) {
		tmp = thread->total_ops;
		ops += tmp - thread->last_ops;
		thread->last_ops = tmp;
		tmp = thread->total_latency;
		latency += tmp - thread->last_latency;
		thread->last_latency = tmp;

		if (min > thread->min_latency)
			min = thread->min_latency;
		thread->min_latency = UINT32_MAX;
		if (max < thread->max_latency)
			max = thread->max_latency;
		thread->max_latency = 0;
	}
	for (i = 0, thread = cfg->rthreads;
	    thread != NULL && i < cfg->read_threads; ++i, ++thread) {
		tmp = thread->total_ops;
		ops += tmp - thread->last_ops;
		thread->last_ops = tmp;
		tmp = thread->total_latency;
		latency += tmp - thread->last_latency;
		thread->last_latency = tmp;

		if (min > thread->min_latency)
			min = thread->min_latency;
		thread->min_latency = UINT32_MAX;
		if (max < thread->max_latency)
			max = thread->max_latency;
		thread->max_latency = 0;
	}
	for (i = 0, thread = cfg->uthreads;
	    thread != NULL && i < cfg->update_threads; ++i, ++thread) {
		tmp = thread->total_ops;
		ops += tmp - thread->last_ops;
		thread->last_ops = tmp;
		tmp = thread->total_latency;
		latency += tmp - thread->last_latency;
		thread->last_latency = tmp;

		if (min > thread->min_latency)
			min = thread->min_latency;
		thread->min_latency = UINT32_MAX;
		if (max < thread->max_latency)
			max = thread->max_latency;
		thread->max_latency = 0;
	}

	/*
	 * If nothing happened, graph the average, minimum and maximum as they
	 * were the last time, it keeps the graphs from having discontinuities.
	 */
	if (ops == 0) {
		*avgp = last_avg;
		*minp = last_min;
		*maxp = last_max;
	} else {
		*minp = last_min = min;
		*maxp = last_max = max;
		*avgp = last_avg = (uint32_t)(latency / ops);
	}
}

/*
 * sum_latency_thread --
 *	Sum latency for a single thread.
 */
static void
sum_latency_thread(CONFIG_THREAD *thread, size_t field_offset, TRACK *total)
{
	TRACK *trk;
	u_int i;

	trk = (TRACK *)((uint8_t *)thread + field_offset);

	for (i = 0; i < ELEMENTS(trk->us); ++i) {
		total->ops += trk->us[i];
		total->us[i] += trk->us[i];
	}
	for (i = 0; i < ELEMENTS(trk->ms); ++i) {
		total->ops += trk->ms[i];
		total->ms[i] += trk->ms[i];
	}
	for (i = 0; i < ELEMENTS(trk->sec); ++i) {
		total->ops += trk->sec[i];
		total->sec[i] += trk->sec[i];
	}
}

/*
 * sum_latency --
 *	Sum latency for a set of threads.
 */
static void
sum_latency(CONFIG *cfg, size_t field_offset, TRACK *total)
{
	CONFIG_THREAD *thread;
	u_int i;

	memset(total, 0, sizeof(*total));

	for (i = 0, thread = cfg->ithreads;
	    thread != NULL && i < cfg->insert_threads; ++i, ++thread)
		sum_latency_thread(thread, field_offset, total);
	for (i = 0, thread = cfg->rthreads;
	    thread != NULL && i < cfg->read_threads; ++i, ++thread)
		sum_latency_thread(thread, field_offset, total);
	for (i = 0, thread = cfg->uthreads;
	    thread != NULL && i < cfg->update_threads; ++i, ++thread)
		sum_latency_thread(thread, field_offset, total);
}

static void
sum_insert_latency(CONFIG *cfg, TRACK *total)
{
	sum_latency(cfg, offsetof(CONFIG_THREAD, insert), total);
}

static void
sum_read_latency(CONFIG *cfg, TRACK *total)
{
	sum_latency(cfg, offsetof(CONFIG_THREAD, read), total);
}

static void
sum_update_latency(CONFIG *cfg, TRACK *total)
{
	sum_latency(cfg, offsetof(CONFIG_THREAD, update), total);
}

static void
latency_print_single(CONFIG *cfg, TRACK *total, const char *name)
{
	FILE *fp;
	u_int i;
	uint64_t cumops;
	char path[1024];

	snprintf(path, sizeof(path), "%s/latency.%s", cfg->home, name);
	if ((fp = fopen(path, "w")) == NULL) {
		lprintf(cfg, errno, 0, "%s", path);
		return;
	}

#ifdef __WRITE_A_HEADER
	fprintf(fp,
	    "usecs,operations,cumulative-operations,total-operations\n");
#endif

	cumops = 0;
	for (i = 0; i < ELEMENTS(total->us); ++i) {
		if (total->us[i] == 0)
			continue;
		cumops += total->us[i];
		fprintf(fp,
		    "%u,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n",
		    (i + 1), total->us[i], cumops, total->ops);
	}
	for (i = 1; i < ELEMENTS(total->ms); ++i) {
		if (total->ms[i] == 0)
			continue;
		cumops += total->ms[i];
		fprintf(fp,
		    "%llu,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n",
		    ms_to_us(i + 1), total->ms[i], cumops, total->ops);
	}
	for (i = 1; i < ELEMENTS(total->sec); ++i) {
		if (total->sec[i] == 0)
			continue;
		cumops += total->sec[i];
		fprintf(fp,
		    "%llu,%" PRIu32 ",%" PRIu64 ",%" PRIu64 "\n",
		    sec_to_us(i + 1), total->sec[i], cumops, total->ops);
	}

	(void)fclose(fp);
}

void
latency_print(CONFIG *cfg)
{
	TRACK total;

	sum_insert_latency(cfg, &total);
	latency_print_single(cfg, &total, "insert");
	sum_read_latency(cfg, &total);
	latency_print_single(cfg, &total, "read");
	sum_update_latency(cfg, &total);
	latency_print_single(cfg, &total, "update");
}

int
enomem(const CONFIG *cfg)
{
	const char *msg;

	msg = "Unable to allocate memory";
	if (cfg->logf == NULL)
		fprintf(stderr, "%s\n", msg);
	else
		lprintf(cfg, ENOMEM, 0, "%s", msg);
	return (ENOMEM);
}

const char *
op_name(uint8_t *op)
{
	switch (*op) {
	case WORKER_INSERT:
		return ("insert");
	case WORKER_INSERT_RMW:
		return ("insert_rmw");
	case WORKER_READ:
		return ("read");
	case WORKER_UPDATE:
		return ("update");
	default:
		return ("unknown");
	}
	/* NOTREACHED */
}

/* Setup the logging output mechanism. */
int
setup_log_file(CONFIG *cfg)
{
	char *fname;
	int ret;

	ret = 0;

	if (cfg->verbose < 1)
		return (0);

	if ((fname = calloc(strlen(cfg->home) +
	    strlen(cfg->table_name) + strlen(".stat") + 2, 1)) == NULL)
		return (enomem(cfg));

	sprintf(fname, "%s/%s.stat", cfg->home, cfg->table_name);
	if ((cfg->logf = fopen(fname, "w")) == NULL) {
		fprintf(stderr, "Statistics failed to open log file.\n");
		ret = EINVAL;
	} else {
		/* Use line buffering for the log file. */
		(void)setvbuf(cfg->logf, NULL, _IOLBF, 0);
	}
	free(fname);
	return (ret);
}

/*
 * Log printf - output a log message.
 */
void
lprintf(const CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
{
	va_list ap;

	if (err == 0 && level <= cfg->verbose) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, "\n");

		if (level < cfg->verbose) {
			va_start(ap, fmt);
			vprintf(fmt, ap);
			va_end(ap);
			printf("\n");
		}
	}
	if (err == 0)
		return;

	/* We are dealing with an error. */
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " Error: %s\n", wiredtiger_strerror(err));
	if (cfg->logf != NULL) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, " Error: %s\n", wiredtiger_strerror(err));
	}

	/* Never attempt to continue if we got a panic from WiredTiger. */
	if (err == WT_PANIC)
		exit(1);
}
