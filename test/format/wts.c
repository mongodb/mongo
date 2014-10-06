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

#include "format.h"

static int
handle_message(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *message)
{
	(void)(handler);
	(void)(session);

	if (g.logfp != NULL)
		return (fprintf(
		    g.logfp, "%p:%s\n", session, message) < 0 ? -1 : 0);

	return (printf("%p:%s\n", session, message) < 0 ? -1 : 0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
handle_progress(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *operation, uint64_t progress)
{
	(void)(handler);
	(void)(session);

	track(operation, progress, NULL);
	return (0);
}

static WT_EVENT_HANDLER event_handler = {
	NULL,
	handle_message,
	handle_progress,
	NULL	/* Close handler. */
};

/*
 * wts_open --
 *	Open a connection to a WiredTiger database.
 */
void
wts_open(const char *home, int set_api, WT_CONNECTION **connp)
{
	WT_CONNECTION *conn;
	int ret;
	char config[2048], evict_config[64];

	*connp = NULL;

	/* Build the eviction worker thread config string, if needed. */
	evict_config[0] = '\0';
	if (g.c_evict_max != 0 &&
	    snprintf(evict_config, sizeof(evict_config),
	    "eviction=(threads_max=%" PRIu32 "),",
	    g.c_evict_max) >= (int)sizeof(evict_config))
		die(ENOMEM, "eviction configuration buffer too small");

	/*
	 * Put configuration file configuration options second to last. Put
	 * command line configuration options at the end. Do this so they
	 * override the standard configuration.
	 */
	if (snprintf(config, sizeof(config),
	    "create,"
	    "checkpoint_sync=false,cache_size=%" PRIu32 "MB,"
	    "buffer_alignment=512,lsm_manager=(worker_thread_max=%" PRIu32
	    "),error_prefix=\"%s\","
	    "%s,%s,%s,%s,%s"
	    "extensions="
	    "[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"],"
	    "%s,%s",
	    g.c_cache,
	    g.c_lsm_worker_threads,
	    g.progname,
	    g.c_data_extend ? "file_extend=(data=8MB)" : "",
	    g.c_logging ? "log=(enabled=true)" : "",
	    g.c_mmap ? "mmap=true" : "mmap=false",
	    g.c_statistics ? "statistics=(fast)" : "statistics=(none)",
	    evict_config,
	    g.c_reverse ? REVERSE_PATH : "",
	    access(BZIP_PATH, R_OK) == 0 ? BZIP_PATH : "",
	    access(LZO_PATH, R_OK) == 0 ? LZO_PATH : "",
	    access(SNAPPY_PATH, R_OK) == 0 ? SNAPPY_PATH : "",
	    access(ZLIB_PATH, R_OK) == 0 ? ZLIB_PATH : "",
	    DATASOURCE("kvsbdb") ? KVS_BDB_PATH : "",
	    g.c_config_open == NULL ? "" : g.c_config_open,
	    g.config_open == NULL ? "" : g.config_open) >= (int)sizeof(config))
		die(ENOMEM, "configuration buffer too small");

	/*
	 * Direct I/O may not work with backups, doing copies through the buffer
	 * cache after configuring direct I/O in Linux won't work.  If direct
	 * I/O is configured, turn off backups.   This isn't a great place to do
	 * this check, but it's only here we have the configuration string.
	 */
	if (strstr(config, "direct_io") != NULL)
		g.c_backups = 0;

	if ((ret = wiredtiger_open(home, &event_handler, config, &conn)) != 0)
		die(ret, "wiredtiger_open: %s", home);

	if (set_api)
		g.wt_api = conn->get_extension_api(conn);

	/*
	 * Load the Helium shared library: it would be possible to do this as
	 * part of the extensions configured for wiredtiger_open, there's no
	 * difference, I am doing it here because it's easier to work with the
	 * configuration strings.
	 */
	if (DATASOURCE("helium")) {
		if (g.helium_mount == NULL)
			die(EINVAL, "no Helium mount point specified");
		(void)snprintf(config, sizeof(config),
		    "entry=wiredtiger_extension_init,config=["
		    "helium_verbose=0,"
		    "dev1=[helium_devices=\"he://./%s\","
		    "helium_o_volume_truncate=1]]",
		    g.helium_mount);
		if ((ret =
		    conn->load_extension(conn, HELIUM_PATH, config)) != 0)
			die(ret,
			   "WT_CONNECTION.load_extension: %s:%s",
			   HELIUM_PATH, config);
	}
	*connp = conn;
}

/*
 * wts_create --
 *	Create the underlying store.
 */
void
wts_create(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	uint32_t maxintlpage, maxintlitem, maxleafpage, maxleafitem;
	int ret;
	char config[4096], *end, *p;

	conn = g.wts_conn;

	/*
	 * Ensure that we can service at least one operation per-thread
	 * concurrently without filling the cache with pinned pages. We
	 * choose a multiplier of three because the max configurations control
	 * on disk size and in memory pages are often significantly larger
	 * than their disk counterparts.
	 */
	maxintlpage = 1U << g.c_intl_page_max;
	maxleafpage = 1U << g.c_leaf_page_max;
	while (3 * g.c_threads * (maxintlpage + maxleafpage) >
	    g.c_cache << 20) {
		if (maxleafpage <= 512 && maxintlpage <= 512)
			break;
		if (maxintlpage > 512)
			maxintlpage >>= 1;
		if (maxleafpage > 512)
			maxleafpage >>= 1;
	}
	maxintlitem = MMRAND(maxintlpage / 50, maxintlpage / 40);
	if (maxintlitem < 40)
		maxintlitem = 40;
	maxleafitem = MMRAND(maxleafpage / 50, maxleafpage / 40);
	if (maxleafitem < 40)
		maxleafitem = 40;

	p = config;
	end = config + sizeof(config);
	p += snprintf(p, (size_t)(end - p),
	    "key_format=%s,"
	    "allocation_size=512,%s"
	    "internal_page_max=%d,internal_item_max=%d,"
	    "leaf_page_max=%d,leaf_item_max=%d",
	    (g.type == ROW) ? "u" : "r",
	    g.c_firstfit ? "block_allocation=first," : "",
	    maxintlpage, maxintlitem, maxleafpage, maxleafitem);

	switch (g.type) {
	case FIX:
		p += snprintf(p, (size_t)(end - p),
		    ",value_format=%" PRIu32 "t", g.c_bitcnt);
		break;
	case ROW:
		if (g.c_huffman_key)
			p += snprintf(p, (size_t)(end - p),
			    ",huffman_key=english");
		if (g.c_prefix_compression)
			p += snprintf(p, (size_t)(end - p),
			    ",prefix_compression_min=%" PRIu32,
			    g.c_prefix_compression_min);
		else
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

	/* Configure checksums. */
	switch (g.c_checksum_flag) {
	case CHECKSUM_OFF:
		p += snprintf(p, (size_t)(end - p), ",checksum=\"off\"");
		break;
	case CHECKSUM_ON:
		p += snprintf(p, (size_t)(end - p), ",checksum=\"on\"");
		break;
	case CHECKSUM_UNCOMPRESSED:
		p += snprintf(
		    p, (size_t)(end - p), ",checksum=\"uncompressed\"");
		break;
	}

	/* Configure compression. */
	switch (g.c_compression_flag) {
	case COMPRESS_NONE:
		break;
	case COMPRESS_BZIP:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"bzip2\"");
		break;
	case COMPRESS_BZIP_RAW:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"bzip2-raw-test\"");
		break;
	case COMPRESS_LZO:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"LZO1B-6\"");
		break;
	case COMPRESS_SNAPPY:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"snappy\"");
		break;
	case COMPRESS_ZLIB:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"zlib\"");
		break;
	case COMPRESS_ZLIB_NO_RAW:
		p += snprintf(p, (size_t)(end - p),
		    ",block_compressor=\"zlib-noraw\"");
		break;
	}

	/* Configure Btree internal key truncation. */
	p += snprintf(
	    p, (size_t)(end - p), ",internal_key_truncate=%s",
	    g.c_internal_key_truncation ? "true" : "false");

	/* Configure Btree page key gap. */
	p += snprintf(p, (size_t)(end - p), ",key_gap=%" PRIu32, g.c_key_gap);

	/* Configure Btree split page percentage. */
	p += snprintf(p, (size_t)(end - p),
	    ",split_pct=%" PRIu32, g.c_split_pct);

	/* Configure LSM and data-sources. */
	if (DATASOURCE("helium"))
		p += snprintf(p, (size_t)(end - p),
		    ",type=helium,helium_o_compress=%d,helium_o_truncate=1",
		    g.c_compression_flag == COMPRESS_NONE ? 0 : 1);

	if (DATASOURCE("kvsbdb"))
		p += snprintf(p, (size_t)(end - p), ",type=kvsbdb");

	if (DATASOURCE("lsm")) {
		p += snprintf(p, (size_t)(end - p), ",type=lsm,lsm=(");
		p += snprintf(p, (size_t)(end - p),
		    "auto_throttle=%s,", g.c_auto_throttle ? "true" : "false");
		p += snprintf(p, (size_t)(end - p),
		    "chunk_size=%" PRIu32 "MB,", g.c_chunk_size);
		/*
		 * We can't set bloom_oldest without bloom, and we want to test
		 * with Bloom filters on most of the time anyway.
		 */
		if (g.c_bloom_oldest)
			g.c_bloom = 1;
		p += snprintf(p, (size_t)(end - p),
		    "bloom=%s,", g.c_bloom ? "true" : "false");
		p += snprintf(p, (size_t)(end - p),
		    "bloom_bit_count=%" PRIu32 ",", g.c_bloom_bit_count);
		p += snprintf(p, (size_t)(end - p),
		    "bloom_hash_count=%" PRIu32 ",", g.c_bloom_hash_count);
		p += snprintf(p, (size_t)(end - p),
		    "bloom_oldest=%s,", g.c_bloom_oldest ? "true" : "false");
		p += snprintf(p, (size_t)(end - p),
		    "merge_max=%" PRIu32 ",", g.c_merge_max);
		p += snprintf(p, (size_t)(end - p), ",)");
	}

	/*
	 * Create the underlying store.
	 */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");
	if ((ret = session->create(session, g.uri, config)) != 0)
		die(ret, "session.create: %s", g.uri);
	if ((ret = session->close(session, NULL)) != 0)
		die(ret, "session.close");
}

