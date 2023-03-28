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
    SAP *sap;
    WT_DECL_RET;
    int nw;

    (void)handler;

    /*
     * Log to the trace database when tracing messages. In threaded paths there will be a per-thread
     * session reference to the tracing database, but that requires a session, and verbose messages
     * can be generated in the library when we don't have a session. There's a global session we can
     * use, but that requires locking.
     */
    if ((sap = session->app_private) != NULL && sap->trace != NULL) {
        testutil_check(sap->trace->log_printf(sap->trace, "%s", message));
        return (0);
    }
    if (g.trace_session != NULL) {
        __wt_spin_lock((WT_SESSION_IMPL *)g.trace_session, &g.trace_lock);
        testutil_check(g.trace_session->log_printf(g.trace_session, "%s", message));
        __wt_spin_unlock((WT_SESSION_IMPL *)g.trace_session, &g.trace_lock);
        return (0);
    }

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

    if (session->app_private != NULL) {
        testutil_check(
          __wt_snprintf(buf, sizeof(buf), "%s %s", (char *)session->app_private, operation));
        track(buf, progress);
        return (0);
    }

    track(operation, progress);
    return (0);
}

static WT_EVENT_HANDLER event_handler = {NULL, handle_message, handle_progress, NULL, NULL};

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
configure_timing_stress(char **p, size_t max)
{
    CONFIG_APPEND(*p, ",timing_stress_for_test=[");
    if (GV(STRESS_AGGRESSIVE_SWEEP))
        CONFIG_APPEND(*p, ",aggressive_sweep");
    if (GV(STRESS_CHECKPOINT))
        CONFIG_APPEND(*p, ",checkpoint_slow");
    if (GV(STRESS_CHECKPOINT_EVICT_PAGE))
        CONFIG_APPEND(*p, ",checkpoint_evict_page");
    if (GV(STRESS_CHECKPOINT_PREPARE))
        CONFIG_APPEND(*p, ",prepare_checkpoint_delay");
    if (GV(STRESS_EVICT_REPOSITION))
        CONFIG_APPEND(*p, ",evict_reposition");
    if (GV(STRESS_FAILPOINT_EVICTION_FAIL_AFTER_RECONCILIATION))
        CONFIG_APPEND(*p, ",failpoint_eviction_fail_after_reconciliation");
    if (GV(STRESS_FAILPOINT_HS_DELETE_KEY_FROM_TS))
        CONFIG_APPEND(*p, ",failpoint_history_store_delete_key_from_ts");
    if (GV(STRESS_HS_CHECKPOINT_DELAY))
        CONFIG_APPEND(*p, ",history_store_checkpoint_delay");
    if (GV(STRESS_HS_SEARCH))
        CONFIG_APPEND(*p, ",history_store_search");
    if (GV(STRESS_HS_SWEEP))
        CONFIG_APPEND(*p, ",history_store_sweep_race");
    if (GV(STRESS_SLEEP_BEFORE_READ_OVERFLOW_ONPAGE))
        CONFIG_APPEND(*p, ",sleep_before_read_overflow_onpage");
    if (GV(STRESS_SPLIT_1))
        CONFIG_APPEND(*p, ",split_1");
    if (GV(STRESS_SPLIT_2))
        CONFIG_APPEND(*p, ",split_2");
    if (GV(STRESS_SPLIT_3))
        CONFIG_APPEND(*p, ",split_3");
    if (GV(STRESS_SPLIT_4))
        CONFIG_APPEND(*p, ",split_4");
    if (GV(STRESS_SPLIT_5))
        CONFIG_APPEND(*p, ",split_5");
    if (GV(STRESS_SPLIT_6))
        CONFIG_APPEND(*p, ",split_6");
    if (GV(STRESS_SPLIT_7))
        CONFIG_APPEND(*p, ",split_7");
    if (GV(STRESS_SPLIT_8))
        CONFIG_APPEND(*p, ",split_8");
    CONFIG_APPEND(*p, "]");
}

/*
 * configure_file_manager --
 *     Configure file manager settings.
 */
