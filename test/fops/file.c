/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

void
file_create(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->create(session, FNAME, NULL)) != 0)
		if (ret != EEXIST)
			die("session.create", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
file_drop(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->drop(session, FNAME, NULL)) != 0)
		if (ret != ENOENT && ret != EBUSY)
			die("session.drop", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
file_sync(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->sync(session, FNAME, NULL)) != 0)
		if (ret != ENOENT)
			die("session.sync", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
file_upgrade(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->upgrade(session, FNAME, NULL)) != 0)
		if (ret != ENOENT)
			die("session.upgrade", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}

void
file_verify(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->verify(session, FNAME, NULL)) != 0)
		if (ret != ENOENT)
			die("session.verify", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}
