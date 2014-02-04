/*-
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

/* Default values. */
static const CONFIG default_cfg = {
	"WT_TEST",			/* home */
	"WT_TEST",			/* monitor dir */
	NULL,				/* uri */
	NULL,				/* uris */
	NULL,				/* conn */
	NULL,				/* logf */
	NULL, NULL,			/* populate, checkpoint threads */

	NULL,				/* worker threads */
	0,				/* worker thread count */
	NULL,				/* workloads */
	0,				/* workload count */

#define	OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT
};

static const char * const small_config_str =
    "conn_config=\"cache_size=500MB\","
    "table_config=\"lsm=(chunk_size=5MB)\","
    "icount=500000,"
    "value_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=20,"
    "populate_threads=1,"
    "threads=((count=8,read=1)),";

static const char * const med_config_str =
    "conn_config=\"cache_size=1GB\","
    "table_config=\"lsm=(chunk_size=20MB)\","
    "icount=50000000,"
    "value_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=100,"
    "populate_threads=1,"
    "threads=((count=16,read=1)),";

static const char * const large_config_str =
    "conn_config=\"cache_size=2GB\","
    "table_config=\"lsm=(chunk_size=50MB)\","
    "icount=500000000,"
    "value_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=600,"
    "populate_threads=1,"
    "threads=((count=16,read=1)),";

static const char * const debug_cconfig = "verbose=[lsm]";
static const char * const debug_tconfig = "";

static uint64_t g_ckpt_ops;		/* checkpoint operations */
static uint64_t g_insert_ops;		/* insert operations */
static uint64_t g_read_ops;		/* read operations */
static uint64_t g_update_ops;		/* update operations */

static uint64_t g_insert_key;		/* insert key */

static volatile int g_ckpt;		/* checkpoint in progress */
static volatile int g_error;		/* thread error */
static volatile int g_stop;		/* notify threads to stop */

/*
 * Atomic update where needed.
 */
#if defined(_lint)
#define	ATOMIC_ADD(v, val)	((v) += (val), (v))
#else
#define	ATOMIC_ADD(v, val)	__sync_add_and_fetch(&(v), val)
#endif

static void	*checkpoint_worker(void *);
static int	create_tables(CONFIG *);
static int	create_uris(CONFIG *);
static int	 execute_populate(CONFIG *);
static int	 execute_workload(CONFIG *);
static int	 find_table_count(CONFIG *);
static void	*monitor(void *);
static void	*populate_thread(void *);
static void	randomize_value(CONFIG *, char *);
static int	 start_threads(CONFIG *,
		    WORKLOAD *, CONFIG_THREAD *, u_int, void *(*)(void *));
static int	 stop_threads(CONFIG *, u_int, CONFIG_THREAD *);
static void	*worker(void *);
static uint64_t	 wtperf_rand(CONFIG *);
static uint64_t	 wtperf_value_range(CONFIG *);

#define	HELIUM_NAME	"dev1"
#define	HELIUM_PATH							\
	"../../ext/test/helium/.libs/libwiredtiger_helium.so"
#define	HELIUM_CONFIG	",type=helium"

/*
 * wtperf uses a couple of internal WiredTiger library routines for timing
 * and generating random numbers.
 */
extern int	__wt_epoch(void *, struct timespec *);
extern uint32_t	__wt_random(void);

/* Retrieve an ID for the next insert operation. */
static inline uint64_t
get_next_incr(void)
{
	return (ATOMIC_ADD(g_insert_key, 1));
}

static void
randomize_value(CONFIG *cfg, char *value_buf)
{
	uint32_t i;

	/*
	 * Each time we're called overwrite value_buf[0] and one
	 * other randomly chosen uint32_t.
	 */
	i = __wt_random() % (cfg->value_sz / sizeof(uint32_t));
	value_buf[0] = __wt_random();
	value_buf[i] = __wt_random();
	return;
}

/*
 * track_operation --
 *	Update an operation's tracking structure with new latency information.
 */
static inline void
track_operation(TRACK *trk, uint64_t usecs)
{
	uint64_t v;

					/* average microseconds per call */
	v = (uint64_t)usecs;

	trk->latency += usecs;		/* track total latency */

	if (v > trk->max_latency)	/* track max/min latency */
		trk->max_latency = (uint32_t)v;
	if (v < trk->min_latency)
		trk->min_latency = (uint32_t)v;

	/*
	 * Update a latency bucket.
	 * First buckets: usecs from 100us to 1000us at 100us each.
	 */
	if (v < 1000)
		++trk->us[v];

	/*
	 * Second buckets: millseconds from 1ms to 1000ms, at 1ms each.
	 */
	else if (v < ms_to_us(1000))
		++trk->ms[us_to_ms(v)];

	/*
	 * Third buckets are seconds from 1s to 100s, at 1s each.
	 */
	else if (v < sec_to_us(100))
		++trk->sec[us_to_sec(v)];

	/* >100 seconds, accumulate in the biggest bucket. */
	else
		++trk->sec[ELEMENTS(trk->sec) - 1];
}

static const char *
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

