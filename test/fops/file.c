/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "thread.h"

void
obj_create(void)
{
	WT_SESSION *session;
	int ret;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	if ((ret = session->create(session, uri, NULL)) != 0)
		if (ret != EEXIST)
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
		if (ret != ENOENT)
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
		if (ret != ENOENT)
			die("session.verify", ret);

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}
