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

#include "wtperf.h"

static int
check_timing(CONFIG *cfg,
    const char *name, struct timespec start, struct timespec *stop)
{
	uint64_t last_interval;
	int ret;

	if ((ret = __wt_epoch(NULL, stop)) != 0) {
		lprintf(cfg, ret, 0,
		    "Get time failed in cycle_idle_tables.");
		cfg->error = ret;
		return (ret);
	}

	last_interval = (uint64_t)(WT_TIMEDIFF_SEC(*stop, start));

	if (last_interval > cfg->idle_table_cycle) {
		lprintf(cfg, ret, 0,
		    "Cycling idle table failed because %s took %" PRIu64
		    " seconds which is longer than configured acceptable"
		    " maximum of %" PRIu32 ".",
		    name, last_interval, cfg->idle_table_cycle);
		cfg->error = ETIMEDOUT;
		return (ETIMEDOUT);
	}
	return (0);
}
/*
 * Regularly create, open a cursor and drop a table.
 * Measure how long each step takes, and flag an error if it exceeds the
 * configured maximum.
 */
static void *
cycle_idle_tables(void *arg)
{
	struct timespec start, stop;
	CONFIG *cfg;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int cycle_count, ret;
	char uri[512];

	cfg = (CONFIG *)arg;
	cycle_count = 0;

	if ((ret = cfg->conn->open_session(
	    cfg->conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error opening a session on %s", cfg->home);
		return (NULL);
	}

	for (cycle_count = 0; cfg->idle_cycle_run; ++cycle_count) {
		snprintf(uri, 512, "%s_cycle%07d", cfg->uris[0], cycle_count);
		/* Don't busy cycle in this loop. */
		__wt_sleep(1, 0);

		/* Setup a start timer. */
		if ((ret = __wt_epoch(NULL, &start)) != 0) {
			lprintf(cfg, ret, 0,
			     "Get time failed in cycle_idle_tables.");
			cfg->error = ret;
			return (NULL);
		}

		/* Create a table. */
		if ((ret = session->create(
		    session, uri, cfg->table_config)) != 0) {
			if (ret == EBUSY)
				continue;
			lprintf(cfg, ret, 0,
			     "Table create failed in cycle_idle_tables.");
			cfg->error = ret;
			return (NULL);
		}
		if (check_timing(cfg, "create", start, &stop) != 0)
			return (NULL);
		start = stop;

		/* Open and close cursor. */
		if ((ret = session->open_cursor(
		    session, uri, NULL, NULL, &cursor)) != 0) {
			lprintf(cfg, ret, 0,
			     "Cursor open failed in cycle_idle_tables.");
			cfg->error = ret;
			return (NULL);
		}
		if ((ret = cursor->close(cursor)) != 0) {
			lprintf(cfg, ret, 0,
			     "Cursor close failed in cycle_idle_tables.");
			cfg->error = ret;
			return (NULL);
		}
		if (check_timing(cfg, "cursor", start, &stop) != 0)
			return (NULL);
		start = stop;

		/*
		 * Drop the table. Keep retrying on EBUSY failure - it is an
		 * expected return when checkpoints are happening.
		 */
		while ((ret = session->drop(session, uri, "force")) == EBUSY)
			__wt_sleep(1, 0);

		if (ret != 0 && ret != EBUSY) {
			lprintf(cfg, ret, 0,
			     "Table drop failed in cycle_idle_tables.");
			cfg->error = ret;
			return (NULL);
		}
		if (check_timing(cfg, "drop", start, &stop) != 0)
			return (NULL);
	}

	return (NULL);
}

/*
 * Start a thread the creates and drops tables regularly.
 * TODO: Currently accepts a pthread_t as a parameter, since it is not
 * possible to portably statically initialize it in the global configuration
 * structure. Should reshuffle the configuration structure so explicit static
 * initialization isn't necessary.
 */
int
start_idle_table_cycle(CONFIG *cfg, pthread_t *idle_table_cycle_thread)
{
	pthread_t thread_id;
	int ret;

	if (cfg->idle_table_cycle == 0)
		return (0);

	cfg->idle_cycle_run = true;
	if ((ret = pthread_create(
	    &thread_id, NULL, cycle_idle_tables, cfg)) != 0) {
		lprintf(
		    cfg, ret, 0, "Error creating idle table cycle thread.");
		cfg->idle_cycle_run = false;
		return (ret);
	}
	*idle_table_cycle_thread = thread_id;

	return (0);
}

int
stop_idle_table_cycle(CONFIG *cfg, pthread_t idle_table_cycle_thread)
{
	int ret;

	if (cfg->idle_table_cycle == 0 || !cfg->idle_cycle_run)
		return (0);

	cfg->idle_cycle_run = false;
	if ((ret = pthread_join(idle_table_cycle_thread, NULL)) != 0) {
		lprintf(
		    cfg, ret, 0, "Error joining idle table cycle thread.");
		return (ret);
	}
	return (0);
}
