/*-
 * Public Domain 2014-2017 MongoDB, Inc.
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

#include "format.h"

static int   col_insert(TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t *);
static int   col_modify(
		TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, bool);
static int   col_remove(WT_CURSOR *, WT_ITEM *, uint64_t, bool);
static int   col_reserve(WT_CURSOR *, uint64_t, bool);
static int   col_update(
		TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, bool);
static int   nextprev(WT_CURSOR *, int);
static WT_THREAD_RET ops(void *);
static int   row_insert(
		TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, bool);
static int   row_modify(
		TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, bool);
static int   row_remove(WT_CURSOR *, WT_ITEM *, uint64_t, bool);
static int   row_reserve(WT_CURSOR *, WT_ITEM *, uint64_t, bool);
static int   row_update(
		TINFO *, WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t, bool);
static void  table_append_init(void);

#ifdef HAVE_BERKELEY_DB
static int   notfound_chk(const char *, int, int, uint64_t);
#endif

static char modify_repl[256];

/*
 * modify_repl_init --
 *	Initialize the replacement information.
 */
static void
modify_repl_init(void)
{
	size_t i;

	for (i = 0; i < sizeof(modify_repl); ++i)
		modify_repl[i] = "zyxwvutsrqponmlkjihgfedcba"[i % 26];
}

/*
 * wts_ops --
 *	Perform a number of operations in a set of threads.
 */
void
wts_ops(int lastrun)
{
	TINFO **tinfo_list, *tinfo, total;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	wt_thread_t alter_tid, backup_tid, checkpoint_tid, compact_tid, lrt_tid;
	wt_thread_t timestamp_tid;
	int64_t fourths, quit_fourths, thread_ops;
	uint32_t i;
	bool running;

	conn = g.wts_conn;

	session = NULL;			/* -Wconditional-uninitialized */
	memset(&alter_tid, 0, sizeof(alter_tid));
	memset(&backup_tid, 0, sizeof(backup_tid));
	memset(&checkpoint_tid, 0, sizeof(checkpoint_tid));
	memset(&compact_tid, 0, sizeof(compact_tid));
	memset(&lrt_tid, 0, sizeof(lrt_tid));
	memset(&timestamp_tid, 0, sizeof(timestamp_tid));

	modify_repl_init();

	/*
	 * There are two mechanisms to specify the length of the run, a number
	 * of operations and a timer, when either expire the run terminates.
	 *
	 * Each thread does an equal share of the total operations (and make
	 * sure that it's not 0).
	 *
	 * Calculate how many fourth-of-a-second sleeps until the timer expires.
	 * If the timer expires and threads don't return in 15 minutes, assume
	 * there is something hung, and force the quit.
	 */
	if (g.c_ops == 0)
		thread_ops = -1;
	else {
		if (g.c_ops < g.c_threads)
			g.c_ops = g.c_threads;
		thread_ops = g.c_ops / g.c_threads;
	}
	if (g.c_timer == 0)
		fourths = quit_fourths = -1;
	else {
		fourths = ((int64_t)g.c_timer * 4 * 60) / FORMAT_OPERATION_REPS;
		quit_fourths = fourths + 15 * 4 * 60;
	}

	/* Initialize the table extension code. */
	table_append_init();

	/*
	 * We support replay of threaded runs, but don't log random numbers
	 * after threaded operations start, there's no point.
	 */
	if (!SINGLETHREADED)
		g.rand_log_stop = true;

	/* Open a session. */
	if (g.logging != 0) {
		testutil_check(conn->open_session(conn, NULL, NULL, &session));
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== thread ops start ===============");
	}

	/*
	 * Create the per-thread structures and start the worker threads.
	 * Allocate the thread structures separately to minimize false sharing.
	 */
	tinfo_list = dcalloc((size_t)g.c_threads, sizeof(TINFO *));
	for (i = 0; i < g.c_threads; ++i) {
		tinfo_list[i] = tinfo = dcalloc(1, sizeof(TINFO));

		tinfo->id = (int)i + 1;

		/*
		 * Characterize the per-thread random number generator. Normally
		 * we want independent behavior so threads start in different
		 * parts of the RNG space, but we've found bugs by having the
		 * threads pound on the same key/value pairs, that is, by making
		 * them traverse the same RNG space. 75% of the time we run in
		 * independent RNG space.
		 */
		if (g.c_independent_thread_rng)
			__wt_random_init_seed(
			    (WT_SESSION_IMPL *)session, &tinfo->rnd);
		else
			__wt_random_init(&tinfo->rnd);

		tinfo->state = TINFO_RUNNING;
		testutil_check(
		    __wt_thread_create(NULL, &tinfo->tid, ops, tinfo));
	}

	/*
	 * If a multi-threaded run, start optional backup, compaction and
	 * long-running reader threads.
	 */
	if (g.c_alter)
		testutil_check(
		    __wt_thread_create(NULL, &alter_tid, alter, NULL));
	if (g.c_backups)
		testutil_check(
		    __wt_thread_create(NULL, &backup_tid, backup, NULL));
	if (g.c_checkpoint_flag == CHECKPOINT_ON)
		testutil_check(__wt_thread_create(
		    NULL, &checkpoint_tid, checkpoint, NULL));
	if (g.c_compact)
		testutil_check(
		    __wt_thread_create(NULL, &compact_tid, compact, NULL));
	if (!SINGLETHREADED && g.c_long_running_txn)
		testutil_check(__wt_thread_create(NULL, &lrt_tid, lrt, NULL));
	if (g.c_txn_timestamps)
		testutil_check(__wt_thread_create(
		    NULL, &timestamp_tid, timestamp, tinfo_list));

	/* Spin on the threads, calculating the totals. */
	for (;;) {
		/* Clear out the totals each pass. */
		memset(&total, 0, sizeof(total));
		for (i = 0, running = false; i < g.c_threads; ++i) {
			tinfo = tinfo_list[i];
			total.commit += tinfo->commit;
			total.deadlock += tinfo->deadlock;
			total.insert += tinfo->insert;
			total.remove += tinfo->remove;
			total.rollback += tinfo->rollback;
			total.search += tinfo->search;
			total.update += tinfo->update;

			switch (tinfo->state) {
			case TINFO_RUNNING:
				running = true;
				break;
			case TINFO_COMPLETE:
				tinfo->state = TINFO_JOINED;
				testutil_check(
				    __wt_thread_join(NULL, tinfo->tid));
				break;
			case TINFO_JOINED:
				break;
			}

			/*
			 * If the timer has expired or this thread has completed
			 * its operations, notify the thread it should quit.
			 */
			if (fourths == 0 ||
			    (thread_ops != -1 &&
			    tinfo->ops >= (uint64_t)thread_ops)) {
				/*
				 * On the last execution, optionally drop core
				 * for recovery testing.
				 */
				if (lastrun && g.c_abort) {
					static char *core = NULL;
					*core = 0;
				}
				tinfo->quit = true;
			}
		}
		track("ops", 0ULL, &total);
		if (!running)
			break;
		__wt_sleep(0, 250000);		/* 1/4th of a second */
		if (fourths != -1)
			--fourths;
		if (quit_fourths != -1 && --quit_fourths == 0) {
			fprintf(stderr, "%s\n",
			    "format run exceeded 15 minutes past the maximum "
			    "time, aborting the process.");
			abort();
		}
	}

	/* Wait for the other threads. */
	g.workers_finished = true;
	if (g.c_alter)
		testutil_check(__wt_thread_join(NULL, alter_tid));
	if (g.c_backups)
		testutil_check(__wt_thread_join(NULL, backup_tid));
	if (g.c_checkpoint_flag == CHECKPOINT_ON)
		testutil_check(__wt_thread_join(NULL, checkpoint_tid));
	if (g.c_compact)
		testutil_check(__wt_thread_join(NULL, compact_tid));
	if (!SINGLETHREADED && g.c_long_running_txn)
		testutil_check(__wt_thread_join(NULL, lrt_tid));
	if (g.c_txn_timestamps)
		testutil_check(__wt_thread_join(NULL, timestamp_tid));
	g.workers_finished = false;

	if (g.logging != 0) {
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== thread ops stop ===============");
		testutil_check(session->close(session, NULL));
	}

	for (i = 0; i < g.c_threads; ++i)
		free(tinfo_list[i]);
	free(tinfo_list);
}