void
wts_close(void)
{
	WT_CONNECTION *conn;
	int ret;
	const char *config;

	conn = g.wts_conn;

	config = g.c_leak_memory ? "leak_memory" : NULL;

	if ((ret = conn->close(conn, config)) != 0)
		die(ret, "connection.close");
}

void
wts_dump(const char *tag, int dump_bdb)
{
	size_t len;
	int ret;
	char *cmd;

	/* Some data-sources don't support dump through the wt utility. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		return;

	track("dump files and compare", 0ULL, NULL);

	len = strlen(g.home) + strlen(BERKELEY_DB_PATH) + strlen(g.uri) + 100;
	if ((cmd = malloc(len)) == NULL)
		die(errno, "malloc");
	(void)snprintf(cmd, len,
	    "sh s_dumpcmp -h %s %s %s %s %s %s",
	    g.home,
	    dump_bdb ? "-b " : "",
	    dump_bdb ? BERKELEY_DB_PATH : "",
	    g.type == FIX || g.type == VAR ? "-c" : "",
	    g.uri == NULL ? "" : "-n",
	    g.uri == NULL ? "" : g.uri);

	if ((ret = system(cmd)) != 0)
		die(ret, "%s: dump comparison failed", tag);
	free(cmd);
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
		(void)g.wt_api->msg_printf(g.wt_api, session,
		    "=============== verify start ===============");

	/* Session operations for LSM can return EBUSY. */
	ret = session->verify(session, g.uri, NULL);
	if (ret != 0 && !(ret == EBUSY && DATASOURCE("lsm")))
		die(ret, "session.verify: %s: %s", g.uri, tag);

	if (g.logging != 0)
		(void)g.wt_api->msg_printf(g.wt_api, session,
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

	/* Ignore statistics if they're not configured. */
	if (g.c_statistics == 0)
		return;

	/* Some data-sources don't support statistics. */
	if (DATASOURCE("helium") || DATASOURCE("kvsbdb"))
		return;

	conn = g.wts_conn;
	track("stat", 0ULL, NULL);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		die(ret, "connection.open_session");

	if ((fp = fopen(g.home_stats, "w")) == NULL)
		die(errno, "fopen: %s", g.home_stats);

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
		die(errno, "malloc");
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
