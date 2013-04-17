/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

static int
handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	UNUSED(handler);

	if (g.logfp != NULL)
		return (fprintf(g.logfp, "%s\n", message) < 0 ? -1 : 0);

	return (printf("%s\n", message) < 0 ? -1 : 0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
handle_progress(
    WT_EVENT_HANDLER *handler, const char *operation, uint64_t progress)
{
	UNUSED(handler);

	track(operation, progress, NULL);
	return (0);
}

static WT_EVENT_HANDLER event_handler = {
	NULL,
	handle_message,
	handle_progress
};

void
wts_open(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	uint32_t maxintlpage, maxintlitem, maxleafpage, maxleafitem;
	int ret;
	char config[2048], *end, *p;

	/*
	 * Open configuration.
	 *
	 * Put configuration file configuration options second to last. Put
	 * command line configuration options at the end. Do this so they
	 * override the standard configuration.
	 */
	snprintf(config, sizeof(config),
	    "create,sync=false,cache_size=%" PRIu32 "MB,"
	    "error_prefix=\"%s\","
	    "extensions=[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"],%s,%s",
	    g.c_cache,
	    g.progname,
	    REVERSE_PATH,
	    access(BZIP_PATH, R_OK) == 0 ? BZIP_PATH : "",
	    access(LZO_PATH, R_OK) == 0 ? LZO_PATH : "",
	    (access(RAW_PATH, R_OK) == 0 &&
	    access(BZIP_PATH, R_OK) == 0) ? RAW_PATH : "",
	    access(SNAPPY_PATH, R_OK) == 0 ? SNAPPY_PATH : "",
	    g.c_config_open == NULL ? "" : g.c_config_open,
	    g.config_open == NULL ? "" : g.config_open);

	if ((ret =
	    wiredtiger_open("RUNDIR", &event_handler, config, &conn)) != 0)
		die(ret, "wiredtiger_open");
	g.wts_conn = conn;
						/* Load extension functions. */
	wt_api = conn->get_extension_api(conn);

	/* Open any underlying key/value store data-source. */
	if (DATASOURCE("kvsbdb"))
		wiredtiger_kvs_bdb_init(conn);
#if 0
	if (DATASOURCE("kvsstec"))
		wiredtiger_kvs_stec_init(conn);
#endif

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	/*
	 * Create the object.
	 *
	 * Make sure at least 2 internal page per thread can fit in cache.
	 */
	maxintlpage = 1U << g.c_intl_page_max;
	while (2 * g.c_threads * maxintlpage > g.c_cache << 20)
		maxintlpage >>= 1;
	maxintlitem = MMRAND(maxintlpage / 50, maxintlpage / 40);
	if (maxintlitem < 40)
		maxintlitem = 40;

	/* Make sure at least one leaf page per thread can fit in cache. */
	maxleafpage = 1U << g.c_leaf_page_max;
	while (g.c_threads * (maxintlpage + maxleafpage) > g.c_cache << 20)
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
	    (g.type == ROW) ? "u" : "r",
	    maxintlpage, maxintlitem, maxleafpage, maxleafitem);

	switch (g.type) {
	case FIX:
		p += snprintf(p, (size_t)(end - p),
		    ",value_format=%dt", g.c_bitcnt);
		break;
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_key=english");
		if (!g.c_prefix)
			p += snprintf(p, (size_t)(end - p),
			    ",prefix_compression=false");
		if (g.c_reverse)
			p += snprintf(p, (size_t)(end - p),
			    ",collator=reverse");
		/* FALLTHROUGH */
	case VAR:
		if (g.c_huffman_value)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_value=english");
		if (g.c_dictionary)
			p += snprintf(p, (size_t)(end - p),
			    ",dictionary=%d", MMRAND(123, 517));
		break;
	}

	/* Configure checksums (not configurable from the command line). */
	switch MMRAND(1, 10) {
	case 1:						/* 10% */
		p += snprintf(p, (size_t)(end - p), ",checksum=\"on\"");
		break;
	case 2:						/* 10% */
		p += snprintf(p, (size_t)(end - p), ",checksum=\"off\"");
		break;
	default:					/* 80% */
		p += snprintf(
		    p, (size_t)(end - p), ",checksum=\"uncompressed\"");
		break;
	}

	/* Configure compression. */
	switch (g.compression) {
	case COMPRESS_NONE:
		break;
	case COMPRESS_BZIP:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"bzip2\"");
		break;
	case COMPRESS_LZO:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"LZO1B-6\"");
		break;
	case COMPRESS_RAW:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"raw\"");
		break;
	case COMPRESS_SNAPPY:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"snappy\"");
		break;
	}

	/* Configure internal key truncation. */
	p += snprintf(
	    p, (size_t)(end - p), ",internal_key_truncate=%s",
	    g.c_internal_key_truncation ? "true" : "false");

	/* Configure Btree page key gap. */
	p += snprintf(p, (size_t)(end - p), ",key_gap=%u", g.c_key_gap);

	/* Configure Btree split page percentage. */
	p += snprintf(p, (size_t)(end - p), ",split_pct=%u", g.c_split_pct);

	/* Configure KVS devices. */
	if (DATASOURCE("kvsstec"))
		p += snprintf(
		    p, (size_t)(end - p), ",kvs_devices=[RUNDIR/KVS]");

	if ((ret = session->create(session, g.uri, config)) != 0)
		die(ret, "session.create: %s", g.uri);

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
wts_close()
{
	WT_CONNECTION *conn;
	int ret;

	conn = g.wts_conn;

	if (DATASOURCE("kvsbdb"))
		wiredtiger_kvs_bdb_close(conn);
#if 0
	if (DATASOURCE("kvsstec"))
		wiredtiger_kvs_stec_close(conn);
#endif
	if ((ret = conn->close(conn, NULL)) != 0)
		die(ret, "connection.close");
}

