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

#include "thread.h"

static u_int uid = 1;

void
obj_bulk(void)
{
	WT_CURSOR *c;
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			testutil_die(ret, "session.create");

	if (ret == 0) {
		__wt_yield();
		if ((ret = session->open_cursor(
		    session, uri, NULL, "bulk", &c)) == 0) {
			if ((ret = c->close(c)) != 0)
				testutil_die(ret, "cursor.close");
		} else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
			testutil_die(ret, "session.open_cursor");
	}
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_bulk_unique(int force)
{
	WT_CURSOR *c;
	WT_SESSION *session;
	int ret;
	char new_uri[64];

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	/* Generate a unique object name. */
	if ((ret = pthread_rwlock_wrlock(&single)) != 0)
		testutil_die(ret, "pthread_rwlock_wrlock single");
	(void)snprintf(new_uri, sizeof(new_uri), "%s.%u", uri, ++uid);
	if ((ret = pthread_rwlock_unlock(&single)) != 0)
		testutil_die(ret, "pthread_rwlock_unlock single");

	if ((ret = session->create(session, new_uri, config)) != 0)
		testutil_die(ret, "session.create: %s", new_uri);

	__wt_yield();
	if ((ret =
	    session->open_cursor(session, new_uri, NULL, "bulk", &c)) != 0)
		testutil_die(ret, "session.open_cursor: %s", new_uri);

	if ((ret = c->close(c)) != 0)
		testutil_die(ret, "cursor.close");

	while ((ret = session->drop(
	    session, new_uri, force ? "force" : NULL)) != 0)
		if (ret != EBUSY)
			testutil_die(ret, "session.drop: %s", new_uri);

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_cursor(void)
{
	WT_SESSION *session;
	WT_CURSOR *cursor;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0) {
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.open_cursor");
	} else {
		if ((ret = cursor->close(cursor)) != 0)
			testutil_die(ret, "cursor.close");
	}
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_create(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			testutil_die(ret, "session.create");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_create_unique(int force)
{
	WT_SESSION *session;
	int ret;
	char new_uri[64];

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	/* Generate a unique object name. */
	if ((ret = pthread_rwlock_wrlock(&single)) != 0)
		testutil_die(ret, "pthread_rwlock_wrlock single");
	(void)snprintf(new_uri, sizeof(new_uri), "%s.%u", uri, ++uid);
	if ((ret = pthread_rwlock_unlock(&single)) != 0)
		testutil_die(ret, "pthread_rwlock_unlock single");

	if ((ret = session->create(session, new_uri, config)) != 0)
		testutil_die(ret, "session.create");

	__wt_yield();
	while ((ret = session->drop(
	    session, new_uri, force ? "force" : NULL)) != 0)
		if (ret != EBUSY)
			testutil_die(ret, "session.drop: %s", new_uri);

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_drop(int force)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->drop(session, uri, force ? "force" : NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.drop");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_checkpoint(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	/* Force the checkpoint so it has to be taken. */
	if ((ret = session->checkpoint(session, "force")) != 0)
		if (ret != ENOENT)
			testutil_die(ret, "session.checkpoint");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_rebalance(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->rebalance(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.rebalance");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_upgrade(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->upgrade(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.upgrade");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
obj_verify(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->verify(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			testutil_die(ret, "session.verify");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}
