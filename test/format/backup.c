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

/*
 * check_copy --
 *	Confirm the backup worked.
 */
static void
check_copy(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;

	wts_open(g.home_backup, false, &conn);

	testutil_checkfmt(
	    conn->open_session(conn, NULL, NULL, &session),
	    "%s", g.home_backup);

	testutil_checkfmt(
	    session->verify(session, g.uri, NULL),
	    "%s: %s", g.home_backup, g.uri);

	testutil_checkfmt(conn->close(conn, NULL), "%s", g.home_backup);
}

/*
 * copy_file --
 *	Copy a single file into the backup directories.
 */
static void
copy_file(WT_SESSION *session, const char *name)
{
	size_t len;
	char *first, *second;

	len = strlen("BACKUP") + strlen(name) + 10;
	first = dmalloc(len);
	(void)snprintf(first, len, "BACKUP/%s", name);
	testutil_check(__wt_copy_and_sync(session, name, first));

	/*
	 * Save another copy of the original file to make debugging recovery
	 * errors easier.
	 */
	len = strlen("BACKUP_COPY") + strlen(name) + 10;
	second = dmalloc(len);
	(void)snprintf(second, len, "BACKUP_COPY/%s", name);
	testutil_check(__wt_copy_and_sync(session, first, second));

	free(first);
	free(second);
}

/*
 * backup --
 *	Periodically do a backup and verify it.
 */
void *
backup(void *arg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *backup_cursor;
	WT_DECL_RET;
	WT_SESSION *session;
	u_int incremental, period;
	bool full;
	const char *config, *key;

	(void)(arg);

	conn = g.wts_conn;

	/* Backups aren't supported for non-standard data sources. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		return (NULL);

	/* Open a session. */
	testutil_check(conn->open_session(conn, NULL, NULL, &session));

	/*
	 * Perform a full backup at somewhere under 10 seconds (that way there's
	 * at least one), then at larger intervals, optionally do incremental
	 * backups between full backups.
	 */
	incremental = 0;
	for (period = mmrand(NULL, 1, 10);; period = mmrand(NULL, 20, 45)) {
		/* Sleep for short periods so we don't make the run wait. */
		while (period > 0 && !g.workers_finished) {
			--period;
			sleep(1);
		}

		/*
		 * We can't drop named checkpoints while there's a backup in
		 * progress, serialize backups with named checkpoints. Wait
		 * for the checkpoint to complete, otherwise backups might be
		 * starved out.
		 */
		testutil_check(pthread_rwlock_wrlock(&g.backup_lock));
		if (g.workers_finished) {
			testutil_check(pthread_rwlock_unlock(&g.backup_lock));
			break;
		}

		if (incremental) {
			config = "target=(\"log:\")";
			full = false;
		} else {
			/* Re-create the backup directory. */
			testutil_checkfmt(
			    system(g.home_backup_init),
			    "%s", "backup directory creation failed");

			config = NULL;
			full = true;
		}

		/*
		 * open_cursor can return EBUSY if concurrent with a metadata
		 * operation, retry in that case.
		 */
		while ((ret = session->open_cursor(
		    session, "backup:", NULL, config, &backup_cursor)) == EBUSY)
			__wt_yield();
		if (ret != 0)
			testutil_die(ret, "session.open_cursor: backup");

		while ((ret = backup_cursor->next(backup_cursor)) == 0) {
			testutil_check(
			    backup_cursor->get_key(backup_cursor, &key));
			copy_file(session, key);
		}
		if (ret != WT_NOTFOUND)
			testutil_die(ret, "backup-cursor");

		/* After an incremental backup, truncate the log files. */
		if (incremental)
			testutil_check(session->truncate(
			    session, "log:", backup_cursor, NULL, NULL));

		testutil_check(backup_cursor->close(backup_cursor));
		testutil_check(pthread_rwlock_unlock(&g.backup_lock));

		/*
		 * If automatic log archival isn't configured, optionally do
		 * incremental backups after each full backup. If we're not
		 * doing any more incrementals, verify the backup (we can't
		 * verify intermediate states, once we perform recovery on the
		 * backup database, we can't do any more incremental backups).
		 */
		if (full)
			incremental =
			    g.c_logging_archive ? 1 : mmrand(NULL, 1, 5);
		if (--incremental == 0)
			check_copy();
	}

	if (incremental != 0)
		check_copy();

	testutil_check(session->close(session, NULL));

	return (NULL);
}
