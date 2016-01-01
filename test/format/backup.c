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
	int ret;

	wts_open(g.home_backup, 0, &conn);

	if ((ret = conn->open_session(
	    conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session: %s", g.home_backup);

	ret = session->verify(session, g.uri, NULL);
	if (ret != 0)
		die(ret, "session.verify: %s: %s", g.home_backup, g.uri);

	if ((ret = conn->close(conn, NULL)) != 0)
		die(ret, "connection.close: %s", g.home_backup);
}

/*
 * copy_file --
 *	Copy a single file into the backup directory.
 */
static void
copy_file(const char *name)
{
	size_t len;
	char *cmd;
	int ret;

	len = strlen(g.home) + strlen(g.home_backup) + strlen(name) * 2 + 20;
	cmd = dmalloc(len);
	(void)snprintf(cmd, len,
	    "cp %s/%s %s/%s", g.home, name, g.home_backup, name);
	if ((ret = system(cmd)) != 0)
		die(ret, "backup copy: %s", cmd);
	free(cmd);
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
	WT_SESSION *session;
	u_int period;
	int ret;
	const char *key;

	(void)(arg);

	conn = g.wts_conn;

	/* Backups aren't supported for non-standard data sources. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		return (NULL);

	/* Open a session. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	/*
	 * Perform a backup at somewhere under 10 seconds (so we get at
	 * least one done), and then at 45 second intervals.
	 */
	for (period = mmrand(NULL, 1, 10);; period = 45) {
		/* Sleep for short periods so we don't make the run wait. */
		while (period > 0 && !g.workers_finished) {
			--period;
			sleep(1);
		}
		if (g.workers_finished)
			break;

		/* Lock out named checkpoints */
		if ((ret = pthread_rwlock_wrlock(&g.backup_lock)) != 0)
			die(ret, "pthread_rwlock_wrlock: backup lock");

		/* Re-create the backup directory. */
		if ((ret = system(g.home_backup_init)) != 0)
			die(ret, "backup directory creation failed");

		/*
		 * open_cursor can return EBUSY if a metadata operation is
		 * currently happening - retry in that case.
		 */
		while ((ret = session->open_cursor(session,
		    "backup:", NULL, NULL, &backup_cursor)) == EBUSY)
			sleep(1);
		if (ret != 0)
			die(ret, "session.open_cursor: backup");

		while ((ret = backup_cursor->next(backup_cursor)) == 0) {
			if ((ret =
			    backup_cursor->get_key(backup_cursor, &key)) != 0)
				die(ret, "cursor.get_key");
			copy_file(key);
		}

		if ((ret = backup_cursor->close(backup_cursor)) != 0)
			die(ret, "cursor.close");

		if ((ret = pthread_rwlock_unlock(&g.backup_lock)) != 0)
			die(ret, "pthread_rwlock_unlock: backup lock");

		check_copy();
	}

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	return (NULL);
}
