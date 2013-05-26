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

#include "format.h"

/*
 * check_copy --
 *	Confirm the hot backup worked.
 */
static void
check_copy(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	wts_open(RUNDIR_BACKUP, 0, &conn);

	/*
	 * Open a session and verify the store; some data-sources don't support
	 * verify.
	 *
	 * XXX
	 * LSM can deadlock if WT_SESSION methods are called at the wrong time,
	 * don't do that for now.
	 */
	if (!DATASOURCE("lsm") && !DATASOURCE("memrata")) {
		if ((ret = conn->open_session(
		    conn, NULL, NULL, &session)) != 0)
			die(ret, "connection.open_session");

		if ((ret = session->verify(session, g.uri, NULL)) != 0)
			die(ret, "session.verify: %s", g.uri);
	}

	if ((ret = conn->close(conn, NULL)) != 0)
		die(ret, "connection.close: %s", RUNDIR_BACKUP);
}

/*
 * hot_copy --
 *	Copy a single file into the hot backup directory.
 */
static void
hot_copy(const char *name)
{
	char buf[1024];

	(void)snprintf(
	    buf, sizeof(buf), "cp RUNDIR/%s RUNDIR/BACKUP/%s", name, name);
	if (system(buf) != 0)
		die(errno, "hot backup copy: %s", buf);
}

/*
 * hot_backup --
 *	Periodically do a hot backup and verify it.
 */
void *
hot_backup(void *arg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *backup_cursor;
	WT_SESSION *session;
	u_int period;
	int ret;
	const char *key;

	(void)arg;

	/* If hot backups aren't configured, we're done. */
	if (!g.c_hot_backups)
		return (NULL);

	/* Hot backups aren't supported for non-standard data sources. */
	if (DATASOURCE("kvsbdb") || DATASOURCE("memrata"))
		return (NULL);

	conn = g.wts_conn;

	/* Open a session. */
	if ((ret = conn->open_session(
	    conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	/*
	 * Perform a hot backup at somewhere under 10 seconds (so we get at
	 * least one done), and then at 45 second intervals.
	 */
	for (period = MMRAND(1, 10); !g.threads_finished; period = 45) {

		/* Sleep for a short period so we don't make the run wait. */
		if (period > 0) {
			--period;
			sleep(1);
			if (g.threads_finished)
				break;
		}

		/* Lock out named checkpoints */
		if ((ret = pthread_rwlock_wrlock(&g.backup_lock)) != 0)
			die(ret, "pthread_rwlock_wrlock: hot-backup lock");

		/* Re-create the backup directory. */
		(void)system("cd RUNDIR && rm -rf BACKUP");
		if (mkdir(RUNDIR_BACKUP, 0777) != 0)
			die(errno, "mkdir: %s", RUNDIR_BACKUP);

		if ((ret = session->open_cursor(session,
		    "backup:", NULL, NULL, &backup_cursor)) != 0)
			die(ret, "session.open_cursor: backup");

		while ((ret = backup_cursor->next(backup_cursor)) == 0) {
			if ((ret =
			    backup_cursor->get_key(backup_cursor, &key)) != 0)
				die(ret, "cursor.get_key");
			hot_copy(key);
		}

		if ((ret = backup_cursor->close(backup_cursor)) != 0)
			die(ret, "cursor.close");

		if ((ret = pthread_rwlock_unlock(&g.backup_lock)) != 0)
			die(ret, "pthread_rwlock_unlock: hot-backup lock");

		check_copy();
	}

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	return (NULL);
}