/*
 * isolation_config --
 *	Return an isolation configuration.
 */
static inline u_int
isolation_config(WT_RAND_STATE *rnd, WT_SESSION *session)
{
	u_int v;
	const char *config;

	if ((v = g.c_isolation_flag) == ISOLATION_RANDOM)
		v = mmrand(rnd, 2, 4);
	switch (v) {
	case ISOLATION_READ_UNCOMMITTED:
		config = "isolation=read-uncommitted";
		break;
	case ISOLATION_READ_COMMITTED:
		config = "isolation=read-committed";
		break;
	case ISOLATION_SNAPSHOT:
	default:
		v = ISOLATION_SNAPSHOT;
		config = "isolation=snapshot";
		break;
	}
	testutil_check(session->reconfigure(session, config));
	return (v);
}

typedef struct {
	uint64_t keyno;			/* Row number */

	void    *kdata;			/* If an insert, the generated key */
	size_t   ksize;
	size_t   kmemsize;

	void    *vdata;			/* If not a delete, the value */
	size_t   vsize;
	size_t   vmemsize;

	bool     deleted;		/* Delete operation */
	bool     insert;		/* Insert operation */
} SNAP_OPS;

#define	SNAP_TRACK							\
	(snap != NULL && (size_t)(snap - snap_list) < WT_ELEMENTS(snap_list))

/*
 * snap_track --
 *     Add a single snapshot isolation returned value to the list.
 */
static void
snap_track(SNAP_OPS *snap, uint64_t keyno, WT_ITEM *key, WT_ITEM *value)
{
	snap->keyno = keyno;
	if (key == NULL)
		snap->insert = false;
	else {
		snap->insert = true;

		if (snap->kmemsize < key->size) {
			snap->kdata = drealloc(snap->kdata, key->size);
			snap->kmemsize = key->size;
		}
		memcpy(snap->kdata, key->data, snap->ksize = key->size);
	}
	if (value == NULL)
		snap->deleted = true;
	else  {
		snap->deleted = false;
		if (snap->vmemsize < value->size) {
			snap->vdata = drealloc(snap->vdata, value->size);
			snap->vmemsize = value->size;
		}
		memcpy(snap->vdata, value->data, snap->vsize = value->size);
	}
}

/*
 * snap_check --
 *	Check snapshot isolation operations are repeatable.
 */