void
wts_dump(const char *tag, int dump_bdb)
{
	int offset, ret;
	char cmd[256];

	/* Data-sources don't support dump comparisons. */
	if (DATASOURCE("kvsbdb") || DATASOURCE("kvsstec"))
		return;

	track("dump files and compare", 0ULL, NULL);

	offset = snprintf(cmd, sizeof(cmd), "sh s_dumpcmp");
	if (dump_bdb)
		offset += snprintf(cmd + offset,
		    sizeof(cmd) - (size_t)offset, " -b");
	if (g.type == FIX || g.type == VAR)
		offset += snprintf(cmd + offset,
		    sizeof(cmd) - (size_t)offset, " -c");

	if (g.uri != NULL)
		offset += snprintf(cmd + offset,
		    sizeof(cmd) - (size_t)offset, " -n %s", g.uri);
	if ((ret = system(cmd)) != 0)
		die(ret, "%s: dump comparison failed", tag);
}

void
wts_salvage(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	/* Data-sources don't support salvage. */
	if (DATASOURCE("kvsbdb") || DATASOURCE("kvsstec"))
		return;

	conn = g.wts_conn;
	track("salvage", 0ULL, NULL);

	/*
	 * Save a copy of the interesting files so we can replay the salvage
	 * step as necessary.
	 */
	if ((ret = system(
	    "cd RUNDIR && "
	    "rm -rf slvg.copy && "
	    "mkdir slvg.copy && "
	    "cp WiredTiger* wt* slvg.copy/")) != 0)
		die(ret, "salvage copy step failed");

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");
	if ((ret = session->salvage(session, g.uri, NULL)) != 0)
		die(ret, "session.salvage: %s", g.uri);
	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
wts_verify(const char *tag)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;

	conn = g.wts_conn;
	track("verify", 0ULL, NULL);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");
	if (g.logging != 0)
		(void)wt_api->msg_printf(wt_api, session,
		    "=============== verify start ===============");
	if ((ret = session->verify(session, g.uri, NULL)) != 0)
		die(ret, "session.verify: %s: %s", g.uri, tag);
	if (g.logging != 0)
		(void)wt_api->msg_printf(wt_api, session,
		    "=============== verify stop ===============");
	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

/*
 * wts_stats --
 *	Dump the run's statistics.
 */
void
wts_stats(void)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	FILE *fp;
	char *stat_name;
	const char *pval, *desc;
	uint64_t v;
	int ret;

	/* Data-sources don't support statistics. */
	if (DATASOURCE("kvsbdb") || DATASOURCE("kvsstec"))
		return;

	conn = g.wts_conn;
	track("stat", 0ULL, NULL);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	if ((fp = fopen("RUNDIR/stats", "w")) == NULL)
		die(errno, "fopen: RUNDIR/stats");

	/* Connection statistics. */
	fprintf(fp, "====== Connection statistics:\n");
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0)
		die(ret, "session.open_cursor");

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0)
			die(errno, "fprintf");

	if (ret != WT_NOTFOUND)
		die(ret, "cursor.next");
	if ((ret = cursor->close(cursor)) != 0)
		die(ret, "cursor.close");

	/* Data source statistics. */
	fprintf(fp, "\n\n====== Data source statistics:\n");
	if ((stat_name =
	    malloc(strlen("statistics:") + strlen(g.uri) + 1)) == NULL)
		syserr("malloc");
	sprintf(stat_name, "statistics:%s", g.uri);
	if ((ret = session->open_cursor(
	    session, stat_name, NULL, NULL, &cursor)) != 0)
		die(ret, "session.open_cursor");
	free(stat_name);

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0)
			die(errno, "fprintf");

	if (ret != WT_NOTFOUND)
		die(ret, "cursor.next");
	if ((ret = cursor->close(cursor)) != 0)
		die(ret, "cursor.close");

	if ((ret = fclose(fp)) != 0)
		die(ret, "fclose");

	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}