static void
configure_file_manager(char **p, size_t max)
{
    CONFIG_APPEND(*p, ",file_manager=[");
    if (GV(FILE_MANAGER_CLOSE_HANDLE_MINIMUM) != 0)
        CONFIG_APPEND(*p, ",close_handle_minimum=%" PRIu32, GV(FILE_MANAGER_CLOSE_HANDLE_MINIMUM));
    if (GV(FILE_MANAGER_CLOSE_IDLE_TIME) != 0)
        CONFIG_APPEND(*p, ",close_idle_time=%" PRIu32, GV(FILE_MANAGER_CLOSE_IDLE_TIME));
    if (GV(FILE_MANAGER_CLOSE_SCAN_INTERVAL) != 0)
        CONFIG_APPEND(*p, ",close_scan_interval=%" PRIu32, GV(FILE_MANAGER_CLOSE_SCAN_INTERVAL));
    CONFIG_APPEND(*p, "]");
}

/*
 * configure_debug_mode --
 *     Configure debug settings.
 */
static void
configure_debug_mode(char **p, size_t max)
{
    CONFIG_APPEND(*p, ",debug_mode=[");

    if (GV(DEBUG_CHECKPOINT_RETENTION) != 0)
        CONFIG_APPEND(*p, ",checkpoint_retention=%" PRIu32, GV(DEBUG_CHECKPOINT_RETENTION));
    if (GV(DEBUG_CURSOR_REPOSITION))
        CONFIG_APPEND(*p, ",cursor_reposition=true");
    if (GV(DEBUG_EVICTION))
        CONFIG_APPEND(*p, ",eviction=true");
    /*
     * Don't configure log retention debug mode during backward compatibility mode. Compatibility
     * requires removing log files on reconfigure. When the version is changed for compatibility,
     * reconfigure requires removing earlier log files and log retention can make that seem to hang.
     */
    if (GV(DEBUG_LOG_RETENTION) != 0 && !g.backward_compatible)
        CONFIG_APPEND(*p, ",log_retention=%" PRIu32, GV(DEBUG_LOG_RETENTION));
    if (GV(DEBUG_REALLOC_EXACT))
        CONFIG_APPEND(*p, ",realloc_exact=true");
    if (GV(DEBUG_REALLOC_MALLOC))
        CONFIG_APPEND(*p, ",realloc_malloc=true");
    if (GV(DEBUG_SLOW_CHECKPOINT))
        CONFIG_APPEND(*p, ",slow_checkpoint=true");
    if (GV(DEBUG_TABLE_LOGGING))
        CONFIG_APPEND(*p, ",table_logging=true");
    if (GV(DEBUG_UPDATE_RESTORE_EVICT))
        CONFIG_APPEND(*p, ",update_restore_evict=true");
    CONFIG_APPEND(*p, "]");
}

/*
 * configure_tiered_storage --
 *     Configure tiered storage settings for opening a connection.
 */
