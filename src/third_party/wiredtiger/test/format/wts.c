/*-
 * Public Domain 2014-present MongoDB, Inc.
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

static void create_object(TABLE *, void *);

/*
 * encryptor --
 *     Configure encryption.
 */
static const char *
encryptor(void)
{
    const char *p, *s;

    s = GVS(DISK_ENCRYPTION);
    if (strcmp(s, "off") == 0)
        p = "none";
    else if (strcmp(s, "rotn-7") == 0)
        p = "rotn,keyid=7";
    else if (strcmp(s, "sodium") == 0)
        p = "sodium,secretkey=" SODIUM_TESTKEY;
    else
        testutil_die(EINVAL, "illegal encryption configuration: %s", s);

    /* Returns "none" or the name of an encryptor. */
    return (p);
}

/*
 * encryptor_at_open --
 *     Configure encryption for wts_open().
 *
 * This must set any secretkey. When keyids are in use it can return NULL.
 */
static const char *
encryptor_at_open(void)
{
    const char *p, *s;

    s = GVS(DISK_ENCRYPTION);
    if (strcmp(s, "off") == 0)
        p = NULL;
    else if (strcmp(s, "rotn-7") == 0)
        p = NULL;
    else if (strcmp(s, "sodium") == 0)
        p = "sodium,secretkey=" SODIUM_TESTKEY;
    else
        testutil_die(EINVAL, "illegal encryption configuration: %s", s);

    /* Returns NULL or the name of an encryptor. */
    return (p);
}

/*
 * handle_message --
 *     Event handler for verbose and error messages.
 */
static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    WT_DECL_RET;
    int nw;

    (void)handler;
    (void)session;

    /*
     * WiredTiger logs a verbose message when the read timestamp is set to a value older than the
     * oldest timestamp. Ignore the message, it happens when repeating operations to confirm
     * timestamped values don't change underneath us.
     */
    if (strstr(message, "less than the oldest timestamp") != NULL)
        return (0);

    /* Write and flush the message so we're up-to-date on error. */
    nw = printf("%p:%s\n", (void *)session, message);
    ret = fflush(stdout);
    return (nw < 0 ? EIO : (ret == EOF ? errno : 0));
}

/*
 * handle_progress --
 *     Event handler for progress messages.
 */
static int
handle_progress(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *operation, uint64_t progress)
{
    char buf[256];

    (void)handler;
    (void)session;

    if (session->app_private == NULL)
        track(operation, progress);
    else {
        testutil_check(
          __wt_snprintf(buf, sizeof(buf), "%s %s", (char *)session->app_private, operation));
        track(buf, progress);
    }
    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  NULL, handle_message, handle_progress, NULL /* Close handler. */
};

#define CONFIG_APPEND(p, ...)                                               \
    do {                                                                    \
        size_t __len;                                                       \
        testutil_check(__wt_snprintf_len_set(p, max, &__len, __VA_ARGS__)); \
        if (__len > max)                                                    \
            __len = max;                                                    \
        p += __len;                                                         \
        max -= __len;                                                       \
    } while (0)

/*
 * configure_timing_stress --
 *     Configure stressing settings.
 */
static void
configure_timing_stress(char *p, size_t max)
{
    CONFIG_APPEND(p, ",timing_stress_for_test=[");
    if (GV(STRESS_AGGRESSIVE_SWEEP))
        CONFIG_APPEND(p, ",aggressive_sweep");
    if (GV(STRESS_CHECKPOINT))
        CONFIG_APPEND(p, ",checkpoint_slow");
    if (GV(STRESS_CHECKPOINT_PREPARE))
        CONFIG_APPEND(p, ",prepare_checkpoint_delay");
    if (GV(STRESS_CHECKPOINT_RESERVED_TXNID_DELAY))
        CONFIG_APPEND(p, ",checkpoint_reserved_txnid_delay");
    if (GV(STRESS_FAILPOINT_HS_DELETE_KEY_FROM_TS))
        CONFIG_APPEND(p, ",failpoint_history_store_delete_key_from_ts");
    if (GV(STRESS_HS_CHECKPOINT_DELAY))
        CONFIG_APPEND(p, ",history_store_checkpoint_delay");
    if (GV(STRESS_HS_SEARCH))
        CONFIG_APPEND(p, ",history_store_search");
    if (GV(STRESS_HS_SWEEP))
        CONFIG_APPEND(p, ",history_store_sweep_race");
    if (GV(STRESS_SPLIT_1))
        CONFIG_APPEND(p, ",split_1");
    if (GV(STRESS_SPLIT_2))
        CONFIG_APPEND(p, ",split_2");
    if (GV(STRESS_SPLIT_3))
        CONFIG_APPEND(p, ",split_3");
    if (GV(STRESS_SPLIT_4))
        CONFIG_APPEND(p, ",split_4");
    if (GV(STRESS_SPLIT_5))
        CONFIG_APPEND(p, ",split_5");
    if (GV(STRESS_SPLIT_6))
        CONFIG_APPEND(p, ",split_6");
    if (GV(STRESS_SPLIT_7))
        CONFIG_APPEND(p, ",split_7");
    CONFIG_APPEND(p, "]");
}

