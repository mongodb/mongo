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

#include "format.h"

static int   col_insert(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t *);
static int   col_remove(WT_CURSOR *, WT_ITEM *, uint64_t);
static int   col_update(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t);
static int   nextprev(WT_CURSOR *, int);
static void *ops(void *);
static int   row_insert(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t);
static int   row_remove(WT_CURSOR *, WT_ITEM *, uint64_t);
static int   row_update(WT_CURSOR *, WT_ITEM *, WT_ITEM *, uint64_t);
static void  table_append_init(void);

#ifdef HAVE_BERKELEY_DB
static int   notfound_chk(const char *, int, int, uint64_t);
static void  print_item(const char *, WT_ITEM *);
#endif

/*
 * wts_ops --
 *	Perform a number of operations in a set of threads.
 */
void
wts_ops(int lastrun)
{
	TINFO *tinfo, total;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	pthread_t backup_tid, compact_tid, lrt_tid;
	int64_t fourths, thread_ops;
	uint32_t i;
	int running;

	conn = g.wts_conn;

	session = NULL;			/* -Wconditional-uninitialized */
	memset(&backup_tid, 0, sizeof(backup_tid));
	memset(&compact_tid, 0, sizeof(compact_tid));
	memset(&lrt_tid, 0, sizeof(lrt_tid));

	/*
	 * There are two mechanisms to specify the length of the run, a number
	 * of operations and a timer, when either expire the run terminates.
	 * Each thread does an equal share of the total operations (and make
	 * sure that it's not 0).
	 *
	 * Calculate how many fourth-of-a-second sleeps until any timer expires.
	 */
	if (g.c_ops == 0)
		thread_ops = -1;
	else {
		if (g.c_ops < g.c_threads)
			g.c_ops = g.c_threads;
		thread_ops = g.c_ops / g.c_threads;
	}
	if (g.c_timer == 0)
		fourths = -1;
	else
		fourths = ((int64_t)g.c_timer * 4 * 60) / FORMAT_OPERATION_REPS;

	/* Initialize the table extension code. */
	table_append_init();

	/*
	 * We support replay of threaded runs, but don't log random numbers
	 * after threaded operations start, there's no point.
	 */
	if (!SINGLETHREADED)
		g.rand_log_stop = 1;

	/* Open a session. */
	if (g.logging != 0) {
		testutil_check(conn->open_session(conn, NULL, NULL, &session));
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== thread ops start ===============");
	}

	/* Create thread structure; start the worker threads. */
	tinfo = dcalloc((size_t)g.c_threads, sizeof(*tinfo));
	for (i = 0; i < g.c_threads; ++i) {
		tinfo[i].id = (int)i + 1;
		tinfo[i].state = TINFO_RUNNING;
		testutil_check(
		    pthread_create(&tinfo[i].tid, NULL, ops, &tinfo[i]));
	}

	/*
	 * If a multi-threaded run, start optional backup, compaction and
	 * long-running reader threads.
	 */
	if (g.c_backups)
		testutil_check(pthread_create(&backup_tid, NULL, backup, NULL));
	if (g.c_compact)
		testutil_check(
		    pthread_create(&compact_tid, NULL, compact, NULL));
	if (!SINGLETHREADED && g.c_long_running_txn)
		testutil_check(pthread_create(&lrt_tid, NULL, lrt, NULL));

	/* Spin on the threads, calculating the totals. */
	for (;;) {
		/* Clear out the totals each pass. */
		memset(&total, 0, sizeof(total));
		for (i = 0, running = 0; i < g.c_threads; ++i) {
			total.commit += tinfo[i].commit;
			total.deadlock += tinfo[i].deadlock;
			total.insert += tinfo[i].insert;
			total.remove += tinfo[i].remove;
			total.rollback += tinfo[i].rollback;
			total.search += tinfo[i].search;
			total.update += tinfo[i].update;

			switch (tinfo[i].state) {
			case TINFO_RUNNING:
				running = 1;
				break;
			case TINFO_COMPLETE:
				tinfo[i].state = TINFO_JOINED;
				(void)pthread_join(tinfo[i].tid, NULL);
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
			    tinfo[i].ops >= (uint64_t)thread_ops)) {
				/*
				 * On the last execution, optionally drop core
				 * for recovery testing.
				 */
				if (lastrun && g.c_abort) {
					static char *core = NULL;
					*core = 0;
				}
				tinfo[i].quit = 1;
			}
		}
		track("ops", 0ULL, &total);
		if (!running)
			break;
		(void)usleep(250000);		/* 1/4th of a second */
		if (fourths != -1)
			--fourths;
	}
	free(tinfo);

	/* Wait for the backup, compaction, long-running reader threads. */
	g.workers_finished = 1;
	if (g.c_backups)
		(void)pthread_join(backup_tid, NULL);
	if (g.c_compact)
		(void)pthread_join(compact_tid, NULL);
	if (!SINGLETHREADED && g.c_long_running_txn)
		(void)pthread_join(lrt_tid, NULL);
	g.workers_finished = 0;

	if (g.logging != 0) {
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== thread ops stop ===============");
		testutil_check(session->close(session, NULL));
	}
}