static int
snap_check(WT_CURSOR *cursor,
    SNAP_OPS *start, SNAP_OPS *stop, WT_ITEM *key, WT_ITEM *value)
{
	WT_DECL_RET;
	SNAP_OPS *p;
	uint8_t bitfield;

	for (; start < stop; ++start) {
		/* Check for subsequent changes to this record. */
		for (p = start + 1; p < stop && p->keyno != start->keyno; ++p)
			;
		if (p != stop)
			continue;

		/*
		 * Retrieve the key/value pair by key. Row-store inserts have a
		 * unique generated key we saved, else generate the key from the
		 * key number.
		 */
		if (start->insert == 0) {
			switch (g.type) {
			case FIX:
			case VAR:
				cursor->set_key(cursor, start->keyno);
				break;
			case ROW:
				key_gen(key, start->keyno);
				cursor->set_key(cursor, key);
				break;
			}
		} else {
			key->data = start->kdata;
			key->size = start->ksize;
			cursor->set_key(cursor, key);
		}
		if ((ret = cursor->search(cursor)) == 0) {
			if (g.type == FIX) {
				testutil_check(
				    cursor->get_value(cursor, &bitfield));
				*(uint8_t *)(value->data) = bitfield;
				value->size = 1;
			} else
				testutil_check(
				    cursor->get_value(cursor, value));
		} else
			if (ret != WT_NOTFOUND)
				return (ret);

		/* Check for simple matches. */
		if (ret == 0 && !start->deleted &&
		    value->size == start->vsize &&
		    memcmp(value->data, start->vdata, value->size) == 0)
			continue;
		if (ret == WT_NOTFOUND && start->deleted)
			continue;

		/*
		 * In fixed length stores, zero values at the end of the key
		 * space are returned as not-found, and not-found row reads
		 * are saved as zero values. Map back-and-forth for simplicity.
		 */
		if (g.type == FIX) {
			if (ret == WT_NOTFOUND &&
			    start->vsize == 1 && *(uint8_t *)start->vdata == 0)
				continue;
			if (start->deleted &&
			    value->size == 1 && *(uint8_t *)value->data == 0)
				continue;
		}

		/* Things went pear-shaped. */
		switch (g.type) {
		case FIX:
			testutil_die(ret,
			    "snapshot-isolation: %" PRIu64 " search: "
			    "expected {0x%02x}, found {0x%02x}",
			    start->keyno,
			    start->deleted ? 0 : *(uint8_t *)start->vdata,
			    ret == WT_NOTFOUND ? 0 : *(uint8_t *)value->data);
			/* NOTREACHED */
		case ROW:
			fprintf(stderr,
			    "snapshot-isolation %.*s search mismatch\n",
			    (int)key->size, (const char *)key->data);

			if (start->deleted)
				fprintf(stderr, "expected {deleted}\n");
			else
				print_item_data(
				    "expected", start->vdata, start->vsize);
			if (ret == WT_NOTFOUND)
				fprintf(stderr, "found {deleted}\n");
			else
				print_item_data(
				    "   found", value->data, value->size);

			testutil_die(ret,
			    "snapshot-isolation: %.*s search mismatch",
			    (int)key->size, key->data);
			/* NOTREACHED */
		case VAR:
			fprintf(stderr,
			    "snapshot-isolation %" PRIu64 " search mismatch\n",
			    start->keyno);

			if (start->deleted)
				fprintf(stderr, "expected {deleted}\n");
			else
				print_item_data(
				    "expected", start->vdata, start->vsize);
			if (ret == WT_NOTFOUND)
				fprintf(stderr, "found {deleted}\n");
			else
				print_item_data(
				    "   found", value->data, value->size);

			testutil_die(ret,
			    "snapshot-isolation: %" PRIu64 " search mismatch",
			    start->keyno);
			/* NOTREACHED */
		}
	}
	return (0);
}

/*
 * commit_transaction --
 *     Commit a transaction
 */
static void
commit_transaction(TINFO *tinfo, WT_SESSION *session)
{
	uint64_t ts;
	char config_buf[64];

	if (g.c_txn_timestamps) {
		ts = __wt_atomic_addv64(&g.timestamp, 1);
		testutil_check(__wt_snprintf(
		    config_buf, sizeof(config_buf),
		    "commit_timestamp=%" PRIx64, ts));
		testutil_check(
		    session->commit_transaction(session, config_buf));

		/*
		 * Update the thread's last-committed timestamp. Don't let the
		 * compiler re-order this statement, if we were to race with
		 * the timestamp thread, it might see our thread update before
		 * the transaction commit.
		 */
		WT_PUBLISH(tinfo->timestamp, ts);
	} else
		testutil_check(session->commit_transaction(session, NULL));
	++tinfo->commit;
}

/*
 * ops --
 *     Per-thread operations.
 */