/*
 * create_database --
 *     Create a WiredTiger database.
 */
void
create_database(const char *home, WT_CONNECTION **connp)
{
    WT_CONNECTION *conn;
    size_t max;
    char config[8 * 1024], *p;
    const char *s;

    p = config;
    max = sizeof(config);

    CONFIG_APPEND(p,
      "create=true"
      ",cache_size=%" PRIu32
      "MB"
      ",checkpoint_sync=false"
      ",error_prefix=\"%s\""
      ",operation_timeout_ms=2000",
      GV(CACHE), progname);

    /* In-memory configuration. */
    if (GV(RUNS_IN_MEMORY) != 0)
        CONFIG_APPEND(p, ",in_memory=1");

        /* FIXME WT-8314: configuring a block cache corrupts tables. */
#if 0
    /* Block cache configuration. */
    if (GV(BLOCK_CACHE) != 0)
        CONFIG_APPEND(p,
          ",block_cache=(enabled=true,type=\"dram\""
          ",cache_on_checkpoint=%s"
          ",cache_on_writes=%s"
          ",size=%" PRIu32 "MB)",
          GV(BLOCK_CACHE_CACHE_ON_CHECKPOINT) == 0 ? "false" : "true",
          GV(BLOCK_CACHE_CACHE_ON_WRITES) == 0 ? "false" : "true", GV(BLOCK_CACHE_SIZE));
#endif

    /* LSM configuration. */
    if (g.lsm_config)
        CONFIG_APPEND(p, ",lsm_manager=(worker_thread_max=%" PRIu32 "),", GV(LSM_WORKER_THREADS));

    if (g.lsm_config || GV(CACHE) < 20)
        CONFIG_APPEND(p, ",eviction_dirty_trigger=95");

    /* Eviction configuration. */
    if (GV(CACHE_EVICT_MAX) != 0)
        CONFIG_APPEND(p, ",eviction=(threads_max=%" PRIu32 ")", GV(CACHE_EVICT_MAX));

    /* Logging configuration. */
    if (GV(LOGGING)) {
        s = GVS(LOGGING_COMPRESSION);
        CONFIG_APPEND(p,
          ",log=(enabled=true,archive=%d,prealloc=%d,file_max=%" PRIu32 ",compressor=\"%s\")",
          GV(LOGGING_ARCHIVE) ? 1 : 0, GV(LOGGING_PREALLOC) ? 1 : 0, KILOBYTE(GV(LOGGING_FILE_MAX)),
          strcmp(s, "off") == 0 ? "none" : s);
    }

    /* Encryption. */
    CONFIG_APPEND(p, ",encryption=(name=%s)", encryptor());

/* Miscellaneous. */
#ifdef HAVE_POSIX_MEMALIGN
    CONFIG_APPEND(p, ",buffer_alignment=512");
#endif

    if (GV(DISK_MMAP))
        CONFIG_APPEND(p, ",mmap=1");
    if (GV(DISK_MMAP_ALL))
        CONFIG_APPEND(p, ",mmap_all=1");

    if (GV(DISK_DIRECT_IO))
        CONFIG_APPEND(p, ",direct_io=(data)");

    if (GV(DISK_DATA_EXTEND))
        CONFIG_APPEND(p, ",file_extend=(data=8MB)");

    /*
     * Run the statistics server and/or maintain statistics in the engine. Sometimes specify a set
     * of sources just to exercise that code.
     */
    if (GV(STATISTICS_SERVER)) {
        if (mmrand(NULL, 0, 20) == 1)
            CONFIG_APPEND(
              p, ",statistics=(fast),statistics_log=(json,on_close,wait=5,sources=(\"file:\"))");
        else
            CONFIG_APPEND(p, ",statistics=(fast),statistics_log=(json,on_close,wait=5)");
    } else
        CONFIG_APPEND(p, ",statistics=(%s)", GV(STATISTICS) ? "fast" : "none");

    /* Optional timing stress. */
    configure_timing_stress(p, max);

    /* Extensions. */
    CONFIG_APPEND(p, ",extensions=[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"],",
      REVERSE_PATH, access(LZ4_PATH, R_OK) == 0 ? LZ4_PATH : "",
      access(ROTN_PATH, R_OK) == 0 ? ROTN_PATH : "",
      access(SNAPPY_PATH, R_OK) == 0 ? SNAPPY_PATH : "",
      access(ZLIB_PATH, R_OK) == 0 ? ZLIB_PATH : "", access(ZSTD_PATH, R_OK) == 0 ? ZSTD_PATH : "",
      access(SODIUM_PATH, R_OK) == 0 ? SODIUM_PATH : "");

    /*
     * Put configuration file configuration options second to last. Put command line configuration
     * options at the end. Do this so they override the standard configuration.
     */
    s = GVS(WIREDTIGER_CONFIG);
    if (strcmp(s, "off") != 0)
        CONFIG_APPEND(p, ",%s", s);
    if (g.config_open != NULL)
        CONFIG_APPEND(p, ",%s", g.config_open);

    if (max == 0)
        testutil_die(ENOMEM, "wiredtiger_open configuration buffer too small");

    testutil_checkfmt(wiredtiger_open(home, &event_handler, config, &conn), "%s", home);

    *connp = conn;
}

