/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "format.h"

static int  wts_close(WT_CONNECTION *);
static int  wts_open(WT_CONNECTION **, WT_SESSION **session);
static int  wts_read(uint64_t);
static int  wts_sync(void);

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	UNUSED(handler);

	if (g.logfp != NULL)
		fprintf(g.logfp, "%s\n", message);
	else
		printf("%s\n", message);
	return (0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
handle_progress(WT_EVENT_HANDLER *handler,
     const char *operation, uint64_t progress)
{
	UNUSED(handler);

	track(operation, progress);
	return (0);
}

static WT_EVENT_HANDLER event_handler = {
	NULL,
	handle_message,
	handle_progress
};

static int
wts_open(WT_CONNECTION **connp, WT_SESSION **sessionp)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;
	const char *ext1, *ext2;
	char config[256];

	/* If the bzip2 compression module has been built, use it. */
	ext1 = "../../ext/compressors/bzip2_compress/.libs/bzip2_compress.so";
	if (access(ext1, R_OK) != 0) {
		ext1 = "";
		g.c_bzip = 0;
	}
	ext2 = "../../ext/collators/reverse/.libs/reverse_collator.so";

	/*
	 * Open configuration -- put command line configuration options at the
	 * end so they can override "standard" configuration.
	 */
	snprintf(config, sizeof(config),
	    "create,error_prefix=\"%s\",cache_size=%" PRIu32 "MB,"
	    "extensions=[\"%s\",\"%s\"],%s",
	    g.progname, g.c_cache, ext1, ext2,
	    g.config_open == NULL ? "" : g.config_open);

	if ((ret = wiredtiger_open(NULL, &event_handler, config, &conn)) != 0) {
		fprintf(stderr, "%s: wiredtiger_open: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr, "%s: conn.session: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		(void)conn->close(conn, NULL);
		return (1);
	}

	*sessionp = session;
	*connp = conn;
	return (0);
}

static int
wts_close(WT_CONNECTION *conn)
{
	int ret;
	if ((ret = conn->close(conn, NULL)) != 0) {
		fprintf(stderr, "%s: conn.close: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	return (0);
}

int
wts_startup(int open_cursors)
{
	time_t now;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_SESSION *session;
	uint32_t maxintlpage, maxintlitem, maxleafpage, maxleafitem;
	int ret;
	char config[512], *end, *p;

	if (wts_open(&conn, &session))
		return (1);

	maxintlpage = 1U << g.c_intl_page_max;
	maxintlitem = MMRAND(maxintlpage / 50, maxintlpage / 40);
	if (maxintlitem < 40)
		maxintlitem = 40;
	maxleafpage = 1U << g.c_leaf_page_max;
	/* Make sure at least 3 leaf pages can fix in cache. */
	while (3 * maxleafpage > g.c_cache << 20)
		maxleafpage >>= 1;
	maxleafitem = MMRAND(maxleafpage / 50, maxleafpage / 40);
	if (maxleafitem < 40)
		maxleafitem = 40;

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "internal_page_max=%d,internal_item_max=%d,"
	    "leaf_page_max=%d,leaf_item_max=%d",
	    (g.c_file_type == ROW) ? "u" : "r",
	    maxintlpage, maxintlitem, maxleafpage, maxleafitem);

	if (g.c_bzip)
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"bzip2_compress\"");

	switch (g.c_file_type) {
	case FIX:
		p += snprintf(p, (size_t)(end - p),
		    ",value_format=%dt", g.c_bitcnt);
		break;
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_key=english");
		if (g.c_reverse)
			p += snprintf(p, (size_t)(end - p),
			    ",collator=reverse");
		/* FALLTHROUGH */
	case VAR:
		if (g.c_huffman_value)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_value=english");
		break;
	}

	if ((ret = session->create(session, WT_TABLENAME, config)) != 0) {
		fprintf(stderr, "%s: create table: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	cursor = cursor_insert = NULL;
	if (open_cursors) {
		/*
		 * We open two cursors: one configured for overwriting and one
		 * configured for append if we're dealing with a column-store.
		 *
		 * The reason is when testing with existing records, we don't
		 * track if a record was deleted or not, which means we need to
		 * use cursor->insert with overwriting configured.  But, in
		 * column-store files where we're testing with new, appended
		 * records, we don't want to have to specify the record number,
		 * which requires an append configuration.
		 */
		if ((ret = session->open_cursor(
		    session, WT_TABLENAME, NULL, "overwrite", &cursor)) != 0) {
			fprintf(stderr, "%s: open_cursor: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
		if ((g.c_file_type == FIX || g.c_file_type == VAR) &&
		    (ret = session->open_cursor(session,
		    WT_TABLENAME, NULL, "append", &cursor_insert)) != 0) {
			fprintf(stderr, "%s: open_cursor: %s\n",
			    g.progname, wiredtiger_strerror(ret));
			return (1);
		}
	}

	if (g.logging == LOG_OPS) {
		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nWT start: %s===============",
		    ctime(&now));
	}

	g.wts_conn = conn;
	g.wts_cursor = cursor;
	g.wts_cursor_insert = cursor_insert;
	g.wts_session = session;

	return (0);
}

int
wts_teardown(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *cursor_insert;
	WT_SESSION *session;
	time_t now;
	int ret;

	conn = g.wts_conn;
	cursor = g.wts_cursor;
	cursor_insert = g.wts_cursor_insert;
	session = g.wts_session;

	if (g.logging == LOG_OPS) {
		(void)time(&now);
		(void)session->msg_printf(session,
		    "===============\nWT stop: %s===============",
		    ctime(&now));
	}

	/*
	 * Close the open cursors -- they will block sync.
	 */
	if ((cursor_insert != NULL &&
	    (ret = cursor_insert->close(cursor_insert)) != 0) ||
	    (cursor != NULL && (ret = cursor->close(cursor)) != 0))
		die("cursor.close", ret);

	ret = wts_sync();
	return (wts_close(conn) ? 1 : ret);
}


int
wts_dump(const char *tag, int dump_bdb)
{
	char cmd[128];

	track("dump files and compare", 0ULL);
	switch (g.c_file_type) {
	case FIX:
	case VAR:
		snprintf(cmd, sizeof(cmd),
		    "sh ./s_dumpcmp%s -c", dump_bdb ? " -b" : "");
		break;
	case ROW:
		snprintf(cmd, sizeof(cmd),
		    "sh ./s_dumpcmp%s", dump_bdb ? " -b" : "");
		break;
	default:
		return (1);
	}
	if (system(cmd) != 0) {
		fprintf(stderr,
		    "%s: %s dump comparison failed\n", g.progname, tag);
		return (1);
	}

	return (0);
}

int
wts_salvage(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	track("salvage", 0ULL);

	if (wts_open(&conn, &session))
		return (1);

	if ((ret = session->salvage(session, WT_TABLENAME, NULL)) != 0) {
		fprintf(stderr, "%s: salvage: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}

	return (wts_close(conn));
}

static int
wts_sync(void)
{
	WT_SESSION *session;
	int ret;

	session = g.wts_session;

	track("sync", 0ULL);

	if ((ret = session->sync(
	    session, WT_TABLENAME, NULL)) != 0 && ret != EBUSY) {
		fprintf(stderr, "%s: sync: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	return (0);
}

int
wts_verify(const char *tag)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	track("verify", 0ULL);

	if (wts_open(&conn, &session))
		return (1);

	if ((ret = session->verify(session, WT_TABLENAME, NULL)) != 0)
		fprintf(stderr, "%s: %s verify: %s\n",
		    g.progname, tag, wiredtiger_strerror(ret));

	return (wts_close(conn) ? 1 : ret);
}

/*
 * wts_stats --
 *	Dump the run's statistics.
 */
int
wts_stats(void)
{
	WT_CURSOR *cursor;
	WT_SESSION *session;
	FILE *fp;
	const char *pval, *desc;
	uint64_t v;
	int ret;

	session = g.wts_session;

	track("stat", 0ULL);

	if ((fp = fopen("__stats", "w")) == NULL)
		die("__stats", errno);

	/* Connection statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: stat cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor)) != 0)
		die("cursor.close", ret);

	/* File statistics. */
	if ((ret = session->open_cursor(session,
	    "statistics:" WT_TABLENAME, NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "%s: stat cursor open failed: %s\n",
		    g.progname, wiredtiger_strerror(ret));
		return (1);
	}
	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0) {
			ret = errno;
			break;
		}

	if (ret != WT_NOTFOUND)
		die("cursor.next", ret);
	if ((ret = cursor->close(cursor)) != 0)
		die("cursor.close", ret);

	(void)fclose(fp);

	return (0);
}
