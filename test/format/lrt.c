/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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
 * lrt --
 *	Start a long-running transaction.
 */
void *
lrt(void *arg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	u_int period;
	int pinned, ret;

	(void)(arg);

	/* Open a session and cursor. */
	conn = g.wts_conn;
	if ((ret = conn->open_session(
	    conn, NULL, "isolation=snapshot", &session)) != 0)
		die(ret, "connection.open_session");
	if ((ret = session->open_cursor(
	    session, g.uri, NULL, NULL, &cursor)) != 0)
		die(ret, "session.open_cursor");

	for (pinned = 0;;) {
		/*
		 * If we have an open cursor, reset it, releasing our pin, else
		 * position the cursor, creating a snapshot.
		 */
		if (pinned) {
			if ((ret = cursor->reset(cursor)) != 0)
				die(ret, "cursor.reset");
			pinned = 0;
		} else {
			if ((ret = cursor->next(cursor)) != 0)
				die(ret, "cursor.reset");
			pinned = 1;
		}

		/* Sleep for some number of seconds. */
		period = mmrand(NULL, 1, 10);

		/* Sleep for short periods so we don't make the run wait. */
		while (period > 0 && !g.workers_finished) {
			--period;
			sleep(1);
		}
		if (g.workers_finished)
			break;
	}

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	return (NULL);
}