static WT_THREAD_RET
ops(void *arg)
{
	enum { INSERT, MODIFY, READ, REMOVE, UPDATE } op;
	SNAP_OPS *snap, snap_list[64];
	TINFO *tinfo;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM *key, _key, *value, _value;
	WT_SESSION *session;
	uint64_t keyno, reset_op, session_op;
	uint32_t rnd;
	u_int i, iso_config;
	int dir;
	bool intxn, positioned, readonly;

	tinfo = arg;

	conn = g.wts_conn;
	readonly = false;		/* -Wconditional-uninitialized */

	/* Initialize tracking of snapshot isolation transaction returns. */
	snap = NULL;
	iso_config = 0;
	memset(snap_list, 0, sizeof(snap_list));

	/* Set up the default key and value buffers. */
	key = &_key;
	key_gen_init(key);
	value = &_value;
	val_gen_init(value);

	/* Set the first operation where we'll create sessions and cursors. */
	cursor = NULL;
	session = NULL;
	session_op = 0;

	/* Set the first operation where we'll reset the session. */
	reset_op = mmrand(&tinfo->rnd, 100, 10000);

	for (intxn = false; !tinfo->quit; ++tinfo->ops) {
		/* Periodically open up a new session and cursors. */
		if (tinfo->ops > session_op ||
		    session == NULL || cursor == NULL) {
			/*
			 * We can't swap sessions/cursors if in a transaction,
			 * resolve any running transaction.
			 */
			if (intxn) {
				commit_transaction(tinfo, session);
				intxn = false;
			}

			if (session != NULL)
				testutil_check(session->close(session, NULL));
			testutil_check(
			    conn->open_session(conn, NULL, NULL, &session));

			/* Pick the next session/cursor close/open. */
			session_op += mmrand(&tinfo->rnd, 100, 5000);

			/*
			 * 10% of the time, perform some read-only operations
			 * from a checkpoint.
			 *
			 * Skip if single-threaded and doing checks against a
			 * Berkeley DB database, that won't work because the
			 * Berkeley DB database won't match the checkpoint.
			 *
			 * Skip if we are using data-sources or LSM, they don't
			 * support reading from checkpoints.
			 */
			if (!SINGLETHREADED && !DATASOURCE("helium") &&
			    !DATASOURCE("kvsbdb") && !DATASOURCE("lsm") &&
			    mmrand(&tinfo->rnd, 1, 10) == 1) {
				/*
				 * open_cursor can return EBUSY if concurrent
				 * with a metadata operation, retry.
				 */
				while ((ret = session->open_cursor(session,
				    g.uri, NULL,
				    "checkpoint=WiredTigerCheckpoint",
				    &cursor)) == EBUSY)
					__wt_yield();
				/*
				 * If the checkpoint hasn't been created yet,
				 * ignore the error.
				 */
				if (ret == ENOENT)
					continue;
				testutil_check(ret);

				/* Checkpoints are read-only. */
				readonly = true;
			} else {
				/*
				 * Configure "append", in the case of column
				 * stores, we append when inserting new rows.
				 * open_cursor can return EBUSY if concurrent
				 * with a metadata operation, retry.
				 */
				while ((ret = session->open_cursor(session,
				    g.uri, NULL, "append", &cursor)) == EBUSY)
					__wt_yield();
				testutil_check(ret);

				/* Updates supported. */
				readonly = false;
			}
		}

		/*
		 * Reset the session every now and then, just to make sure that
		 * operation gets tested. Note the test is not for equality, we
		 * have to do the reset outside of a transaction.
		 */
		if (tinfo->ops > reset_op && !intxn) {
			testutil_check(session->reset(session));

			/* Pick the next reset operation. */
			reset_op += mmrand(&tinfo->rnd, 20000, 50000);
		}

		/*
		 * If we're not single-threaded and not in a transaction, choose
		 * an isolation level and start a transaction some percentage of
		 * the time.
		 */
		if (!SINGLETHREADED &&
		    !intxn && mmrand(&tinfo->rnd, 1, 100) >= g.c_txn_freq) {
			iso_config = isolation_config(&tinfo->rnd, session);
			testutil_check(
			    session->begin_transaction(session, NULL));

			snap =
			    iso_config == ISOLATION_SNAPSHOT ? snap_list : NULL;
			intxn = true;
		}

		/* Select a row. */
		keyno = mmrand(&tinfo->rnd, 1, (u_int)g.rows);
		positioned = false;

		/* Select an operation. */
		op = READ;
		if (!readonly) {
			i = mmrand(&tinfo->rnd, 1, 100);
			if (i < g.c_delete_pct)
				op = REMOVE;
			else if (i < g.c_delete_pct + g.c_insert_pct)
				op = INSERT;
			else if (i < g.c_delete_pct +
			    g.c_insert_pct + g.c_modify_pct)
				op = MODIFY;
			else if (i < g.c_delete_pct +
			    g.c_insert_pct + g.c_modify_pct + g.c_write_pct)
				op = UPDATE;
		}

		/*
		 * Inserts, removes and updates can be done following a cursor
		 * set-key, or based on a cursor position taken from a previous
		 * search. If not already doing a read, position the cursor at
		 * an existing point in the tree 20% of the time.
		 */
		positioned = false;
		if (op != READ && mmrand(&tinfo->rnd, 1, 5) == 1) {
			++tinfo->search;
			ret = read_row(cursor, key, value, keyno);
			if (ret == 0) {
				positioned = true;
				if (SNAP_TRACK)
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_NOTFOUND);
			}
		}

		/* Optionally reserve a row. */
		if (!readonly && intxn && mmrand(&tinfo->rnd, 0, 20) == 1) {
			switch (g.type) {
			case ROW:
				ret =
				    row_reserve(cursor, key, keyno, positioned);
				break;
			case FIX:
			case VAR:
				ret = col_reserve(cursor, keyno, positioned);
				break;
			}
			if (ret == 0) {
				positioned = true;
				__wt_yield();
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_NOTFOUND);
			}
		}

		/* Perform the operation. */
		switch (op) {
		case INSERT:
			switch (g.type) {
			case ROW:
				ret = row_insert(tinfo,
				    cursor, key, value, keyno, positioned);
				break;
			case FIX:
			case VAR:
				/*
				 * We can only append so many new records, once
				 * we reach that limit, update a record instead
				 * of inserting.
				 */
				if (g.append_cnt >= g.append_max)
					goto update_instead_of_chosen_op;

				ret = col_insert(
				    tinfo, cursor, key, value, &keyno);
				break;
			}

			/* Insert never leaves the cursor positioned. */
			positioned = false;
			if (ret == 0) {
				++tinfo->insert;
				if (SNAP_TRACK)
					snap_track(snap++, keyno,
					    g.type == ROW ? key : NULL, value);
			} else {
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_ROLLBACK);
			}
			break;
		case MODIFY:
			/*
			 * Change modify into update if in a read-uncommitted
			 * transaction, modify isn't supported in that case.
			 */
			if (iso_config == ISOLATION_READ_UNCOMMITTED)
				goto update_instead_of_chosen_op;

			++tinfo->update;
			switch (g.type) {
			case ROW:
				ret = row_modify(tinfo, cursor,
				    key, value, keyno, positioned);
				break;
			case VAR:
				ret = col_modify(tinfo, cursor,
				    key, value, keyno, positioned);
				break;
			}
			if (ret == 0) {
				positioned = true;
				if (SNAP_TRACK)
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(
				    ret == WT_NOTFOUND || ret == WT_ROLLBACK);
			}
			break;
		case READ:
			++tinfo->search;
			ret = read_row(cursor, key, value, keyno);
			if (ret == 0) {
				positioned = true;
				if (SNAP_TRACK)
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_NOTFOUND);
			}
			break;
		case REMOVE:
			switch (g.type) {
			case ROW:
				ret =
				    row_remove(cursor, key, keyno, positioned);
				break;
			case FIX:
			case VAR:
				ret =
				    col_remove(cursor, key, keyno, positioned);
				break;
			}
			if (ret == 0) {
				++tinfo->remove;
				/*
				 * Don't set positioned: it's unchanged from the
				 * previous state, but not necessarily set.
				 */
				if (SNAP_TRACK)
					snap_track(snap++, keyno, NULL, NULL);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_NOTFOUND);
			}
			break;
		case UPDATE:
