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

#include "thread.h"

void
obj_bulk(void)
{
	WT_CURSOR *c;
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			die(ret, "session.create");

	if (ret == 0) {
		if ((ret = session->open_cursor(
		    session, uri, NULL, "bulk", &c)) == 0) {
			/* Yield so that other threads can interfere. */
			sched_yield();
			if ((ret = c->close(c)) != 0)
				die(ret, "cursor.close");
		} else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
			die(ret, "session.open_cursor");
	}
	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_cursor(void)
{
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
		if (ret != ENOENT && ret != EBUSY)
			die(ret, "session.open_cursor");
	} else {
		if ((ret = cursor->close(cursor)) != 0)
			die(ret, "cursor.close");
	}
	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_create(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			die(ret, "session.create");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_drop(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret = session->drop(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die(ret, "session.drop");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_checkpoint(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	/* Force the checkpoint so it has to be taken. */
	if ((ret = session->checkpoint(session, "force")) != 0)
		if (ret != ENOENT)
			die(ret, "session.checkpoint");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_upgrade(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret = session->upgrade(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die(ret, "session.upgrade");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
obj_verify(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "conn.session");

	if ((ret = session->verify(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die(ret, "session.verify");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}