static void *
worker(void *arg)
{
	struct timespec start, stop;
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	TRACK *trk;
	WT_CONNECTION *conn;
	WT_CURSOR **cursors, *cursor;
	WT_SESSION *session;
	char *value_buf, *key_buf, *value;
	int measure_latency, ret;
	size_t i;
	uint64_t next_val, usecs;
	uint8_t *op, *op_end;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	cursors = NULL;
	session = NULL;
	trk = NULL;

	if ((ret = conn->open_session(
	    conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0, "worker: WT_CONNECTION.open_session");
		goto err;
	}
	if (cfg->table_count > 1) {
		cursors = (WT_CURSOR **)calloc(
		    cfg->table_count, sizeof(WT_CURSOR *));
		if (cursors == NULL) {
			lprintf(cfg, ENOMEM, 0,
			    "worker: couldn't allocate cursor array");
			goto err;
		}
		for (i = 0; i < cfg->table_count; i++) {
			if ((ret = session->open_cursor(session,
			    cfg->uris[i], NULL, NULL, &cursors[i])) != 0) {
				lprintf(cfg, ret, 0,
				    "worker: WT_SESSION.open_cursor: %s",
				    cfg->uris[i]);
				goto err;
			}
		}
	} else if ((ret = session->open_cursor(
	    session, cfg->uri, NULL, NULL, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "worker: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	op = thread->workload->ops;
	op_end = op + sizeof(thread->workload->ops);

	while (!g_stop) {
		/* Pick a cursor if there are multiple tables. */
		if (cfg->table_count > 1)
			cursor = cursors[
			    __wt_random() % (cfg->table_count - 1)];

		/*
		 * Generate the next key and setup operation specific
		 * statistics tracking objects.
		 */
		switch (*op) {
		case WORKER_INSERT:
		case WORKER_INSERT_RMW:
			trk = &thread->insert;
			if (cfg->random_range)
				next_val = wtperf_rand(cfg);
			else
				next_val = cfg->icount + get_next_incr();
			break;
		case WORKER_READ:
			trk = &thread->read;
			/* FALLTHROUGH */
		case WORKER_UPDATE:
			if (*op == WORKER_UPDATE)
				trk = &thread->update;
			next_val = wtperf_rand(cfg);

			/*
			 * If the workload is started without a populate phase
			 * we rely on at least one insert to get a valid item
			 * id.
			 */
			if (wtperf_value_range(cfg) < next_val)
				continue;
			break;
		default:
			goto err;		/* can't happen */
		}

		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, next_val);
		measure_latency = cfg->sample_interval != 0 && (
		    trk->ops % cfg->sample_rate == 0);
		if (measure_latency &&
		    (ret = __wt_epoch(NULL, &start)) != 0) {
			lprintf(cfg, ret, 0, "Get time call failed");
			goto err;
		}

		cursor->set_key(cursor, key_buf);

		switch (*op) {
		case WORKER_READ:
			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			ret = cursor->search(cursor);
			if (ret == 0 || ret == WT_NOTFOUND)
				break;
			goto op_err;
		case WORKER_INSERT_RMW:
			if ((ret = cursor->search(cursor)) != WT_NOTFOUND)
				goto op_err;

			/* The error return reset the cursor's key. */
			cursor->set_key(cursor, key_buf);

			/* FALLTHROUGH */
		case WORKER_INSERT:
			if (cfg->random_value)
				randomize_value(cfg, value_buf);
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) == 0)
				break;
			goto op_err;
		case WORKER_UPDATE:
			if ((ret = cursor->search(cursor)) == 0) {
				if ((ret = cursor->get_value(
				    cursor, &value)) != 0) {
					lprintf(cfg, ret, 0,
					    "get_value in update.");
					goto err;
				}
				memcpy(value_buf, value, strlen(value));
				if (value_buf[0] == 'a')
					value_buf[0] = 'b';
				else
					value_buf[0] = 'a';
				if (cfg->random_value)
					randomize_value(cfg, value_buf);
				cursor->set_value(cursor, value_buf);
				if ((ret = cursor->update(cursor)) == 0)
					break;
				goto op_err;
			}

			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			if (ret == WT_NOTFOUND)
				break;

op_err:			lprintf(cfg, ret, 0,
			    "%s failed for: %s, range: %"PRIu64,
			    op_name(op), key_buf, wtperf_value_range(cfg));
			goto err;
		default:
			goto err;		/* can't happen */
		}

		/* Gather statistics */
		if (measure_latency) {
			if ((ret = __wt_epoch(NULL, &stop)) != 0) {
				lprintf(cfg, ret, 0, "Get time call failed");
				goto err;
			}
			++trk->latency_ops;
			usecs = ns_to_us(WT_TIMEDIFF(stop, start));
			track_operation(trk, usecs);
		}
		++trk->ops;		/* increment operation counts */

		if (++op == op_end)	/* schedule the next operation */
			op = thread->workload->ops;
	}

	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Session close in worker failed");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}
	if (cursors != NULL)
		free(cursors);

	return (NULL);
}

/*
 * run_mix_schedule_op --
 *	Replace read operations with another operation, in the configured
 * percentage.
 */
static void
run_mix_schedule_op(WORKLOAD *workp, int op, int64_t op_cnt)
{
	int jump, pass;
	uint8_t *p, *end;

	/* Jump around the array to roughly spread out the operations. */
	jump = 100 / op_cnt;

	/*
	 * Find a read operation and replace it with another operation.  This
	 * is roughly n-squared, but it's an N of 100, leave it.
	 */
	p = workp->ops;
	end = workp->ops + sizeof(workp->ops);
	while (op_cnt-- > 0) {
		for (pass = 0; *p != WORKER_READ; ++p)
			if (p == end) {
				/*
				 * Passed a percentage of total operations and
				 * should always be a read operation to replace,
				 * but don't allow infinite loops.
				 */
				if (++pass > 1)
					return;
				p = workp->ops;
			}
		*p = (uint8_t)op;

		if (end - jump < p)
			p = workp->ops;
		else
			p += jump;
	}
}

/*
 * run_mix_schedule --
 *	Schedule the mixed-run operations.
 */
static int
run_mix_schedule(CONFIG *cfg, WORKLOAD *workp)
{
	int64_t pct;

	/* Confirm reads, inserts and updates cannot all be zero. */
	if (workp->insert == 0 && workp->read == 0 && workp->update == 0) {
		lprintf(cfg, EINVAL, 0, "no operations scheduled");
		return (EINVAL);
	}

	/*
	 * Check for a simple case where the thread is only doing insert or
	 * update operations (because the default operation for a job-mix is
	 * read, the subsequent code works fine if only reads are specified).
	 */
	if (workp->insert != 0 && workp->read == 0 && workp->update == 0) {
		memset(workp->ops,
		    cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT,
		    sizeof(workp->ops));
		return (0);
	}
	if (workp->insert == 0 && workp->read == 0 && workp->update != 0) {
		memset(workp->ops, WORKER_UPDATE, sizeof(workp->ops));
		return (0);
	}

	/*
	 * The worker thread configuration is done as ratios of operations.  If
	 * the caller gives us something insane like "reads=77,updates=23" (do
	 * 77 reads for every 23 updates), we don't want to do 77 reads followed
	 * by 23 updates, we want to uniformly distribute the read and update
	 * operations across the space.  Convert to percentages and then lay out
	 * the operations across an array.
	 *
	 * Percentage conversion is lossy, the application can do stupid stuff
	 * here, for example, imagine a configured ratio of "reads=1,inserts=2,
	 * updates=999999".  First, if the percentages are skewed enough, some
	 * operations might never be done.  Second, we set the base operation to
	 * read, which means any fractional results from percentage conversion
	 * will be reads, implying read operations in some cases where reads
	 * weren't configured.  We should be fine if the application configures
	 * something approaching a rational set of ratios.
	 */
	memset(workp->ops, WORKER_READ, sizeof(workp->ops));

	pct = (workp->insert * 100) /
	    (workp->insert + workp->read + workp->update);
	if (pct != 0)
		run_mix_schedule_op(workp,
		    cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT, pct);
	pct = (workp->update * 100) /
	    (workp->insert + workp->read + workp->update);
	if (pct != 0)
		run_mix_schedule_op(workp, WORKER_UPDATE, pct);
	return (0);
}

static void *
populate_thread(void *arg)
{
	struct timespec start, stop;
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	TRACK *trk;
	WT_CONNECTION *conn;
	WT_CURSOR **cursors, *cursor;
	WT_SESSION *session;
	char *value_buf, *key_buf;
	const char *cursor_config;
	int intxn, measure_latency, ret;
	size_t i;
	uint32_t opcount;
	uint64_t op, usecs;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;
	cursors = NULL;
	ret = 0;
	trk = &thread->insert;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	if ((ret = conn->open_session(
	    conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0, "populate: WT_CONNECTION.open_session");
		goto err;
	}

	/* Do bulk loads if populate is single-threaded. */
	cursor_config = cfg->populate_threads == 1 ? "bulk" : NULL;
	/* Create the cursor or cursors if there are multiple tables. */
	if (cfg->table_count > 1) {
		cursors = (WT_CURSOR **)calloc(
		    cfg->table_count, sizeof(WT_CURSOR *));
		if (cursors == NULL) {
			lprintf(cfg, ENOMEM, 0,
			    "worker: couldn't allocate cursor array");
			goto err;
		}
		for (i = 0; i < cfg->table_count; i++) {
			if ((ret = session->open_cursor(
			    session, cfg->uris[i], NULL,
			    cursor_config, &cursors[i])) != 0) {
				lprintf(cfg, ret, 0,
				    "populate: WT_SESSION.open_cursor: %s",
				    cfg->uris[i]);
				goto err;
			}
		}
	} else if ((ret = session->open_cursor(
	    session, cfg->uri, NULL, cursor_config, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "populate: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	/* Populate the databases. */
	for (intxn = 0, opcount = 0;;) {
		op = get_next_incr();
		if (op > cfg->icount)
			break;

		if (cfg->populate_ops_per_txn != 0 && !intxn) {
			if ((ret = session->begin_transaction(
			    session, cfg->transaction_config)) != 0) {
				lprintf(cfg, ret, 0,
				    "Failed starting transaction.");
				goto err;
			}
			intxn = 1;
		}
		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
		measure_latency = cfg->sample_interval != 0 && (
		    trk->ops % cfg->sample_rate == 0);
		if (measure_latency &&
		    (ret = __wt_epoch(NULL, &start)) != 0) {
			lprintf(cfg, ret, 0, "Get time call failed");
			goto err;
		}
		for (i = 0; i < cfg->table_count; i++) {
			if (cfg->table_count > 1)
				cursor = cursors[i];
			cursor->set_key(cursor, key_buf);
			if (cfg->random_value)
				randomize_value(cfg, value_buf);
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
			/* Gather statistics */
			if (measure_latency) {
				if ((ret = __wt_epoch(NULL, &stop)) != 0) {
					lprintf(cfg, ret, 0,
					    "Get time call failed");
					goto err;
				}
				++trk->latency_ops;
				usecs = ns_to_us(WT_TIMEDIFF(stop, start));
				track_operation(trk, usecs);
			}
			++thread->insert.ops;	/* Same as trk->ops */
		}

		if (cfg->populate_ops_per_txn != 0) {
			if (++opcount < cfg->populate_ops_per_txn)
				continue;
			opcount = 0;

			if ((ret = session->commit_transaction(
			    session, NULL)) != 0)
				lprintf(cfg, ret, 0,
				    "Fail committing, transaction was aborted");
			intxn = 0;
		}
	}
	if (intxn &&
	    (ret = session->commit_transaction(session, NULL)) != 0)
		lprintf(cfg, ret, 0,
		    "Fail committing, transaction was aborted");

	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Error closing session in populate");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}
	if (cursors != NULL)
		free(cursors);

	return (NULL);
}

static void *
monitor(void *arg)
{
	struct timespec t;
	struct tm *tm, _tm;
	CONFIG *cfg;
	FILE *fp;
	char buf[64], *path;
	int ret;
	uint64_t reads, inserts, updates;
	uint64_t cur_reads, cur_inserts, cur_updates;
	uint64_t last_reads, last_inserts, last_updates;
	uint32_t read_avg, read_min, read_max;
	uint32_t insert_avg, insert_min, insert_max;
	uint32_t update_avg, update_min, update_max;
	size_t len;
	u_int i;

	cfg = (CONFIG *)arg;
	assert(cfg->sample_interval != 0);
	fp = NULL;
	path = NULL;

	/* Open the logging file. */
	len = strlen(cfg->monitor_dir) + 100;
	if ((path = malloc(len)) == NULL) {
		(void)enomem(cfg);
		goto err;
	}
	snprintf(path, len, "%s/monitor", cfg->monitor_dir);
	if ((fp = fopen(path, "w")) == NULL) {
		lprintf(cfg, errno, 0, "%s", path);
		goto err;
	}
	/* Set line buffering for monitor file. */
	(void)setvbuf(fp, NULL, _IOLBF, 0);
#ifdef __WRITE_A_HEADER
	fprintf(fp,
	    "#time,"
	    "read operations,insert operations,update operations,"
	    "checkpoints,"
	    "read average latency(NS),read minimum latency(NS),"
	    "read maximum latency(NS),"
	    "insert average latency(NS),insert min latency(NS),"
	    "insert maximum latency(NS),"
	    "update average latency(NS),update min latency(NS),"
	    "update maximum latency(NS)"
	    "\n");
#endif
	last_reads = last_inserts = last_updates = 0;
	while (!g_stop) {
		for (i = 0; i < cfg->sample_interval; i++) {
			sleep(1);
			if (g_stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (g_stop)
			break;

		if ((ret = __wt_epoch(NULL, &t)) != 0) {
			lprintf(cfg, ret, 0, "Get time call failed");
			goto err;
		}
		tm = localtime_r(&t.tv_sec, &_tm);
		(void)strftime(buf, sizeof(buf), "%b %d %H:%M:%S", tm);

		reads = sum_read_ops(cfg);
		inserts = sum_insert_ops(cfg);
		updates = sum_update_ops(cfg);
		latency_read(cfg, &read_avg, &read_min, &read_max);
		latency_insert(cfg, &insert_avg, &insert_min, &insert_max);
		latency_update(cfg, &update_avg, &update_min, &update_max);

		cur_reads = reads - last_reads;
		cur_updates = updates - last_updates;
		/*
		 * For now the only item we need to worry about changing is
		 * inserts when we transition from the populate phase to
		 * workload phase.
		 */
		if (inserts < last_inserts)
			cur_inserts = 0;
		else
			cur_inserts = inserts - last_inserts;

		(void)fprintf(fp,
		    "%s"
		    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		    ",%c"
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    "\n",
		    buf,
		    cur_reads / cfg->sample_interval,
		    cur_inserts / cfg->sample_interval,
		    cur_updates / cfg->sample_interval,
		    g_ckpt ? 'Y' : 'N',
		    read_avg, read_min, read_max,
		    insert_avg, insert_min, insert_max,
		    update_avg, update_min, update_max);

		last_reads = reads;
		last_inserts = inserts;
		last_updates = updates;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	if (fp != NULL)
		(void)fclose(fp);
	free(path);

	return (NULL);
}

static void *
checkpoint_worker(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timespec e, s;
	uint32_t i;
	int ret;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;

	if ((ret = conn->open_session(
	    conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in checkpoint thread.");
		goto err;
	}

	while (!g_stop) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < cfg->checkpoint_interval; i++) {
			sleep(1);
			if (g_stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (g_stop)
			break;

		if ((ret = __wt_epoch(NULL, &s)) != 0) {
			lprintf(cfg, ret, 0, "Get time failed in checkpoint.");
			goto err;
		}
		g_ckpt = 1;
		if ((ret = session->checkpoint(session, NULL)) != 0) {
			lprintf(cfg, ret, 0, "Checkpoint failed.");
			goto err;
		}
		g_ckpt = 0;
		++thread->ckpt.ops;

		if ((ret = __wt_epoch(NULL, &e)) != 0) {
			lprintf(cfg, ret, 0, "Get time failed in checkpoint.");
			goto err;
		}
	}

	if (session != NULL &&
	    ((ret = session->close(session, NULL)) != 0)) {
		lprintf(cfg, ret, 0,
		    "Error closing session in checkpoint worker.");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		g_error = g_stop = 1;
	}

	return (NULL);
}

static int
execute_populate(CONFIG *cfg)
{
	CONFIG_THREAD *popth;
	WT_SESSION *session;
	struct timespec start, stop;
	double secs;
	uint64_t last_ops;
	uint32_t interval, total;
	int elapsed, ret;

	session = NULL;
	lprintf(cfg, 0, 1,
	    "Starting %" PRIu32 " populate thread(s)", cfg->populate_threads);

	if ((cfg->popthreads =
	    calloc(cfg->populate_threads, sizeof(CONFIG_THREAD))) == NULL)
		return (enomem(cfg));
	if ((ret = start_threads(cfg, NULL,
	    cfg->popthreads, cfg->populate_threads, populate_thread)) != 0)
		return (ret);

	g_insert_key = 0;

	if ((ret = __wt_epoch(NULL, &start)) != 0) {
		lprintf(cfg, ret, 0, "Get time failed in populate.");
		return (ret);
	}
	for (elapsed = 0, interval = 0, last_ops = 0, total = 0;
	    g_insert_key < cfg->icount && g_error == 0;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, each 100th increment of elapsed is a single
		 * increment of interval.
		 */
		(void)usleep(10000);
		if (cfg->report_interval == 0 || ++elapsed < 100)
			continue;
		elapsed = 0;
		if (++interval < cfg->report_interval)
			continue;
		interval = 0;
		total += cfg->report_interval;
		g_insert_ops = sum_pop_ops(cfg);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " populate inserts (%" PRIu64 " of %"
		    PRIu32 ") in %" PRIu32 " secs (%" PRIu32 " total secs)",
		    g_insert_ops - last_ops, g_insert_ops,
		    cfg->icount, cfg->report_interval, total);
		last_ops = g_insert_ops;
	}
	if ((ret = __wt_epoch(NULL, &stop)) != 0) {
		lprintf(cfg, ret, 0, "Get time failed in populate.");
		return (ret);
	}

	/*
	 * Move popthreads aside to narrow possible race with the monitor
	 * thread. The latency tracking code also requires that popthreads be
	 * NULL when the populate phase is finished, to know that the workload
	 * phase has started.
	 */
	popth = cfg->popthreads;
	cfg->popthreads = NULL;
	ret = stop_threads(cfg, cfg->populate_threads, popth);
	free(popth);
	if (ret != 0)
		return (ret);

	/* Report if any worker threads didn't finish. */
	if (g_error != 0) {
		lprintf(cfg, WT_ERROR, 0,
		    "Populate thread(s) exited without finishing.");
		return (WT_ERROR);
	}

	lprintf(cfg, 0, 1, "Finished load of %" PRIu32 " items", cfg->icount);
	secs = stop.tv_sec + stop.tv_nsec / (double)BILLION;
	secs -= start.tv_sec + start.tv_nsec / (double)BILLION;
	if (secs == 0)
		++secs;
	lprintf(cfg, 0, 1,
	    "Load time: %.2f\n" "load ops/sec: %.2f", secs, cfg->icount / secs);

	/*
	 * If configured, compact to allow LSM merging to complete.
	 */
	if (cfg->compact) {
		if ((ret = cfg->conn->open_session(
		    cfg->conn, NULL, cfg->sess_config, &session)) != 0) {
			lprintf(cfg, ret, 0,
			     "execute_populate: WT_CONNECTION.open_session");
			return (ret);
		}
		lprintf(cfg, 0, 1, "Compact after populate");
		if ((ret = session->compact(session, cfg->uri, NULL)) != 0) {
			lprintf(cfg, ret, 0,
			     "execute_populate: WT_SESSION.compact");
			return (ret);
		}
		if ((ret = session->close(session, NULL)) != 0) {
			lprintf(cfg, ret, 0,
			     "execute_populate: WT_SESSION.close");
			return (ret);
		}
	}

	/*
	 * Reopen the connection.  We do this so that the workload phase always
	 * starts with the on-disk files, and so that read-only workloads can
	 * be identified.  This is particularly important for LSM, where the
	 * merge algorithm is more aggressive for read-only trees.
	 */
	if ((ret = cfg->conn->close(cfg->conn, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Closing the connection failed");
		return (ret);
	}
	if ((ret = wiredtiger_open(
	    cfg->home, NULL, cfg->conn_config, &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "Re-opening the connection failed");
		return (ret);
	}

	return (0);
}

static int
execute_workload(CONFIG *cfg)
{
	CONFIG_THREAD *threads;
	WORKLOAD *workp;
	uint64_t last_ckpts, last_inserts, last_reads, last_updates;
	uint32_t interval, run_ops, run_time, total;
	u_int i;
	int ret, t_ret;

	g_insert_key = 0;
	g_insert_ops = g_read_ops = g_update_ops = 0;

	last_ckpts = last_inserts = last_reads = last_updates = 0;
	ret = 0;
	
	/* Allocate memory for the worker threads. */
	if ((cfg->workers =
	    calloc((size_t)cfg->workers_cnt, sizeof(CONFIG_THREAD))) == NULL) {
		ret = enomem(cfg);
		goto err;
	}

	/* Start each workload. */
	for (threads = cfg->workers, i = 0,
	    workp = cfg->workload; i < cfg->workload_cnt; ++i, ++workp) {
		lprintf(cfg, 0, 1,
		    "Starting workload #%d: %" PRId64 " threads, inserts=%"
		    PRId64 ", reads=%" PRId64 ", updates=%" PRId64,
		    i + 1,
		    workp->threads, workp->insert, workp->read, workp->update);

		/* Figure out the workload's schedule. */
		if ((ret = run_mix_schedule(cfg, workp)) != 0)
			goto err;

		/* Start the workload's threads. */
		if ((ret = start_threads(
		    cfg, workp, threads, (u_int)workp->threads, worker)) != 0)
			goto err;
		threads += workp->threads;
	}

	for (interval = cfg->report_interval, total = 0,
	    run_time = cfg->run_time, run_ops = cfg->run_ops; g_error == 0;) {
		/*
		 * Sleep for one second at a time.
		 * If we are tracking run time, check to see if we're done, and
		 * if we're only tracking run time, go back to sleep.
		 */
		sleep(1);
		if (run_time != 0) {
			if (--run_time == 0)
				break;
			if (!interval && !run_ops)
				continue;
		}

		/* Sum the operations we've done. */
		g_ckpt_ops = sum_ckpt_ops(cfg);
		g_insert_ops = sum_insert_ops(cfg);
		g_read_ops = sum_read_ops(cfg);
		g_update_ops = sum_update_ops(cfg);

		/* If we're checking total operations, see if we're done. */
		if (run_ops != 0 &&
		    run_ops <= g_insert_ops + g_read_ops + g_update_ops)
			break;

		/* If writing out throughput information, see if it's time. */
		if (interval == 0 || --interval > 0)
			continue;
		interval = cfg->report_interval;
		total += cfg->report_interval;

		lprintf(cfg, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates, %" PRIu64 " checkpoints in %" PRIu32
		    " secs (%" PRIu32 " total secs)",
		    g_read_ops - last_reads,
		    g_insert_ops - last_inserts,
		    g_update_ops - last_updates,
		    g_ckpt_ops - last_ckpts,
		    cfg->report_interval, total);
		last_reads = g_read_ops;
		last_inserts = g_insert_ops;
		last_updates = g_update_ops;
		last_ckpts = g_ckpt_ops;
	}

	/* Notify the worker threads they are done. */
err:	g_stop = 1;

	if ((t_ret = stop_threads(
	    cfg, (u_int)cfg->workers_cnt, cfg->workers)) != 0 && ret == 0)
		ret = t_ret;

	/* Report if any worker threads didn't finish. */
	if (g_error != 0) {
		lprintf(cfg, WT_ERROR, 0,
		    "Worker thread(s) exited without finishing.");
		if (ret == 0)
			ret = WT_ERROR;
	}
	return (ret);
}

/*
 * Ensure that icount matches the number of records in the 
 * existing table.
 */
static int
find_table_count(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *key;
	int ret, t_ret;

	conn = cfg->conn;

	if ((ret = conn->open_session(
	    conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed finding existing table count");
		goto err;
	}
	if ((ret = session->open_cursor(session, cfg->uri,
	    NULL, NULL, &cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_cursor failed finding existing table count");
		goto err;
	}
	if ((ret = cursor->prev(cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "cursor prev failed finding existing table count");
		goto err;
	}
	if ((ret = cursor->get_key(cursor, &key)) != 0) {
		lprintf(cfg, ret, 0,
		    "cursor get_key failed finding existing table count");
		goto err;
	}
	cfg->icount = (uint32_t)atoi(key);

err:	if ((t_ret = session->close(session, NULL)) != 0) {
		if (ret == 0)
			ret = t_ret;
		lprintf(cfg, ret, 0,
		    "session close failed finding existing table count");
	}
	return (ret);
}

/*
 * Populate the uri array if more than one table is being used.
 */
int
create_uris(CONFIG *cfg)
{
	char *uri;
	int ret;
	size_t base_uri_len;
	uint32_t i;

	ret = 0;
	if (cfg->table_count < 2) {
		cfg->uris = NULL;
		return (0);
	}

	base_uri_len = strlen(cfg->uri);
	cfg->uris = (char **)malloc(cfg->table_count * sizeof(char *));
	if (cfg->uris == NULL) {
		ret = ENOMEM;
		goto err;
	}
	for (i = 0; i < cfg->table_count; i++) {
		uri = cfg->uris[i] = (char *)calloc(base_uri_len + 3, 1);
		if (uri == NULL) {
			ret = ENOMEM;
			goto err;
		}
		memcpy(uri, cfg->uri, base_uri_len);
		uri[base_uri_len] = uri[base_uri_len + 1] = '0';
		uri[base_uri_len] = '0' + (i / 10);
		uri[base_uri_len + 1] = '0' + (i % 10);
	}
err:	if (ret != 0) {
		for (i = 0; i < cfg->table_count; i++)
			free(cfg->uris[i]);
		free(cfg->uris);
		cfg->uris = NULL;
	}
	return (ret);
}

int
create_tables(CONFIG *cfg)
{
	WT_SESSION *session;
	char *uri;
	int ret;
	size_t i;

	session = NULL;
	if (cfg->create == 0)
		return (0);

	uri = cfg->uri;
	if ((ret = cfg->conn->open_session(
	    cfg->conn, NULL, cfg->sess_config, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error opening a session on %s", cfg->home);
		return (ret);
	}
	for (i = 0; i < cfg->table_count; i++) {
		if (cfg->table_count > 1)
			uri = cfg->uris[i];
		if ((ret = session->create(
		    session, uri, cfg->table_config)) != 0) {
			lprintf(cfg, ret, 0,
			    "Error creating table %s", cfg->uri);
			return (ret);
		}
	}
	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(cfg,
		    ret, 0, "Error closing session");
		return (ret);
	}
	return (ret);
}

int
main(int argc, char *argv[])
{
	CONFIG *cfg, _cfg;
	pthread_t monitor_thread;
	size_t len;
	uint64_t req_len, total_ops;
	int ch, monitor_created, monitor_set, ret, t_ret;
	const char *helium_mount;
	const char *opts = "C:H:h:LMm:O:o:ST:";
	const char *wtperftmp_subdir = "wtperftmp";
	const char *user_cconfig, *user_tconfig;
	char *cmd, *cc_buf, *tc_buf, *tmphome;

	monitor_created = monitor_set = ret = 0;
	helium_mount = user_cconfig = user_tconfig = NULL;
	cmd = cc_buf = tc_buf = tmphome = NULL;

	/* Setup the default configuration values. */
	cfg = &_cfg;
	memset(cfg, 0, sizeof(*cfg));
	if (config_assign(cfg, &default_cfg))
		goto err;

	/* Do a basic validation of options, and home is needed before open. */
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'h':
			cfg->home = optarg;
			break;
		case 'm':
			cfg->monitor_dir = optarg;
			monitor_set = 1;
			break;
		case '?':
			fprintf(stderr, "Invalid option\n");
			usage();
			goto einval;
		}

	/*
	 * If the user did not specify a monitor directory
	 * then set the monitor directory to the home dir.
	 */
	if (!monitor_set)
		cfg->monitor_dir = cfg->home;

	/*
	 * Create a temporary directory underneath the test directory in which
	 * we do an initial WiredTiger open, because we need a connection in
	 * order to use the extension configuration parser.  We will open the
	 * real WiredTiger database after parsing the options.
	 */
	len = strlen(cfg->home) + strlen(wtperftmp_subdir) + 2;
	if ((tmphome = malloc(len)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(tmphome, len, "%s/%s", cfg->home, wtperftmp_subdir);
	len = len * 2 + 100;
	if ((cmd = malloc(len)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(cmd, len, "rm -rf %s && mkdir %s", tmphome, tmphome);
	if (system(cmd) != 0) {
		fprintf(stderr, "%s: failed\n", cmd);
		goto einval;
	}
	if ((ret = wiredtiger_open(
	    tmphome, NULL, "create", &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "wiredtiger_open: %s", tmphome);
		goto err;
	}

	/*
	 * Then parse different config structures - other options override
	 * fields within the structure.
	 */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'S':
			if (config_opt_line(cfg, small_config_str) != 0)
				goto einval;
			break;
		case 'M':
			if (config_opt_line(cfg, med_config_str) != 0)
				goto einval;
			break;
		case 'L':
			if (config_opt_line(cfg, large_config_str) != 0)
				goto einval;
			break;
		case 'O':
			if (config_opt_file(cfg, optarg) != 0)
				goto einval;
			break;
		default:
			/* Validation done previously. */
			break;
		}

	/* Parse other options */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'C':
			user_cconfig = optarg;
			break;
		case 'H':
			helium_mount = optarg;
			break;
		case 'o':
			/* Allow -o key=value */
			if (config_opt_line(cfg, optarg) != 0)
				goto einval;
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		}

	/* Build the URI from the table name. */
	req_len = strlen("table:") +
	    strlen(HELIUM_NAME) + strlen(cfg->table_name) + 2;
	if ((cfg->uri = calloc(req_len, 1)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	snprintf(cfg->uri, req_len, "table:%s%s%s",
	    helium_mount == NULL ? "" : HELIUM_NAME,
	    helium_mount == NULL ? "" : "/",
	    cfg->table_name);
	
	if ((ret = setup_log_file(cfg)) != 0)
		goto err;

	/* Make stdout line buffered, so verbose output appears quickly. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

	/* Concatenate non-default configuration strings. */
	if (cfg->verbose > 1 || user_cconfig != NULL) {
		req_len = strlen(cfg->conn_config) + strlen(debug_cconfig) + 3;
		if (user_cconfig != NULL)
			req_len += strlen(user_cconfig);
		if ((cc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(cfg);
			goto err;
		}
		snprintf(cc_buf, req_len, "%s%s%s%s%s",
		    cfg->conn_config,
		    cfg->verbose > 1 ? "," : "",
		    cfg->verbose > 1 ? debug_cconfig : "",
		    user_cconfig ? "," : "", user_cconfig ? user_cconfig : "");
		if ((ret = config_opt_str(cfg, "conn_config", cc_buf)) != 0)
			goto err;
	}
	if (cfg->verbose > 1 || helium_mount != NULL || user_tconfig != NULL) {
		req_len = strlen(cfg->table_config) +
		    strlen(HELIUM_CONFIG) + strlen(debug_tconfig) + 3;
		if (user_tconfig != NULL)
			req_len += strlen(user_tconfig);
		if ((tc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(cfg);
			goto err;
		}
		snprintf(tc_buf, req_len, "%s%s%s%s%s%s",
		    cfg->table_config,
		    cfg->verbose > 1 ? "," : "",
		    cfg->verbose > 1 ? debug_tconfig : "",
		    user_tconfig ? "," : "", user_tconfig ? user_tconfig : "",
		    helium_mount == NULL ? "" : HELIUM_CONFIG);
		if ((ret = config_opt_str(cfg, "table_config", tc_buf)) != 0)
			goto err;
	}

	ret = cfg->conn->close(cfg->conn, NULL);
	cfg->conn = NULL;
	if (ret != 0) {
		lprintf(cfg, ret, 0, "WT_CONNECTION.close: %s", tmphome);
		goto err;
	}
					/* Sanity-check the configuration */
	if (config_sanity(cfg) != 0)
		goto err;

	if (cfg->verbose > 1)		/* Display the configuration. */
		config_print(cfg);

	if ((ret = wiredtiger_open(	/* Open the real connection. */
	    cfg->home, NULL, cfg->conn_config, &cfg->conn)) != 0) {
		lprintf(cfg, ret, 0, "Error connecting to %s", cfg->home);
		goto err;
	}

	if (helium_mount != NULL) {	/* Configure optional Helium volume. */
		char helium_buf[256];
		snprintf(helium_buf, sizeof(helium_buf),
		    "entry=wiredtiger_extension_init,config=["
		    "%s=[helium_devices=\"he://./%s\","
		    "helium_o_volume_truncate=1]]",
		    HELIUM_NAME, helium_mount);
		if ((ret = cfg->conn->load_extension(
		    cfg->conn, HELIUM_PATH, helium_buf)) != 0)
			lprintf(cfg,
			    ret, 0, "Error loading Helium: %s", helium_buf);
	}

	if ((ret = create_uris(cfg)) != 0)
		goto err;
	if ((ret = create_tables(cfg)) != 0)
		goto err;
					/* Start the monitor thread. */
	if (cfg->sample_interval != 0) {
		if ((ret = pthread_create(
		    &monitor_thread, NULL, monitor, cfg)) != 0) {
			lprintf(
			    cfg, ret, 0, "Error creating monitor thread.");
			goto err;
		}
		monitor_created = 1;
	}
					/* If creating, populate the table. */
	if (cfg->create != 0 && execute_populate(cfg) != 0)
		goto err;
					/* Optional workload. */
	if (cfg->run_time != 0 || cfg->run_ops != 0) {
					/* Didn't create, set insert count. */
		if (cfg->create == 0 && find_table_count(cfg) != 0)
			goto err;
					/* Start the checkpoint thread. */
		if (cfg->checkpoint_threads != 0) {
			lprintf(cfg, 0, 1,
			    "Starting %" PRIu32 " checkpoint thread(s)",
			    cfg->checkpoint_threads);
			if ((cfg->ckptthreads =
			    calloc(cfg->checkpoint_threads,
			    sizeof(CONFIG_THREAD))) == NULL) {
				ret = enomem(cfg);
				goto err;
			}
			if (start_threads(cfg, NULL, cfg->ckptthreads,
			    cfg->checkpoint_threads, checkpoint_worker) != 0)
				goto err;
		}
					/* Execute the workload. */
		if ((ret = execute_workload(cfg)) != 0)
			goto err;

		/* One final summation of the operations we've completed. */
		g_read_ops = sum_read_ops(cfg);
		g_insert_ops = sum_insert_ops(cfg);
		g_update_ops = sum_update_ops(cfg);
		g_ckpt_ops = sum_ckpt_ops(cfg);
		total_ops = g_read_ops + g_insert_ops + g_update_ops;

		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " read operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    g_read_ops, (g_read_ops * 100) / total_ops,
		    g_read_ops / cfg->run_time);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " insert operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    g_insert_ops, (g_insert_ops * 100) / total_ops,
		    g_insert_ops / cfg->run_time);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " update operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    g_update_ops, (g_update_ops * 100) / total_ops,
		    g_update_ops / cfg->run_time);
		lprintf(cfg, 0, 1,
		    "Executed %" PRIu64 " checkpoint operations",
		    g_ckpt_ops);

		latency_print(cfg);
	}

	if (0) {
einval:		ret = EINVAL;
err:		if (ret == 0)
			ret = EXIT_FAILURE;
	}

	/* Notify the worker threads they are done. */
	g_stop = 1;

	if ((t_ret = stop_threads(cfg, 1, cfg->ckptthreads)) != 0)
		if (ret == 0)
			ret = t_ret;

	if (monitor_created != 0 &&
	    (t_ret = pthread_join(monitor_thread, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Error joining monitor thread.");
		if (ret == 0)
			ret = t_ret;
	}

	if (cfg->conn != NULL &&
	    (t_ret = cfg->conn->close(cfg->conn, NULL)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error closing connection to %s", cfg->home);
		if (ret == 0)
			ret = t_ret;
	}

	if (ret == 0) {
		if (cfg->run_time == 0 && cfg->run_ops == 0)
			lprintf(cfg, 0, 1, "Run completed");
		else
			lprintf(cfg, 0, 1, "Run completed: %" PRIu32 " %s",
			    cfg->run_time == 0 ? cfg->run_ops : cfg->run_time,
			    cfg->run_time == 0 ? "operations" : "seconds");
	}

	if (cfg->logf != NULL) {
		if ((t_ret = fflush(cfg->logf)) != 0 && ret == 0)
			ret = t_ret;
		if ((t_ret = fclose(cfg->logf)) != 0 && ret == 0)
			ret = t_ret;
	}
	config_free(cfg);

	free(cc_buf);
	free(cmd);
	free(tc_buf);
	free(tmphome);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
start_threads(CONFIG *cfg,
    WORKLOAD *workp, CONFIG_THREAD *thread, u_int num, void *(*func)(void *))
{
	u_int i;
	int ret;

	for (i = 0; i < num; ++i, ++thread) {
		thread->cfg = cfg;
		thread->workload = workp;

		/*
		 * Every thread gets a key/data buffer because we don't bother
		 * to distinguish between threads needing them and threads that
		 * don't, it's not enough memory to bother.
		 */
		if ((thread->key_buf = calloc(cfg->key_sz + 1, 1)) == NULL)
			return (enomem(cfg));
		if ((thread->value_buf = calloc(cfg->value_sz, 1)) == NULL)
			return (enomem(cfg));
		/*
		 * Initialize and then toss in a bit of random values if needed.
		 */
		memset(thread->value_buf, 'a', cfg->value_sz - 1);
		if (cfg->random_value)
			randomize_value(cfg, thread->value_buf);

		/*
		 * Every thread gets tracking information and is initialized
		 * for latency measurements, for the same reason.
		 */
		thread->ckpt.min_latency =
		thread->insert.min_latency = thread->read.min_latency =
		thread->update.min_latency = UINT32_MAX;
		thread->ckpt.max_latency = thread->insert.max_latency =
		thread->read.max_latency = thread->update.max_latency = 0;

		if ((ret = pthread_create(
		    &thread->handle, NULL, func, thread)) != 0) {
			lprintf(cfg, ret, 0, "Error creating thread");
			return (ret);
		}
	}
	return (0);
}

static int
stop_threads(CONFIG *cfg, u_int num, CONFIG_THREAD *threads)
{
	u_int i;
	int ret;

	if (num == 0 || threads == NULL)
		return (0);

	for (i = 0; i < num; ++i, ++threads) {
		if ((ret = pthread_join(threads->handle, NULL)) != 0) {
			lprintf(cfg, ret, 0, "Error joining thread");
			return (ret);
		}

		free(threads->key_buf);
		threads->key_buf = NULL;
		free(threads->value_buf);
		threads->value_buf = NULL;
	}

	/*
	 * We don't free the thread structures or any memory referenced, or NULL
	 * the reference when we stop the threads; the thread structure is still
	 * being read by the monitor thread (among others).  As a standalone
	 * program, leaking memory isn't a concern, and it's simpler that way.
	 */
	return (0);
}

static uint64_t
wtperf_value_range(CONFIG *cfg)
{
	if (cfg->random_range)
		return (cfg->icount + cfg->random_range);

	return (cfg->icount + g_insert_key - (u_int)(cfg->workers_cnt + 1));
}

static uint64_t
wtperf_rand(CONFIG *cfg)
{
	double S1, S2, U;
	uint64_t rval;

	/*
	 * Use WiredTiger's random number routine: it's lock-free and fairly
	 * good.
	 */
	rval = (uint64_t)__wt_random();

	/* Use Pareto distribution to give 80/20 hot/cold values. */
	if (cfg->pareto) {
#define	PARETO_SHAPE	1.5
		S1 = (-1 / PARETO_SHAPE);
		S2 = wtperf_value_range(cfg) * 0.2 * (PARETO_SHAPE - 1);
		U = 1 - (double)rval / (double)RAND_MAX;
		rval = (pow(U, S1) - 1) * S2;
		/*
		 * This Pareto calculation chooses out of range values about
		 * about 2% of the time, from my testing. That will lead to the
		 * last item in the table being "hot".
		 */
		if (rval > wtperf_value_range(cfg))
			rval = wtperf_value_range(cfg);
	}
	/* Avoid zero - LSM doesn't like it. */
	rval = (rval % wtperf_value_range(cfg)) + 1;
	return (rval);
}