update_instead_of_chosen_op:
			++tinfo->update;
			switch (g.type) {
			case ROW:
				ret = row_update(tinfo, cursor,
				    key, value, keyno, positioned);
				break;
			case FIX:
			case VAR:
				ret = col_update(tinfo, cursor,
				    key, value, keyno, positioned);
				break;
			}
			if (ret == 0) {
				positioned = true;
				if (SNAP_TRACK)
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_ROLLBACK);
			}
			break;
		}

		/*
		 * The cursor is positioned if we did any operation other than
		 * insert, do a small number of next/prev cursor operations in
		 * a random direction.
		 */
		if (positioned) {
			dir = (int)mmrand(&tinfo->rnd, 0, 1);
			for (i = 0; i < mmrand(&tinfo->rnd, 1, 100); ++i) {
				if ((ret = nextprev(cursor, dir)) == 0)
					continue;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
				testutil_assert(ret == WT_NOTFOUND);
				break;
			}
		}

		/* Reset the cursor: there is no reason to keep pages pinned. */
		testutil_check(cursor->reset(cursor));

		/*
		 * Continue if not in a transaction, else add more operations
		 * to the transaction half the time.
		 */
		if (!intxn || (rnd = mmrand(&tinfo->rnd, 1, 10)) > 5)
			continue;

		/*
		 * Ending the transaction. If in snapshot isolation, repeat the
		 * operations and confirm they're unchanged.
		 */
		if (snap != NULL && (ret = snap_check(
		    cursor, snap_list, snap, key, value)) == WT_ROLLBACK)
			goto deadlock;

		/*
		 * If we're in a transaction, commit 40% of the time and
		 * rollback 10% of the time.
		 */
		switch (rnd) {
		case 1: case 2: case 3: case 4:			/* 40% */
			commit_transaction(tinfo, session);
			break;
		case 5:						/* 10% */
			if (0) {
deadlock:			++tinfo->deadlock;
			}
			testutil_check(
			    session->rollback_transaction(session, NULL));
			++tinfo->rollback;
			break;
		}

		intxn = false;
		snap = NULL;
	}

	if (session != NULL)
		testutil_check(session->close(session, NULL));

	for (i = 0; i < WT_ELEMENTS(snap_list); ++i) {
		free(snap_list[i].kdata);
		free(snap_list[i].vdata);
	}
	key_gen_teardown(key);
	val_gen_teardown(value);

	tinfo->state = TINFO_COMPLETE;
	return (WT_THREAD_RET_VALUE);
}

/*
 * wts_read_scan --
 *	Read and verify all elements in a file.
 */
void
wts_read_scan(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_SESSION *session;
	uint64_t keyno, last_keyno;

	conn = g.wts_conn;

	/* Set up the default key/value buffers. */
	key_gen_init(&key);
	val_gen_init(&value);

	/* Open a session and cursor pair. */
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	/*
	 * open_cursor can return EBUSY if concurrent with a metadata
	 * operation, retry in that case.
	 */
	while ((ret = session->open_cursor(
	    session, g.uri, NULL, NULL, &cursor)) == EBUSY)
		__wt_yield();
	testutil_check(ret);

	/* Check a random subset of the records using the key. */
	for (last_keyno = keyno = 0; keyno < g.key_cnt;) {
		keyno += mmrand(NULL, 1, 17);
		if (keyno > g.rows)
			keyno = g.rows;
		if (keyno - last_keyno > 1000) {
			track("read row scan", keyno, NULL);
			last_keyno = keyno;
		}

		switch (ret = read_row(cursor, &key, &value, keyno)) {
		case 0:
		case WT_NOTFOUND:
		case WT_ROLLBACK:
			break;
		default:
			testutil_die(
			    ret, "wts_read_scan: read row %" PRIu64, keyno);
		}
	}

	testutil_check(session->close(session, NULL));

	key_gen_teardown(&key);
	val_gen_teardown(&value);
}

/*
 * read_row --
 *	Read and verify a single element in a row- or column-store file.
 */
int
read_row(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno)
{
	static int sn = 0;
	WT_SESSION *session;
	uint8_t bitfield;
	int exact, ret;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api,
		    session, "%-10s%" PRIu64, "read", keyno);

	/* Retrieve the key/value pair by key. */
	switch (g.type) {
	case FIX:
	case VAR:
		cursor->set_key(cursor, keyno);
		break;
	case ROW:
		key_gen(key, keyno);
		cursor->set_key(cursor, key);
		break;
	}

	if (sn) {
		ret = cursor->search_near(cursor, &exact);
		if (ret == 0 && exact != 0)
			ret = WT_NOTFOUND;
		sn = 0;
	} else {
		ret = cursor->search(cursor);
		sn = 1;
	}
	switch (ret) {
	case 0:
		if (g.type == FIX) {
			testutil_check(cursor->get_value(cursor, &bitfield));
			*(uint8_t *)(value->data) = bitfield;
			value->size = 1;
		} else
			testutil_check(cursor->get_value(cursor, value));
		break;
	case WT_NOTFOUND:
		/*
		 * In fixed length stores, zero values at the end of the key
		 * space are returned as not found.  Treat this the same as
		 * a zero value in the key space, to match BDB's behavior.
		 */
		if (g.type == FIX) {
			*(uint8_t *)(value->data) = 0;
			value->size = 1;
			ret = 0;
		}
		break;
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret, "read_row: read row %" PRIu64, keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (ret);

	/* Retrieve the BDB value. */
	{
	WT_ITEM bdb_value;
	int notfound;

	bdb_read(keyno, &bdb_value.data, &bdb_value.size, &notfound);

	/* Check for not-found status. */
	if (notfound_chk("read_row", ret, notfound, keyno))
		return (ret);

	/* Compare the two. */
	if (value->size != bdb_value.size ||
	    memcmp(value->data, bdb_value.data, value->size) != 0) {
		fprintf(stderr,
		    "read_row: value mismatch %" PRIu64 ":\n", keyno);
		print_item("bdb", &bdb_value);
		print_item(" wt", value);
		testutil_die(0, NULL);
	}
	}
#endif
	return (ret);
}

