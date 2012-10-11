/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

void
obj_bulk(void)
{
	WT_CURSOR *c;
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			die("session.create", ret);

	if (ret == 0) {
		if ((ret = session->open_cursor(
		    session, uri, NULL, "bulk", &c)) == 0) {
			/* Yield so that other threads can interfere. */
			sched_yield();
			if ((ret = c->close(c)) != 0)
				die("cursor.close", ret);
		} else if (ret != ENOENT && ret != EBUSY && ret != EINVAL)
			die("session.open_cursor", ret);
	}
	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
obj_create(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->create(session, uri, config)) != 0)
		if (ret != EEXIST && ret != EBUSY)
			die("session.create", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
obj_drop(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->drop(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die("session.drop", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
obj_checkpoint(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	/*
	 * Name the checkpoint so the checkpoint has to be taken, don't specify
	 * a target, it might not exist.
	 */
	if ((ret = session->checkpoint(session, "name=fops")) != 0)
		if (ret != ENOENT)
			die("session.checkpoint", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
obj_upgrade(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->upgrade(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die("session.upgrade", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
obj_verify(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->verify(session, uri, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die("session.verify", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}
