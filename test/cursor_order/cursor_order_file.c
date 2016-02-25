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

#include "cursor_order.h"

static void
file_create(SHARED_CONFIG *cfg, const char *name)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;
	char *p, *end, config[128];

	conn = cfg->conn;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "internal_page_max=%d,"
	    "split_deepen_min_child=200,"
	    "leaf_page_max=%d,",
	    cfg->ftype == ROW ? "S" : "r", 16 * 1024, 128 * 1024);
	if (cfg->ftype == FIX)
		(void)snprintf(p, (size_t)(end - p), ",value_format=3t");

	if ((ret = session->create(session, name, config)) != 0)
		if (ret != EEXIST)
			testutil_die(ret, "session.create");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
load(SHARED_CONFIG *cfg, const char *name)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM *value, _value;
	WT_SESSION *session;
	char keybuf[64], valuebuf[64];
	int64_t keyno;
	int ret;

	conn = cfg->conn;

	file_create(cfg, name);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret =
	    session->open_cursor(session, name, NULL, "bulk", &cursor)) != 0)
		testutil_die(ret, "cursor.open");

	value = &_value;
	for (keyno = 1; keyno <= (int64_t)cfg->nkeys; ++keyno) {
		if (cfg->ftype == ROW) {
			snprintf(keybuf, sizeof(keybuf), "%016u", (u_int)keyno);
			cursor->set_key(cursor, keybuf);
		} else
			cursor->set_key(cursor, (uint32_t)keyno);
		value->data = valuebuf;
		if (cfg->ftype == FIX)
			cursor->set_value(cursor, 0x01);
		else {
			value->size = (uint32_t)snprintf(
			    valuebuf, sizeof(valuebuf), "%37u", (u_int)keyno);
			cursor->set_value(cursor, value);
		}
		if ((ret = cursor->insert(cursor)) != 0)
			testutil_die(ret, "cursor.insert");
	}

	/* Setup the starting key range for the workload phase. */
	cfg->key_range = cfg->nkeys;
	if ((ret = cursor->close(cursor)) != 0)
		testutil_die(ret, "cursor.close");
	if ((ret = session->checkpoint(session, NULL)) != 0)
		testutil_die(ret, "session.checkpoint");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}

void
verify(SHARED_CONFIG *cfg, const char *name)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	conn = cfg->conn;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "conn.session");

	if ((ret = session->verify(session, name, NULL)) != 0)
		testutil_die(ret, "session.create");

	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "session.close");
}