static void
configure_tiered_storage(const char *home, char **p, size_t max, char *ext_cfg, size_t ext_cfg_size)
{
    TEST_OPTS opts;
    char tiered_cfg[1024];

    if (!g.tiered_storage_config) {
        testutil_assert(ext_cfg_size > 0);
        ext_cfg[0] = '\0';
        return;
    }

    memset(&opts, 0, sizeof(opts));
    opts.tiered_storage = true;

    /*
     * We need to cast these values. Normally, testutil allocates and fills these strings based on
     * command line arguments and frees them when done. Format doesn't use the standard test command
     * line parser and doesn't rely on testutil to free anything in this struct. We're only using
     * the options struct on a temporary basis to help create the tiered storage configuration.
     */
    opts.tiered_storage_source = (char *)GVS(TIERED_STORAGE_STORAGE_SOURCE);
    opts.home = (char *)home;
    opts.build_dir = (char *)BUILDDIR;

    /*
     * Have testutil create the bucket directory for us when using the directory store.
     */
    opts.make_bucket_dir = true;

    /*
     * Use an absolute path for the bucket directory when using the directory store. Then we can
     * create copies of the home directory to be used for backup, and we'll be able to find the
     * bucket.
     */
    opts.absolute_bucket_dir = true;

    testutil_tiered_storage_configuration(
      &opts, tiered_cfg, sizeof(tiered_cfg), ext_cfg, ext_cfg_size);
    CONFIG_APPEND(*p, ",%s", tiered_cfg);
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
    char config[8 * 1024], *p, tiered_ext_cfg[1024];
    const char *s, *sources;

    p = config;
    max = sizeof(config);

    CONFIG_APPEND(p,
      "create=true"
      ",cache_size=%" PRIu32
      "MB"
      ",checkpoint_sync=false"
      ",error_prefix=\"%s\""
      ",operation_timeout_ms=2000"
      ",statistics=(%s)",
      GV(CACHE), progname, GVS(STATISTICS_MODE));

    /* Statistics log configuration. */
    sources = GVS(STATISTICS_LOG_SOURCES);
    if (strcmp(sources, "off") != 0)
        CONFIG_APPEND(p, ",statistics_log=(json,on_close,wait=5,sources=(\"%s\"))", sources);
    else
        CONFIG_APPEND(p, ",statistics_log=(json,on_close,wait=5)");

    /* In-memory configuration. */
    if (GV(RUNS_IN_MEMORY) != 0)
        CONFIG_APPEND(p, ",in_memory=1");

    /* Block cache configuration. */
    if (GV(BLOCK_CACHE) != 0)
        CONFIG_APPEND(p,
          ",block_cache=(enabled=true,type=\"dram\""
          ",cache_on_checkpoint=%s"
          ",cache_on_writes=%s"
          ",size=%" PRIu32 "MB)",
          GV(BLOCK_CACHE_CACHE_ON_CHECKPOINT) == 0 ? "false" : "true",
          GV(BLOCK_CACHE_CACHE_ON_WRITES) == 0 ? "false" : "true", GV(BLOCK_CACHE_SIZE));

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
          ",log=(enabled=true,%s=%d,prealloc=%d,file_max=%" PRIu32 ",compressor=\"%s\")",
          g.backward_compatible ? "archive" : "remove", GV(LOGGING_REMOVE) ? 1 : 0,
          GV(LOGGING_PREALLOC) ? 1 : 0, KILOBYTE(GV(LOGGING_FILE_MAX)),
          strcmp(s, "off") == 0 ? "none" : s);
    }

    /* Encryption. */
    CONFIG_APPEND(p, ",encryption=(name=%s)", encryptor());

    /* Miscellaneous. */
    if (GV(BUFFER_ALIGNMENT)) {
#ifdef HAVE_POSIX_MEMALIGN
        CONFIG_APPEND(p, ",buffer_alignment=512");
#else
        WARN("%s", "Ignoring buffer_alignment=1, missing HAVE_POSIX_MEMALIGN support")
#endif
    }

    if (GV(DISK_MMAP))
        CONFIG_APPEND(p, ",mmap=1");
    if (GV(DISK_MMAP_ALL))
        CONFIG_APPEND(p, ",mmap_all=1");

    if (GV(DISK_DIRECT_IO))
        CONFIG_APPEND(p, ",direct_io=(data)");

    if (GV(DISK_DATA_EXTEND))
        CONFIG_APPEND(p, ",file_extend=(data=8MB)");

    /* Optional timing stress. */
    configure_timing_stress(&p, max);

    /* Optional file manager. */
    configure_file_manager(&p, max);

    /* Optional debug mode. */
    configure_debug_mode(&p, max);

    /* Optional tiered storage. */
    configure_tiered_storage(home, &p, max, tiered_ext_cfg, sizeof(tiered_ext_cfg));

