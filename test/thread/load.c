/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "thread.h"

void
load(void)
{
	WT_CURSOR *cursor;
	WT_ITEM *key, _key, *value, _value;
	WT_SESSION *session;
	char *p, *end, keybuf[64], valuebuf[64], config[128];
	u_int keyno;
	int ret;

	key = &_key;
	value = &_value;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die("conn.session", ret);

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "internal_node_max=%d,"
	    "leaf_node_max=%d,",
	    ftype == ROW ? "u" : "r", 16 * 1024, 128 * 1024);
	if (ftype == FIX)
		(void)snprintf(p, (size_t)(end - p), ",value_format=3t");

	if ((ret = session->create(session, FNAME, config)) != 0)
		die("session.create", ret);

	if ((ret =
	    session->open_cursor(session, FNAME, NULL, "bulk", &cursor)) != 0)
		die("cursor.open", ret);

	for (keyno = 0; keyno < nkeys; ++keyno) {
		if (ftype == ROW) {
			key->data = keybuf;
			key->size = (uint32_t)
			    snprintf(keybuf, sizeof(keybuf), "%017u", keyno);
			cursor->set_key(cursor, key);
		} else
			cursor->set_key(cursor, (uint32_t)keyno);
		value->data = valuebuf;
		if (ftype == FIX)
			cursor->set_value(cursor, 0x01);
		else {
			value->size = (uint32_t)
			    snprintf(valuebuf, sizeof(valuebuf), "%37u", keyno);
			cursor->set_value(cursor, value);
		}
		if ((ret = cursor->insert(cursor)) != 0)
			die("cursor.insert", ret);
	}

	if ((ret = session->close(session, NULL)) != 0)
		die("session.close", ret);
}