/*
 * create_object --
 *     Create the database object.
 */
static void
create_object(TABLE *table, void *arg)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    size_t max;
    uint32_t maxleafkey, maxleafvalue;
    char config[4096], *p;
    const char *s;

    conn = (WT_CONNECTION *)arg;
    p = config;
    max = sizeof(config);

/* The page must be a multiple of the allocation size, and 512 always works. */
#define BLOCK_ALLOCATION_SIZE 512
    CONFIG_APPEND(p,
      "key_format=%s,allocation_size=%d,%s,internal_page_max=%" PRIu32 ",leaf_page_max=%" PRIu32
      ",memory_page_max=%" PRIu32,
      (table->type == ROW) ? "u" : "r", BLOCK_ALLOCATION_SIZE,
      TV(DISK_FIRSTFIT) ? "block_allocation=first" : "", table->max_intl_page, table->max_leaf_page,
      table->max_mem_page);

    /*
     * Configure the maximum key/value sizes, but leave it as the default if we come up with
     * something crazy.
     */
    maxleafkey = mmrand(NULL, table->max_leaf_page / 50, table->max_leaf_page / 40);
    if (maxleafkey > 20)
        CONFIG_APPEND(p, ",leaf_key_max=%" PRIu32, maxleafkey);
    maxleafvalue = mmrand(NULL, table->max_leaf_page * 10, table->max_leaf_page / 40);
    if (maxleafvalue > 40 && maxleafvalue < 100 * 1024)
        CONFIG_APPEND(p, ",leaf_value_max=%" PRIu32, maxleafvalue);

    switch (table->type) {
    case FIX:
        CONFIG_APPEND(p, ",value_format=%" PRIu32 "t", TV(BTREE_BITCNT));
        break;
    case ROW:
        CONFIG_APPEND(p, ",prefix_compression=%s,prefix_compression_min=%" PRIu32,
          TV(BTREE_PREFIX_COMPRESSION) == 0 ? "false" : "true", TV(BTREE_PREFIX_COMPRESSION_MIN));
        if (TV(BTREE_REVERSE))
            CONFIG_APPEND(p, ",collator=reverse");
    /* FALLTHROUGH */
    case VAR:
        if (TV(BTREE_HUFFMAN_VALUE))
            CONFIG_APPEND(p, ",huffman_value=english");
        if (TV(BTREE_DICTIONARY))
            CONFIG_APPEND(p, ",dictionary=%" PRIu32, mmrand(NULL, 123, 517));
        break;
    }

    /* Configure checksums. */
    CONFIG_APPEND(p, ",checksum=\"%s\"", TVS(DISK_CHECKSUM));

    /* Configure compression. */
    s = TVS(BTREE_COMPRESSION);
    CONFIG_APPEND(p, ",block_compressor=\"%s\"", strcmp(s, "off") == 0 ? "none" : s);

    /* Configure Btree. */
    CONFIG_APPEND(
      p, ",internal_key_truncate=%s", TV(BTREE_INTERNAL_KEY_TRUNCATION) ? "true" : "false");
    CONFIG_APPEND(p, ",split_pct=%" PRIu32, TV(BTREE_SPLIT_PCT));

    /* Assertions: assertions slow down the code for additional diagnostic checking.  */
    if (GV(ASSERT_READ_TIMESTAMP))
        CONFIG_APPEND(
          p, ",assert=(read_timestamp=%s)", g.transaction_timestamps_config ? "always" : "never");
    if (GV(ASSERT_WRITE_TIMESTAMP))
        CONFIG_APPEND(p, ",assert=(write_timestamp=on),write_timestamp_usage=%s",
          g.transaction_timestamps_config ? "key_consistent" : "never");

    /* Configure LSM. */
    if (DATASOURCE(table, "lsm")) {
        CONFIG_APPEND(p, ",type=lsm,lsm=(");
        CONFIG_APPEND(p, "auto_throttle=%s,", TV(LSM_AUTO_THROTTLE) ? "true" : "false");
        CONFIG_APPEND(p, "chunk_size=%" PRIu32 "MB,", TV(LSM_CHUNK_SIZE));
        /*
         * We can't set bloom_oldest without bloom, and we want to test with Bloom filters on most
         * of the time anyway.
         */
        if (TV(LSM_BLOOM_OLDEST))
            TV(LSM_BLOOM) = 1;
        CONFIG_APPEND(p, "bloom=%s,", TV(LSM_BLOOM) ? "true" : "false");
        CONFIG_APPEND(p, "bloom_bit_count=%" PRIu32 ",", TV(LSM_BLOOM_BIT_COUNT));
        CONFIG_APPEND(p, "bloom_hash_count=%" PRIu32 ",", TV(LSM_BLOOM_HASH_COUNT));
        CONFIG_APPEND(p, "bloom_oldest=%s,", TV(LSM_BLOOM_OLDEST) ? "true" : "false");
        CONFIG_APPEND(p, "merge_max=%" PRIu32 ",", TV(LSM_MERGE_MAX));
        CONFIG_APPEND(p, ",)");
    }

    if (max == 0)
        testutil_die(ENOMEM, "WT_SESSION.create configuration buffer too small");

    /*
     * Create the underlying store.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_checkfmt(session->create(session, table->uri, config), "%s", table->uri);
    testutil_check(session->close(session, NULL));
}

/*
 * wts_create_home --
 *     Remove and re-create the directory.
 */