/*
 * nextprev --
 *	Read and verify the next/prev element in a row- or column-store file.
 */
static int
nextprev(WT_CURSOR *cursor, int next)
{
	WT_DECL_RET;
	WT_ITEM key, value;
	uint64_t keyno;
	uint8_t bitfield;
	const char *which;

	keyno = 0;
	which = next ? "next" : "prev";

	switch (ret = (next ? cursor->next(cursor) : cursor->prev(cursor))) {
	case 0:
		switch (g.type) {
		case FIX:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0 &&
			    (ret = cursor->get_value(cursor, &bitfield)) == 0) {
				value.data = &bitfield;
				value.size = 1;
			}
			break;
		case ROW:
			if ((ret = cursor->get_key(cursor, &key)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		case VAR:
			if ((ret = cursor->get_key(cursor, &keyno)) == 0)
				ret = cursor->get_value(cursor, &value);
			break;
		}
		if (ret != 0)
			testutil_die(ret, "nextprev: get_key/get_value");
		break;
	case WT_NOTFOUND:
		break;
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret, "%s", which);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (ret);

	{
	WT_ITEM bdb_key, bdb_value;
	WT_SESSION *session;
	int notfound;
	char *p;

	session = cursor->session;

	/* Retrieve the BDB key/value. */
	bdb_np(next, &bdb_key.data, &bdb_key.size,
	    &bdb_value.data, &bdb_value.size, &notfound);
	if (notfound_chk(
	    next ? "nextprev(next)" : "nextprev(prev)", ret, notfound, keyno))
		return (ret);

	/* Compare the two. */
	if ((g.type == ROW &&
	    (key.size != bdb_key.size ||
	    memcmp(key.data, bdb_key.data, key.size) != 0)) ||
	    (g.type != ROW && keyno != (uint64_t)atoll(bdb_key.data))) {
		fprintf(stderr, "nextprev: %s KEY mismatch:\n", which);
		goto mismatch;
	}
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "nextprev: %s VALUE mismatch:\n", which);
mismatch:	if (g.type == ROW) {
			print_item("bdb-key", &bdb_key);
			print_item(" wt-key", &key);
		} else {
			if ((p = (char *)strchr(bdb_key.data, '.')) != NULL)
				*p = '\0';
			fprintf(stderr, "\t%.*s != %" PRIu64 "\n",
			    (int)bdb_key.size, (char *)bdb_key.data, keyno);
		}
		print_item("bdb-value", &bdb_value);
		print_item(" wt-value", &value);
		testutil_die(0, NULL);
	}

	if (g.logging == LOG_OPS)
		switch (g.type) {
		case FIX:
			(void)g.wt_api->msg_printf(g.wt_api,
			    session, "%-10s%" PRIu64 " {0x%02x}", which,
			    keyno, ((char *)value.data)[0]);
			break;
		case ROW:
			(void)g.wt_api->msg_printf(
			    g.wt_api, session, "%-10s{%.*s}, {%.*s}", which,
			    (int)key.size, (char *)key.data,
			    (int)value.size, (char *)value.data);
			break;
		case VAR:
			(void)g.wt_api->msg_printf(g.wt_api, session,
			    "%-10s%" PRIu64 " {%.*s}", which,
			    keyno, (int)value.size, (char *)value.data);
			break;
		}
	}
#endif
	return (ret);
}

/*
 * row_reserve --
 *	Reserve a row in a row-store file.
 */
static int
row_reserve(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned) {
		key_gen(key, keyno);
		cursor->set_key(cursor, key);
	}

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s{%.*s}", "reserve", (int)key->size, key->data);

	switch (ret = cursor->reserve(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	case WT_NOTFOUND:
		return (WT_NOTFOUND);
	default:
		testutil_die(ret,
		    "row_reserve: reserve row %" PRIu64 " by key", keyno);
	}
	return (0);
}

/*
 * col_reserve --
 *	Reserve a row in a column-store file.
 */
static int
col_reserve(WT_CURSOR *cursor, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned)
		cursor->set_key(cursor, keyno);

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s%" PRIu64, "reserve", keyno);

	switch (ret = cursor->reserve(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	case WT_NOTFOUND:
		return (WT_NOTFOUND);
	default:
		testutil_die(ret, "col_reserve: %" PRIu64, keyno);
	}
	return (0);
}

/*
 * modify_build --
 *	Generate a set of modify vectors.
 */
static void
modify_build(TINFO *tinfo, WT_MODIFY *entries, int *nentriesp)
{
	int i, nentries;

	/* Randomly select a number of byte changes, offsets and lengths. */
	nentries = (int)mmrand(&tinfo->rnd, 1, MAX_MODIFY_ENTRIES);
	for (i = 0; i < nentries; ++i) {
		entries[i].data.data = modify_repl +
		    mmrand(&tinfo->rnd, 1, sizeof(modify_repl) - 10);
		entries[i].data.size = (size_t)mmrand(&tinfo->rnd, 0, 10);
		/*
		 * Start at least 11 bytes into the buffer so we skip leading
		 * key information.
		 */
		entries[i].offset = (size_t)mmrand(&tinfo->rnd, 20, 40);
		entries[i].size = (size_t)mmrand(&tinfo->rnd, 0, 10);
	}

	*nentriesp = (int)nentries;
}

