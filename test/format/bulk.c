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

void
wts_load(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM key, value;
	WT_SESSION *session;
	uint8_t *keybuf, *valbuf;
	bool is_bulk;
	int ret;

	conn = g.wts_conn;
	keybuf = valbuf = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	if (g.logging != 0)
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== bulk load start ===============");

	/*
	 * No bulk load with data-sources.
	 *
	 * No bulk load with custom collators, the order of insertion will not
	 * match the collation order.
	 */
	is_bulk = true;
	if (DATASOURCE("kvsbdb") && DATASOURCE("helium"))
		is_bulk = false;
	if (g.c_reverse)
		is_bulk = false;

	if ((ret = session->open_cursor(session, g.uri, NULL,
	    is_bulk ? "bulk,append" : NULL, &cursor)) != 0)
		die(ret, "session.open_cursor");

	/* Set up the key/value buffers. */
	key_gen_setup(&keybuf);
	val_gen_setup(NULL, &valbuf);

	for (;;) {
		if (++g.key_cnt > g.c_rows) {
			g.key_cnt = g.rows = g.c_rows;
			break;
		}

		/* Report on progress every 100 inserts. */
		if (g.key_cnt % 100 == 0)
			track("bulk load", g.key_cnt, NULL);

		key_gen(keybuf, &key.size, (uint64_t)g.key_cnt);
		key.data = keybuf;
		val_gen(NULL, valbuf, &value.size, (uint64_t)g.key_cnt);
		value.data = valbuf;

		switch (g.type) {
		case FIX:
			if (!is_bulk)
				cursor->set_key(cursor, g.key_cnt);
			cursor->set_value(cursor, *(uint8_t *)value.data);
			if (g.logging == LOG_OPS)
				(void)g.wt_api->msg_printf(g.wt_api, session,
				    "%-10s %" PRIu32 " {0x%02" PRIx8 "}",
				    "bulk V",
				    g.key_cnt, ((uint8_t *)value.data)[0]);
			break;
		case VAR:
			if (!is_bulk)
				cursor->set_key(cursor, g.key_cnt);
			cursor->set_value(cursor, &value);
			if (g.logging == LOG_OPS)
				(void)g.wt_api->msg_printf(g.wt_api, session,
				    "%-10s %" PRIu32 " {%.*s}", "bulk V",
				    g.key_cnt,
				    (int)value.size, (char *)value.data);
			break;
		case ROW:
			cursor->set_key(cursor, &key);
			if (g.logging == LOG_OPS)
				(void)g.wt_api->msg_printf(g.wt_api, session,
				    "%-10s %" PRIu32 " {%.*s}", "bulk K",
				    g.key_cnt, (int)key.size, (char *)key.data);
			cursor->set_value(cursor, &value);
			if (g.logging == LOG_OPS)
				(void)g.wt_api->msg_printf(g.wt_api, session,
				    "%-10s %" PRIu32 " {%.*s}", "bulk V",
				    g.key_cnt,
				    (int)value.size, (char *)value.data);
			break;
		}

		if ((ret = cursor->insert(cursor)) != 0)
			die(ret, "cursor.insert");

#ifdef HAVE_BERKELEY_DB
		if (SINGLETHREADED)
			bdb_insert(key.data, key.size, value.data, value.size);
#endif
	}

	if ((ret = cursor->close(cursor)) != 0)
		die(ret, "cursor.close");

	if (g.logging != 0)
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== bulk load stop ===============");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");

	free(keybuf);
	free(valbuf);
}