void
wts_create_home(void)
{
    char buf[MAX_FORMAT_PATH * 2];

    testutil_check(__wt_snprintf(buf, sizeof(buf), "rm -rf %s && mkdir %s", g.home, g.home));
    testutil_checkfmt(system(buf), "database home creation (\"%s\") failed", buf);
}

/*
 * wts_create_database --
 *     Create the database.
 */
void
wts_create_database(void)
{
    WT_CONNECTION *conn;

    create_database(g.home, &conn);

    g.wts_conn = conn;
    tables_apply(create_object, g.wts_conn);
    if (GV(RUNS_IN_MEMORY) != 0)
        g.wts_conn_inmemory = g.wts_conn;
    else
        testutil_check(conn->close(conn, NULL));
    g.wts_conn = NULL;
}

/*
 * wts_open --
 *     Open a connection to a WiredTiger database.
 */
void
wts_open(const char *home, WT_CONNECTION **connp, WT_SESSION **sessionp, bool allow_verify)
{
    WT_CONNECTION *conn;
    size_t max;
    char config[1024], *p;
    const char *enc;

    *connp = NULL;
    *sessionp = NULL;

    p = config;
    max = sizeof(config);
    config[0] = '\0';

    /* Configuration settings that are not persistent between open calls. */
    enc = encryptor_at_open();
    if (enc != NULL)
        CONFIG_APPEND(p, ",encryption=(name=%s)", enc);

    CONFIG_APPEND(p, ",error_prefix=\"%s\"", progname);

    /* Optional timing stress. */
    configure_timing_stress(p, max);

    /* If in-memory, there's only a single, shared WT_CONNECTION handle. */
    if (GV(RUNS_IN_MEMORY) != 0)
        conn = g.wts_conn_inmemory;
    else {
#if WIREDTIGER_VERSION_MAJOR >= 10
        if (GV(OPS_VERIFY) && allow_verify)
            CONFIG_APPEND(p, ",verify_metadata=true");
#else
        WT_UNUSED(allow_verify);
#endif
        testutil_checkfmt(wiredtiger_open(home, &event_handler, config, &conn), "%s", home);
    }

    testutil_check(conn->open_session(conn, NULL, NULL, sessionp));
    *connp = conn;
}