/*
 * row_modify --
 *	Modify a row in a row-store file.
 */
static int
row_modify(TINFO *tinfo, WT_CURSOR *cursor,
    WT_ITEM *key, WT_ITEM *value, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;
	WT_MODIFY entries[MAX_MODIFY_ENTRIES];
	int nentries;

	if (!positioned) {
		key_gen(key, keyno);
		cursor->set_key(cursor, key);
	}

	modify_build(tinfo, entries, &nentries);
	switch (ret = cursor->modify(cursor, entries, nentries)) {
	case 0:
		testutil_check(cursor->get_value(cursor, value));
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	case WT_NOTFOUND:
		return (WT_NOTFOUND);
	default:
		testutil_die(ret,
		    "row_modify: modify row %" PRIu64 " by key", keyno);
	}

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s{%.*s}, {%.*s}",
		    "modify",
		    (int)key->size, key->data, (int)value->size, value->data);

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	bdb_update(key->data, key->size, value->data, value->size);
#endif
	return (0);
}

/*
 * col_modify --
 *	Modify a row in a column-store file.
 */
static int
col_modify(TINFO *tinfo, WT_CURSOR *cursor,
    WT_ITEM *key, WT_ITEM *value, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;
	WT_MODIFY entries[MAX_MODIFY_ENTRIES];
	int nentries;

	if (!positioned)
		cursor->set_key(cursor, keyno);

	modify_build(tinfo, entries, &nentries);
	switch (ret = cursor->modify(cursor, entries, nentries)) {
	case 0:
		testutil_check(cursor->get_value(cursor, value));
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	case WT_NOTFOUND:
		return (WT_NOTFOUND);
	default:
		testutil_die(ret, "col_modify: modify row %" PRIu64, keyno);
	}

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s{%.*s}, {%.*s}",
		    "modify",
		    (int)key->size, key->data, (int)value->size, value->data);

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	key_gen(key, keyno);
	bdb_update(key->data, key->size, value->data, value->size);
#else
	(void)key;				/* [-Wunused-variable] */
#endif
	return (0);
}

/*
 * row_update --
 *	Update a row in a row-store file.
 */