#define EXTENSION_PATH(path) (access((path), R_OK) == 0 ? (path) : "")

    /* Extensions. */
    CONFIG_APPEND(p,
      ",extensions=["
      "\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", %s],",
      /* Collators. */
      REVERSE_PATH,
      /* Compressors. */
      EXTENSION_PATH(LZ4_PATH), EXTENSION_PATH(SNAPPY_PATH), EXTENSION_PATH(ZLIB_PATH),
      EXTENSION_PATH(ZSTD_PATH),
      /* Encryptors. */
      EXTENSION_PATH(ROTN_PATH), EXTENSION_PATH(SODIUM_PATH),
      /* Storage source. */
      tiered_ext_cfg);

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
    SAP sap;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    size_t max;
    uint32_t maxleafkey, maxleafvalue;
    char config[4096], *p;
    const char *s;

    conn = (WT_CONNECTION *)arg;
    testutil_assert(table != NULL);

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
    maxleafkey = mmrand(&g.extra_rnd, table->max_leaf_page / 50, table->max_leaf_page / 40);
    if (maxleafkey > 20)
        CONFIG_APPEND(p, ",leaf_key_max=%" PRIu32, maxleafkey);
    maxleafvalue = mmrand(&g.extra_rnd, table->max_leaf_page * 10, table->max_leaf_page / 40);
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
            CONFIG_APPEND(p, ",dictionary=%" PRIu32, mmrand(&g.extra_rnd, 123, 517));
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

    /* Timestamps */
    if (g.transaction_timestamps_config)
        CONFIG_APPEND(p, ",log=(enabled=false)");

    /* Assertions: assertions slow down the code for additional diagnostic checking.  */
    if (GV(ASSERT_READ_TIMESTAMP))
        CONFIG_APPEND(
          p, ",assert=(read_timestamp=%s)", g.transaction_timestamps_config ? "none" : "never");

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
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);
    testutil_checkfmt(session->create(session, table->uri, config), "%s", table->uri);
    wt_wrap_close_session(session);
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
wts_open(const char *home, WT_CONNECTION **connp, bool verify_metadata)
{
    WT_CONNECTION *conn;
    size_t max;
    char config[1024], *p;
    const char *enc, *s;

    *connp = NULL;

    p = config;
    max = sizeof(config);
    config[0] = '\0';

    /* Configuration settings that are not persistent between open calls. */
    enc = encryptor_at_open();
    if (enc != NULL)
        CONFIG_APPEND(p, ",encryption=(name=%s)", enc);

    CONFIG_APPEND(
      p, ",error_prefix=\"%s\",statistics=(fast),statistics_log=(json,on_close,wait=5)", progname);

    /* Optional timing stress. */
    configure_timing_stress(&p, max);

    /* Optional file manager. */
    configure_file_manager(&p, max);

    /* Optional debug mode. */
    configure_debug_mode(&p, max);

    /* If in-memory, there's only a single, shared WT_CONNECTION handle. */
    if (GV(RUNS_IN_MEMORY) != 0)
        conn = g.wts_conn_inmemory;
    else {
        /*
         * Put configuration file configuration options second to last. Put command line
         * configuration options at the end. Do this so they override the standard configuration.
         */
        s = GVS(WIREDTIGER_CONFIG);
        if (strcmp(s, "off") != 0)
            CONFIG_APPEND(p, ",%s", s);
        if (g.config_open != NULL)
            CONFIG_APPEND(p, ",%s", g.config_open);

#if WIREDTIGER_VERSION_MAJOR >= 10
        if (GV(OPS_VERIFY) && verify_metadata)
            CONFIG_APPEND(p, ",verify_metadata=true");
#else
        WT_UNUSED(verify_metadata);
#endif
        testutil_checkfmt(wiredtiger_open(home, &event_handler, config, &conn), "%s", home);
    }

    *connp = conn;
}

/*
 * wts_close --
 *     Close the open database.
 */
void
wts_close(WT_CONNECTION **connp)
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

    if (g.backward_compatible)
        testutil_check(conn->reconfigure(conn, "compatibility=(release=3.3)"));

    testutil_check(conn->close(conn, GV(WIREDTIGER_LEAK_MEMORY) ? "leak_memory" : NULL));
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

    wt_wrap_open_cursor(session, uri, NULL, &cursor);
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
    testutil_assert(table != NULL);

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
    SAP sap;
    WT_CONNECTION *conn;
    WT_SESSION *session;

    conn = g.wts_conn;
    track("stat", 0ULL);

    /* Connection statistics. */
    testutil_assert((fp = fopen(g.home_stats, "w")) != NULL);
    testutil_assert(fprintf(fp, "====== Connection statistics:\n") >= 0);

    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);
    stats_data_print(session, "statistics:", fp);

    /*
     * Data source statistics.
     *     FIXME-WT-9785: Statistics cursors on tiered storage objects are not yet supported.
     */
    if (!g.tiered_storage_config) {
        args.fp = fp;
        args.session = session;
        tables_apply(stats_data_source, &args);
    }

    wt_wrap_close_session(session);

    fclose_and_clear(&fp);
}