/*
 * wts_close --
 *     Close the open database.
 */
void
wts_close(WT_CONNECTION **connp, WT_SESSION **sessionp)
{
    WT_CONNECTION *conn;

    conn = *connp;
    *connp = NULL;

    /*
     * If running in-memory, there's only a single, shared WT_CONNECTION handle. Format currently
     * doesn't perform the operations coded to close and then re-open the database on in-memory
     * databases (for example, salvage), so the close gets all references, it doesn't have to avoid
     * closing the real handle.
     */
    if (conn == g.wts_conn_inmemory)
        g.wts_conn_inmemory = NULL;
    *sessionp = NULL;

    if (g.backward_compatible)
        testutil_check(conn->reconfigure(conn, "compatibility=(release=3.3)"));

    testutil_check(conn->close(conn, GV(WIREDTIGER_LEAK_MEMORY) ? "leak_memory" : NULL));
}

/*
 * wts_verify --
 *     Verify a table.
 */
void
wts_verify(TABLE *table, void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;

    conn = (WT_CONNECTION *)arg;

    if (GV(OPS_VERIFY) == 0)
        return;

    /*
     * Verify can return EBUSY if the handle isn't available. Don't yield and retry, in the case of
     * LSM, the handle may not be available for a long time.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    session->app_private = table->track_prefix;
    ret = session->verify(session, table->uri, "strict");
    testutil_assert(ret == 0 || ret == EBUSY);
    testutil_check(session->close(session, NULL));
}

struct stats_args {
    FILE *fp;
    WT_SESSION *session;
};

/*
 * stats_data_print --
 *     Print out the statistics.
 */
static void
stats_data_print(WT_SESSION *session, const char *uri, FILE *fp)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t v;
    const char *desc, *pval;

    wiredtiger_open_cursor(session, uri, NULL, &cursor);
    while (
      (ret = cursor->next(cursor)) == 0 && (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
        testutil_assert(fprintf(fp, "%s=%s\n", desc, pval) >= 0);
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
}

/*
 * stats_data_source --
 *     Dump each data source's statistics.
 */
static void
stats_data_source(TABLE *table, void *arg)
{
    struct stats_args *args;
    FILE *fp;
    WT_SESSION *session;
    char buf[1024];

    args = arg;
    fp = args->fp;
    session = args->session;

    testutil_assert(fprintf(fp, "\n\n====== Data source statistics: %s\n", table->uri) >= 0);
    testutil_check(__wt_snprintf(buf, sizeof(buf), "statistics:%s", table->uri));
    stats_data_print(session, buf, fp);
}

/*
 * wts_stats --
 *     Dump the run's statistics.
 */
void
wts_stats(void)
{
    struct stats_args args;
    FILE *fp;
    WT_CONNECTION *conn;
    WT_SESSION *session;

    /* Ignore statistics if they're not configured. */
    if (GV(STATISTICS) == 0)
        return;

    conn = g.wts_conn;
    track("stat", 0ULL);

    /* Connection statistics. */
    testutil_assert((fp = fopen(g.home_stats, "w")) != NULL);
    testutil_assert(fprintf(fp, "====== Connection statistics:\n") >= 0);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    stats_data_print(session, "statistics:", fp);

    /* Data source statistics. */
    args.fp = fp;
    args.session = session;
    tables_apply(stats_data_source, &args);

    testutil_check(session->close(session, NULL));
    fclose_and_clear(&fp);
}