static int
row_update(TINFO *tinfo, WT_CURSOR *cursor,
    WT_ITEM *key, WT_ITEM *value, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned) {
		key_gen(key, keyno);
		cursor->set_key(cursor, key);
	}
	val_gen(&tinfo->rnd, value, keyno);
	cursor->set_value(cursor, value);

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s{%.*s}, {%.*s}",
		    "put",
		    (int)key->size, key->data, (int)value->size, value->data);

	switch (ret = cursor->update(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret,
		    "row_update: update row %" PRIu64 " by key", keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	bdb_update(key->data, key->size, value->data, value->size);
#endif
	return (0);
}

/*
 * col_update --
 *	Update a row in a column-store file.
 */
static int
col_update(TINFO *tinfo, WT_CURSOR *cursor,
    WT_ITEM *key, WT_ITEM *value, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned)
		cursor->set_key(cursor, keyno);
	val_gen(&tinfo->rnd, value, keyno);
	if (g.type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value->data);
	else
		cursor->set_value(cursor, value);

	if (g.logging == LOG_OPS) {
		if (g.type == FIX)
			(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "update", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "update", keyno,
			    (int)value->size, (char *)value->data);
	}

	switch (ret = cursor->update(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret, "col_update: %" PRIu64, keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	key_gen(key, keyno);
	bdb_update(key->data, key->size, value->data, value->size);
#else
	(void)key;				/* [-Wunused-variable] */
#endif
	return (0);
}

/*
 * table_append_init --
 *	Re-initialize the appended records list.
 */
static void
table_append_init(void)
{
	/* Append up to 10 records per thread before waiting on resolution. */
	g.append_max = (size_t)g.c_threads * 10;
	g.append_cnt = 0;

	free(g.append);
	g.append = dcalloc(g.append_max, sizeof(uint64_t));
}

/*
 * table_append --
 *	Resolve the appended records.
 */
static void
table_append(uint64_t keyno)
{
	uint64_t *ep, *p;
	int done;

	ep = g.append + g.append_max;

	/*
	 * We don't want to ignore records we append, which requires we update
	 * the "last row" as we insert new records. Threads allocating record
	 * numbers can race with other threads, so the thread allocating record
	 * N may return after the thread allocating N + 1.  We can't update a
	 * record before it's been inserted, and so we can't leave gaps when the
	 * count of records in the table is incremented.
	 *
	 * The solution is the append table, which contains an unsorted list of
	 * appended records.  Every time we finish appending a record, process
	 * the table, trying to update the total records in the object.
	 *
	 * First, enter the new key into the append list.
	 *
	 * It's technically possible to race: we allocated space for 10 records
	 * per thread, but the check for the maximum number of records being
	 * appended doesn't lock.  If a thread allocated a new record and went
	 * to sleep (so the append table fills up), then N threads of control
	 * used the same g.append_cnt value to decide there was an available
	 * slot in the append table and both allocated new records, we could run
	 * out of space in the table. It's unfortunately not even unlikely in
	 * the case of a large number of threads all inserting as fast as they
	 * can and a single thread going to sleep for an unexpectedly long time.
	 * If it happens, sleep and retry until earlier records are resolved
	 * and we find a slot.
	 */
	for (done = 0;;) {
		testutil_check(pthread_rwlock_wrlock(&g.append_lock));

		/*
		 * If this is the thread we've been waiting for, and its record
		 * won't fit, we'd loop infinitely.  If there are many append
		 * operations and a thread goes to sleep for a little too long,
		 * it can happen.
		 */
		if (keyno == g.rows + 1) {
			g.rows = keyno;
			done = 1;

			/*
			 * Clean out the table, incrementing the total count of
			 * records until we don't find the next key.
			 */
			for (;;) {
				for (p = g.append; p < ep; ++p)
					if (*p == g.rows + 1) {
						g.rows = *p;
						*p = 0;
						--g.append_cnt;
						break;
					}
				if (p == ep)
					break;
			}
		} else
			/* Enter the key into the table. */
			for (p = g.append; p < ep; ++p)
				if (*p == 0) {
					*p = keyno;
					++g.append_cnt;
					done = 1;
					break;
				}

		testutil_check(pthread_rwlock_unlock(&g.append_lock));

		if (done)
			break;
		__wt_sleep(1, 0);
	}
}

/*
 * row_insert --
 *	Insert a row in a row-store file.
 */
static int
row_insert(TINFO *tinfo, WT_CURSOR *cursor,
    WT_ITEM *key, WT_ITEM *value, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	/*
	 * If we positioned the cursor already, it's a test of an update using
	 * the insert method. Otherwise, generate a unique key and insert.
	 */
	if (!positioned) {
		key_gen_insert(&tinfo->rnd, key, keyno);
		cursor->set_key(cursor, key);
	}
	val_gen(&tinfo->rnd, value, keyno);
	cursor->set_value(cursor, value);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
		    "%-10s{%.*s}, {%.*s}",
		    "insert",
		    (int)key->size, key->data, (int)value->size, value->data);

	switch (ret = cursor->insert(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret,
		    "row_insert: insert row %" PRIu64 " by key", keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	bdb_update(key->data, key->size, value->data, value->size);
#endif
	return (0);
}

/*
 * col_insert --
 *	Insert an element in a column-store file.
 */
static int
col_insert(TINFO *tinfo,
    WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t *keynop)
{
	WT_DECL_RET;
	uint64_t keyno;

	val_gen(&tinfo->rnd, value, g.rows + 1);
	if (g.type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value->data);
	else
		cursor->set_value(cursor, value);
	switch (ret = cursor->insert(cursor)) {
	case 0:
		break;
	case WT_CACHE_FULL:
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret, "cursor.insert");
	}
	testutil_check(cursor->get_key(cursor, &keyno));
	*keynop = (uint32_t)keyno;

	table_append(keyno);			/* Extend the object. */

	if (g.logging == LOG_OPS) {
		if (g.type == FIX)
			(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "insert", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)g.wt_api->msg_printf(g.wt_api, cursor->session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "insert", keyno,
			    (int)value->size, (char *)value->data);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (0);

	key_gen(key, keyno);
	bdb_update(key->data, key->size, value->data, value->size);
#else
	(void)key;				/* [-Wunused-variable] */
#endif
	return (0);
}

/*
 * row_remove --
 *	Remove an row from a row-store file.
 */
static int
row_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned) {
		key_gen(key, keyno);
		cursor->set_key(cursor, key);
	}

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api,
		    cursor->session, "%-10s%" PRIu64, "remove", keyno);

	/* We use the cursor in overwrite mode, check for existence. */
	if ((ret = cursor->search(cursor)) == 0)
		ret = cursor->remove(cursor);
	switch (ret) {
	case 0:
	case WT_NOTFOUND:
		break;
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret,
		    "row_remove: remove %" PRIu64 " by key", keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (ret);

	{
	int notfound;

	bdb_remove(keyno, &notfound);
	(void)notfound_chk("row_remove", ret, notfound, keyno);
	}
#else
	(void)key;				/* [-Wunused-variable] */
#endif
	return (ret);
}

/*
 * col_remove --
 *	Remove a row from a column-store file.
 */
static int
col_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno, bool positioned)
{
	WT_DECL_RET;

	if (!positioned)
		cursor->set_key(cursor, keyno);

	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api,
		    cursor->session, "%-10s%" PRIu64, "remove", keyno);

	/* We use the cursor in overwrite mode, check for existence. */
	if ((ret = cursor->search(cursor)) == 0)
		ret = cursor->remove(cursor);
	switch (ret) {
	case 0:
	case WT_NOTFOUND:
		break;
	case WT_ROLLBACK:
		return (WT_ROLLBACK);
	default:
		testutil_die(ret,
		    "col_remove: remove %" PRIu64 " by key", keyno);
	}

#ifdef HAVE_BERKELEY_DB
	if (!SINGLETHREADED)
		return (ret);

	/*
	 * Deleting a fixed-length item is the same as setting the bits to 0;
	 * do the same thing for the BDB store.
	 */
	if (g.type == FIX) {
		key_gen(key, keyno);
		bdb_update(key->data, key->size, "", 1);
	} else {
		int notfound;

		bdb_remove(keyno, &notfound);
		(void)notfound_chk("col_remove", ret, notfound, keyno);
	}
#else
	(void)key;				/* [-Wunused-variable] */
#endif
	return (ret);
}

#ifdef HAVE_BERKELEY_DB
/*
 * notfound_chk --
 *	Compare notfound returns for consistency.
 */
static int
notfound_chk(const char *f, int wt_ret, int bdb_notfound, uint64_t keyno)
{
	/* Check for not found status. */
	if (bdb_notfound && wt_ret == WT_NOTFOUND)
		return (1);

	if (bdb_notfound) {
		fprintf(stderr, "%s: %s:", progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " not found in Berkeley DB, found in WiredTiger\n");
		testutil_die(0, NULL);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s:", progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " found in Berkeley DB, not found in WiredTiger\n");
		testutil_die(0, NULL);
	}
	return (0);
}
#endif