/*
 * isolation_config --
 *	Return an isolation configuration.
 */
static inline const char *
isolation_config(WT_RAND_STATE *rnd, bool *iso_snapshotp)
{
	u_int v;

	if ((v = g.c_isolation_flag) == ISOLATION_RANDOM)
		v = mmrand(rnd, 2, 4);
	switch (v) {
	case ISOLATION_READ_UNCOMMITTED:
		*iso_snapshotp = false;
		return ("isolation=read-uncommitted");
	case ISOLATION_READ_COMMITTED:
		*iso_snapshotp = false;
		return ("isolation=read-committed");
	case ISOLATION_SNAPSHOT:
	default:
		*iso_snapshotp = true;
		return ("isolation=snapshot");
	}
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
			    "snap_check: %" PRIu64 " search: "
			    "expected {0x%02x}, found {0x%02x}",
			    start->keyno,
			    start->deleted ? 0 : *(uint8_t *)start->vdata,
			    ret == WT_NOTFOUND ? 0 : *(uint8_t *)value->data);
			/* NOTREACHED */
		case ROW:
			testutil_die(ret,
			    "snap_check: %.*s search: "
			    "expected {%.*s}, found {%.*s}",
			    (int)key->size, key->data,
			    start->deleted ?
			    (int)strlen("deleted") : (int)start->vsize,
			    start->deleted ? "deleted" : start->vdata,
			    ret == WT_NOTFOUND ?
			    (int)strlen("deleted") : (int)value->size,
			    ret == WT_NOTFOUND ? "deleted" : value->data);
			/* NOTREACHED */
		case VAR:
			testutil_die(ret,
			    "snap_check: %" PRIu64 " search: "
			    "expected {%.*s}, found {%.*s}",
			    start->keyno,
			    start->deleted ?
			    (int)strlen("deleted") : (int)start->vsize,
			    start->deleted ? "deleted" : start->vdata,
			    ret == WT_NOTFOUND ?
			    (int)strlen("deleted") : (int)value->size,
			    ret == WT_NOTFOUND ? "deleted" : value->data);
			/* NOTREACHED */
		}
	}
	return (0);
}

/*
 * ops --
 *     Per-thread operations.
 */
static void *
ops(void *arg)
{
	SNAP_OPS *snap, snap_list[64];
	TINFO *tinfo;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_DECL_RET;
	WT_ITEM *key, _key, *value, _value;
	WT_SESSION *session;
	uint64_t keyno, ckpt_op, reset_op, session_op;
	uint32_t op, rnd;
	u_int i;
	int dir;
	char *ckpt_config, ckpt_name[64];
	bool ckpt_available, intxn, iso_snapshot, positioned, readonly;

	tinfo = arg;

	conn = g.wts_conn;
	readonly = false;		/* -Wconditional-uninitialized */

	/* Initialize tracking of snapshot isolation transaction returns. */
	snap = NULL;
	iso_snapshot = false;
	memset(snap_list, 0, sizeof(snap_list));

	/* Initialize the per-thread random number generator. */
	__wt_random_init(&tinfo->rnd);

	/* Set up the default key and value buffers. */
	key = &_key;
	key_gen_setup(key);
	value = &_value;
	val_gen_setup(&tinfo->rnd, value);

	/* Set the first operation where we'll create sessions and cursors. */
	session_op = 0;
	session = NULL;
	cursor = cursor_insert = NULL;

	/* Set the first operation where we'll perform checkpoint operations. */
	ckpt_op = g.c_checkpoints ? mmrand(&tinfo->rnd, 100, 10000) : 0;
	ckpt_available = false;

	/* Set the first operation where we'll reset the session. */
	reset_op = mmrand(&tinfo->rnd, 100, 10000);

	for (intxn = false; !tinfo->quit; ++tinfo->ops) {
		/*
		 * We can't checkpoint or swap sessions/cursors while in a
		 * transaction, resolve any running transaction.
		 */
		if (intxn &&
		    (tinfo->ops == ckpt_op || tinfo->ops == session_op)) {
			testutil_check(
			    session->commit_transaction(session, NULL));
			++tinfo->commit;
			intxn = false;
		}

		/* Open up a new session and cursors. */
		if (tinfo->ops == session_op ||
		    session == NULL || cursor == NULL) {
			if (session != NULL)
				testutil_check(session->close(session, NULL));

			testutil_check(
			    conn->open_session(conn, NULL, NULL, &session));

			/*
			 * 10% of the time, perform some read-only operations
			 * from a checkpoint.
			 *
			 * Skip that if we single-threaded and doing checks
			 * against a Berkeley DB database, because that won't
			 * work because the Berkeley DB database records won't
			 * match the checkpoint.  Also skip if we are using
			 * LSM, because it doesn't support reads from
			 * checkpoints.
			 */
			if (!SINGLETHREADED && !DATASOURCE("lsm") &&
			    ckpt_available && mmrand(&tinfo->rnd, 1, 10) == 1) {
				testutil_check(session->open_cursor(session,
				    g.uri, NULL, ckpt_name, &cursor));

				/* Pick the next session/cursor close/open. */
				session_op += 250;

				/* Checkpoints are read-only. */
				readonly = true;
			} else {
				/*
				 * Open two cursors: one for overwriting and one
				 * for append (if it's a column-store).
				 *
				 * The reason is when testing with existing
				 * records, we don't track if a record was
				 * deleted or not, which means we must use
				 * cursor->insert with overwriting configured.
				 * But, in column-store files where we're
				 * testing with new, appended records, we don't
				 * want to have to specify the record number,
				 * which requires an append configuration.
				 */
				testutil_check(session->open_cursor(session,
				    g.uri, NULL, "overwrite", &cursor));
				if (g.type == FIX || g.type == VAR)
					testutil_check(session->open_cursor(
					    session, g.uri,
					    NULL, "append", &cursor_insert));

				/* Pick the next session/cursor close/open. */
				session_op += mmrand(&tinfo->rnd, 100, 5000);

				/* Updates supported. */
				readonly = false;
			}
		}

		/* Checkpoint the database. */
		if (tinfo->ops == ckpt_op && g.c_checkpoints) {
			/*
			 * Checkpoints are single-threaded inside WiredTiger,
			 * skip our checkpoint if another thread is already
			 * doing one.
			 */
			ret = pthread_rwlock_trywrlock(&g.checkpoint_lock);
			if (ret == EBUSY)
				goto skip_checkpoint;
			testutil_check(ret);

			/*
			 * LSM and data-sources don't support named checkpoints
			 * and we can't drop a named checkpoint while there's a
			 * backup in progress, otherwise name the checkpoint 5%
			 * of the time.
			 */
			if (mmrand(&tinfo->rnd, 1, 20) != 1 ||
			    DATASOURCE("helium") ||
			    DATASOURCE("kvsbdb") || DATASOURCE("lsm") ||
			    pthread_rwlock_trywrlock(&g.backup_lock) == EBUSY)
				ckpt_config = NULL;
			else {
				(void)snprintf(ckpt_name, sizeof(ckpt_name),
				    "name=thread-%d", tinfo->id);
				ckpt_config = ckpt_name;
			}

			ret = session->checkpoint(session, ckpt_config);
			/*
			 * We may be trying to create a named checkpoint while
			 * we hold a cursor open to the previous checkpoint.
			 * Tolerate EBUSY.
			 */
			if (ret != 0 && ret != EBUSY)
				testutil_die(ret, "%s",
				    ckpt_config == NULL ? "" : ckpt_config);
			ret = 0;

			if (ckpt_config != NULL)
				testutil_check(
				    pthread_rwlock_unlock(&g.backup_lock));
			testutil_check(
			    pthread_rwlock_unlock(&g.checkpoint_lock));

			/* Rephrase the checkpoint name for cursor open. */
			if (ckpt_config == NULL)
				strcpy(ckpt_name,
				    "checkpoint=WiredTigerCheckpoint");
			else
				(void)snprintf(ckpt_name, sizeof(ckpt_name),
				    "checkpoint=thread-%d", tinfo->id);
			ckpt_available = true;

skip_checkpoint:	/* Pick the next checkpoint operation. */
			ckpt_op += mmrand(&tinfo->rnd, 5000, 20000);
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
			testutil_check(
			    session->reconfigure(session,
				isolation_config(&tinfo->rnd, &iso_snapshot)));
			testutil_check(
			    session->begin_transaction(session, NULL));

			snap = iso_snapshot ? snap_list : NULL;
			intxn = true;
		}

		keyno = mmrand(&tinfo->rnd, 1, (u_int)g.rows);
		positioned = false;

		/*
		 * Perform some number of operations: the percentage of deletes,
		 * inserts and writes are specified, reads are the rest.  The
		 * percentages don't have to add up to 100, a high percentage
		 * of deletes will mean fewer inserts and writes.  Modifications
		 * are always followed by a read to confirm it worked.
		 */
		op = readonly ? UINT32_MAX : mmrand(&tinfo->rnd, 1, 100);
		if (op < g.c_delete_pct) {
			++tinfo->remove;
			switch (g.type) {
			case ROW:
				ret = row_remove(cursor, key, keyno);
				break;
			case FIX:
			case VAR:
				ret = col_remove(cursor, key, keyno);
				break;
			}
			if (ret == 0) {
				positioned = true;
				if (snap != NULL && (size_t)
				    (snap - snap_list) < WT_ELEMENTS(snap_list))
					snap_track(snap++, keyno, NULL, NULL);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
			}
		} else if (op < g.c_delete_pct + g.c_insert_pct) {
			++tinfo->insert;
			switch (g.type) {
			case ROW:
				key_gen_insert(&tinfo->rnd, key, keyno);
				val_gen(&tinfo->rnd, value, keyno);
				ret = row_insert(cursor, key, value, keyno);
				break;
			case FIX:
			case VAR:
				/*
				 * We can only append so many new records, if
				 * we've reached that limit, update a record
				 * instead of doing an insert.
				 */
				if (g.append_cnt >= g.append_max)
					goto skip_insert;

				/* Insert, then reset the insert cursor. */
				val_gen(&tinfo->rnd, value, g.rows + 1);
				ret = col_insert(
				    cursor_insert, key, value, &keyno);
				testutil_check(
				    cursor_insert->reset(cursor_insert));
				break;
			}
			positioned = false;
			if (ret == 0) {
				if (snap != NULL && (size_t)
				    (snap - snap_list) < WT_ELEMENTS(snap_list))
					snap_track(snap++, keyno,
					    g.type == ROW ? key : NULL, value);
			} else
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
		} else if (
		    op < g.c_delete_pct + g.c_insert_pct + g.c_write_pct) {
			++tinfo->update;
			switch (g.type) {
			case ROW:
				key_gen(key, keyno);
				val_gen(&tinfo->rnd, value, keyno);
				ret = row_update(cursor, key, value, keyno);
				break;
			case FIX:
			case VAR:
skip_insert:			val_gen(&tinfo->rnd, value, keyno);
				ret = col_update(cursor, key, value, keyno);
				break;
			}
			if (ret == 0) {
				positioned = true;
				if (snap != NULL && (size_t)
				    (snap - snap_list) < WT_ELEMENTS(snap_list))
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
			}
		} else {
			++tinfo->search;
			ret = read_row(cursor, key, value, keyno);
			if (ret == 0) {
				positioned = true;
				if (snap != NULL && (size_t)
				    (snap - snap_list) < WT_ELEMENTS(snap_list))
					snap_track(snap++, keyno, NULL, value);
			} else {
				positioned = false;
				if (ret == WT_ROLLBACK && intxn)
					goto deadlock;
			}
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
				break;
			}
		}

		/* Reset the cursor: there is no reason to keep pages pinned. */
		testutil_check(cursor->reset(cursor));

		/*
		 * If we're in a transaction, commit 40% of the time and
		 * rollback 10% of the time.
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

		switch (rnd) {
		case 1: case 2: case 3: case 4:			/* 40% */
			testutil_check(
			    session->commit_transaction(session, NULL));
			++tinfo->commit;
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
	free(key->mem);
	free(value->mem);

	tinfo->state = TINFO_COMPLETE;
	return (NULL);
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
	key_gen_setup(&key);
	val_gen_setup(NULL, &value);

	/* Open a session and cursor pair. */
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(
	    session->open_cursor(session, g.uri, NULL, NULL, &cursor));

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

	free(key.mem);
	free(value.mem);
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
	int exact, ret;
	uint8_t bitfield;

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

	/* Retrieve the BDB value. */
	bdb_np(next, &bdb_key.data, &bdb_key.size,
	    &bdb_value.data, &bdb_value.size, &notfound);
	if (notfound_chk(
	    next ? "nextprev(next)" : "nextprev(prev)", ret, notfound, keyno))
		return (ret);

	/* Compare the two. */
	if (g.type == ROW) {
		if (key.size != bdb_key.size ||
		    memcmp(key.data, bdb_key.data, key.size) != 0) {
			fprintf(stderr, "nextprev: %s key mismatch:\n", which);
			print_item("bdb-key", &bdb_key);
			print_item(" wt-key", &key);
			testutil_die(0, NULL);
		}
	} else {
		if (keyno != (uint64_t)atoll(bdb_key.data)) {
			if ((p = strchr((char *)bdb_key.data, '.')) != NULL)
				*p = '\0';
			fprintf(stderr,
			    "nextprev: %s key mismatch: %.*s != %" PRIu64 "\n",
			    which,
			    (int)bdb_key.size, (char *)bdb_key.data, keyno);
			testutil_die(0, NULL);
		}
	}
	if (value.size != bdb_value.size ||
	    memcmp(value.data, bdb_value.data, value.size) != 0) {
		fprintf(stderr, "nextprev: %s value mismatch:\n", which);
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
			    g.wt_api, session, "%-10s{%.*s/%.*s}", which,
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
 * row_update --
 *	Update a row in a row-store file.
 */
static int
row_update(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno)
{
	WT_DECL_RET;
	WT_SESSION *session;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "%-10s{%.*s}, {%.*s}",
		    "put",
		    (int)key->size, key->data, (int)value->size, value->data);

	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
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
col_update(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno)
{
	WT_DECL_RET;
	WT_SESSION *session;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS) {
		if (g.type == FIX)
			(void)g.wt_api->msg_printf(g.wt_api, session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "update", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)g.wt_api->msg_printf(g.wt_api, session,
			    "%-10s%" PRIu64 " {%.*s}",
			    "update", keyno,
			    (int)value->size, (char *)value->data);
	}

	cursor->set_key(cursor, keyno);
	if (g.type == FIX)
		cursor->set_value(cursor, *(uint8_t *)value->data);
	else
		cursor->set_value(cursor, value);
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
	uint64_t *p, *ep;
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
		sleep(1);
	}
}

/*
 * row_insert --
 *	Insert a row in a row-store file.
 */
static int
row_insert(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t keyno)
{
	WT_DECL_RET;
	WT_SESSION *session;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "%-10s{%.*s}, {%.*s}",
		    "insert",
		    (int)key->size, key->data, (int)value->size, value->data);

	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
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
col_insert(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value, uint64_t *keynop)
{
	WT_DECL_RET;
	WT_SESSION *session;
	uint64_t keyno;

	session = cursor->session;

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
			(void)g.wt_api->msg_printf(g.wt_api, session,
			    "%-10s%" PRIu64 " {0x%02" PRIx8 "}",
			    "insert", keyno,
			    ((uint8_t *)value->data)[0]);
		else
			(void)g.wt_api->msg_printf(g.wt_api, session,
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
row_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno)
{
	WT_DECL_RET;
	WT_SESSION *session;

	session = cursor->session;

	key_gen(key, keyno);

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(
		    g.wt_api, session, "%-10s%" PRIu64, "remove", keyno);

	cursor->set_key(cursor, key);
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
col_remove(WT_CURSOR *cursor, WT_ITEM *key, uint64_t keyno)
{
	WT_DECL_RET;
	WT_SESSION *session;

	session = cursor->session;

	/* Log the operation */
	if (g.logging == LOG_OPS)
		(void)g.wt_api->msg_printf(
		    g.wt_api, session, "%-10s%" PRIu64, "remove", keyno);

	cursor->set_key(cursor, keyno);
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
		bdb_update(key->data, key->size, "\0", 1);
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
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " not found in Berkeley DB, found in WiredTiger\n");
		testutil_die(0, NULL);
	}
	if (wt_ret == WT_NOTFOUND) {
		fprintf(stderr, "%s: %s:", g.progname, f);
		if (keyno != 0)
			fprintf(stderr, " row %" PRIu64 ":", keyno);
		fprintf(stderr,
		    " found in Berkeley DB, not found in WiredTiger\n");
		testutil_die(0, NULL);
	}
	return (0);
}

/*
 * print_item --
 *	Display a single data/size pair, with a tag.
 */
static void
print_item(const char *tag, WT_ITEM *item)
{
	static const char hex[] = "0123456789abcdef";
	const uint8_t *data;
	size_t size;
	u_char ch;

	data = item->data;
	size = item->size;

	fprintf(stderr, "\t%s {", tag);
	if (g.type == FIX)
		fprintf(stderr, "0x%02x", data[0]);
	else
		for (; size > 0; --size, ++data) {
			ch = data[0];
			if (__wt_isprint(ch))
				fprintf(stderr, "%c", (int)ch);
			else
				fprintf(stderr, "%x%x",
				    hex[(data[0] & 0xf0) >> 4],
				    hex[data[0] & 0x0f]);
		}
	fprintf(stderr, "}\n");
}
#endif
